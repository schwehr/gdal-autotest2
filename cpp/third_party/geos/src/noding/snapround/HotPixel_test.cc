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

// Unit tests for third_party/geos/src/noding/snapround/HotPixel.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/noding/snapround/HotPixel.cpp

#include "third_party/geos/include/geos/noding/snapround/HotPixel.h"

#include <array>
#include <cmath>
#include <ios>
#include <limits>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/io/WKTWriter.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYM;
using geos::geom::CoordinateXYZM;
using geos::io::WKTWriter;
using geos::noding::snapround::HotPixel;
using geos::util::IllegalArgumentException;

struct PointCase {
  std::string_view name;
  double x;
  double y;
  bool expected;
};

struct SegmentCase {
  std::string_view name;
  double x0;
  double y0;
  double x1;
  double y1;
  bool expected;
};

class FailAfterPrefixStreambuf : public std::streambuf {
 public:
  explicit FailAfterPrefixStreambuf(std::streamsize allow_bytes)
      : remaining_(allow_bytes) {}

 protected:
  int_type overflow(int_type ch) override {
    if (traits_type::eq_int_type(ch, traits_type::eof())) {
      return traits_type::not_eof(ch);
    }
    if (remaining_ <= 0) {
      return traits_type::eof();
    }
    --remaining_;
    return ch;
  }

  std::streamsize xsputn(const char* /*s*/, std::streamsize count) override {
    if (count <= remaining_) {
      remaining_ -= count;
      return count;
    }
    const std::streamsize written = remaining_;
    remaining_ = 0;
    return written;
  }

 private:
  std::streamsize remaining_;
};

TEST(HotPixelTest, PreservesAllCoordinateTypesAndAccessors) {
  HotPixel hp_xy(CoordinateXY(1.5, -2.5), 1.0);
  EXPECT_DOUBLE_EQ(hp_xy.getScaleFactor(), 1.0);
  EXPECT_DOUBLE_EQ(hp_xy.getWidth(), 1.0);
  EXPECT_FALSE(hp_xy.isNode());
  EXPECT_DOUBLE_EQ(hp_xy.getCoordinate().x, 1.5);
  EXPECT_DOUBLE_EQ(hp_xy.getCoordinate().y, -2.5);
  EXPECT_TRUE(std::isnan(hp_xy.getCoordinate().z));
  EXPECT_TRUE(std::isnan(hp_xy.getCoordinate().m));

  HotPixel hp_xyz(Coordinate(3.0, 4.0, 5.0), 10.0);
  EXPECT_DOUBLE_EQ(hp_xyz.getScaleFactor(), 10.0);
  EXPECT_DOUBLE_EQ(hp_xyz.getWidth(), 0.1);
  EXPECT_DOUBLE_EQ(hp_xyz.getCoordinate().z, 5.0);
  EXPECT_TRUE(std::isnan(hp_xyz.getCoordinate().m));

  HotPixel hp_xym(CoordinateXYM(3.0, 4.0, 6.0), 0.25);
  EXPECT_DOUBLE_EQ(hp_xym.getScaleFactor(), 0.25);
  EXPECT_DOUBLE_EQ(hp_xym.getWidth(), 4.0);
  EXPECT_TRUE(std::isnan(hp_xym.getCoordinate().z));
  EXPECT_DOUBLE_EQ(hp_xym.getCoordinate().m, 6.0);

  HotPixel hp_xyzm(CoordinateXYZM(3.0, 4.0, 5.0, 6.0), 2.0);
  EXPECT_DOUBLE_EQ(hp_xyzm.getScaleFactor(), 2.0);
  EXPECT_DOUBLE_EQ(hp_xyzm.getWidth(), 0.5);
  EXPECT_DOUBLE_EQ(hp_xyzm.getCoordinate().z, 5.0);
  EXPECT_DOUBLE_EQ(hp_xyzm.getCoordinate().m, 6.0);
}

TEST(HotPixelTest, ThrowsOnNonPositiveScaleFactor) {
  EXPECT_THROW(HotPixel(CoordinateXY(0.0, 0.0), 0.0), IllegalArgumentException);
  EXPECT_THROW(HotPixel(CoordinateXY(0.0, 0.0), -0.0),
               IllegalArgumentException);
  EXPECT_THROW(HotPixel(CoordinateXY(0.0, 0.0), -1.0),
               IllegalArgumentException);
  EXPECT_THROW(HotPixel(Coordinate(1.0, 2.0, 3.0), -1e-9),
               IllegalArgumentException);
}

