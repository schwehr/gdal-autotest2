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

#include "third_party/geos/include/geos/geom/CoordinateSequenceIterator.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <numeric>
#include <type_traits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateSequenceIterator;
using ::geos::geom::CoordinateXY;
using ::geos::geom::CoordinateXYZM;

template <typename Seq, typename Coord>
void VerifyIteratorTraits() {
  using Iter = CoordinateSequenceIterator<Seq, Coord>;
  static_assert(std::is_same_v<typename Iter::iterator_category,
                               std::random_access_iterator_tag>);
  static_assert(std::is_same_v<typename Iter::value_type, Coord>);
  static_assert(std::is_same_v<typename Iter::reference, Coord&>);
  // Upstream declares `using pointer = CoordinateType;` (not
  // `CoordinateType*`), so operator->() cannot be instantiated for
  // CoordinateXY-derived types.
  static_assert(std::is_same_v<typename Iter::pointer, Coord>);
  static_assert(std::is_same_v<typename Iter::difference_type, std::ptrdiff_t>);
  static_assert(
      std::is_same_v<typename std::iterator_traits<Iter>::iterator_category,
                     std::random_access_iterator_tag>);
}

TEST(CoordinateSequenceIteratorTest, IteratorTraits) {
  VerifyIteratorTraits<CoordinateSequence, Coordinate>();
  VerifyIteratorTraits<const CoordinateSequence, const Coordinate>();
  VerifyIteratorTraits<CoordinateSequence, CoordinateXY>();
  VerifyIteratorTraits<const CoordinateSequence, const CoordinateXY>();
  VerifyIteratorTraits<CoordinateSequence, CoordinateXYZM>();
  VerifyIteratorTraits<const CoordinateSequence, const CoordinateXYZM>();
}

TEST(CoordinateSequenceIteratorTest, ConstructorsAndEmptySequence) {
  CoordinateSequence empty_seq(0, 3);
  CoordinateSequence::iterator begin(&empty_seq);
  CoordinateSequence::iterator end(&empty_seq, empty_seq.size());

  EXPECT_EQ(begin, end);
  EXPECT_FALSE(begin != end);
  EXPECT_EQ(end - begin, 0);

  const CoordinateSequence& const_empty = empty_seq;
  CoordinateSequence::const_iterator cbegin(&const_empty);
  CoordinateSequence::const_iterator cend(&const_empty, const_empty.size());
  EXPECT_EQ(cbegin, cend);
  EXPECT_EQ(cend - cbegin, 0);

  auto range = empty_seq.items<Coordinate>();
  EXPECT_EQ(range.begin(), range.end());
  EXPECT_EQ(range.cbegin(), range.cend());
}

TEST(CoordinateSequenceIteratorTest, DereferenceReadAndWriteAcrossTypes) {
  CoordinateSequence xyz_seq{
      Coordinate(1.0, 2.0, 3.0),
      Coordinate(4.0, 5.0, 6.0),
  };

  CoordinateSequence::iterator it(&xyz_seq);
  EXPECT_DOUBLE_EQ((*it).x, 1.0);
  EXPECT_DOUBLE_EQ((*it).y, 2.0);
  EXPECT_DOUBLE_EQ((*it).z, 3.0);

  *it = Coordinate(10.0, 20.0, 30.0);
  EXPECT_DOUBLE_EQ(xyz_seq.getAt<Coordinate>(0).x, 10.0);
  EXPECT_DOUBLE_EQ(xyz_seq.getAt<Coordinate>(0).y, 20.0);
  EXPECT_DOUBLE_EQ(xyz_seq.getAt<Coordinate>(0).z, 30.0);

  const CoordinateSequence& const_xyz = xyz_seq;
  CoordinateSequence::const_iterator cit(&const_xyz);
  EXPECT_DOUBLE_EQ((*cit).x, 10.0);
  EXPECT_DOUBLE_EQ((*cit).z, 30.0);

  CoordinateSequence xy_seq{
      CoordinateXY(7.0, 8.0),
      CoordinateXY(9.0, 10.0),
  };
  auto xy_range = xy_seq.items<CoordinateXY>();
  (*xy_range.begin()).y = 88.0;
  EXPECT_DOUBLE_EQ(xy_seq.getY(0), 88.0);
  const auto const_xy_range =
      static_cast<const CoordinateSequence&>(xy_seq).items<CoordinateXY>();
  EXPECT_DOUBLE_EQ((*const_xy_range.begin()).x, 7.0);
  EXPECT_DOUBLE_EQ((*const_xy_range.cbegin()).y, 88.0);
  EXPECT_DOUBLE_EQ((*xy_seq.items<const CoordinateXY>().begin()).x, 7.0);
  EXPECT_DOUBLE_EQ((*xy_seq.items<CoordinateXY>().cbegin()).y, 88.0);

  CoordinateSequence xyzm_seq{
      CoordinateXYZM(1.0, 2.0, 3.0, 4.0),
      CoordinateXYZM(5.0, 6.0, 7.0, 8.0),
  };
  auto xyzm_it = xyzm_seq.items<CoordinateXYZM>().begin();
  (*xyzm_it).m = 44.0;
  EXPECT_DOUBLE_EQ(xyzm_seq.getAt<CoordinateXYZM>(0).m, 44.0);
}

