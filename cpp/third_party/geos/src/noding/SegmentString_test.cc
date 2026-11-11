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

// Unit tests for third_party/geos/src/noding/SegmentString.cpp and
// third_party/geos/include/geos/noding/SegmentString.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/noding/SegmentString.cpp

#include "third_party/geos/include/geos/noding/SegmentString.h"

#include <array>
#include <cstddef>
#include <sstream>
#include <string>
#include <type_traits>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/noding/BasicSegmentString.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;
using geos::noding::BasicSegmentString;
using geos::noding::SegmentString;
using ::testing::HasSubstr;

static_assert(!std::is_copy_constructible_v<SegmentString>);
static_assert(!std::is_copy_assignable_v<SegmentString>);

class CustomSegmentString final : public SegmentString {
 public:
  CustomSegmentString(const void* context, CoordinateSequence* seq)
      : SegmentString(context, seq) {}

  std::ostream& print(std::ostream& os) const override {
    return os << "CustomSegmentString(" << size() << ")\n";
  }
};

TEST(SegmentStringTest, ConstructGetSetDataAndAccessCoordinates) {
  CoordinateSequence seq = {
      CoordinateXYZM(1.0, 2.0, 3.0, 4.0),
      CoordinateXYZM(5.0, 6.0, 7.0, 8.0),
      CoordinateXYZM(9.0, 10.0, 11.0, 12.0),
  };
  const int initial_tag = 42;
  const int updated_tag = 99;

  SegmentString ss(&initial_tag, &seq);
  EXPECT_EQ(ss.getData(), &initial_tag);
  EXPECT_EQ(ss.size(), 3u);

  ss.setData(&updated_tag);
  EXPECT_EQ(ss.getData(), &updated_tag);
  ss.setData(nullptr);
  EXPECT_EQ(ss.getData(), nullptr);

  const SegmentString& const_ss = ss;
  EXPECT_EQ(const_ss.getCoordinates(), &seq);
  EXPECT_EQ(ss.getCoordinates(), &seq);

  EXPECT_DOUBLE_EQ(const_ss.getCoordinate<CoordinateXY>(0).x, 1.0);
  EXPECT_DOUBLE_EQ(const_ss.getCoordinate<CoordinateXY>(0).y, 2.0);
  EXPECT_DOUBLE_EQ(const_ss.getCoordinate(1).z, 7.0);
  EXPECT_DOUBLE_EQ(const_ss.getCoordinate<CoordinateXYZM>(2).m, 12.0);

  // Mutating through non-const getCoordinates() updates SegmentString view.
  ss.getCoordinates()->add(CoordinateXYZM(13.0, 14.0, 15.0, 16.0));
  EXPECT_EQ(ss.size(), 4u);
  EXPECT_DOUBLE_EQ(ss.getCoordinate<CoordinateXYZM>(3).x, 13.0);

  CoordinateSequence empty_seq;
  SegmentString empty_ss(nullptr, &empty_seq);
  EXPECT_EQ(empty_ss.size(), 0u);
}

TEST(SegmentStringTest, GetSegmentOctantCoversAllOctantsAndEdgeCases) {
  struct SegmentOctantCase {
    double x0;
    double y0;
    double x1;
    double y1;
    int expected_octant;
  };
  constexpr auto kCases = std::to_array<SegmentOctantCase>({
      // Axis-aligned and diagonal boundaries across octants 0..7.
      {.x0 = 0.0, .y0 = 0.0, .x1 = 5.0, .y1 = 0.0, .expected_octant = 0},
      {.x0 = 0.0, .y0 = 0.0, .x1 = 4.0, .y1 = 2.0, .expected_octant = 0},
      {.x0 = 0.0, .y0 = 0.0, .x1 = 3.0, .y1 = 3.0, .expected_octant = 0},
      {.x0 = 0.0, .y0 = 0.0, .x1 = 2.0, .y1 = 4.0, .expected_octant = 1},
      {.x0 = 0.0, .y0 = 0.0, .x1 = 0.0, .y1 = 5.0, .expected_octant = 1},
      {.x0 = 0.0, .y0 = 0.0, .x1 = -2.0, .y1 = 4.0, .expected_octant = 2},
      {.x0 = 0.0, .y0 = 0.0, .x1 = -3.0, .y1 = 3.0, .expected_octant = 3},
      {.x0 = 0.0, .y0 = 0.0, .x1 = -4.0, .y1 = 2.0, .expected_octant = 3},
      {.x0 = 0.0, .y0 = 0.0, .x1 = -5.0, .y1 = 0.0, .expected_octant = 3},
      {.x0 = 0.0, .y0 = 0.0, .x1 = -4.0, .y1 = -2.0, .expected_octant = 4},
      {.x0 = 0.0, .y0 = 0.0, .x1 = -3.0, .y1 = -3.0, .expected_octant = 4},
      {.x0 = 0.0, .y0 = 0.0, .x1 = -2.0, .y1 = -4.0, .expected_octant = 5},
      {.x0 = 0.0, .y0 = 0.0, .x1 = 0.0, .y1 = -5.0, .expected_octant = 6},
      {.x0 = 0.0, .y0 = 0.0, .x1 = 2.0, .y1 = -4.0, .expected_octant = 6},
      {.x0 = 0.0, .y0 = 0.0, .x1 = 3.0, .y1 = -3.0, .expected_octant = 7},
      {.x0 = 0.0, .y0 = 0.0, .x1 = 4.0, .y1 = -2.0, .expected_octant = 7},
      // Degenerate zero-length segment in 2D falls back safely to octant 0.
      {.x0 = 3.0, .y0 = 4.0, .x1 = 3.0, .y1 = 4.0, .expected_octant = 0},
  });

  for (std::size_t i = 0; i < kCases.size(); ++i) {
    SCOPED_TRACE(i);
    const auto& tc = kCases[i];
    CoordinateSequence seq = {
        Coordinate(tc.x0, tc.y0, 1.0),
        Coordinate(tc.x1, tc.y1, 99.0),
    };
    SegmentString ss(nullptr, &seq);
    EXPECT_EQ(ss.getSegmentOctant(0), tc.expected_octant);
    EXPECT_EQ(SegmentString::getSegmentOctant(ss, 0), tc.expected_octant);
    EXPECT_EQ(ss.getSegmentOctant(1), -1);
    EXPECT_EQ(SegmentString::getSegmentOctant(ss, 2), -1);
  }

  CoordinateSequence single_pt = {CoordinateXY(1.0, 2.0)};
  SegmentString single_ss(nullptr, &single_pt);
  EXPECT_EQ(single_ss.getSegmentOctant(0), -1);
}

