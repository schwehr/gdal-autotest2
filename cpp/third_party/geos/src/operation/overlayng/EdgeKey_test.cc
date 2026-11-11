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

// Unit tests for third_party/geos/include/geos/operation/overlayng/EdgeKey.h
// and third_party/geos/src/operation/overlayng/EdgeKey.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/operation/overlayng/EdgeKey.h

#include "third_party/geos/include/geos/operation/overlayng/EdgeKey.h"

#include <initializer_list>
#include <map>
#include <memory>
#include <set>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/operation/overlayng/Edge.h"
#include "third_party/geos/include/geos/operation/overlayng/EdgeSourceInfo.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::operation::overlayng::Edge;
using geos::operation::overlayng::EdgeKey;
using geos::operation::overlayng::EdgeSourceInfo;
using geos::util::GEOSException;

Edge MakeEdge(std::initializer_list<Coordinate> coords) {
  const EdgeSourceInfo info(0);
  return Edge(std::make_unique<CoordinateSequence>(coords), &info);
}

TEST(EdgeKeyTest, CanonicalizesForwardAndReversedEdges) {
  const Edge fwd_2pt = MakeEdge({Coordinate(0, 0), Coordinate(3, 4)});
  const Edge rev_2pt = MakeEdge({Coordinate(3, 4), Coordinate(0, 0)});
  ASSERT_TRUE(fwd_2pt.direction());
  ASSERT_FALSE(rev_2pt.direction());

  const EdgeKey key_fwd(&fwd_2pt);
  const EdgeKey key_rev(&rev_2pt);
  EXPECT_TRUE(key_fwd.equals(&key_rev));
  EXPECT_EQ(key_fwd.compareTo(&key_rev), 0);
  EXPECT_TRUE(key_fwd == key_rev);
  EXPECT_FALSE(key_fwd < key_rev);
  EXPECT_FALSE(key_rev < key_fwd);

  // Multi-segment edges use first two points in canonical direction.
  const Edge fwd_multi =
      MakeEdge({Coordinate(0, 0), Coordinate(1, 2), Coordinate(5, 5)});
  const Edge rev_multi =
      MakeEdge({Coordinate(5, 5), Coordinate(1, 2), Coordinate(0, 0)});
  const EdgeKey key_fwd_multi(&fwd_multi);
  const EdgeKey key_rev_multi(&rev_multi);
  EXPECT_EQ(key_fwd_multi, key_rev_multi);
  EXPECT_EQ(key_fwd_multi.compareTo(&key_rev_multi), 0);

  // Vertical segment where x coordinates match and y decides direction.
  const Edge vert_fwd = MakeEdge({Coordinate(2, -3), Coordinate(2, 7)});
  const Edge vert_rev = MakeEdge({Coordinate(2, 7), Coordinate(2, -3)});
  EXPECT_EQ(EdgeKey(&vert_fwd), EdgeKey(&vert_rev));
}

TEST(EdgeKeyTest, CanonicalizesClosedRingEdges) {
  const Edge ring_fwd =
      MakeEdge({Coordinate(0, 0), Coordinate(1, 0), Coordinate(1, 1),
                Coordinate(2, 0), Coordinate(0, 0)});
  const Edge ring_rev =
      MakeEdge({Coordinate(0, 0), Coordinate(2, 0), Coordinate(1, 1),
                Coordinate(1, 0), Coordinate(0, 0)});

  ASSERT_TRUE(ring_fwd.direction());
  ASSERT_FALSE(ring_rev.direction());

  const EdgeKey ek_fwd(&ring_fwd);
  const EdgeKey ek_rev(&ring_rev);
  EXPECT_TRUE(ek_fwd.equals(&ek_rev));
  EXPECT_EQ(ek_fwd.compareTo(&ek_rev), 0);
  EXPECT_EQ(ek_fwd, ek_rev);
}

TEST(EdgeKeyTest, IgnoresZOrdinates) {
  const Edge edge_a = MakeEdge({Coordinate(1, 2, 10), Coordinate(4, 5, 20)});
  const Edge edge_b = MakeEdge({Coordinate(4, 5, -9), Coordinate(1, 2, 99)});
  EXPECT_EQ(EdgeKey(&edge_a), EdgeKey(&edge_b));
}

