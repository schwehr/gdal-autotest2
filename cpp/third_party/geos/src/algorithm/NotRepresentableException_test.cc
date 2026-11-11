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

#include "third_party/geos/include/geos/algorithm/NotRepresentableException.h"

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/algorithm/HCoordinate.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using ::geos::algorithm::HCoordinate;
using ::geos::algorithm::NotRepresentableException;
using ::geos::geom::Coordinate;
using ::geos::util::GEOSException;
using ::testing::HasSubstr;

TEST(NotRepresentableExceptionTest, DefaultConstructorMessageAndHierarchy) {
  NotRepresentableException ex;
  EXPECT_STREQ(ex.what(),
               "NotRepresentableException: "
               "Projective point not representable on the Cartesian plane.");

  const GEOSException* as_geos_ex = &ex;
  const std::runtime_error* as_runtime = &ex;
  const std::exception* as_std_ex = &ex;
  EXPECT_STREQ(as_geos_ex->what(), ex.what());
  EXPECT_STREQ(as_runtime->what(), ex.what());
  EXPECT_STREQ(as_std_ex->what(), ex.what());
}

TEST(NotRepresentableExceptionTest, MessageConstructorFormatsWhat) {
  NotRepresentableException ex("division by zero in w");
  EXPECT_STREQ(ex.what(), "NotRepresentableException: division by zero in w");

  NotRepresentableException empty_msg("");
  EXPECT_STREQ(empty_msg.what(), "NotRepresentableException: ");
}

TEST(NotRepresentableExceptionTest, CopyAndThrowCatchAsBaseTypes) {
  NotRepresentableException original("custom projective error");
  NotRepresentableException copied(original);
  EXPECT_STREQ(copied.what(), original.what());

  try {
    throw NotRepresentableException();
    FAIL() << "Expected NotRepresentableException";
  } catch (const GEOSException& caught) {
    EXPECT_THAT(caught.what(), HasSubstr("NotRepresentableException"));
  }

  try {
    throw NotRepresentableException("parallel segments");
    FAIL() << "Expected NotRepresentableException";
  } catch (const NotRepresentableException& caught) {
    EXPECT_THAT(caught.what(), HasSubstr("parallel segments"));
  }
}

TEST(NotRepresentableExceptionTest, PolymorphicHeapDeletion) {
  std::unique_ptr<GEOSException> ptr1 =
      std::make_unique<NotRepresentableException>();
  EXPECT_THAT(ptr1->what(), HasSubstr("Projective point not representable"));
  ptr1.reset();

  std::unique_ptr<std::runtime_error> ptr2 =
      std::make_unique<NotRepresentableException>("heap msg");
  EXPECT_THAT(ptr2->what(), HasSubstr("heap msg"));
  ptr2.reset();
}

TEST(NotRepresentableExceptionTest, ThrownByHCoordinateOnParallelLines) {
  Coordinate result;
  EXPECT_THROW(HCoordinate::intersection(
                   Coordinate(0.0, 0.0), Coordinate(1.0, 0.0),
                   Coordinate(0.0, 1.0), Coordinate(1.0, 1.0), result),
               NotRepresentableException);

  // Collinear (0/0 -> NaN) also throws NotRepresentableException.
  EXPECT_THROW(HCoordinate::intersection(
                   Coordinate(0.0, 0.0), Coordinate(1.0, 0.0),
                   Coordinate(2.0, 0.0), Coordinate(3.0, 0.0), result),
               NotRepresentableException);

  // Intersecting lines succeed without throwing.
  HCoordinate::intersection(Coordinate(0.0, 0.0), Coordinate(2.0, 2.0),
                            Coordinate(0.0, 2.0), Coordinate(2.0, 0.0), result);
  EXPECT_DOUBLE_EQ(result.x, 1.0);
  EXPECT_DOUBLE_EQ(result.y, 1.0);
}

}  // namespace
}  // namespace geos_google
