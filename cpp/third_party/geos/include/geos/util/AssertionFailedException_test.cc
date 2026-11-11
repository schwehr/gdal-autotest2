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
// third_party/geos/include/geos/util/AssertionFailedException.h.
//
// google3 notes:
// - Requires copts = ["-fexceptions"] in BUILD for exception throw/catch tests.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/include/geos/util/AssertionFailedException.h

#include "third_party/geos/include/geos/util/AssertionFailedException.h"

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using ::geos::util::AssertionFailedException;
using ::geos::util::GEOSException;

static_assert(std::is_base_of_v<GEOSException, AssertionFailedException>);
static_assert(std::is_base_of_v<std::runtime_error, AssertionFailedException>);
static_assert(std::is_nothrow_destructible_v<AssertionFailedException>);

TEST(AssertionFailedExceptionTest, DefaultConstructorSetsPrefixOnly) {
  AssertionFailedException ex;
  EXPECT_STREQ(ex.what(), "AssertionFailedException: ");
}

TEST(AssertionFailedExceptionTest, MessageConstructorFormatsPrefixAndMessage) {
  const std::string detail = "invariant violated at index 3";
  AssertionFailedException ex(detail);
  EXPECT_STREQ(ex.what(),
               "AssertionFailedException: invariant violated at index 3");

  AssertionFailedException empty_msg("");
  EXPECT_STREQ(empty_msg.what(), "AssertionFailedException: ");
}

TEST(AssertionFailedExceptionTest, CopyPreservesMessage) {
  AssertionFailedException original("copied message");
  AssertionFailedException copied(original);
  EXPECT_STREQ(copied.what(), "AssertionFailedException: copied message");

  AssertionFailedException assigned;
  assigned = original;
  EXPECT_STREQ(assigned.what(), "AssertionFailedException: copied message");
}

TEST(AssertionFailedExceptionTest, CaughtAsDerivedAndBaseExceptionTypes) {
  EXPECT_THROW(throw AssertionFailedException("direct"),
               AssertionFailedException);
  EXPECT_THROW(throw AssertionFailedException("base"), GEOSException);

  try {
    throw AssertionFailedException("caught via GEOSException");
  } catch (const GEOSException& ex) {
    EXPECT_STREQ(ex.what(),
                 "AssertionFailedException: caught via GEOSException");
  }

  try {
    throw AssertionFailedException("caught via runtime_error");
  } catch (const std::runtime_error& ex) {
    EXPECT_STREQ(ex.what(),
                 "AssertionFailedException: caught via runtime_error");
  }

  try {
    throw AssertionFailedException();
  } catch (const std::exception& ex) {
    EXPECT_STREQ(ex.what(), "AssertionFailedException: ");
  }
}

TEST(AssertionFailedExceptionTest, PolymorphicDestructionViaBasePointer) {
  std::unique_ptr<GEOSException> geos_ptr =
      std::make_unique<AssertionFailedException>("heap allocated");
  EXPECT_STREQ(geos_ptr->what(), "AssertionFailedException: heap allocated");
  geos_ptr.reset();

  std::unique_ptr<std::runtime_error> std_ptr =
      std::make_unique<AssertionFailedException>();
  EXPECT_STREQ(std_ptr->what(), "AssertionFailedException: ");
  std_ptr.reset();
}

}  // namespace
}  // namespace geos_google
