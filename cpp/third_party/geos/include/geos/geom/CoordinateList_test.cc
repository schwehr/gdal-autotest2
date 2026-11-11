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

// Unit tests for third_party/geos/include/geos/geom/CoordinateList.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/geom/CoordinateList.h

#include "third_party/geos/include/geos/geom/CoordinateList.h"

#include <cmath>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <sstream>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateList;
using ::geos::geom::CoordinateSequence;
using ::geos::geom::CoordinateXY;

TEST(CoordinateListTest, DefaultAndContainerConstructors) {
  CoordinateList empty_list;
  EXPECT_TRUE(empty_list.empty());
  EXPECT_EQ(empty_list.size(), 0u);
  // Compare iterators in EXPECT_TRUE so GTest does not instantiate
  // CoordinateList(const T&) via ADL on geos::geom::operator<<.
  EXPECT_TRUE(empty_list.begin() == empty_list.end());

  const CoordinateList& const_empty = empty_list;
  EXPECT_TRUE(const_empty.empty());
  EXPECT_EQ(const_empty.size(), 0u);
  EXPECT_TRUE(const_empty.begin() == const_empty.end());

  const std::vector<Coordinate> empty_vec;
  const CoordinateList from_empty_vec(empty_vec);
  EXPECT_TRUE(from_empty_vec.empty());
  EXPECT_EQ(from_empty_vec.size(), 0u);

  // Container constructor preserves repeated coordinates.
  const std::vector<Coordinate> vec = {
      Coordinate(1.0, 2.0, 3.0),
      Coordinate(1.0, 2.0, 3.0),
      Coordinate(4.0, 5.0, 6.0),
  };
  CoordinateList from_vec(vec);
  EXPECT_FALSE(from_vec.empty());
  EXPECT_EQ(from_vec.size(), 3u);
  EXPECT_TRUE(from_vec.begin() != from_vec.end());
  auto it = from_vec.begin();
  EXPECT_TRUE(it->equals3D(Coordinate(1.0, 2.0, 3.0)));
  ++it;
  EXPECT_TRUE(it->equals3D(Coordinate(1.0, 2.0, 3.0)));
  ++it;
  EXPECT_TRUE(it->equals3D(Coordinate(4.0, 5.0, 6.0)));

  // Template constructor also accepts CoordinateXY vectors and std::list.
  const std::vector<CoordinateXY> xy_vec = {CoordinateXY(7.0, 8.0)};
  const CoordinateList from_xy_vec(xy_vec);
  ASSERT_EQ(from_xy_vec.size(), 1u);
  EXPECT_DOUBLE_EQ(from_xy_vec.begin()->x, 7.0);
  EXPECT_DOUBLE_EQ(from_xy_vec.begin()->y, 8.0);
  EXPECT_TRUE(std::isnan(from_xy_vec.begin()->z));

  const std::list<Coordinate> std_list = {Coordinate(9.0, 10.0),
                                          Coordinate(11.0, 12.0)};
  const CoordinateList from_std_list(std_list);
  ASSERT_EQ(from_std_list.size(), 2u);
  EXPECT_TRUE(from_std_list.begin()->equals2D(Coordinate(9.0, 10.0)));
  EXPECT_TRUE(
      std::next(from_std_list.begin())->equals2D(Coordinate(11.0, 12.0)));
}

TEST(CoordinateListTest, ConstructFromCoordinateSequence) {
  const CoordinateSequence empty_seq = CoordinateSequence::XYZ(0);
  const CoordinateList from_empty(empty_seq);
  EXPECT_TRUE(from_empty.empty());
  EXPECT_EQ(from_empty.size(), 0u);

  const CoordinateSequence seq3d(std::initializer_list<Coordinate>{
      Coordinate(1.0, 2.0, 10.0),
      Coordinate(1.0, 2.0, 10.0),
      Coordinate(3.0, 4.0, 20.0),
  });
  const CoordinateList from_seq3d(seq3d);
  ASSERT_EQ(from_seq3d.size(), 3u);
  auto it = from_seq3d.begin();
  EXPECT_TRUE(it->equals3D(Coordinate(1.0, 2.0, 10.0)));
  ++it;
  EXPECT_TRUE(it->equals3D(Coordinate(1.0, 2.0, 10.0)));
  ++it;
  EXPECT_TRUE(it->equals3D(Coordinate(3.0, 4.0, 20.0)));
}