TEST(HotPixelTest, NodeFlagTransitionsToTrue) {
  HotPixel hp(CoordinateXY(0.0, 0.0), 1.0);
  EXPECT_FALSE(hp.isNode());
  hp.setToNode();
  EXPECT_TRUE(hp.isNode());
  hp.setToNode();
  EXPECT_TRUE(hp.isNode());
}

TEST(HotPixelTest, MemberStreamOperatorFormatsOriginalPoint) {
  HotPixel hp(CoordinateXY(1.0, 2.0), 1.0);
  std::ostringstream oss;
  std::ostream& returned = (hp << oss);
  EXPECT_EQ(&returned, &oss);
  EXPECT_EQ(oss.str(), "HP(" + WKTWriter::toPoint(hp.getCoordinate()) + ")");
  EXPECT_EQ(oss.str(), "HP(POINT (1 2 ))");

  // Even when scaleFactor != 1.0 rounds the internal grid pixel, operator<<
  // writes the unrounded original coordinate.
  HotPixel hp_scaled(CoordinateXY(1.25, -3.5), 4.0);
  std::ostringstream scaled_oss;
  hp_scaled << scaled_oss;
  EXPECT_EQ(scaled_oss.str(), "HP(POINT (1.25 -3.5 ))");
}

TEST(HotPixelTest, MemberStreamOperatorPropagatesStreamFailure) {
  HotPixel hp(CoordinateXY(1.0, 2.0), 1.0);
  // Allow "HP(" (3 bytes) so WKTWriter::toPoint temporary is live when stream
  // fails.
  FailAfterPrefixStreambuf buf(3);
  std::ostream os(&buf);
  os.exceptions(std::ios_base::badbit);
  EXPECT_THROW((void)(hp << os), std::ios_base::failure);
  EXPECT_TRUE(os.bad());
}

