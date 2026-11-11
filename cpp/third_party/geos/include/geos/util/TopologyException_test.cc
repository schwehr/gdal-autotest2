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

// Unit tests for third_party/geos/include/geos/util/TopologyException.h.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for throw/catch assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/util/TopologyException.h

#include "third_party/geos/include/geos/util/TopologyException.h"

#include <array>
#include <cmath>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::util::GEOSException;
using geos::util::TopologyException;

static_assert(std::is_base_of_v<GEOSException, TopologyException>);
static_assert(std::is_base_of_v<std::runtime_error, TopologyException>);
static_assert(std::is_base_of_v<std::exception, TopologyException>);
static_assert(std::is_default_constructible_v<TopologyException>);
static_assert(std::is_constructible_v<TopologyException, const std::string&>);
static_assert(std::is_constructible_v<TopologyException, const std::string&,
                                      const Coordinate&>);
static_assert(std::is_copy_constructible_v<TopologyException>);
static_assert(std::is_copy_assignable_v<TopologyException>);
static_assert(std::is_move_constructible_v<TopologyException>);
static_assert(std::is_move_assignable_v<TopologyException>);
static_assert(std::is_nothrow_destructible_v<TopologyException>);

struct MessageCase {
  std::string_view label;
  std::string_view message;
  std::string_view expected_what;
};

constexpr auto kMessageCases = std::to_array<MessageCase>({
    {
        .label = "empty",
        .message = "",
        .expected_what = "TopologyException: ",
    },
    {
        .label = "side_location_conflict",
        .message = "side location conflict",
        .expected_what = "TopologyException: side location conflict",
    },
    {
        .label = "multiline",
        .message = "non-noded intersection\nbetween edges",
        .expected_what =
            "TopologyException: non-noded intersection\nbetween edges",
    },
});

struct CoordMessageCase {
  std::string_view label;
  std::string_view message;
  double x;
  double y;
  double z;
  std::string_view expected_what;
};

constexpr auto kCoordMessageCases = std::to_array<CoordMessageCase>({
    {
        .label = "xy_coordinate",
        .message = "found non-noded intersection",
        .x = 12.5,
        .y = -3.25,
        .z = geos::DoubleNotANumber,
        .expected_what =
            "TopologyException: found non-noded intersection at 12.5 -3.25",
    },
    {
        .label = "xyz_coordinate",
        .message = "depth mismatch",
        .x = 1.0,
        .y = 2.0,
        .z = 42.0,
        .expected_what = "TopologyException: depth mismatch at 1 2 42",
    },
    {
        .label = "empty_message",
        .message = "",
        .x = -0.5,
        .y = 0.25,
        .z = geos::DoubleNotANumber,
        .expected_what = "TopologyException:  at -0.5 0.25",
    },
    {
        .label = "high_precision",
        .message = "edge collapse",
        .x = 1.25,
        .y = 1.125,
        .z = 0.0,
        .expected_what = "TopologyException: edge collapse at 1.25 1.125 0",
    },
});

template <typename CatchType, typename ThrowFn>
void ExpectCaughtAs(ThrowFn&& throw_fn, std::string_view expected_what) {
  bool caught = false;
  try {
    throw_fn();
  } catch (const CatchType& err) {
    caught = true;
    EXPECT_EQ(std::string_view(err.what()), expected_what);
    if constexpr (!std::is_same_v<CatchType, TopologyException>) {
      EXPECT_NE(dynamic_cast<const TopologyException*>(&err), nullptr);
    }
  }
  EXPECT_TRUE(caught);
}

void ThrowDefaultException() { throw TopologyException(); }

void ThrowWithMessage(const std::string& msg) { throw TopologyException(msg); }

void ThrowWithMessageAndCoordinate(const std::string& msg,
                                   const Coordinate& pt) {
  throw TopologyException(msg, pt);
}

TEST(TopologyExceptionTest, DefaultConstructorSetsPrefixAndDefaultCoordinate) {
  TopologyException ex;
  EXPECT_STREQ(ex.what(), "TopologyException: ");

  const Coordinate& pt = ex.getCoordinate();
  EXPECT_DOUBLE_EQ(pt.x, 0.0);
  EXPECT_DOUBLE_EQ(pt.y, 0.0);
  EXPECT_TRUE(std::isnan(pt.z));
  EXPECT_FALSE(pt.isNull());

  ExpectCaughtAs<TopologyException>(ThrowDefaultException,
                                    "TopologyException: ");
  EXPECT_THROW(ThrowDefaultException(), TopologyException);
}

TEST(TopologyExceptionTest, MessageConstructorSetsMessageAndDefaultCoordinate) {
  for (const auto& tc : kMessageCases) {
    SCOPED_TRACE(tc.label);
    const std::string msg(tc.message);
    TopologyException ex(msg);
    EXPECT_EQ(std::string_view(ex.what()), tc.expected_what);

    const Coordinate& pt = ex.getCoordinate();
    EXPECT_DOUBLE_EQ(pt.x, 0.0);
    EXPECT_DOUBLE_EQ(pt.y, 0.0);
    EXPECT_TRUE(std::isnan(pt.z));

    ExpectCaughtAs<TopologyException>([&] { ThrowWithMessage(msg); },
                                      tc.expected_what);
  }
}

