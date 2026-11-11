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

// Unit tests for third_party/geos/include/geos/util/IllegalStateException.h.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for throw/catch assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/util/IllegalStateException.h

#include "third_party/geos/include/geos/util/IllegalStateException.h"

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
using geos::util::IllegalStateException;

static_assert(std::is_base_of_v<GEOSException, IllegalStateException>);
static_assert(std::is_base_of_v<std::runtime_error, IllegalStateException>);
static_assert(std::is_base_of_v<std::exception, IllegalStateException>);
static_assert(std::is_default_constructible_v<IllegalStateException>);
static_assert(
    std::is_constructible_v<IllegalStateException, const std::string&>);
static_assert(std::is_copy_constructible_v<IllegalStateException>);
static_assert(std::is_copy_assignable_v<IllegalStateException>);
static_assert(std::is_nothrow_destructible_v<IllegalStateException>);

struct MessageCase {
  std::string_view label;
  std::string_view message;
  std::string_view expected_what;
};

constexpr auto kMessageCases = std::to_array<MessageCase>({
    {
        .label = "empty",
        .message = "",
        .expected_what = "IllegalStateException: ",
    },
    {
        .label = "uninitialized",
        .message = "Operation invoked before initialization",
        .expected_what =
            "IllegalStateException: Operation invoked before initialization",
    },
    {
        .label = "multiline",
        .message = "unexpected phase\nalready finalized",
        .expected_what =
            "IllegalStateException: unexpected phase\nalready finalized",
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
    if constexpr (!std::is_same_v<CatchType, IllegalStateException>) {
      EXPECT_NE(dynamic_cast<const IllegalStateException*>(&err), nullptr);
    }
  }
  EXPECT_TRUE(caught);
}

void ThrowDefaultException() { throw IllegalStateException(); }

void ThrowWithMessage(const std::string& msg) {
  throw IllegalStateException(msg);
}

TEST(IllegalStateExceptionTest, DefaultConstructorSetsPrefix) {
  const IllegalStateException ex;
  EXPECT_STREQ(ex.what(), "IllegalStateException: ");

  ExpectCaughtAs<IllegalStateException>(ThrowDefaultException,
                                        "IllegalStateException: ");
  EXPECT_THROW(ThrowDefaultException(), IllegalStateException);
}

TEST(IllegalStateExceptionTest, MessageConstructorPrependsClassName) {
  for (const auto& tc : kMessageCases) {
    SCOPED_TRACE(tc.label);
    const std::string msg(tc.message);
    const IllegalStateException ex(msg);
    EXPECT_EQ(std::string_view(ex.what()), tc.expected_what);

    ExpectCaughtAs<IllegalStateException>([&] { ThrowWithMessage(msg); },
                                          tc.expected_what);
  }
}

TEST(IllegalStateExceptionTest, CatchableThroughExceptionHierarchy) {
  auto throw_consumed = [] { ThrowWithMessage("already consumed"); };
  constexpr std::string_view kExpected =
      "IllegalStateException: already consumed";

  ExpectCaughtAs<IllegalStateException>(throw_consumed, kExpected);
  ExpectCaughtAs<GEOSException>(throw_consumed, kExpected);
  ExpectCaughtAs<std::runtime_error>(throw_consumed, kExpected);
  ExpectCaughtAs<std::exception>(throw_consumed, kExpected);

  EXPECT_THROW(throw_consumed(), GEOSException);
  EXPECT_THROW(throw_consumed(), std::runtime_error);
}

TEST(IllegalStateExceptionTest, CopyPolymorphicDeleteAndExceptionPtr) {
  const IllegalStateException original("invalid state");
  const IllegalStateException copied(original);
  EXPECT_STREQ(copied.what(), "IllegalStateException: invalid state");

  IllegalStateException assigned("initial");
  assigned = original;
  EXPECT_STREQ(assigned.what(), "IllegalStateException: invalid state");

  std::unique_ptr<GEOSException> as_geos =
      std::make_unique<IllegalStateException>("heap geos");
  EXPECT_STREQ(as_geos->what(), "IllegalStateException: heap geos");
  as_geos.reset();

  std::unique_ptr<std::runtime_error> as_runtime =
      std::make_unique<IllegalStateException>("heap runtime");
  EXPECT_STREQ(as_runtime->what(), "IllegalStateException: heap runtime");
  as_runtime.reset();

  std::unique_ptr<std::exception> as_std =
      std::make_unique<IllegalStateException>();
  EXPECT_STREQ(as_std->what(), "IllegalStateException: ");
  as_std.reset();

  const std::exception_ptr ptr =
      std::make_exception_ptr(IllegalStateException("propagated"));
  ExpectCaughtAs<IllegalStateException>([&] { std::rethrow_exception(ptr); },
                                        "IllegalStateException: propagated");
}

}  // namespace
}  // namespace geos_google