TEST(CoordinateListTest, MutableAndConstIteration) {
  CoordinateList list(std::vector<Coordinate>{Coordinate(1.0, 2.0, 3.0),
                                              Coordinate(4.0, 5.0, 6.0)});
  for (Coordinate& c : list) {
    c.x += 10.0;
    c.y -= 1.0;
  }

  const CoordinateList& const_ref = list;
  std::vector<Coordinate> collected(const_ref.begin(), const_ref.end());
  ASSERT_EQ(collected.size(), 2u);
  EXPECT_TRUE(collected[0].equals3D(Coordinate(11.0, 1.0, 3.0)));
  EXPECT_TRUE(collected[1].equals3D(Coordinate(14.0, 4.0, 6.0)));
}

TEST(CoordinateListTest, InsertWithAndWithoutAllowRepeated) {
  CoordinateList list;

  // Inserting at begin() on empty list with allowRepeated == false succeeds.
  CoordinateList::iterator it0 =
      list.insert(list.begin(), Coordinate(1.0, 1.0, 10.0), false);
  EXPECT_EQ(list.size(), 1u);
  EXPECT_TRUE(it0 == list.begin());

  // Inserting at begin() checks only previous (none here), NOT next element.
  CoordinateList::iterator it_dup_front =
      list.insert(list.begin(), Coordinate(1.0, 1.0, 99.0), false);
  EXPECT_EQ(list.size(), 2u);
  EXPECT_TRUE(it_dup_front == list.begin());
  EXPECT_DOUBLE_EQ(list.begin()->z, 99.0);

  // Inserting after a 2D-equal predecessor with allowRepeated == false
  // collapses and returns iterator to the previous element without modifying
  // it.
  CoordinateList::iterator after_first = std::next(list.begin());
  CoordinateList::iterator collapsed =
      list.insert(after_first, Coordinate(1.0, 1.0, 55.0), false);
  EXPECT_EQ(list.size(), 2u);
  EXPECT_TRUE(collapsed == list.begin());
  EXPECT_DOUBLE_EQ(collapsed->z, 99.0);

  // Inserting after a distinct predecessor succeeds even if equal to
  // the next element.
  CoordinateList::iterator inserted_mid =
      list.insert(after_first, Coordinate(2.0, 2.0, 20.0), false);
  EXPECT_EQ(list.size(), 3u);
  EXPECT_TRUE(inserted_mid->equals3D(Coordinate(2.0, 2.0, 20.0)));

  // Inserting equal to next (and unequal to prev) with allowRepeated == false.
  CoordinateList::iterator dup_next =
      list.insert(after_first, Coordinate(1.0, 1.0, 30.0), false);
  EXPECT_EQ(list.size(), 4u);
  EXPECT_TRUE(dup_next->equals3D(Coordinate(1.0, 1.0, 30.0)));

  // Inserting with allowRepeated == true allows consecutive duplicates.
  CoordinateList::iterator allowed_dup =
      list.insert(std::next(dup_next), Coordinate(1.0, 1.0, 40.0), true);
  EXPECT_EQ(list.size(), 5u);
  EXPECT_TRUE(allowed_dup->equals3D(Coordinate(1.0, 1.0, 40.0)));

  // Two-argument insert inserts unconditionally at arbitrary positions.
  CoordinateList::iterator uncond_front =
      list.insert(list.begin(), Coordinate(1.0, 1.0, 45.0));
  EXPECT_EQ(list.size(), 6u);
  EXPECT_TRUE(uncond_front == list.begin());
  EXPECT_TRUE(uncond_front->equals3D(Coordinate(1.0, 1.0, 45.0)));

  CoordinateList::iterator uncond_end =
      list.insert(list.end(), Coordinate(1.0, 1.0, 50.0));
  EXPECT_EQ(list.size(), 7u);
  EXPECT_TRUE(uncond_end->equals3D(Coordinate(1.0, 1.0, 50.0)));
  EXPECT_TRUE(std::next(uncond_end) == list.end());

  // NaN coordinates are never equal in 2D, so allowRepeated == false keeps
  // consecutive NaN entries.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  list.insert(list.end(), Coordinate(nan, nan), false);
  list.insert(list.end(), Coordinate(nan, nan), false);
  EXPECT_EQ(list.size(), 9u);
  EXPECT_TRUE(std::prev(list.end())->isNull());
}