TEST(CoordinateSequenceIteratorTest, PreAndPostIncrement) {
  CoordinateSequence seq{
      Coordinate(1.0, 10.0, 100.0),
      Coordinate(2.0, 20.0, 200.0),
      Coordinate(3.0, 30.0, 300.0),
  };

  CoordinateSequence::iterator it(&seq);
  CoordinateSequence::iterator end(&seq, seq.size());

  CoordinateSequence::iterator& pre_ref = ++it;
  EXPECT_EQ(&pre_ref, &it);
  EXPECT_DOUBLE_EQ((*it).x, 2.0);

  CoordinateSequence::iterator prev = it++;
  EXPECT_DOUBLE_EQ((*prev).x, 2.0);
  EXPECT_DOUBLE_EQ((*it).x, 3.0);

  ++it;
  EXPECT_EQ(it, end);

  std::vector<double> xs;
  for (CoordinateSequence::const_iterator cit(&seq), cend(&seq, seq.size());
       cit != cend; cit++) {
    xs.push_back((*cit).x);
  }
  ASSERT_EQ(xs.size(), 3u);
  EXPECT_DOUBLE_EQ(xs[0], 1.0);
  EXPECT_DOUBLE_EQ(xs[1], 2.0);
  EXPECT_DOUBLE_EQ(xs[2], 3.0);
}

TEST(CoordinateSequenceIteratorTest, PreAndPostDecrement) {
  CoordinateSequence seq{
      Coordinate(1.0, 10.0, 100.0),
      Coordinate(2.0, 20.0, 200.0),
      Coordinate(3.0, 30.0, 300.0),
  };

  CoordinateSequence::iterator begin(&seq);
  CoordinateSequence::iterator it(&seq, seq.size());

  CoordinateSequence::iterator& pre_ref = --it;
  EXPECT_EQ(&pre_ref, &it);
  EXPECT_DOUBLE_EQ((*it).x, 3.0);

  CoordinateSequence::iterator prev = it--;
  EXPECT_DOUBLE_EQ((*prev).x, 3.0);
  EXPECT_DOUBLE_EQ((*it).x, 2.0);

  --it;
  EXPECT_EQ(it, begin);
  EXPECT_DOUBLE_EQ((*it).x, 1.0);
}

TEST(CoordinateSequenceIteratorTest, DifferenceAndRandomAccessArithmetic) {
  CoordinateSequence seq{
      Coordinate(10.0, 0.0, 0.0),
      Coordinate(20.0, 0.0, 0.0),
      Coordinate(30.0, 0.0, 0.0),
      Coordinate(40.0, 0.0, 0.0),
  };

  CoordinateSequence::iterator begin(&seq);
  CoordinateSequence::iterator end(&seq, seq.size());

  EXPECT_EQ(end - begin, 4);
  EXPECT_EQ(begin - end, -4);
  EXPECT_EQ(begin - begin, 0);

  auto p2 = begin + 2;
  EXPECT_DOUBLE_EQ((*p2).x, 30.0);
  EXPECT_EQ(p2 - begin, 2);
  EXPECT_DOUBLE_EQ((*(begin + 0)).x, 10.0);
  EXPECT_DOUBLE_EQ((*(p2 + (-1))).x, 20.0);

  auto p1 = end - 3;
  EXPECT_DOUBLE_EQ((*p1).x, 20.0);
  EXPECT_EQ(end - p1, 3);
  EXPECT_EQ(end - 0, end);
  EXPECT_DOUBLE_EQ((*(p1 - (-2))).x, 40.0);

  CoordinateSequence::iterator it = begin;
  auto add_res = (it += 3);
  EXPECT_EQ(add_res, begin + 3);
  EXPECT_EQ(it, begin + 3);
  EXPECT_DOUBLE_EQ((*it).x, 40.0);

  it += 0;
  EXPECT_EQ(it, begin + 3);
  it += -1;
  EXPECT_EQ(it, begin + 2);
  EXPECT_DOUBLE_EQ((*it).x, 30.0);

  auto sub_res = (it -= 2);
  EXPECT_EQ(sub_res, begin);
  EXPECT_EQ(it, begin);
  EXPECT_DOUBLE_EQ((*it).x, 10.0);

  it -= 0;
  EXPECT_EQ(it, begin);
  it -= -1;
  EXPECT_EQ(it, begin + 1);
  EXPECT_DOUBLE_EQ((*it).x, 20.0);
}

