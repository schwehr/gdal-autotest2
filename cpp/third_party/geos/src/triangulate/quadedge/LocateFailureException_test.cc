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
// third_party/geos/src/triangulate/quadedge/LocateFailureException.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/triangulate/quadedge/LocateFailureException.cpp

#include "third_party/geos/include/geos/triangulate/quadedge/LocateFailureException.h"

#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Envelope.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdge.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeQuartet.h"
#include "third_party/geos/include/geos/triangulate/quadedge/QuadEdgeSubdivision.h"
#include "third_party/geos/include/geos/triangulate/quadedge/Vertex.h"
#include "third_party/geos/include/geos/util/GEOSException.h"

namespace geos_google {
namespace {

using geos::geom::Envelope;
using geos::triangulate::quadedge::LocateFailureException;
using geos::triangulate::quadedge::QuadEdge;
using geos::triangulate::quadedge::QuadEdgeSubdivision;
using geos::triangulate::quadedge::Vertex;
using geos::util::GEOSException;

static_assert(std::is_base_of_v<GEOSException, LocateFailureException>);
static_assert(std::is_base_of_v<std::runtime_error, LocateFailureException>);
static_assert(std::is_base_of_v<std::exception, LocateFailureException>);

TEST(LocateFailureExceptionTest, ConstructsWithFormattedNamePrefix) {
  const LocateFailureException ex("vertex lookup failed");
  EXPECT_STREQ(ex.what(), "LocateFailureException: vertex lookup failed");

  const LocateFailureException empty_msg("");
  EXPECT_STREQ(empty_msg.what(), "LocateFailureException: ");
}

TEST(LocateFailureExceptionTest, CatchableAsBaseExceptionTypes) {
  try {
    throw LocateFailureException("non-convergent walk");
  } catch (const GEOSException& ex) {
    EXPECT_STREQ(ex.what(), "LocateFailureException: non-convergent walk");
  }

  try {
    throw LocateFailureException("base runtime check");
  } catch (const std::runtime_error& ex) {
    EXPECT_STREQ(ex.what(), "LocateFailureException: base runtime check");
  }
}

TEST(LocateFailureExceptionTest, CopyPreservesMessage) {
  const LocateFailureException original("copied message");
  const LocateFailureException copied(original);
  EXPECT_STREQ(copied.what(), original.what());
}

TEST(LocateFailureExceptionTest, ThrownWhenSubdivisionLocateCannotConverge) {
  QuadEdgeSubdivision subdiv(Envelope(0.0, 10.0, 0.0, 10.0), 0.0);
  const QuadEdge& start = subdiv.getEdges()[0].base();

  // Point strictly outside the subdivision frame triangle causes locateFromEdge
  // to exceed maxIter and throw LocateFailureException.
  const Vertex outside(-1000.0, 0.0);
  try {
    subdiv.locateFromEdge(outside, start);
    FAIL() << "Expected LocateFailureException";
  } catch (const LocateFailureException& ex) {
    const std::string_view message(ex.what());
    EXPECT_NE(message.find("LocateFailureException:"), std::string_view::npos);
    EXPECT_NE(message.find("Could not locate vertex."), std::string_view::npos);
  }
}

}  // namespace
}  // namespace geos_google