TEST(CoordinateListTest, AddWithAndWithoutAllowRepeated) {
  CoordinateList list;

  // Empty list accepts first coordinate even with allowRepeated == false.
  CoordinateList::iterator a = list.add(Coordinate(0.0, 0.0, 1.0), false);
  EXPECT_EQ(list.size(), 1u);
  EXPECT_TRUE(a == list.begin());

  // Consecutive 2D duplicate is collapsed when allowRepeated == false.
  CoordinateList::iterator collapsed =
      list.add(Coordinate(0.0, 0.0, 99.0), false);
  EXPECT_EQ(list.size(), 1u);
  EXPECT_TRUE(collapsed == a);
  EXPECT_DOUBLE_EQ(collapsed->z, 1.0);

  // Distinct coordinate is appended.
  CoordinateList::iterator b = list.add(Coordinate(5.0, 5.0, 2.0), false);
  EXPECT_EQ(list.size(), 2u);
  EXPECT_TRUE(b == std::next(a));

  // Non-adjacent duplicate is allowed when allowRepeated == false.
  CoordinateList::iterator c = list.add(Coordinate(0.0, 0.0, 3.0), false);
  EXPECT_EQ(list.size(), 3u);
  EXPECT_TRUE(c->equals3D(Coordinate(0.0, 0.0, 3.0)));

  // Consecutive duplicate is appended when allowRepeated == true.
  CoordinateList::iterator d = list.add(Coordinate(0.0, 0.0, 4.0), true);
  EXPECT_EQ(list.size(), 4u);
  EXPECT_TRUE(d == std::prev(list.end()));
  EXPECT_TRUE(d->equals3D(Coordinate(0.0, 0.0, 4.0)));
}

TEST(CoordinateListTest, EraseSingleAndRange) {
  CoordinateList list(std::vector<Coordinate>{
      Coordinate(1.0, 0.0),
      Coordinate(2.0, 0.0),
      Coordinate(3.0, 0.0),
      Coordinate(4.0, 0.0),
      Coordinate(5.0, 0.0),
  });

  // Erase single element in the middle.
  CoordinateList::iterator after_two = list.erase(std::next(list.begin()));
  EXPECT_EQ(list.size(), 4u);
  EXPECT_EQ(*after_two, Coordinate(3.0, 0.0));

  // Erase empty range is a no-op.
  CoordinateList::iterator same = list.erase(after_two, after_two);
  EXPECT_EQ(list.size(), 4u);
  EXPECT_TRUE(same == after_two);

  // Erase subrange [3.0, 5.0).
  CoordinateList::iterator after_range =
      list.erase(std::next(list.begin()), std::prev(list.end()));
  EXPECT_EQ(list.size(), 2u);
  EXPECT_EQ(*after_range, Coordinate(5.0, 0.0));
  EXPECT_EQ(*list.begin(), Coordinate(1.0, 0.0));

  // Erase single element at the back returns end().
  CoordinateList::iterator after_back = list.erase(std::prev(list.end()));
  EXPECT_EQ(list.size(), 1u);
  EXPECT_TRUE(after_back == list.end());
  EXPECT_EQ(*list.begin(), Coordinate(1.0, 0.0));

  // Erase entire remaining range.
  CoordinateList::iterator at_end = list.erase(list.begin(), list.end());
  EXPECT_TRUE(at_end == list.end());
  EXPECT_TRUE(list.empty());
}

TEST(CoordinateListTest, ToCoordinateArrayAndToCoordinateSequence) {
  CoordinateList empty_list;
  std::unique_ptr<Coordinate::Vect> empty_arr = empty_list.toCoordinateArray();
  ASSERT_TRUE(empty_arr != nullptr);
  EXPECT_TRUE(empty_arr->empty());

  std::unique_ptr<CoordinateSequence> empty_seq =
      empty_list.toCoordinateSequence();
  ASSERT_TRUE(empty_seq != nullptr);
  EXPECT_TRUE(empty_seq->isEmpty());
  EXPECT_EQ(empty_seq->size(), 0u);

  CoordinateList list(std::vector<Coordinate>{
      Coordinate(1.0, 2.0, 3.0),
      Coordinate(4.0, 5.0, 6.0),
      Coordinate(7.0, 8.0, 9.0),
  });

  std::unique_ptr<Coordinate::Vect> arr = list.toCoordinateArray();
  ASSERT_TRUE(arr != nullptr);
  ASSERT_EQ(arr->size(), 3u);
  EXPECT_TRUE((*arr)[0].equals3D(Coordinate(1.0, 2.0, 3.0)));
  EXPECT_TRUE((*arr)[1].equals3D(Coordinate(4.0, 5.0, 6.0)));
  EXPECT_TRUE((*arr)[2].equals3D(Coordinate(7.0, 8.0, 9.0)));

  std::unique_ptr<CoordinateSequence> seq = list.toCoordinateSequence();
  ASSERT_TRUE(seq != nullptr);
  ASSERT_EQ(seq->size(), 3u);
  EXPECT_TRUE(seq->hasZ());
  EXPECT_FALSE(seq->hasM());
  EXPECT_TRUE(seq->getAt<Coordinate>(0).equals3D(Coordinate(1.0, 2.0, 3.0)));
  EXPECT_TRUE(seq->getAt<Coordinate>(1).equals3D(Coordinate(4.0, 5.0, 6.0)));
  EXPECT_TRUE(seq->getAt<Coordinate>(2).equals3D(Coordinate(7.0, 8.0, 9.0)));

  // Verify 2D list preserves NaN Z when converted to CoordinateSequence.
  CoordinateList list_2d(
      std::vector<Coordinate>{Coordinate(1.0, 2.0), Coordinate(3.0, 4.0)});
  std::unique_ptr<CoordinateSequence> seq_2d = list_2d.toCoordinateSequence();
  ASSERT_TRUE(seq_2d != nullptr);
  ASSERT_EQ(seq_2d->size(), 2u);
  EXPECT_FALSE(seq_2d->hasZ());
  EXPECT_TRUE(std::isnan(seq_2d->getAt<Coordinate>(0).z));
}