TEST(SegmentStringTest, RingNavigationAndIsClosed) {
  CoordinateSequence closed_seq = {
      CoordinateXY(0.0, 0.0),
      CoordinateXY(10.0, 0.0),
      CoordinateXY(10.0, 10.0),
      CoordinateXY(0.0, 0.0),
  };
  SegmentString closed_ss(nullptr, &closed_seq);
  EXPECT_TRUE(closed_ss.isClosed());

  EXPECT_EQ(closed_ss.nextInRing(0), CoordinateXY(10.0, 0.0));
  EXPECT_EQ(closed_ss.nextInRing(1), CoordinateXY(10.0, 10.0));
  EXPECT_EQ(closed_ss.nextInRing(2), CoordinateXY(0.0, 0.0));
  // Index at the repeated closing vertex wraps to index 1.
  EXPECT_EQ(closed_ss.nextInRing(3), CoordinateXY(10.0, 0.0));

  // Index 0 wraps around to size() - 2 (the vertex preceding closing point).
  EXPECT_EQ(closed_ss.prevInRing(0), CoordinateXY(10.0, 10.0));
  EXPECT_EQ(closed_ss.prevInRing(1), CoordinateXY(0.0, 0.0));
  EXPECT_EQ(closed_ss.prevInRing(2), CoordinateXY(10.0, 0.0));
  EXPECT_EQ(closed_ss.prevInRing(3), CoordinateXY(10.0, 10.0));

  CoordinateSequence open_seq = {
      CoordinateXY(0.0, 0.0),
      CoordinateXY(10.0, 0.0),
      CoordinateXY(10.0, 10.0),
  };
  SegmentString open_ss(nullptr, &open_seq);
  EXPECT_FALSE(open_ss.isClosed());

  // Closure is checked in 2D even when Z differs.
  CoordinateSequence closed_2d_diff_z = {
      Coordinate(0.0, 0.0, 1.0),
      Coordinate(5.0, 0.0, 2.0),
      Coordinate(0.0, 0.0, 9.0),
  };
  SegmentString closed_2d_ss(nullptr, &closed_2d_diff_z);
  EXPECT_TRUE(closed_2d_ss.isClosed());

  CoordinateSequence single_pt = {CoordinateXY(2.0, 3.0)};
  SegmentString single_ss(nullptr, &single_pt);
  EXPECT_TRUE(single_ss.isClosed());
}

TEST(SegmentStringTest, PrintAndStreamOperatorFormatBaseAndDerived) {
  CoordinateSequence seq = {
      CoordinateXY(0.0, 0.0),
      CoordinateXY(1.0, 1.0),
  };
  const SegmentString base_ss(nullptr, &seq);

  std::ostringstream oss;
  std::ostream& ret_stream = (oss << base_ss);
  EXPECT_EQ(&ret_stream, &oss);
  EXPECT_EQ(oss.str(), "SegmentString\n");

  std::ostringstream direct_oss;
  std::ostream& ret_print = base_ss.print(direct_oss);
  EXPECT_EQ(&ret_print, &direct_oss);
  EXPECT_EQ(direct_oss.str(), "SegmentString\n");

  std::ostringstream chained_oss;
  chained_oss << base_ss << '|' << base_ss;
  EXPECT_EQ(chained_oss.str(), "SegmentString\n|SegmentString\n");

  const BasicSegmentString basic_ss(&seq, nullptr);
  const SegmentString& basic_as_base = basic_ss;
  std::ostringstream basic_oss;
  basic_oss << basic_as_base;
  EXPECT_THAT(basic_oss.str(), HasSubstr("BasicSegmentString:"));
  EXPECT_THAT(basic_oss.str(), HasSubstr("LINESTRING"));

  const CustomSegmentString custom_ss(nullptr, &seq);
  const SegmentString& custom_as_base = custom_ss;
  std::ostringstream custom_oss;
  custom_oss << custom_as_base;
  EXPECT_EQ(custom_oss.str(), "CustomSegmentString(2)\n");
}

}  // namespace
}  // namespace geos_google