TEST(HotPixelTest, IntersectsPointUnitScaleHalfOpenSquare) {
  const HotPixel hp(CoordinateXY(0.0, 0.0), 1.0);

  constexpr auto kCases = std::to_array<PointCase>({
      {.name = "center", .x = 0.0, .y = 0.0, .expected = true},
      {.name = "interior", .x = 0.25, .y = -0.25, .expected = true},
      {.name = "lower_left_closed", .x = -0.5, .y = -0.5, .expected = true},
      {.name = "left_edge_closed", .x = -0.5, .y = 0.0, .expected = true},
      {.name = "bottom_edge_closed", .x = 0.0, .y = -0.5, .expected = true},
      {.name = "near_upper_left_inside",
       .x = -0.5,
       .y = 0.49,
       .expected = true},
      {.name = "near_lower_right_inside",
       .x = 0.49,
       .y = -0.5,
       .expected = true},
      {.name = "upper_left_top_open", .x = -0.5, .y = 0.5, .expected = false},
      {.name = "lower_right_right_open",
       .x = 0.5,
       .y = -0.5,
       .expected = false},
      {.name = "upper_right_open", .x = 0.5, .y = 0.5, .expected = false},
      {.name = "right_edge_open", .x = 0.5, .y = 0.0, .expected = false},
      {.name = "top_edge_open", .x = 0.0, .y = 0.5, .expected = false},
      {.name = "outside_right", .x = 0.51, .y = 0.0, .expected = false},
      {.name = "outside_left", .x = -0.51, .y = 0.0, .expected = false},
      {.name = "outside_top", .x = 0.0, .y = 0.51, .expected = false},
      {.name = "outside_bottom", .x = 0.0, .y = -0.51, .expected = false},
      {.name = "pos_inf_x",
       .x = std::numeric_limits<double>::infinity(),
       .y = 0.0,
       .expected = false},
      {.name = "neg_inf_x",
       .x = -std::numeric_limits<double>::infinity(),
       .y = 0.0,
       .expected = false},
      {.name = "pos_inf_y",
       .x = 0.0,
       .y = std::numeric_limits<double>::infinity(),
       .expected = false},
      {.name = "neg_inf_y",
       .x = 0.0,
       .y = -std::numeric_limits<double>::infinity(),
       .expected = false},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_EQ(hp.intersects(CoordinateXY(tc.x, tc.y)), tc.expected);
  }

  // Unit scaleFactor (1.0) keeps fractional center coordinates unrounded:
  // [-0.25, 0.75) x [0.25, 1.25).
  const HotPixel hp_fractional(CoordinateXY(0.25, 0.75), 1.0);
  EXPECT_TRUE(hp_fractional.intersects(CoordinateXY(-0.25, 0.25)));
  EXPECT_TRUE(hp_fractional.intersects(CoordinateXY(0.74, 1.24)));
  EXPECT_FALSE(hp_fractional.intersects(CoordinateXY(0.75, 0.75)));
  EXPECT_FALSE(hp_fractional.intersects(CoordinateXY(0.25, 1.25)));
  EXPECT_FALSE(hp_fractional.intersects(CoordinateXY(-0.26, 0.75)));
}

TEST(HotPixelTest, IntersectsPointWithNonUnitScaleAndRounding) {
  // With scaleFactor = 8.0: (1.27, 5.73) * 8 = (10.16, 45.84) -> rounds to
  // (10, 46). Unscaled half-open square is [1.1875, 1.3125) x [5.6875, 5.8125).
  const HotPixel hp(CoordinateXY(1.27, 5.73), 8.0);
  EXPECT_TRUE(hp.intersects(CoordinateXY(1.27, 5.73)));
  EXPECT_TRUE(hp.intersects(CoordinateXY(1.25, 5.75)));
  EXPECT_TRUE(hp.intersects(CoordinateXY(1.1875, 5.6875)));
  EXPECT_FALSE(hp.intersects(CoordinateXY(1.3125, 5.75)));
  EXPECT_FALSE(hp.intersects(CoordinateXY(1.25, 5.8125)));
  EXPECT_FALSE(hp.intersects(CoordinateXY(1.18, 5.75)));
  EXPECT_FALSE(hp.intersects(CoordinateXY(1.25, 5.68)));

  // Half-integer rounding uses Java Math.round semantics (ties toward +inf):
  // (-0.25 * 2 = -0.5 -> 0), (-0.75 * 2 = -1.5 -> -1).
  const HotPixel hp_half_neg(CoordinateXY(-0.25, -0.75), 2.0);
  EXPECT_TRUE(hp_half_neg.intersects(CoordinateXY(-0.25, -0.75)));
  EXPECT_TRUE(hp_half_neg.intersects(CoordinateXY(0.24, -0.26)));
  EXPECT_FALSE(hp_half_neg.intersects(CoordinateXY(0.25, -0.5)));
  EXPECT_FALSE(hp_half_neg.intersects(CoordinateXY(-0.26, -0.5)));

  // Positive half-integer ties also round toward +inf:
  // (0.25 * 2 = 0.5 -> 1), (0.75 * 2 = 1.5 -> 2), giving [0.25, 0.75) x
  // [0.75, 1.25).
  const HotPixel hp_half_pos(CoordinateXY(0.25, 0.75), 2.0);
  EXPECT_TRUE(hp_half_pos.intersects(CoordinateXY(0.25, 0.75)));
  EXPECT_TRUE(hp_half_pos.intersects(CoordinateXY(0.74, 1.24)));
  EXPECT_FALSE(hp_half_pos.intersects(CoordinateXY(0.75, 0.75)));
  EXPECT_FALSE(hp_half_pos.intersects(CoordinateXY(0.25, 1.25)));

  // Coarse grid (scaleFactor < 1.0): pixel width = 4.0, (2.0 * 0.25 = 0.5 ->
  // 1),
  // (-2.0 * 0.25 = -0.5 -> 0), giving [2.0, 6.0) x [-2.0, 2.0).
  const HotPixel hp_coarse(CoordinateXY(2.0, -2.0), 0.25);
  EXPECT_TRUE(hp_coarse.intersects(CoordinateXY(2.0, -2.0)));
  EXPECT_TRUE(hp_coarse.intersects(CoordinateXY(5.9, 1.9)));
  EXPECT_FALSE(hp_coarse.intersects(CoordinateXY(6.0, 0.0)));
  EXPECT_FALSE(hp_coarse.intersects(CoordinateXY(4.0, 2.0)));
}

TEST(HotPixelTest, IntersectsSegmentUnitScaleAllBranches) {
  const HotPixel hp(CoordinateXY(0.0, 0.0), 1.0);

  constexpr auto kCases = std::to_array<SegmentCase>({
      // Envelope rejection checks.
      {.name = "disjoint_right",
       .x0 = 1.0,
       .y0 = 0.0,
       .x1 = 2.0,
       .y1 = 0.0,
       .expected = false},
      {.name = "touch_open_right_edge",
       .x0 = 0.5,
       .y0 = -0.2,
       .x1 = 1.5,
       .y1 = 0.2,
       .expected = false},
      {.name = "disjoint_left",
       .x0 = -2.0,
       .y0 = 0.0,
       .x1 = -1.0,
       .y1 = 0.0,
       .expected = false},
      {.name = "disjoint_top",
       .x0 = -1.0,
       .y0 = 1.0,
       .x1 = 1.0,
       .y1 = 1.0,
       .expected = false},
      {.name = "touch_open_top_edge",
       .x0 = -0.2,
       .y0 = 0.5,
       .x1 = 0.2,
       .y1 = 1.5,
       .expected = false},
      {.name = "disjoint_bottom",
       .x0 = -1.0,
       .y0 = -2.0,
       .x1 = 1.0,
       .y1 = -1.0,
       .expected = false},

      // Vertical, horizontal, and degenerate segments.
      {.name = "vertical_interior",
       .x0 = 0.0,
       .y0 = -1.0,
       .x1 = 0.0,
       .y1 = 1.0,
       .expected = true},
      {.name = "vertical_closed_left",
       .x0 = -0.5,
       .y0 = -1.0,
       .x1 = -0.5,
       .y1 = 1.0,
       .expected = true},
      {.name = "vertical_touch_closed_ll",
       .x0 = -0.5,
       .y0 = -1.0,
       .x1 = -0.5,
       .y1 = -0.5,
       .expected = true},
      {.name = "degenerate_inside",
       .x0 = 0.1,
       .y0 = 0.1,
       .x1 = 0.1,
       .y1 = 0.1,
       .expected = true},
      {.name = "degenerate_closed_ll",
       .x0 = -0.5,
       .y0 = -0.5,
       .x1 = -0.5,
       .y1 = -0.5,
       .expected = true},
      {.name = "degenerate_open_ur",
       .x0 = 0.5,
       .y0 = 0.5,
       .x1 = 0.5,
       .y1 = 0.5,
       .expected = false},
      {.name = "horizontal_interior",
       .x0 = -1.0,
       .y0 = 0.0,
       .x1 = 1.0,
       .y1 = 0.0,
       .expected = true},
      {.name = "horizontal_closed_bottom",
       .x0 = -1.0,
       .y0 = -0.5,
       .x1 = 1.0,
       .y1 = -0.5,
       .expected = true},
      {.name = "horizontal_touch_closed_left",
       .x0 = -1.5,
       .y0 = 0.0,
       .x1 = -0.5,
       .y1 = 0.0,
       .expected = true},

      // Diagonal segments testing each corner orientation and side check.
      {.name = "through_ul_upward",
       .x0 = -1.5,
       .y0 = -0.5,
       .x1 = 0.5,
       .y1 = 1.5,
       .expected = false},
      {.name = "through_ul_downward",
       .x0 = -1.0,
       .y0 = 1.0,
       .x1 = 0.0,
       .y1 = 0.0,
       .expected = true},
      {.name = "through_ur_downward",
       .x0 = -0.5,
       .y0 = 1.5,
       .x1 = 1.5,
       .y1 = -0.5,
       .expected = false},
      {.name = "through_ur_upward",
       .x0 = 0.0,
       .y0 = 0.0,
       .x1 = 1.0,
       .y1 = 1.0,
       .expected = true},
      {.name = "crosses_top_side",
       .x0 = -0.2,
       .y0 = 0.0,
       .x1 = 0.4,
       .y1 = 1.0,
       .expected = true},
      {.name = "through_ll_downward",
       .x0 = -1.0,
       .y0 = 0.0,
       .x1 = 0.0,
       .y1 = -1.0,
       .expected = true},
      {.name = "through_ll_upward_shallow",
       .x0 = -0.5,
       .y0 = -0.5,
       .x1 = 0.5,
       .y1 = 0.0,
       .expected = true},
      {.name = "crosses_left_side",
       .x0 = -1.0,
       .y0 = -0.2,
       .x1 = 1.0,
       .y1 = 0.2,
       .expected = true},
      {.name = "through_lr_upward",
       .x0 = -0.5,
       .y0 = -1.5,
       .x1 = 1.5,
       .y1 = 0.5,
       .expected = false},
      {.name = "crosses_bottom_side",
       .x0 = 0.0,
       .y0 = -1.0,
       .x1 = 1.0,
       .y1 = 0.25,
       .expected = true},
      {.name = "env_overlap_misses_above_ul",
       .x0 = -1.0,
       .y0 = 0.2,
       .x1 = 0.0,
       .y1 = 1.2,
       .expected = false},
      {.name = "env_overlap_misses_below_lr",
       .x0 = 0.0,
       .y0 = -1.2,
       .x1 = 1.0,
       .y1 = -0.2,
       .expected = false},
      {.name = "strictly_contained_diagonal",
       .x0 = -0.2,
       .y0 = -0.1,
       .x1 = 0.3,
       .y1 = 0.2,
       .expected = true},
  });

  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const CoordinateXY p0(tc.x0, tc.y0);
    const CoordinateXY p1(tc.x1, tc.y1);
    EXPECT_EQ(hp.intersects(p0, p1), tc.expected);
    // Exercises endpoint swap when first.x > second.x in the reversed call.
    EXPECT_EQ(hp.intersects(p1, p0), tc.expected);
  }
}