TEST(EdgeKeyTest, CompareToCoversAllCoordinateOrderingCases) {
  const Edge base_edge = MakeEdge({Coordinate(10, 20), Coordinate(30, 40)});
  const EdgeKey base(&base_edge);

  struct OrderCase {
    Coordinate p0;
    Coordinate p1;
    int expected_cmp_to_base;
  };

  const OrderCase cases[] = {
      {Coordinate(9, 99), Coordinate(99, 99), -1},   // p0x <
      {Coordinate(11, 0), Coordinate(12, 0), 1},     // p0x >
      {Coordinate(10, 19), Coordinate(50, 50), -1},  // p0y <
      {Coordinate(10, 21), Coordinate(50, 50), 1},   // p0y >
      {Coordinate(10, 20), Coordinate(29, 99), -1},  // p1x <
      {Coordinate(10, 20), Coordinate(31, 0), 1},    // p1x >
      {Coordinate(10, 20), Coordinate(30, 39), -1},  // p1y <
      {Coordinate(10, 20), Coordinate(30, 41), 1},   // p1y >
      {Coordinate(10, 20), Coordinate(30, 40), 0},   // equal
  };

  for (const OrderCase& tc : cases) {
    const Edge candidate_edge = MakeEdge({tc.p0, tc.p1});
    const EdgeKey candidate(&candidate_edge);

    EXPECT_EQ(candidate.compareTo(&base), tc.expected_cmp_to_base);
    EXPECT_EQ(base.compareTo(&candidate), -tc.expected_cmp_to_base);
    EXPECT_EQ(candidate < base, tc.expected_cmp_to_base < 0);
    EXPECT_EQ(base < candidate, tc.expected_cmp_to_base > 0);
    EXPECT_EQ(candidate == base, tc.expected_cmp_to_base == 0);
  }
}

TEST(EdgeKeyTest, EqualsChecksAllFourOrdinates) {
  const Edge base_edge = MakeEdge({Coordinate(10, 20), Coordinate(30, 40)});
  const EdgeKey base(&base_edge);

  const Edge same_edge = MakeEdge({Coordinate(10, 20), Coordinate(30, 40)});
  const Edge diff_p0x = MakeEdge({Coordinate(11, 20), Coordinate(30, 40)});
  const Edge diff_p0y = MakeEdge({Coordinate(10, 21), Coordinate(30, 40)});
  const Edge diff_p1x = MakeEdge({Coordinate(10, 20), Coordinate(31, 40)});
  const Edge diff_p1y = MakeEdge({Coordinate(10, 20), Coordinate(30, 41)});

  const EdgeKey k_same(&same_edge);
  const EdgeKey k_p0x(&diff_p0x);
  const EdgeKey k_p0y(&diff_p0y);
  const EdgeKey k_p1x(&diff_p1x);
  const EdgeKey k_p1y(&diff_p1y);

  EXPECT_TRUE(base.equals(&k_same));
  EXPECT_TRUE(base == k_same);
  EXPECT_FALSE(base.equals(&k_p0x));
  EXPECT_FALSE(base.equals(&k_p0y));
  EXPECT_FALSE(base.equals(&k_p1x));
  EXPECT_FALSE(base.equals(&k_p1y));
  EXPECT_NE(base, k_p0x);
  EXPECT_NE(base, k_p0y);
  EXPECT_NE(base, k_p1x);
  EXPECT_NE(base, k_p1y);
}

TEST(EdgeKeyTest, WorksInOrderedContainers) {
  const Edge e1 = MakeEdge({Coordinate(2, 0), Coordinate(3, 0)});
  const Edge e2 = MakeEdge({Coordinate(0, 0), Coordinate(1, 0)});
  const Edge e2_rev = MakeEdge({Coordinate(1, 0), Coordinate(0, 0)});
  const Edge e3 = MakeEdge({Coordinate(0, 0), Coordinate(1, 1)});

  std::set<EdgeKey> key_set;
  key_set.emplace(&e1);
  key_set.emplace(&e2);
  key_set.emplace(&e2_rev);
  key_set.emplace(&e3);
  EXPECT_EQ(key_set.size(), 3u);

  auto it = key_set.begin();
  EXPECT_EQ(*it++, EdgeKey(&e2));
  EXPECT_EQ(*it++, EdgeKey(&e3));
  EXPECT_EQ(*it++, EdgeKey(&e1));

  std::map<EdgeKey, int> key_map;
  key_map[EdgeKey(&e2)] = 10;
  key_map[EdgeKey(&e2_rev)] += 5;
  EXPECT_EQ(key_map.size(), 1u);
  EXPECT_EQ(key_map.at(EdgeKey(&e2)), 15);
}

TEST(EdgeKeyTest, ThrowsOnDegenerateEdge) {
  const Edge empty_edge = MakeEdge({});
  EXPECT_THROW(static_cast<void>(EdgeKey(&empty_edge)), GEOSException);

  const Edge single_pt = MakeEdge({Coordinate(1, 2)});
  EXPECT_THROW(static_cast<void>(EdgeKey(&single_pt)), GEOSException);

  const Edge palindrome =
      MakeEdge({Coordinate(0, 0), Coordinate(1, 1), Coordinate(0, 0)});
  EXPECT_THROW(static_cast<void>(EdgeKey(&palindrome)), GEOSException);
}

}  // namespace
}  // namespace geos_google
