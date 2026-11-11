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

// Unit tests for
// third_party/geos/src/operation/valid/TopologyValidationError.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/operation/valid/TopologyValidationError.cpp

#include "third_party/geos/include/geos/operation/valid/TopologyValidationError.h"

#include <array>
#include <cmath>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"

namespace geos_google {
namespace {

using geos::DoubleNotANumber;
using geos::geom::CoordinateXY;
using geos::operation::valid::TopologyValidationError;

struct ErrorMessageCase {
  TopologyValidationError::errorEnum error_type;
  int expected_code;
  std::string_view expected_message;
};

constexpr auto kErrorMessageCases = std::to_array<ErrorMessageCase>({
    {TopologyValidationError::eError, 0, "Topology Validation Error"},
    {TopologyValidationError::eRepeatedPoint, 1, "Repeated Point"},
    {TopologyValidationError::eHoleOutsideShell, 2, "Hole lies outside shell"},
    {TopologyValidationError::eNestedHoles, 3, "Holes are nested"},
    {TopologyValidationError::eDisconnectedInterior, 4,
     "Interior is disconnected"},
    {TopologyValidationError::eSelfIntersection, 5, "Self-intersection"},
    {TopologyValidationError::eRingSelfIntersection, 6,
     "Ring Self-intersection"},
    {TopologyValidationError::eNestedShells, 7, "Nested shells"},
    {TopologyValidationError::eDuplicatedRings, 8, "Duplicate Rings"},
    {TopologyValidationError::eTooFewPoints, 9,
     "Too few points in geometry component"},
    {TopologyValidationError::eInvalidCoordinate, 10, "Invalid Coordinate"},
    {TopologyValidationError::eRingNotClosed, 11, "Ring is not closed"},
});

static_assert(TopologyValidationError::oNoInvalidIntersection == -1);
static_assert(TopologyValidationError::eError == 0);
static_assert(TopologyValidationError::eRingNotClosed == 11);

TEST(TopologyValidationErrorTest, ConstructsWithAllKnownErrorTypesAndPoint) {
  const CoordinateXY location(12.5, -3.25);

  for (const ErrorMessageCase& test_case : kErrorMessageCases) {
    SCOPED_TRACE(test_case.expected_message);
    const TopologyValidationError error(test_case.error_type, location);

    EXPECT_EQ(error.getErrorType(), test_case.expected_code);
    EXPECT_DOUBLE_EQ(error.getCoordinate().x, 12.5);
    EXPECT_DOUBLE_EQ(error.getCoordinate().y, -3.25);
    EXPECT_EQ(error.getMessage(), test_case.expected_message);
    EXPECT_EQ(error.toString(), std::string(test_case.expected_message) +
                                    " at or near point 12.5 -3.25");
  }
}

TEST(TopologyValidationErrorTest, SingleArgConstructorUsesNullCoordinate) {
  const TopologyValidationError error(
      TopologyValidationError::eSelfIntersection);

  EXPECT_EQ(error.getErrorType(),
            static_cast<int>(TopologyValidationError::eSelfIntersection));
  EXPECT_TRUE(error.getCoordinate().isNull());
  EXPECT_TRUE(std::isnan(error.getCoordinate().x));
  EXPECT_TRUE(std::isnan(error.getCoordinate().y));
  EXPECT_EQ(error.getMessage(), "Self-intersection");
  EXPECT_EQ(error.toString(), "Self-intersection at or near point " +
                                  error.getCoordinate().toString());
}

TEST(TopologyValidationErrorTest, NegativeErrorTypeReturnsEmptyMessage) {
  const TopologyValidationError sentinel_error(
      TopologyValidationError::oNoInvalidIntersection, CoordinateXY(4.0, 5.0));
  EXPECT_EQ(sentinel_error.getErrorType(), -1);
  EXPECT_EQ(sentinel_error.getMessage(), "");
  EXPECT_EQ(sentinel_error.toString(), " at or near point 4 5");

  const TopologyValidationError custom_negative_error(-42);
  EXPECT_EQ(custom_negative_error.getErrorType(), -42);
  EXPECT_TRUE(custom_negative_error.getCoordinate().isNull());
  EXPECT_EQ(custom_negative_error.getMessage(), "");
  EXPECT_THAT(custom_negative_error.toString(),
              ::testing::StartsWith(" at or near point "));
}

TEST(TopologyValidationErrorTest, PreservesPartialNaNCoordinateLocation) {
  const TopologyValidationError nan_x_error(
      TopologyValidationError::eInvalidCoordinate,
      CoordinateXY(DoubleNotANumber, 7.5));
  EXPECT_EQ(nan_x_error.getErrorType(),
            static_cast<int>(TopologyValidationError::eInvalidCoordinate));
  EXPECT_TRUE(std::isnan(nan_x_error.getCoordinate().x));
  EXPECT_DOUBLE_EQ(nan_x_error.getCoordinate().y, 7.5);
  EXPECT_FALSE(nan_x_error.getCoordinate().isNull());
  EXPECT_EQ(nan_x_error.getMessage(), "Invalid Coordinate");
  EXPECT_EQ(nan_x_error.toString(), "Invalid Coordinate at or near point " +
                                        nan_x_error.getCoordinate().toString());
}

}  // namespace
}  // namespace geos_google
