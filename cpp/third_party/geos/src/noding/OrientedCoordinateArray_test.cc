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

#include "third_party/geos/include/geos/noding/OrientedCoordinateArray.h"

#include <set>
#include <unordered_set>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::noding::OrientedCoordinateArray;

TEST(OrientedCoordinateArrayTest, ForwardAndReversedSequencesCompareEqual) {
  const CoordinateSequence seq_fwd{Coordinate(2.0, 0.0), Coordinate(1.0, 0.0)};
  const CoordinateSequence seq_rev{Coordinate(1.0, 0.0), Coordinate(2.0, 0.0)};
  const CoordinateSequence seq_fwd_copy{Coordinate(2.0, 0.0),
                                        Coordinate(1.0, 0.0)};
  const CoordinateSequence seq_rev_copy{Coordinate(1.0, 0.0),
                                        Coordinate(2.0, 0.0)};

  const OrientedCoordinateArray oca_fwd(seq_fwd);
  const OrientedCoordinateArray oca_rev(seq_rev);
  const OrientedCoordinateArray oca_fwd_copy(seq_fwd_copy);
  const OrientedCoordinateArray oca_rev_copy(seq_rev_copy);

  // Opposite orientations representing the same canonical path.
  EXPECT_EQ(oca_fwd.compareTo(oca_rev), 0);
  EXPECT_EQ(oca_rev.compareTo(oca_fwd), 0);
  EXPECT_TRUE(oca_fwd == oca_rev);
  EXPECT_TRUE(oca_rev == oca_fwd);
  EXPECT_FALSE(oca_fwd < oca_rev);
  EXPECT_FALSE(oca_rev < oca_fwd);

  // Same orientations (both forward or both reverse).
  EXPECT_EQ(oca_fwd.compareTo(oca_fwd_copy), 0);
  EXPECT_TRUE(oca_fwd == oca_fwd_copy);
  EXPECT_EQ(oca_rev.compareTo(oca_rev_copy), 0);
  EXPECT_TRUE(oca_rev == oca_rev_copy);

  // Matching outer endpoints whose interior vertices dictate orientation.
  const CoordinateSequence ring_a{Coordinate(0.0, 0.0), Coordinate(3.0, 0.0),
                                  Coordinate(1.0, 0.0), Coordinate(0.0, 0.0)};
  const CoordinateSequence ring_b{Coordinate(0.0, 0.0), Coordinate(1.0, 0.0),
                                  Coordinate(3.0, 0.0), Coordinate(0.0, 0.0)};
  const OrientedCoordinateArray oca_ring_a(ring_a);
  const OrientedCoordinateArray oca_ring_b(ring_b);
  EXPECT_EQ(oca_ring_a.compareTo(oca_ring_b), 0);
  EXPECT_TRUE(oca_ring_a == oca_ring_b);
}

TEST(OrientedCoordinateArrayTest, PalindromeSinglePointAndIgnoresZ) {
  const CoordinateSequence single{Coordinate(5.0, 5.0)};
  const CoordinateSequence palindrome{
      Coordinate(0.0, 0.0), Coordinate(2.0, 1.0), Coordinate(0.0, 0.0)};
  const OrientedCoordinateArray oca_single(single);
  const OrientedCoordinateArray oca_palindrome(palindrome);

  EXPECT_EQ(oca_single.compareTo(oca_single), 0);
  EXPECT_TRUE(oca_single == oca_single);
  EXPECT_FALSE(oca_single < oca_single);
  EXPECT_EQ(oca_palindrome.compareTo(oca_palindrome), 0);
  EXPECT_TRUE(oca_palindrome == oca_palindrome);

  // Z ordinates do not affect compareTo, equality, or hash.
  const CoordinateSequence seq_z1{Coordinate(2.0, 0.0, 10.0),
                                  Coordinate(1.0, 0.0, 20.0)};
  const CoordinateSequence seq_z2{Coordinate(1.0, 0.0, 99.0),
                                  Coordinate(2.0, 0.0, -5.0)};
  const OrientedCoordinateArray oca_z1(seq_z1);
  const OrientedCoordinateArray oca_z2(seq_z2);
  EXPECT_EQ(oca_z1.compareTo(oca_z2), 0);
  EXPECT_TRUE(oca_z1 == oca_z2);
  EXPECT_EQ(OrientedCoordinateArray::HashCode{}(oca_z1),
            OrientedCoordinateArray::HashCode{}(oca_z2));
}

