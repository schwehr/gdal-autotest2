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
// third_party/geos/include/geos/util/UnsupportedOperationException.h.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/util/UnsupportedOperationException.h

#include "third_party/geos/include/geos/util/UnsupportedOperationException.h"

#include <array>
#include <exception>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/CircularString.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/CoordinateSequence.h"
#include "third_party/geos/include/geos/geom/Curve.h"
#include "third_party/geos/include/geos/geom/CurvePolygon.h"
#include "third_party/geos/include/geos/geom/Geometry.h"
#include "third_party/geos/include/geos/geom/GeometryFactory.h"
#include "third_party/geos/include/geos/geom/Point.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::geom::CircularString;
using geos::geom::CoordinateSequence;
using geos::geom::CoordinateXY;
using geos::geom::Curve;
using geos::geom::CurvePolygon;
using geos::geom::Geometry;
using geos::geom::GeometryFactory;
using geos::geom::Point;
using geos::util::GEOSException;
using geos::util::UnsupportedOperationException;

static_assert(std::is_base_of_v<GEOSException, UnsupportedOperationException>);
static_assert(
    std::is_base_of_v<std::runtime_error, UnsupportedOperationException>);
static_assert(std::is_base_of_v<std::exception, UnsupportedOperationException>);
static_assert(std::is_nothrow_destructible_v<UnsupportedOperationException>);
static_assert(std::has_virtual_destructor_v<UnsupportedOperationException>);
static_assert(std::is_default_constructible_v<UnsupportedOperationException>);
static_assert(
    std::is_constructible_v<UnsupportedOperationException, const std::string&>);
static_assert(
    std::is_convertible_v<std::string, UnsupportedOperationException>);
static_assert(std::is_copy_constructible_v<UnsupportedOperationException>);
static_assert(std::is_copy_assignable_v<UnsupportedOperationException>);

TEST(UnsupportedOperationExceptionTest, DefaultConstructorFormatsMessage) {
  const UnsupportedOperationException ex;
  EXPECT_STREQ(ex.what(), "UnsupportedOperationException: ");

  bool caught = false;
  try {
    throw UnsupportedOperationException();
  } catch (const UnsupportedOperationException& thrown) {
    caught = true;
    EXPECT_STREQ(thrown.what(), "UnsupportedOperationException: ");
  }
  EXPECT_TRUE(caught);
}

TEST(UnsupportedOperationExceptionTest,
     MessageConstructorFormatsPrefixAndMessage) {
  struct MessageCase {
    std::string_view name;
    std::string_view input;
    std::string_view expected_what;
  };

  constexpr auto kCases = std::to_array<MessageCase>({
      {
          .name = "empty_message",
          .input = "",
          .expected_what = "UnsupportedOperationException: ",
      },
      {
          .name = "simple_message",
          .input = "operation not implemented",
          .expected_what =
              "UnsupportedOperationException: operation not implemented",
      },
      {
          .name = "multiline_and_punctuation",
          .input = "line 1: detail\nline 2",
          .expected_what =
              "UnsupportedOperationException: line 1: detail\nline 2",
      },
  });

  for (const auto& test_case : kCases) {
    SCOPED_TRACE(test_case.name);
    const std::string msg(test_case.input);
    const UnsupportedOperationException ex(msg);
    EXPECT_EQ(std::string_view(ex.what()), test_case.expected_what);

    // Non-explicit constructor allows implicit conversion from std::string.
    const UnsupportedOperationException implicit_ex = msg;
    EXPECT_EQ(std::string_view(implicit_ex.what()), test_case.expected_what);
  }
}

TEST(UnsupportedOperationExceptionTest, CatchableThroughInheritanceHierarchy) {
  {
    bool caught_geos = false;
    try {
      throw UnsupportedOperationException();
    } catch (const GEOSException& ex) {
      caught_geos = true;
      EXPECT_STREQ(ex.what(), "UnsupportedOperationException: ");
    }
    EXPECT_TRUE(caught_geos);
  }

  {
    bool caught_runtime = false;
    try {
      throw UnsupportedOperationException("curved input");
    } catch (const std::runtime_error& ex) {
      caught_runtime = true;
      EXPECT_STREQ(ex.what(), "UnsupportedOperationException: curved input");
    }
    EXPECT_TRUE(caught_runtime);
  }

  {
    bool caught_std = false;
    try {
      throw UnsupportedOperationException("std base catch");
    } catch (const std::exception& ex) {
      caught_std = true;
      EXPECT_STREQ(ex.what(), "UnsupportedOperationException: std base catch");
    }
    EXPECT_TRUE(caught_std);
  }
}

