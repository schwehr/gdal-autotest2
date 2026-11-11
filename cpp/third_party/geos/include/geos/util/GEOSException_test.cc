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

// Unit tests for third_party/geos/include/geos/util/GEOSException.h.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for throw/catch assertions.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/util/GEOSException.h

#include "third_party/geos/include/geos/util/GEOSException.h"

#include <array>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "testing/base/public/gunit.h"

namespace geos_google {
namespace {

using geos::util::GEOSException;

static_assert(std::is_base_of_v<std::runtime_error, GEOSException>);
static_assert(std::is_base_of_v<std::exception, GEOSException>);
static_assert(std::is_default_constructible_v<GEOSException>);
static_assert(std::is_constructible_v<GEOSException, const std::string&>);
static_assert(std::is_constructible_v<GEOSException, const std::string&,
                                      const std::string&>);
static_assert(std::is_copy_constructible_v<GEOSException>);
static_assert(std::is_copy_assignable_v<GEOSException>);
static_assert(std::is_move_constructible_v<GEOSException>);
static_assert(std::is_move_assignable_v<GEOSException>);
static_assert(std::is_nothrow_destructible_v<GEOSException>);

struct MessageCase {
  std::string_view label;
  std::string_view message;
};

constexpr auto kMessageCases = std::to_array<MessageCase>({
    {
        .label = "empty",
        .message = "",
    },
    {
        .label = "simple",
        .message = "Invalid coordinate",
    },
    {
        .label = "multiline",
        .message = "line one\nline two",
    },
    {
        .label = "with_colon",
        .message = "Reader: unexpected EOF",
    },
});

struct NamedMessageCase {
  std::string_view label;
  std::string_view name;
  std::string_view message;
  std::string_view expected_what;
};

constexpr auto kNamedMessageCases = std::to_array<NamedMessageCase>({
    {
        .label = "standard",
        .name = "ParseException",
        .message = "Unexpected token",
        .expected_what = "ParseException: Unexpected token",
    },
    {
        .label = "empty_message",
        .name = "TopologyException",
        .message = "",
        .expected_what = "TopologyException: ",
    },
    {
        .label = "empty_name",
        .name = "",
        .message = "details only",
        .expected_what = ": details only",
    },
    {
        .label = "both_empty",
        .name = "",
        .message = "",
        .expected_what = ": ",
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
    if constexpr (!std::is_same_v<CatchType, GEOSException>) {
      EXPECT_NE(dynamic_cast<const GEOSException*>(&err), nullptr);
    }
  }
  EXPECT_TRUE(caught);
}

void ThrowDefaultException() { throw GEOSException(); }

void ThrowWithMessage(const std::string& msg) { throw GEOSException(msg); }

void ThrowWithNameAndMessage(const std::string& name, const std::string& msg) {
  throw GEOSException(name, msg);
}

TEST(GEOSExceptionTest, DefaultConstructorSetsUnknownErrorMessage) {
  const GEOSException ex;
  EXPECT_STREQ(ex.what(), "Unknown error");

  ExpectCaughtAs<GEOSException>(ThrowDefaultException, "Unknown error");
  EXPECT_THROW(ThrowDefaultException(), GEOSException);
}

TEST(GEOSExceptionTest, MessageConstructorPreservesMessage) {
  for (const auto& tc : kMessageCases) {
    SCOPED_TRACE(tc.label);
    const std::string msg(tc.message);
    const GEOSException ex(msg);
    EXPECT_EQ(std::string_view(ex.what()), tc.message);

    ExpectCaughtAs<GEOSException>([&] { ThrowWithMessage(msg); }, tc.message);
  }
}

TEST(GEOSExceptionTest, NameAndMessageConstructorFormatsColonSeparator) {
  for (const auto& tc : kNamedMessageCases) {
    SCOPED_TRACE(tc.label);
    const std::string name(tc.name);
    const std::string msg(tc.message);
    const GEOSException ex(name, msg);
    EXPECT_EQ(std::string_view(ex.what()), tc.expected_what);

    ExpectCaughtAs<GEOSException>([&] { ThrowWithNameAndMessage(name, msg); },
                                  tc.expected_what);
  }
}

TEST(GEOSExceptionTest, CatchableAsStdRuntimeErrorAndStdException) {
  auto throw_named = [] {
    ThrowWithNameAndMessage("TopologyException", "side location conflict");
  };
  ExpectCaughtAs<std::runtime_error>(
      throw_named, "TopologyException: side location conflict");
  ExpectCaughtAs<std::exception>(throw_named,
                                 "TopologyException: side location conflict");
  EXPECT_THROW(throw_named(), std::runtime_error);
  EXPECT_THROW(throw_named(), std::exception);
}

TEST(GEOSExceptionTest, CopyAndMovePreserveWhatMessage) {
  const GEOSException original("CustomError", "original message");
  const GEOSException copied(original);
  EXPECT_STREQ(copied.what(), "CustomError: original message");

  GEOSException copy_assigned("initial");
  copy_assigned = original;
  EXPECT_STREQ(copy_assigned.what(), "CustomError: original message");

  GEOSException move_source("MoveError", "moved message");
  const GEOSException moved(std::move(move_source));
  EXPECT_STREQ(moved.what(), "MoveError: moved message");

  GEOSException move_assign_source("AssignError", "move assigned");
  GEOSException move_assigned;
  move_assigned = std::move(move_assign_source);
  EXPECT_STREQ(move_assigned.what(), "AssignError: move assigned");
}

TEST(GEOSExceptionTest, PolymorphicDestructionAndExceptionPtrRoundTrip) {
  std::unique_ptr<std::runtime_error> as_runtime =
      std::make_unique<GEOSException>("heap runtime");
  EXPECT_STREQ(as_runtime->what(), "heap runtime");
  as_runtime.reset();

  std::unique_ptr<std::exception> as_std =
      std::make_unique<GEOSException>("Heap", "std");
  EXPECT_STREQ(as_std->what(), "Heap: std");
  as_std.reset();

  const std::exception_ptr ptr =
      std::make_exception_ptr(GEOSException("Async", "propagated"));
  ExpectCaughtAs<GEOSException>([&] { std::rethrow_exception(ptr); },
                                "Async: propagated");
}

}  // namespace
}  // namespace geos_google