TEST(HotPixelTest, IntersectsSegmentWithNonUnitScaleFactor) {
  // scaleFactor = 10.0, center (1.23, 5.67) rounds to scaled (12, 57),
  // unscaled pixel [1.15, 1.25) x [5.65, 5.75).
  const HotPixel hp(CoordinateXY(1.23, 5.67), 10.0);
  EXPECT_TRUE(hp.intersects(CoordinateXY(1.0, 5.70), CoordinateXY(1.4, 5.70)));
  EXPECT_TRUE(
      hp.intersects(CoordinateXY(1.20, 5.50), CoordinateXY(1.20, 5.90)));
  EXPECT_FALSE(
      hp.intersects(CoordinateXY(1.26, 5.50), CoordinateXY(1.26, 5.90)));
  EXPECT_FALSE(hp.intersects(CoordinateXY(1.0, 5.76), CoordinateXY(1.4, 5.76)));

  // Exact dyadic boundaries at scaleFactor = 8.0: pixel [1.1875, 1.3125) x
  // [5.6875, 5.8125).
  const HotPixel hp_exact(CoordinateXY(1.25, 5.75), 8.0);
  EXPECT_TRUE(hp_exact.intersects(CoordinateXY(1.1875, 5.5),
                                  CoordinateXY(1.1875, 6.0)));
  EXPECT_FALSE(hp_exact.intersects(CoordinateXY(1.3125, 5.5),
                                   CoordinateXY(1.3125, 6.0)));
  // Diagonal segments through scaled corners: UL=(1.1875, 5.8125),
  // LR=(1.3125, 5.6875).
  EXPECT_FALSE(hp_exact.intersects(CoordinateXY(1.125, 5.75),
                                   CoordinateXY(1.25, 5.875)));
  EXPECT_TRUE(hp_exact.intersects(CoordinateXY(1.125, 5.875),
                                  CoordinateXY(1.25, 5.75)));
  EXPECT_FALSE(hp_exact.intersects(CoordinateXY(1.25, 5.625),
                                   CoordinateXY(1.375, 5.75)));
  EXPECT_FALSE(hp_exact.intersects(CoordinateXY(1.375, 5.75),
                                   CoordinateXY(1.25, 5.625)));
}