TEST(TopologyExceptionTest,
     MessageAndCoordinateConstructorFormatsWhatAndStoresPt) {
  for (const auto& tc : kCoordMessageCases) {
    SCOPED_TRACE(tc.label);
    const std::string msg(tc.message);
    const Coordinate pt(tc.x, tc.y, tc.z);
    TopologyException ex(msg, pt);

    EXPECT_EQ(std::string_view(ex.what()), tc.expected_what);
    EXPECT_TRUE(ex.getCoordinate().equals3D(pt));

    ExpectCaughtAs<TopologyException>(
        [&] { ThrowWithMessageAndCoordinate(msg, pt); }, tc.expected_what);
  }

  const Coordinate& null_pt = Coordinate::getNull();
  TopologyException null_ex("invalid vertex", null_pt);
  EXPECT_EQ(std::string(null_ex.what()),
            "TopologyException: invalid vertex at " + null_pt.toString());
  EXPECT_TRUE(null_ex.getCoordinate().isNull());
}

TEST(TopologyExceptionTest, GetCoordinateReturnsMutableReference) {
  TopologyException ex("ring self-intersection", Coordinate(1.0, 2.0, 3.0));
  Coordinate& ref = ex.getCoordinate();
  ref.x = 9.5;
  ref.y = -4.5;
  ref.z = 7.0;
  EXPECT_TRUE(ex.getCoordinate().equals3D(Coordinate(9.5, -4.5, 7.0)));

  bool caught = false;
  try {
    ThrowWithMessageAndCoordinate("hole outside shell", Coordinate(2.0, 3.0));
  } catch (TopologyException& err) {
    caught = true;
    EXPECT_STREQ(err.what(), "TopologyException: hole outside shell at 2 3");
    err.getCoordinate().setNull();
    EXPECT_TRUE(err.getCoordinate().isNull());
  }
  EXPECT_TRUE(caught);
}

TEST(TopologyExceptionTest,
     CatchableThroughHierarchyAndCopyMovePolymorphicDelete) {
  const Coordinate loc(3.0, 4.0, 5.0);
  auto throw_conflict = [&] {
    ThrowWithMessageAndCoordinate("no outgoing dirEdge", loc);
  };
  constexpr std::string_view kExpected =
      "TopologyException: no outgoing dirEdge at 3 4 5";

  ExpectCaughtAs<TopologyException>(throw_conflict, kExpected);
  ExpectCaughtAs<GEOSException>(throw_conflict, kExpected);
  ExpectCaughtAs<std::runtime_error>(throw_conflict, kExpected);
  ExpectCaughtAs<std::exception>(throw_conflict, kExpected);
  EXPECT_THROW(throw_conflict(), GEOSException);

  TopologyException original("copy check", loc);
  TopologyException copied(original);
  EXPECT_STREQ(copied.what(), "TopologyException: copy check at 3 4 5");
  EXPECT_TRUE(copied.getCoordinate().equals3D(loc));

  TopologyException assigned("initial");
  assigned = original;
  EXPECT_STREQ(assigned.what(), "TopologyException: copy check at 3 4 5");
  EXPECT_TRUE(assigned.getCoordinate().equals3D(loc));

  TopologyException move_source("move check", Coordinate(8.0, 9.0));
  TopologyException moved(std::move(move_source));
  EXPECT_STREQ(moved.what(), "TopologyException: move check at 8 9");
  EXPECT_TRUE(moved.getCoordinate().equals2D(Coordinate(8.0, 9.0)));
  EXPECT_TRUE(std::isnan(moved.getCoordinate().z));

  TopologyException move_assign_source("move assign", Coordinate(10.0, 11.0));
  TopologyException move_assigned;
  move_assigned = std::move(move_assign_source);
  EXPECT_STREQ(move_assigned.what(), "TopologyException: move assign at 10 11");
  EXPECT_TRUE(move_assigned.getCoordinate().equals2D(Coordinate(10.0, 11.0)));

  std::unique_ptr<GEOSException> as_geos =
      std::make_unique<TopologyException>("heap geos", loc);
  EXPECT_STREQ(as_geos->what(), "TopologyException: heap geos at 3 4 5");
  as_geos.reset();

  std::unique_ptr<std::runtime_error> as_runtime =
      std::make_unique<TopologyException>("heap runtime");
  EXPECT_STREQ(as_runtime->what(), "TopologyException: heap runtime");
  as_runtime.reset();

  std::unique_ptr<std::exception> as_std =
      std::make_unique<TopologyException>();
  EXPECT_STREQ(as_std->what(), "TopologyException: ");
  as_std.reset();

  const std::exception_ptr ptr =
      std::make_exception_ptr(TopologyException("propagated", loc));
  ExpectCaughtAs<TopologyException>([&] { std::rethrow_exception(ptr); },
                                    "TopologyException: propagated at 3 4 5");
}

}  // namespace
}  // namespace geos_google