TEST(CoordinateSequenceIteratorTest, SubscriptOperatorReadAndWrite) {
  CoordinateSequence seq{
      Coordinate(1.0, 2.0, 3.0),
      Coordinate(4.0, 5.0, 6.0),
      Coordinate(7.0, 8.0, 9.0),
      Coordinate(10.0, 11.0, 12.0),
  };

  CoordinateSequence::iterator begin(&seq);
  CoordinateSequence::iterator end(&seq, seq.size());

  EXPECT_DOUBLE_EQ(begin[0].x, 1.0);
  EXPECT_DOUBLE_EQ(begin[1].y, 5.0);
  EXPECT_DOUBLE_EQ(begin[3].z, 12.0);
  EXPECT_DOUBLE_EQ(end[-1].x, 10.0);
  EXPECT_DOUBLE_EQ((begin + 3)[-2].x, 4.0);

  begin[2] = Coordinate(70.0, 80.0, 90.0);
  begin[1].y = 55.0;

  const CoordinateSequence& const_seq = seq;
  CoordinateSequence::const_iterator cbegin(&const_seq);
  EXPECT_DOUBLE_EQ(cbegin[1].y, 55.0);
  EXPECT_DOUBLE_EQ(cbegin[2].x, 70.0);
  EXPECT_DOUBLE_EQ(cbegin[2].z, 90.0);
}

TEST(CoordinateSequenceIteratorTest, RelationalOperators) {
  CoordinateSequence seq{
      Coordinate(1.0, 0.0, 0.0),
      Coordinate(2.0, 0.0, 0.0),
      Coordinate(3.0, 0.0, 0.0),
  };

  CoordinateSequence::iterator a(&seq, 0);
  CoordinateSequence::iterator a_copy(&seq, 0);
  CoordinateSequence::iterator b(&seq, 2);

  // Equal positions.
  EXPECT_TRUE(a == a_copy);
  EXPECT_FALSE(a != a_copy);
  EXPECT_FALSE(a < a_copy);
  EXPECT_TRUE(a <= a_copy);
  EXPECT_FALSE(a > a_copy);
  EXPECT_TRUE(a >= a_copy);

  // Earlier < later.
  EXPECT_FALSE(a == b);
  EXPECT_TRUE(a != b);
  EXPECT_TRUE(a < b);
  EXPECT_TRUE(a <= b);
  EXPECT_FALSE(a > b);
  EXPECT_FALSE(a >= b);

  // Later > earlier.
  EXPECT_FALSE(b == a);
  EXPECT_TRUE(b != a);
  EXPECT_FALSE(b < a);
  EXPECT_FALSE(b <= a);
  EXPECT_TRUE(b > a);
  EXPECT_TRUE(b >= a);
}

TEST(CoordinateSequenceIteratorTest, StandardAlgorithmsIntegration) {
  CoordinateSequence seq{
      Coordinate(30.0, 1.0, 3.0),
      Coordinate(10.0, 2.0, 1.0),
      Coordinate(20.0, 3.0, 2.0),
      Coordinate(10.0, 0.5, 4.0),
  };

  auto range = seq.items<Coordinate>();
  EXPECT_EQ(std::distance(range.begin(), range.end()), 4);

  auto second = std::next(range.begin(), 1);
  EXPECT_DOUBLE_EQ((*second).x, 10.0);
  EXPECT_DOUBLE_EQ((*std::prev(range.end(), 1)).y, 0.5);

  auto adv = range.begin();
  std::advance(adv, 2);
  EXPECT_DOUBLE_EQ((*adv).x, 20.0);

  auto found = std::find(range.cbegin(), range.cend(), Coordinate(20.0, 3.0));
  ASSERT_NE(found, range.cend());
  EXPECT_EQ(found - range.cbegin(), 2);

  double x_sum = std::accumulate(
      range.cbegin(), range.cend(), 0.0,
      [](double acc, const Coordinate& c) { return acc + c.x; });
  EXPECT_DOUBLE_EQ(x_sum, 70.0);

  std::sort(range.begin(), range.end());
  EXPECT_DOUBLE_EQ(seq.getAt<Coordinate>(0).x, 10.0);
  EXPECT_DOUBLE_EQ(seq.getAt<Coordinate>(0).y, 0.5);
  EXPECT_DOUBLE_EQ(seq.getAt<Coordinate>(1).x, 10.0);
  EXPECT_DOUBLE_EQ(seq.getAt<Coordinate>(1).y, 2.0);
  EXPECT_DOUBLE_EQ(seq.getAt<Coordinate>(2).x, 20.0);
  EXPECT_DOUBLE_EQ(seq.getAt<Coordinate>(3).x, 30.0);

  std::reverse(range.begin(), range.end());
  EXPECT_DOUBLE_EQ(seq.getAt<Coordinate>(0).x, 30.0);
  EXPECT_DOUBLE_EQ(seq.getAt<Coordinate>(1).x, 20.0);
  EXPECT_DOUBLE_EQ(seq.getAt<Coordinate>(2).y, 2.0);
  EXPECT_DOUBLE_EQ(seq.getAt<Coordinate>(3).y, 0.5);
}

}  // namespace
}  // namespace geos_google
