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

// Unit tests for third_party/geos/include/geos/util/IllegalArgumentException.h.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for throw/catch assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/util/IllegalArgumentException.h

#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

#include <array>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::util::GEOSException;
using geos::util::IllegalArgumentException;

static_assert(std::is_base_of_v<GEOSException, IllegalArgumentException>);
static_assert(std::is_base_of_v<std::runtime_error, IllegalArgumentException>);
static_assert(std::is_base_of_v<std::exception, IllegalArgumentException>);
static_assert(std::is_default_constructible_v<IllegalArgumentException>);
static_assert(
    std::is_constructible_v<IllegalArgumentException, const std::string&>);
static_assert(std::is_copy_constructible_v<IllegalArgumentException>);
static_assert(std::is_copy_assignable_v<IllegalArgumentException>);
static_assert(std::is_nothrow_destructible_v<IllegalArgumentException>);

struct MessageCase {
  std::string_view label;
  std::string_view message;
  std::string_view expected_what;
};

constexpr auto kMessageCases = std::to_array<MessageCase>({
    {
        .label = "empty",
        .message = "",
        .expected_what = "IllegalArgumentException: ",
    },
    {
        .label = "geometry_collection",
        .message = "GeometryCollection arguments are unsupported",
        .expected_what =
            "IllegalArgumentException: GeometryCollection arguments are "
            "unsupported",
    },
    {
        .label = "multiline",
        .message = "bad ring\nnot closed",
        .expected_what = "IllegalArgumentException: bad ring\nnot closed",
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
    if constexpr (!std::is_same_v<CatchType, IllegalArgumentException>) {
      EXPECT_NE(dynamic_cast<const IllegalArgumentException*>(&err), nullptr);
    }
  }
  EXPECT_TRUE(caught);
}

void ThrowDefaultException() { throw IllegalArgumentException(); }

void ThrowWithMessage(const std::string& msg) {
  throw IllegalArgumentException(msg);
}

TEST(IllegalArgumentExceptionTest, DefaultConstructorSetsPrefix) {
  const IllegalArgumentException ex;
  EXPECT_STREQ(ex.what(), "IllegalArgumentException: ");

  ExpectCaughtAs<IllegalArgumentException>(ThrowDefaultException,
                                           "IllegalArgumentException: ");
  EXPECT_THROW(ThrowDefaultException(), IllegalArgumentException);
}

TEST(IllegalArgumentExceptionTest, MessageConstructorPrependsClassName) {
  for (const auto& tc : kMessageCases) {
    SCOPED_TRACE(tc.label);
    const std::string msg(tc.message);
    const IllegalArgumentException ex(msg);
    EXPECT_EQ(std::string_view(ex.what()), tc.expected_what);

    ExpectCaughtAs<IllegalArgumentException>([&] { ThrowWithMessage(msg); },
                                             tc.expected_what);
  }
}

TEST(IllegalArgumentExceptionTest, CatchableThroughExceptionHierarchy) {
  auto throw_negative_radius = [] { ThrowWithMessage("negative radius"); };
  constexpr std::string_view kExpected =
      "IllegalArgumentException: negative radius";

  ExpectCaughtAs<IllegalArgumentException>(throw_negative_radius, kExpected);
  ExpectCaughtAs<GEOSException>(throw_negative_radius, kExpected);
  ExpectCaughtAs<std::runtime_error>(throw_negative_radius, kExpected);
  ExpectCaughtAs<std::exception>(throw_negative_radius, kExpected);

  EXPECT_THROW(throw_negative_radius(), GEOSException);
  EXPECT_THROW(throw_negative_radius(), std::runtime_error);
}

TEST(IllegalArgumentExceptionTest, CopyPolymorphicDeleteAndExceptionPtr) {
  const IllegalArgumentException original("invalid ordinate");
  const IllegalArgumentException copied(original);
  EXPECT_STREQ(copied.what(), "IllegalArgumentException: invalid ordinate");

  IllegalArgumentException assigned("initial");
  assigned = original;
  EXPECT_STREQ(assigned.what(), "IllegalArgumentException: invalid ordinate");

  std::unique_ptr<GEOSException> as_geos =
      std::make_unique<IllegalArgumentException>("heap geos");
  EXPECT_STREQ(as_geos->what(), "IllegalArgumentException: heap geos");
  as_geos.reset();

  std::unique_ptr<std::runtime_error> as_runtime =
      std::make_unique<IllegalArgumentException>("heap runtime");
  EXPECT_STREQ(as_runtime->what(), "IllegalArgumentException: heap runtime");
  as_runtime.reset();

  std::unique_ptr<std::exception> as_std =
      std::make_unique<IllegalArgumentException>();
  EXPECT_STREQ(as_std->what(), "IllegalArgumentException: ");
  as_std.reset();

  const std::exception_ptr ptr =
      std::make_exception_ptr(IllegalArgumentException("propagated"));
  ExpectCaughtAs<IllegalArgumentException>(
      [&] { std::rethrow_exception(ptr); },
      "IllegalArgumentException: propagated");
}

}  // namespace
}  // namespace geos_google