TEST(HotPixelTest, ThrowsOnNanPixelCenterDuringDiagonalSegmentIntersection) {
  const HotPixel hp(CoordinateXY(geos::DoubleNotANumber, 0.0), 1.0);
  EXPECT_THROW(hp.intersects(CoordinateXY(-1.0, -1.0), CoordinateXY(1.0, 1.0)),
               IllegalArgumentException);
}

template <typename Tag, typename Tag::type M>
struct AccessHotPixelPrivate {
  friend typename Tag::type GetHotPixelPrivate(Tag) { return M; }
};

struct IntersectsPixelClosureTag {
  using type = bool (HotPixel::*)(const Coordinate&, const Coordinate&) const;
  friend type GetHotPixelPrivate(IntersectsPixelClosureTag);
};

template struct AccessHotPixelPrivate<IntersectsPixelClosureTag,
                                      &HotPixel::intersectsPixelClosure>;

TEST(HotPixelTest, IntersectsPixelClosureAllBoundariesAndDisjoint) {
  // Unit scale around (0, 0): closure square [-0.5, 0.5] x [-0.5, 0.5].
  const HotPixel hp(CoordinateXY(0.0, 0.0), 1.0);
  auto closure_fn = GetHotPixelPrivate(IntersectsPixelClosureTag{});

  EXPECT_TRUE((hp.*closure_fn)(Coordinate(0.0, 0.25), Coordinate(0.0, 1.0)));
  EXPECT_TRUE((hp.*closure_fn)(Coordinate(-1.0, 0.0), Coordinate(-0.25, 0.0)));
  EXPECT_TRUE((hp.*closure_fn)(Coordinate(0.0, -1.0), Coordinate(0.0, -0.25)));
  EXPECT_TRUE((hp.*closure_fn)(Coordinate(0.25, 0.0), Coordinate(1.0, 0.0)));
  EXPECT_FALSE((hp.*closure_fn)(Coordinate(-0.1, 0.0), Coordinate(0.1, 0.0)));
  EXPECT_FALSE((hp.*closure_fn)(Coordinate(2.0, 2.0), Coordinate(3.0, 3.0)));
}

}  // namespace
}  // namespace geos_google