TEST(UnsupportedOperationExceptionTest, CopyAndExceptionPtrPreserveMessage) {
  const UnsupportedOperationException original("copied detail");
  const UnsupportedOperationException copy_constructed(original);
  EXPECT_STREQ(copy_constructed.what(),
               "UnsupportedOperationException: copied detail");

  UnsupportedOperationException assigned;
  EXPECT_STREQ(assigned.what(), "UnsupportedOperationException: ");
  assigned = original;
  EXPECT_STREQ(assigned.what(), "UnsupportedOperationException: copied detail");

  const std::exception_ptr ptr =
      std::make_exception_ptr(UnsupportedOperationException("rethrow me"));
  ASSERT_TRUE(static_cast<bool>(ptr));

  bool caught_rethrow = false;
  try {
    std::rethrow_exception(ptr);
  } catch (const UnsupportedOperationException& ex) {
    caught_rethrow = true;
    EXPECT_STREQ(ex.what(), "UnsupportedOperationException: rethrow me");
  }
  EXPECT_TRUE(caught_rethrow);

  std::exception_ptr captured;
  try {
    throw UnsupportedOperationException();
  } catch (...) {
    captured = std::current_exception();
  }
  ASSERT_TRUE(static_cast<bool>(captured));

  bool caught_captured = false;
  try {
    std::rethrow_exception(captured);
  } catch (const GEOSException& ex) {
    caught_captured = true;
    EXPECT_STREQ(ex.what(), "UnsupportedOperationException: ");
  }
  EXPECT_TRUE(caught_captured);
}

TEST(UnsupportedOperationExceptionTest,
     VirtualDestructorDeletesThroughBasePointers) {
  std::unique_ptr<GEOSException> as_geos =
      std::make_unique<UnsupportedOperationException>();
  EXPECT_STREQ(as_geos->what(), "UnsupportedOperationException: ");
  as_geos.reset();

  std::unique_ptr<std::runtime_error> as_runtime =
      std::make_unique<UnsupportedOperationException>("heap runtime");
  EXPECT_STREQ(as_runtime->what(),
               "UnsupportedOperationException: heap runtime");
  as_runtime.reset();

  std::unique_ptr<std::exception> as_std =
      std::make_unique<UnsupportedOperationException>("heap std");
  EXPECT_STREQ(as_std->what(), "UnsupportedOperationException: heap std");
  as_std.reset();
}

TEST(UnsupportedOperationExceptionTest, ThrownByGeosOperations) {
  const GeometryFactory::Ptr factory = GeometryFactory::create();
  const std::unique_ptr<Point> empty_point = factory->createPoint();
  ASSERT_NE(empty_point, nullptr);
  ASSERT_TRUE(empty_point->isEmpty());

  struct AccessorCase {
    std::string_view name;
    double (Point::*accessor)() const;
  };
  constexpr auto kAccessors = std::to_array<AccessorCase>({
      {.name = "getX", .accessor = &Point::getX},
      {.name = "getY", .accessor = &Point::getY},
      {.name = "getZ", .accessor = &Point::getZ},
      {.name = "getM", .accessor = &Point::getM},
  });

  for (const auto& test_case : kAccessors) {
    SCOPED_TRACE(test_case.name);
    bool caught = false;
    try {
      (void)(empty_point.get()->*test_case.accessor)();
    } catch (const UnsupportedOperationException& ex) {
      caught = true;
      const std::string_view what(ex.what());
      EXPECT_TRUE(what.starts_with("UnsupportedOperationException: "));
      EXPECT_NE(what.find(test_case.name), std::string_view::npos);
    }
    EXPECT_TRUE(caught);
  }

  auto ring_coords =
      std::make_unique<CoordinateSequence>(std::initializer_list<CoordinateXY>{
          {0.0, 0.0}, {1.0, 1.0}, {2.0, 0.0}, {1.0, -1.0}, {0.0, 0.0}});
  std::unique_ptr<CircularString> arc =
      factory->createCircularString(std::move(ring_coords));
  std::unique_ptr<Curve> shell = std::move(arc);
  std::unique_ptr<CurvePolygon> curve_poly =
      factory->createCurvePolygon(std::move(shell));
  ASSERT_NE(curve_poly, nullptr);

  std::unique_ptr<Geometry> boundary;
  EXPECT_THROW(boundary = curve_poly->getBoundary(),
               UnsupportedOperationException);
  EXPECT_EQ(boundary, nullptr);
  EXPECT_THROW(curve_poly->normalize(), UnsupportedOperationException);
}

}  // namespace
}  // namespace geos_google