TEST(CoordinateListTest, CloseRingMemberAndStaticVectorOverload) {
  // Member closeRing on empty list.
  CoordinateList empty_list;
  empty_list.closeRing();
  EXPECT_TRUE(empty_list.empty());

  // Member closeRing on single-point list is already closed.
  CoordinateList single(std::vector<Coordinate>{Coordinate(1.0, 2.0, 3.0)});
  single.closeRing();
  EXPECT_EQ(single.size(), 1u);

  // Member closeRing on open list appends copy of first coordinate (with Z).
  CoordinateList open_list(std::vector<Coordinate>{
      Coordinate(0.0, 0.0, 12.0),
      Coordinate(4.0, 0.0, 13.0),
      Coordinate(4.0, 4.0, 14.0),
  });
  open_list.closeRing();
  ASSERT_EQ(open_list.size(), 4u);
  EXPECT_TRUE(std::prev(open_list.end())->equals3D(Coordinate(0.0, 0.0, 12.0)));

  // Calling closeRing again is a no-op (even if closing Z differs, since
  // Coordinate::equals is 2D).
  std::prev(open_list.end())->z = 99.0;
  open_list.closeRing();
  EXPECT_EQ(open_list.size(), 4u);
  EXPECT_DOUBLE_EQ(std::prev(open_list.end())->z, 99.0);

  // Static vector overload: empty, single-point, open, and already closed.
  std::vector<Coordinate> empty_vec;
  CoordinateList::closeRing(empty_vec);
  EXPECT_TRUE(empty_vec.empty());

  std::vector<Coordinate> single_vec = {Coordinate(2.0, 3.0, 4.0)};
  CoordinateList::closeRing(single_vec);
  EXPECT_EQ(single_vec.size(), 1u);

  std::vector<Coordinate> open_vec = {
      Coordinate(0.0, 0.0, 7.0),
      Coordinate(2.0, 0.0, 8.0),
      Coordinate(2.0, 2.0, 9.0),
  };
  CoordinateList::closeRing(open_vec);
  ASSERT_EQ(open_vec.size(), 4u);
  EXPECT_TRUE(open_vec.back().equals3D(Coordinate(0.0, 0.0, 7.0)));

  // Already closed in 2D with different Z remains size 4.
  open_vec.back().z = 42.0;
  CoordinateList::closeRing(open_vec);
  EXPECT_EQ(open_vec.size(), 4u);
  EXPECT_DOUBLE_EQ(open_vec.back().z, 42.0);
}

TEST(CoordinateListTest, StreamOutputFormatsEmptySingleAndMultipleElements) {
  {
    const CoordinateList empty_list;
    std::ostringstream oss;
    oss << empty_list;
    EXPECT_EQ(oss.str(), "()");
  }
  {
    const CoordinateList single(std::vector<Coordinate>{Coordinate(1.0, 2.0)});
    std::ostringstream oss;
    oss << single;
    EXPECT_EQ(oss.str(), "(1 2)");
  }
  {
    const CoordinateList multi(std::vector<Coordinate>{
        Coordinate(1.0, 2.0), Coordinate(3.0, 4.0, 5.0), Coordinate(6.0, 7.0)});
    std::ostringstream oss;
    oss << multi << "|";
    EXPECT_EQ(oss.str(), "(1 2, 3 4 5, 6 7)|");
  }
}

}  // namespace
}  // namespace geos_google
