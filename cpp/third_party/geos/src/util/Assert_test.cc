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

// Unit tests for third_party/geos/src/util/Assert.cpp.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for GEOSException assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/util/Assert.cpp

#include "third_party/geos/include/geos/util/Assert.h"

#include <limits>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/util/AssertionFailedException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::geom::CoordinateXY;
using geos::util::Assert;
using geos::util::AssertionFailedException;
using ::testing::HasSubstr;

template <typename Fn>
void ExpectAssertionFailureEq(Fn&& fn, std::string_view expected_what) {
  try {
    fn();
    FAIL() << "Expected AssertionFailedException: " << expected_what;
  } catch (const AssertionFailedException& e) {
    EXPECT_EQ(std::string_view(e.what()), expected_what);
  }
}

TEST(AssertTest, IsTrueDoesNotThrowWhenConditionHolds) {
  const std::string empty_msg;
  const std::string non_empty_msg = "should not fail";
  EXPECT_NO_THROW(Assert::isTrue(true));
  EXPECT_NO_THROW(Assert::isTrue(true, empty_msg));
  EXPECT_NO_THROW(Assert::isTrue(true, non_empty_msg));
}

TEST(AssertTest, IsTrueThrowsEmptyMessageWhenOmittedOrBlank) {
  const std::string empty_msg;
  {
    SCOPED_TRACE("one-argument isTrue(false)");
    ExpectAssertionFailureEq([] { Assert::isTrue(false); },
                             "AssertionFailedException: ");
  }
  {
    SCOPED_TRACE("two-argument isTrue(false, empty)");
    ExpectAssertionFailureEq([&] { Assert::isTrue(false, empty_msg); },
                             "AssertionFailedException: ");
  }
}

TEST(AssertTest, IsTrueThrowsWithProvidedMessage) {
  const std::string msg = "invariant violated";
  ExpectAssertionFailureEq([&] { Assert::isTrue(false, msg); },
                           "AssertionFailedException: invariant violated");
}

TEST(AssertTest, EqualsDoesNotThrowWhenCoordinatesMatchIn2D) {
  const CoordinateXY a(1.5, -2.5);
  const CoordinateXY b(1.5, -2.5);
  EXPECT_NO_THROW(Assert::equals(a, b));
  EXPECT_NO_THROW(Assert::equals(a, b, ""));
  EXPECT_NO_THROW(Assert::equals(a, b, "unused message"));

  // Assert::equals compares CoordinateXY (2D), ignoring Z ordinates.
  const Coordinate z1(3.25, 4.75, 10.0);
  const Coordinate z2(3.25, 4.75, 99.0);
  EXPECT_NO_THROW(Assert::equals(z1, z2));
}

TEST(AssertTest, EqualsThrowsWithoutMessageOnCoordinateMismatch) {
  const CoordinateXY expected(1.0, 2.0);
  const CoordinateXY diff_x(3.0, 2.0);
  const CoordinateXY diff_y(1.0, 4.0);

  {
    SCOPED_TRACE("x mismatch without message");
    ExpectAssertionFailureEq(
        [&] { Assert::equals(expected, diff_x); },
        "AssertionFailedException: Expected 1 2 but encountered 3 2");
  }
  {
    SCOPED_TRACE("y mismatch with empty message");
    ExpectAssertionFailureEq(
        [&] { Assert::equals(expected, diff_y, ""); },
        "AssertionFailedException: Expected 1 2 but encountered 1 4");
  }
}

TEST(AssertTest, EqualsThrowsWithMessageOnCoordinateMismatch) {
  const CoordinateXY expected(1.0, 2.0);
  const CoordinateXY actual(3.0, 4.0);
  ExpectAssertionFailureEq(
      [&] { Assert::equals(expected, actual, "vertex mismatch"); },
      "AssertionFailedException: Expected 1 2 but encountered 3 4: "
      "vertex mismatch");
}

TEST(AssertTest, EqualsThrowsWhenCoordinatesContainNaN) {
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  const CoordinateXY nan_coord(kNan, 2.0);
  try {
    Assert::equals(nan_coord, nan_coord, "nan check");
    FAIL() << "Expected AssertionFailedException for NaN comparison";
  } catch (const AssertionFailedException& e) {
    EXPECT_THAT(e.what(), HasSubstr("Expected "));
    EXPECT_THAT(e.what(), HasSubstr(" but encountered "));
    EXPECT_THAT(e.what(), HasSubstr(": nan check"));
  }

  EXPECT_THROW(Assert::equals(CoordinateXY::getNull(), CoordinateXY::getNull()),
               AssertionFailedException);
}

TEST(AssertTest, ShouldNeverReachHereAlwaysThrows) {
  {
    SCOPED_TRACE("no message");
    ExpectAssertionFailureEq(
        [] { Assert::shouldNeverReachHere(); },
        "AssertionFailedException: Should never reach here");
  }
  {
    SCOPED_TRACE("empty message");
    ExpectAssertionFailureEq(
        [] { Assert::shouldNeverReachHere(""); },
        "AssertionFailedException: Should never reach here");
  }
  {
    SCOPED_TRACE("with message");
    ExpectAssertionFailureEq(
        [] { Assert::shouldNeverReachHere("unhandled enum"); },
        "AssertionFailedException: Should never reach here: unhandled enum");
  }
}

}  // namespace
}  // namespace geos_google
