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

// Unit tests for third_party/geos/src/triangulate/tri/TriEdge.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/triangulate/tri/TriEdge.cpp

#include "third_party/geos/include/geos/triangulate/tri/TriEdge.h"

#include <cmath>
#include <cstddef>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::triangulate::tri::TriEdge;

TEST(TriEdgeTest, NormalizesAscendingXToDescendingEndpoints) {
  const TriEdge asc(Coordinate(1.0, 2.0), Coordinate(3.0, 4.0));
  EXPECT_DOUBLE_EQ(asc.p0.x, 3.0);
  EXPECT_DOUBLE_EQ(asc.p0.y, 4.0);
  EXPECT_TRUE(std::isnan(asc.p0.z));
  EXPECT_DOUBLE_EQ(asc.p1.x, 1.0);
  EXPECT_DOUBLE_EQ(asc.p1.y, 2.0);
  EXPECT_TRUE(std::isnan(asc.p1.z));

  const TriEdge desc(Coordinate(3.0, 4.0), Coordinate(1.0, 2.0));
  EXPECT_DOUBLE_EQ(desc.p0.x, 3.0);
  EXPECT_DOUBLE_EQ(desc.p0.y, 4.0);
  EXPECT_DOUBLE_EQ(desc.p1.x, 1.0);
  EXPECT_DOUBLE_EQ(desc.p1.y, 2.0);
}

TEST(TriEdgeTest, NormalizesTiedXUsingYCoordinate) {
  const TriEdge asc_y(Coordinate(5.0, 1.0), Coordinate(5.0, 9.0));
  EXPECT_TRUE(asc_y.p0.equals2D(Coordinate(5.0, 9.0)));
  EXPECT_TRUE(asc_y.p1.equals2D(Coordinate(5.0, 1.0)));

  const TriEdge desc_y(Coordinate(5.0, 9.0), Coordinate(5.0, 1.0));
  EXPECT_TRUE(desc_y.p0.equals2D(Coordinate(5.0, 9.0)));
  EXPECT_TRUE(desc_y.p1.equals2D(Coordinate(5.0, 1.0)));

  const TriEdge negative(Coordinate(-2.5, -10.0), Coordinate(-2.5, -3.25));
  EXPECT_TRUE(negative.p0.equals2D(Coordinate(-2.5, -3.25)));
  EXPECT_TRUE(negative.p1.equals2D(Coordinate(-2.5, -10.0)));
}

TEST(TriEdgeTest, NormalizationSwapsZOnReorderAndPreservesEqual2DEndpoints) {
  const TriEdge swapped(Coordinate(1.0, 2.0, 10.0), Coordinate(3.0, 4.0, 20.0));
  EXPECT_TRUE(swapped.p0.equals3D(Coordinate(3.0, 4.0, 20.0)));
  EXPECT_TRUE(swapped.p1.equals3D(Coordinate(1.0, 2.0, 10.0)));

  const TriEdge zero_length(Coordinate(4.0, 5.0, 100.0),
                            Coordinate(4.0, 5.0, 200.0));
  EXPECT_TRUE(zero_length.p0.equals3D(Coordinate(4.0, 5.0, 100.0)));
  EXPECT_TRUE(zero_length.p1.equals3D(Coordinate(4.0, 5.0, 200.0)));
}

TEST(TriEdgeTest, EqualityComparesNormalized2DEndpoints) {
  const Coordinate a(1.0, 2.0, 5.0);
  const Coordinate b(3.0, 4.0, 8.0);
  const Coordinate c(7.0, 8.0, 0.0);

  const TriEdge ab(a, b);
  const TriEdge ab_copy(a, b);
  const TriEdge ba(b, a);
  const TriEdge ab_different_z(Coordinate(1.0, 2.0, 99.0),
                               Coordinate(3.0, 4.0, -42.0));
  const TriEdge ac(a, c);
  const TriEdge bc(b, c);
  const TriEdge other(Coordinate(0.0, 0.0), Coordinate(9.0, 9.0));

  EXPECT_TRUE(ab == ab_copy);
  EXPECT_TRUE(ab == ba);
  EXPECT_TRUE(ab == ab_different_z);
  EXPECT_EQ(ab, ba);

  EXPECT_FALSE(ab == bc);
  EXPECT_FALSE(ab == ac);
  EXPECT_FALSE(ab == other);
  EXPECT_NE(ab, ac);
  EXPECT_NE(ab, bc);
}

TEST(TriEdgeTest, HashCodeConsistentWithEqualityInUnorderedContainers) {
  const TriEdge::HashCode hasher;
  const TriEdge ab(Coordinate(1.0, 2.0), Coordinate(3.0, 4.0));
  const TriEdge ba(Coordinate(3.0, 4.0), Coordinate(1.0, 2.0));
  const TriEdge ab_with_z(Coordinate(1.0, 2.0, 11.0),
                          Coordinate(3.0, 4.0, 22.0));
  const TriEdge bc(Coordinate(3.0, 4.0), Coordinate(5.0, 1.0));
  const TriEdge ca(Coordinate(5.0, 1.0), Coordinate(1.0, 2.0));
  const TriEdge degenerate(Coordinate(1.0, 2.0), Coordinate(1.0, 2.0));

  const std::size_t ab_hash = hasher(ab);
  EXPECT_EQ(ab_hash, hasher(ba));
  EXPECT_EQ(ab_hash, hasher(ab_with_z));

  std::unordered_map<TriEdge, int, TriEdge::HashCode> edge_map;
  edge_map[ab] = 1;
  edge_map[bc] = 2;
  edge_map[ca] = 3;
  edge_map[degenerate] = 4;

  EXPECT_EQ(edge_map.size(), 4u);
  EXPECT_EQ(edge_map.at(ba), 1);
  EXPECT_EQ(edge_map.at(ab_with_z), 1);
  EXPECT_EQ(edge_map.at(TriEdge(Coordinate(5.0, 1.0), Coordinate(3.0, 4.0))),
            2);
  EXPECT_EQ(edge_map.at(degenerate), 4);

  std::unordered_set<TriEdge, TriEdge::HashCode> edge_set;
  EXPECT_TRUE(edge_set.insert(ab).second);
  EXPECT_FALSE(edge_set.insert(ba).second);
  EXPECT_FALSE(edge_set.insert(ab_with_z).second);
  EXPECT_TRUE(edge_set.insert(bc).second);
  EXPECT_EQ(edge_set.size(), 2u);
}

TEST(TriEdgeTest, StreamOutputFormatsNormalizedWktLinestring) {
  const TriEdge edge2d(Coordinate(1.0, 2.0), Coordinate(3.0, 4.0));
  std::ostringstream oss2d;
  oss2d << edge2d;
  EXPECT_EQ(oss2d.str(), "LINESTRING (3 4, 1 2)");

  const TriEdge edge3d(Coordinate(1.0, 2.0, 5.0), Coordinate(3.0, 4.0, 6.0));
  std::ostringstream oss3d;
  oss3d << edge3d << " | " << edge2d;
  EXPECT_EQ(oss3d.str(), "LINESTRING (3 4 6, 1 2 5) | LINESTRING (3 4, 1 2)");
}

}  // namespace
}  // namespace geos_google
