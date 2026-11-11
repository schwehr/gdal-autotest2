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

// Unit tests for third_party/geos/src/dissolve/DissolveHalfEdge.cpp.
//
// See also:
//   https://github.com/libgeos/geos/blob/main/src/dissolve/DissolveHalfEdge.cpp

#include "third_party/geos/include/geos/dissolve/DissolveHalfEdge.h"

#include <cmath>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/edgegraph/HalfEdge.h"
#include "third_party/geos/include/geos/edgegraph/MarkHalfEdge.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"

namespace geos_google {
namespace {

using geos::dissolve::DissolveHalfEdge;
using geos::edgegraph::HalfEdge;
using geos::edgegraph::MarkHalfEdge;
using geos::geom::CoordinateXY;
using geos::geom::CoordinateXYZM;

static_assert(std::is_base_of_v<MarkHalfEdge, DissolveHalfEdge>);
static_assert(std::is_base_of_v<HalfEdge, DissolveHalfEdge>);

TEST(DissolveHalfEdgeTest, DefaultStateAndCoordinatePreservation) {
  const CoordinateXYZM p0(1.0, 2.0, 3.0, 4.0);
  const CoordinateXYZM p1(5.0, 6.0, 7.0, 8.0);
  DissolveHalfEdge e0(p0);
  DissolveHalfEdge e1(p1);
  e0.link(&e1);

  EXPECT_FALSE(e0.isStart());
  EXPECT_FALSE(e1.isStart());
  EXPECT_FALSE(e0.isMarked());
  EXPECT_FALSE(e1.isMarked());
  EXPECT_TRUE(e0.orig().equals4D(p0));
  EXPECT_TRUE(e0.dest().equals4D(p1));
  EXPECT_EQ(e0.sym(), &e1);
  EXPECT_EQ(e1.sym(), &e0);
}

TEST(DissolveHalfEdgeTest, MissingZAndMAreNaN) {
  const CoordinateXYZM p0(CoordinateXY(-3.25, 7.5));
  const CoordinateXYZM p1(CoordinateXY(2.5, -1.5));
  DissolveHalfEdge e0(p0);
  DissolveHalfEdge e1(p1);
  e0.link(&e1);

  EXPECT_DOUBLE_EQ(e0.orig().x, -3.25);
  EXPECT_DOUBLE_EQ(e0.orig().y, 7.5);
  EXPECT_TRUE(std::isnan(e0.orig().z));
  EXPECT_TRUE(std::isnan(e0.orig().m));
  EXPECT_FALSE(e0.isStart());
}

TEST(DissolveHalfEdgeTest, SetStartIsDirectionalAndIdempotent) {
  DissolveHalfEdge e0(CoordinateXYZM(0.0, 0.0, 0.0, 0.0));
  DissolveHalfEdge e1(CoordinateXYZM(1.0, 1.0, 0.0, 0.0));
  e0.link(&e1);

  EXPECT_FALSE(e0.isStart());
  EXPECT_FALSE(e1.isStart());

  e0.setStart();
  EXPECT_TRUE(e0.isStart());
  EXPECT_FALSE(e1.isStart());

  // Repeated calls remain true and keep sym unchanged.
  e0.setStart();
  EXPECT_TRUE(e0.isStart());
  EXPECT_FALSE(e1.isStart());

  e1.setStart();
  EXPECT_TRUE(e0.isStart());
  EXPECT_TRUE(e1.isStart());
}

TEST(DissolveHalfEdgeTest, StartFlagIsIndependentFromMarkState) {
  DissolveHalfEdge e0(CoordinateXYZM(0.0, 0.0, 1.0, 2.0));
  DissolveHalfEdge e1(CoordinateXYZM(4.0, 3.0, 1.0, 2.0));
  e0.link(&e1);

  MarkHalfEdge::mark(&e0);
  EXPECT_TRUE(e0.isMarked());
  EXPECT_FALSE(e0.isStart());

  e0.setStart();
  EXPECT_TRUE(e0.isStart());
  EXPECT_FALSE(e1.isStart());
  EXPECT_FALSE(e1.isMarked());

  MarkHalfEdge::markBoth(&e0);
  EXPECT_TRUE(e0.isMarked());
  EXPECT_TRUE(e1.isMarked());
  EXPECT_TRUE(e0.isStart());
  EXPECT_FALSE(e1.isStart());

  MarkHalfEdge::setMarkBoth(&e0, false);
  EXPECT_FALSE(e0.isMarked());
  EXPECT_FALSE(e1.isMarked());
  EXPECT_TRUE(e0.isStart());
  EXPECT_FALSE(e1.isStart());
}

}  // namespace
}  // namespace geos_google