TEST(OrientedCoordinateArrayTest, CompareToOrderingAndPrefixLengths) {
  // Differing at canonical first point (one forward, one reverse).
  const CoordinateSequence seq_a{Coordinate(2.0, 0.0), Coordinate(0.0, 0.0)};
  const CoordinateSequence seq_b{Coordinate(0.0, 0.0), Coordinate(3.0, 0.0)};
  const OrientedCoordinateArray oca_a(seq_a);  // canonical: (2,0), (0,0)
  const OrientedCoordinateArray oca_b(seq_b);  // canonical: (3,0), (0,0)
  EXPECT_EQ(oca_a.compareTo(oca_b), -1);
  EXPECT_EQ(oca_b.compareTo(oca_a), 1);
  EXPECT_TRUE(oca_a < oca_b);
  EXPECT_FALSE(oca_b < oca_a);

  // Tie-break on Y ordinate when X matches.
  const CoordinateSequence seq_y1{Coordinate(2.0, 1.0), Coordinate(0.0, 0.0)};
  const CoordinateSequence seq_y2{Coordinate(2.0, 2.0), Coordinate(0.0, 0.0)};
  const OrientedCoordinateArray oca_y1(seq_y1);
  const OrientedCoordinateArray oca_y2(seq_y2);
  EXPECT_EQ(oca_y1.compareTo(oca_y2), -1);
  EXPECT_EQ(oca_y2.compareTo(oca_y1), 1);
  EXPECT_TRUE(oca_y1 < oca_y2);

  // Differing at an interior vertex after equal canonical start points.
  const CoordinateSequence seq_mid1{Coordinate(2.0, 0.0), Coordinate(1.0, 0.0),
                                    Coordinate(0.0, 0.0)};
  const CoordinateSequence seq_mid2{Coordinate(0.0, 0.0), Coordinate(1.0, 5.0),
                                    Coordinate(2.0, 0.0)};
  const OrientedCoordinateArray oca_mid1(seq_mid1);
  const OrientedCoordinateArray oca_mid2(seq_mid2);
  EXPECT_EQ(oca_mid1.compareTo(oca_mid2), -1);
  EXPECT_EQ(oca_mid2.compareTo(oca_mid1), 1);

  // Shorter prefix compares less than longer sequence with identical prefix.
  const CoordinateSequence short_fwd{Coordinate(2.0, 0.0),
                                     Coordinate(1.0, 0.0)};
  const CoordinateSequence long_fwd{Coordinate(2.0, 0.0), Coordinate(1.0, 0.0),
                                    Coordinate(0.0, 0.0)};
  const CoordinateSequence short_rev{Coordinate(1.0, 0.0),
                                     Coordinate(2.0, 0.0)};
  const CoordinateSequence long_rev{Coordinate(0.0, 0.0), Coordinate(1.0, 0.0),
                                    Coordinate(2.0, 0.0)};
  const OrientedCoordinateArray oca_short_fwd(short_fwd);
  const OrientedCoordinateArray oca_long_fwd(long_fwd);
  const OrientedCoordinateArray oca_short_rev(short_rev);
  const OrientedCoordinateArray oca_long_rev(long_rev);

  EXPECT_EQ(oca_short_fwd.compareTo(oca_long_fwd), -1);
  EXPECT_EQ(oca_long_fwd.compareTo(oca_short_fwd), 1);
  EXPECT_EQ(oca_short_rev.compareTo(oca_long_rev), -1);
  EXPECT_EQ(oca_long_rev.compareTo(oca_short_rev), 1);
  EXPECT_TRUE(oca_short_fwd < oca_long_rev);
  EXPECT_FALSE(oca_long_rev < oca_short_fwd);
}

TEST(OrientedCoordinateArrayTest, EqualityOperatorCoversAllMismatchBranches) {
  const CoordinateSequence seq_2_fwd{Coordinate(2.0, 0.0),
                                     Coordinate(1.0, 0.0)};
  const CoordinateSequence seq_3_fwd{Coordinate(2.0, 0.0), Coordinate(1.0, 0.0),
                                     Coordinate(0.0, 0.0)};
  const OrientedCoordinateArray oca_2_fwd(seq_2_fwd);
  const OrientedCoordinateArray oca_3_fwd(seq_3_fwd);

  // Different sizes.
  EXPECT_FALSE(oca_2_fwd == oca_3_fwd);

  // Same orientation, mismatch at index 0 and at interior index.
  const CoordinateSequence seq_2_fwd_diff{Coordinate(3.0, 0.0),
                                          Coordinate(1.0, 0.0)};
  const CoordinateSequence seq_3_fwd_mid1{
      Coordinate(2.0, 0.0), Coordinate(5.0, 0.0), Coordinate(1.0, 0.0)};
  const CoordinateSequence seq_3_fwd_mid2{
      Coordinate(2.0, 0.0), Coordinate(6.0, 0.0), Coordinate(1.0, 0.0)};
  EXPECT_FALSE(oca_2_fwd == OrientedCoordinateArray(seq_2_fwd_diff));
  EXPECT_FALSE(OrientedCoordinateArray(seq_3_fwd_mid1) ==
               OrientedCoordinateArray(seq_3_fwd_mid2));

  // Opposite orientations, mismatch at canonical start and at interior vertex.
  const CoordinateSequence seq_2_rev_diff{Coordinate(1.0, 0.0),
                                          Coordinate(3.0, 0.0)};
  const CoordinateSequence seq_3_rev_mid_diff{
      Coordinate(1.0, 0.0), Coordinate(6.0, 0.0), Coordinate(2.0, 0.0)};
  EXPECT_FALSE(oca_2_fwd == OrientedCoordinateArray(seq_2_rev_diff));
  EXPECT_FALSE(OrientedCoordinateArray(seq_3_fwd_mid1) ==
               OrientedCoordinateArray(seq_3_rev_mid_diff));

  // Empty sequences are equal to each other and unequal to non-empty ones.
  const CoordinateSequence empty_a;
  const CoordinateSequence empty_b;
  const OrientedCoordinateArray oca_empty_a(empty_a);
  const OrientedCoordinateArray oca_empty_b(empty_b);
  EXPECT_TRUE(oca_empty_a == oca_empty_b);
  EXPECT_FALSE(oca_empty_a == oca_2_fwd);
}

TEST(OrientedCoordinateArrayTest, HashCodeAndContainerDeduplication) {
  const CoordinateSequence s1_fwd{Coordinate(3.0, 0.0), Coordinate(2.0, 0.0),
                                  Coordinate(1.0, 0.0)};
  const CoordinateSequence s1_rev{Coordinate(1.0, 0.0), Coordinate(2.0, 0.0),
                                  Coordinate(3.0, 0.0)};
  const CoordinateSequence s2_fwd{Coordinate(4.0, 0.0), Coordinate(1.0, 0.0)};
  const CoordinateSequence s2_rev{Coordinate(1.0, 0.0), Coordinate(4.0, 0.0)};
  const CoordinateSequence empty_seq;

  const OrientedCoordinateArray oca1_fwd(s1_fwd);
  const OrientedCoordinateArray oca1_rev(s1_rev);
  const OrientedCoordinateArray oca2_fwd(s2_fwd);
  const OrientedCoordinateArray oca2_rev(s2_rev);
  const OrientedCoordinateArray oca_empty(empty_seq);

  const OrientedCoordinateArray::HashCode hasher;
  EXPECT_EQ(hasher(oca1_fwd), hasher(oca1_rev));
  EXPECT_EQ(hasher(oca2_fwd), hasher(oca2_rev));
  EXPECT_EQ(hasher(oca_empty), hasher(OrientedCoordinateArray(empty_seq)));

  // Unordered set deduplicates forward and reversed equivalents.
  std::unordered_set<OrientedCoordinateArray, OrientedCoordinateArray::HashCode>
      unordered;
  unordered.insert(oca1_fwd);
  unordered.insert(oca1_rev);
  unordered.insert(oca2_rev);
  unordered.insert(oca2_fwd);
  EXPECT_EQ(unordered.size(), 2u);
  EXPECT_NE(unordered.find(oca1_rev), unordered.end());
  EXPECT_NE(unordered.find(oca2_fwd), unordered.end());

  // Ordered set deduplicates and sorts according to canonical ordering.
  std::set<OrientedCoordinateArray> ordered;
  ordered.insert(oca2_rev);
  ordered.insert(oca1_fwd);
  ordered.insert(oca1_rev);
  ordered.insert(oca2_fwd);
  ASSERT_EQ(ordered.size(), 2u);
  auto it = ordered.begin();
  EXPECT_TRUE(*it == oca1_fwd);
  ++it;
  EXPECT_TRUE(*it == oca2_fwd);
}

}  // namespace
}  // namespace geos_google
