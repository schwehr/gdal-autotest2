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

#include "third_party/geos/include/geos/geom/Envelope.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/constants.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"

namespace geos_google {
namespace {

using ::geos::DoubleInfinity;
using ::geos::DoubleNegInfinity;
using ::geos::DoubleNotANumber;
using ::geos::geom::CoordinateXY;
using ::geos::geom::Envelope;

TEST(EnvelopeTest, DefaultConstructorCreatesNullEnvelope) {
  Envelope env;
  EXPECT_TRUE(env.isNull());
  EXPECT_DOUBLE_EQ(env.getWidth(), 0.0);
  EXPECT_DOUBLE_EQ(env.getHeight(), 0.0);
  EXPECT_DOUBLE_EQ(env.getArea(), 0.0);
  EXPECT_DOUBLE_EQ(env.getPerimeter(), 0.0);
  EXPECT_DOUBLE_EQ(env.getDiameter(), 0.0);
  EXPECT_FALSE(env.isfinite());
  EXPECT_TRUE(env.isFinite());
}

TEST(EnvelopeTest, CoordinateAndScalarConstructorsSortBounds) {
  Envelope sorted(1.0, 5.0, 2.0, 8.0);
  EXPECT_FALSE(sorted.isNull());
  EXPECT_DOUBLE_EQ(sorted.getMinX(), 1.0);
  EXPECT_DOUBLE_EQ(sorted.getMaxX(), 5.0);
  EXPECT_DOUBLE_EQ(sorted.getMinY(), 2.0);
  EXPECT_DOUBLE_EQ(sorted.getMaxY(), 8.0);
  EXPECT_DOUBLE_EQ(sorted.getWidth(), 4.0);
  EXPECT_DOUBLE_EQ(sorted.getHeight(), 6.0);
  EXPECT_DOUBLE_EQ(sorted.getArea(), 24.0);
  EXPECT_DOUBLE_EQ(sorted.getPerimeter(), 20.0);
  EXPECT_DOUBLE_EQ(sorted.getDiameter(), std::hypot(4.0, 6.0));
  EXPECT_TRUE(sorted.isfinite());
  EXPECT_TRUE(sorted.isFinite());

  Envelope reversed(5.0, 1.0, 8.0, 2.0);
  EXPECT_EQ(reversed, sorted);

  Envelope from_coords(CoordinateXY(5.0, 2.0), CoordinateXY(1.0, 8.0));
  EXPECT_EQ(from_coords, sorted);

  Envelope single(CoordinateXY(-3.5, 4.25));
  EXPECT_FALSE(single.isNull());
  EXPECT_DOUBLE_EQ(single.getMinX(), -3.5);
  EXPECT_DOUBLE_EQ(single.getMaxX(), -3.5);
  EXPECT_DOUBLE_EQ(single.getMinY(), 4.25);
  EXPECT_DOUBLE_EQ(single.getMaxY(), 4.25);
  EXPECT_DOUBLE_EQ(single.getWidth(), 0.0);
  EXPECT_DOUBLE_EQ(single.getHeight(), 0.0);
  EXPECT_DOUBLE_EQ(single.getArea(), 0.0);

  Envelope::Ptr ptr = std::make_unique<Envelope>(1.0, 5.0, 2.0, 8.0);
  EXPECT_EQ(*ptr, sorted);
}

TEST(EnvelopeTest, StringConstructorAndFormatting) {
  Envelope from_str("Env[7.2:2.3,7.1:8.2]");
  EXPECT_FALSE(from_str.isNull());
  EXPECT_DOUBLE_EQ(from_str.getMinX(), 2.3);
  EXPECT_DOUBLE_EQ(from_str.getMaxX(), 7.2);
  EXPECT_DOUBLE_EQ(from_str.getMinY(), 7.1);
  EXPECT_DOUBLE_EQ(from_str.getMaxY(), 8.2);

  Envelope neg("Env[-10.5:5.25,-3.5:4.5]");
  EXPECT_DOUBLE_EQ(neg.getMinX(), -10.5);
  EXPECT_DOUBLE_EQ(neg.getMaxX(), 5.25);
  EXPECT_DOUBLE_EQ(neg.getMinY(), -3.5);
  EXPECT_DOUBLE_EQ(neg.getMaxY(), 4.5);

  Envelope bare("[1:4,2:6]");
  EXPECT_EQ(bare, Envelope(1.0, 4.0, 2.0, 6.0));

  Envelope exact(1.0, 4.0, 2.0, 6.0);
  EXPECT_EQ(exact.toString(), "Env[1:4,2:6]");
  std::ostringstream oss;
  oss << exact;
  EXPECT_EQ(oss.str(), "Env[1:4,2:6]");
  EXPECT_EQ(Envelope(exact.toString()), exact);

  Envelope null_env;
  EXPECT_EQ(null_env.toString(), "Env[nan:nan,nan:nan]");
  Envelope parsed_null(null_env.toString());
  EXPECT_TRUE(parsed_null.isNull());
}

TEST(EnvelopeTest, InitAndSetToNull) {
  Envelope env(1.0, 2.0, 3.0, 4.0);
  EXPECT_FALSE(env.isNull());

  env.setToNull();
  EXPECT_TRUE(env.isNull());

  env.init(10.0, 2.0, 20.0, 5.0);
  EXPECT_EQ(env, Envelope(2.0, 10.0, 5.0, 20.0));

  env.init();
  EXPECT_TRUE(env.isNull());

  env.init(CoordinateXY(-1.0, 3.0), CoordinateXY(4.0, -2.0));
  EXPECT_EQ(env, Envelope(-1.0, 4.0, -2.0, 3.0));

  env.init(CoordinateXY(7.0, 9.0));
  EXPECT_EQ(env, Envelope(7.0, 7.0, 9.0, 9.0));
}

TEST(EnvelopeTest, IsFiniteChecksAllExtentsAndArea) {
  EXPECT_TRUE(Envelope(0.0, 1.0, 0.0, 1.0).isfinite());
  EXPECT_FALSE(Envelope().isfinite());

  EXPECT_FALSE(Envelope(DoubleNegInfinity, 1.0, 0.0, 1.0).isfinite());
  EXPECT_FALSE(Envelope(0.0, DoubleInfinity, 0.0, 1.0).isfinite());
  EXPECT_FALSE(Envelope(0.0, 1.0, DoubleNegInfinity, 1.0).isfinite());
  EXPECT_FALSE(Envelope(0.0, 1.0, 0.0, DoubleInfinity).isfinite());

  EXPECT_FALSE(Envelope(0.0, DoubleInfinity, 0.0, 1.0).isFinite());
  EXPECT_FALSE(Envelope(0.0, DoubleInfinity, 0.0, 0.0).isFinite());
  EXPECT_TRUE(Envelope(0.0, 2.0, 0.0, 3.0).isFinite());
}

TEST(EnvelopeTest, StaticIntersectsPointAndExtent) {
  CoordinateXY p1(0.0, 0.0);
  CoordinateXY p2(10.0, 10.0);

  EXPECT_TRUE(Envelope::intersects(p1, p2, CoordinateXY(5.0, 5.0)));
  EXPECT_TRUE(Envelope::intersects(p2, p1, CoordinateXY(5.0, 5.0)));
  EXPECT_TRUE(Envelope::intersects(p1, p2, CoordinateXY(0.0, 10.0)));
  EXPECT_TRUE(Envelope::intersects(p1, p2, CoordinateXY(10.0, 0.0)));

  EXPECT_FALSE(Envelope::intersects(p1, p2, CoordinateXY(-0.1, 5.0)));
  EXPECT_FALSE(Envelope::intersects(p1, p2, CoordinateXY(10.1, 5.0)));
  EXPECT_FALSE(Envelope::intersects(p1, p2, CoordinateXY(5.0, -0.1)));
  EXPECT_FALSE(Envelope::intersects(p1, p2, CoordinateXY(5.0, 10.1)));
  EXPECT_FALSE(
      Envelope::intersects(p1, p2, CoordinateXY(DoubleNotANumber, 5.0)));
  EXPECT_FALSE(
      Envelope::intersects(p1, p2, CoordinateXY(5.0, DoubleNotANumber)));

  EXPECT_TRUE(Envelope::intersects(p1, p2, CoordinateXY(5.0, 5.0),
                                   CoordinateXY(15.0, 15.0)));
  EXPECT_TRUE(Envelope::intersects(p2, p1, CoordinateXY(15.0, 15.0),
                                   CoordinateXY(10.0, 10.0)));
  EXPECT_FALSE(Envelope::intersects(p1, p2, CoordinateXY(11.0, 0.0),
                                    CoordinateXY(15.0, 10.0)));
  EXPECT_FALSE(Envelope::intersects(p1, p2, CoordinateXY(-5.0, 0.0),
                                    CoordinateXY(-1.0, 10.0)));
  EXPECT_FALSE(Envelope::intersects(p1, p2, CoordinateXY(0.0, 11.0),
                                    CoordinateXY(10.0, 15.0)));
  EXPECT_FALSE(Envelope::intersects(p1, p2, CoordinateXY(0.0, -5.0),
                                    CoordinateXY(10.0, -1.0)));
}

TEST(EnvelopeTest, MemberIntersectsTwoCoordinates) {
  Envelope null_env;
  EXPECT_FALSE(
      null_env.intersects(CoordinateXY(0.0, 0.0), CoordinateXY(10.0, 10.0)));

  Envelope env(0.0, 10.0, 0.0, 10.0);
  EXPECT_TRUE(env.intersects(CoordinateXY(2.0, 2.0), CoordinateXY(8.0, 8.0)));
  EXPECT_TRUE(env.intersects(CoordinateXY(8.0, 8.0), CoordinateXY(2.0, 2.0)));
  EXPECT_TRUE(env.intersects(CoordinateXY(-2.0, 5.0), CoordinateXY(12.0, 5.0)));
  EXPECT_TRUE(
      env.intersects(CoordinateXY(10.0, 10.0), CoordinateXY(15.0, 15.0)));

  // Disjoint to the right, left, above, and below (with both point orders).
  EXPECT_FALSE(
      env.intersects(CoordinateXY(11.0, 2.0), CoordinateXY(15.0, 8.0)));
  EXPECT_FALSE(
      env.intersects(CoordinateXY(15.0, 8.0), CoordinateXY(11.0, 2.0)));
  EXPECT_FALSE(
      env.intersects(CoordinateXY(-5.0, 2.0), CoordinateXY(-1.0, 8.0)));
  EXPECT_FALSE(
      env.intersects(CoordinateXY(-1.0, 8.0), CoordinateXY(-5.0, 2.0)));
  EXPECT_FALSE(
      env.intersects(CoordinateXY(2.0, 11.0), CoordinateXY(8.0, 15.0)));
  EXPECT_FALSE(
      env.intersects(CoordinateXY(8.0, 15.0), CoordinateXY(2.0, 11.0)));
  EXPECT_FALSE(
      env.intersects(CoordinateXY(2.0, -5.0), CoordinateXY(8.0, -1.0)));
  EXPECT_FALSE(
      env.intersects(CoordinateXY(8.0, -1.0), CoordinateXY(2.0, -5.0)));
}

TEST(EnvelopeTest, MemberIntersectsAndDisjoint) {
  Envelope env(0.0, 10.0, 0.0, 10.0);
  Envelope overlapping(5.0, 15.0, 5.0, 15.0);
  Envelope touching(10.0, 12.0, 0.0, 5.0);
  Envelope disjoint_env(11.0, 15.0, 0.0, 10.0);
  Envelope null_env;

  EXPECT_TRUE(env.intersects(CoordinateXY(5.0, 5.0)));
  EXPECT_TRUE(env.intersects(CoordinateXY(0.0, 10.0)));
  EXPECT_FALSE(env.intersects(CoordinateXY(-1.0, 5.0)));
  EXPECT_FALSE(null_env.intersects(CoordinateXY(5.0, 5.0)));

  EXPECT_TRUE(env.intersects(5.0, 5.0));
  EXPECT_FALSE(env.intersects(5.0, 11.0));
  EXPECT_FALSE(env.intersects(DoubleNotANumber, 5.0));
  EXPECT_FALSE(null_env.intersects(5.0, 5.0));

  EXPECT_TRUE(env.intersects(overlapping));
  EXPECT_TRUE(env.intersects(&overlapping));
  EXPECT_TRUE(env.intersects(touching));
  EXPECT_FALSE(env.intersects(disjoint_env));
  EXPECT_FALSE(env.intersects(null_env));
  EXPECT_FALSE(null_env.intersects(env));
  EXPECT_FALSE(null_env.intersects(null_env));

  EXPECT_FALSE(env.disjoint(overlapping));
  EXPECT_FALSE(env.disjoint(&overlapping));
  EXPECT_TRUE(env.disjoint(disjoint_env));
  EXPECT_TRUE(env.disjoint(&disjoint_env));
  EXPECT_TRUE(env.disjoint(null_env));
  EXPECT_TRUE(null_env.disjoint(env));
}

TEST(EnvelopeTest, CoversAndContains) {
  Envelope outer(0.0, 10.0, 0.0, 10.0);
  Envelope inner(2.0, 8.0, 3.0, 7.0);
  Envelope same(0.0, 10.0, 0.0, 10.0);
  Envelope null_env;

  EXPECT_TRUE(outer.covers(inner));
  EXPECT_TRUE(outer.covers(&inner));
  EXPECT_TRUE(outer.covers(same));
  EXPECT_TRUE(outer.contains(inner));
  EXPECT_TRUE(outer.contains(&inner));
  EXPECT_TRUE(outer.contains(same));

  // Fails on each of the 4 boundaries individually.
  EXPECT_FALSE(outer.covers(Envelope(-0.1, 8.0, 2.0, 8.0)));
  EXPECT_FALSE(outer.covers(Envelope(2.0, 10.1, 2.0, 8.0)));
  EXPECT_FALSE(outer.covers(Envelope(2.0, 8.0, -0.1, 8.0)));
  EXPECT_FALSE(outer.covers(Envelope(2.0, 8.0, 2.0, 10.1)));

  // Null semantics: NaN comparisons return false.
  EXPECT_FALSE(outer.covers(null_env));
  EXPECT_FALSE(null_env.covers(outer));
  EXPECT_FALSE(null_env.covers(null_env));

  CoordinateXY inside(5.0, 5.0);
  CoordinateXY edge(10.0, 5.0);
  CoordinateXY outside(10.1, 5.0);
  EXPECT_TRUE(outer.covers(5.0, 5.0));
  EXPECT_TRUE(outer.covers(&inside));
  EXPECT_TRUE(outer.covers(&edge));
  EXPECT_FALSE(outer.covers(&outside));
  EXPECT_TRUE(outer.contains(inside));
  EXPECT_TRUE(outer.contains(10.0, 10.0));
  EXPECT_FALSE(outer.contains(10.1, 5.0));
  EXPECT_FALSE(null_env.covers(5.0, 5.0));
}

TEST(EnvelopeTest, EqualsAndEqualityOperators) {
  Envelope a(1.0, 3.0, 2.0, 4.0);
  Envelope b(1.0, 3.0, 2.0, 4.0);
  Envelope null_a;
  Envelope null_b;

  EXPECT_TRUE(a.equals(&b));
  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a != b);

  EXPECT_TRUE(null_a.equals(&null_b));
  EXPECT_TRUE(null_a == null_b);
  EXPECT_FALSE(null_a != null_b);

  EXPECT_FALSE(null_a.equals(&a));
  EXPECT_FALSE(a.equals(&null_a));
  EXPECT_TRUE(null_a != a);
  EXPECT_TRUE(a != null_a);

  Envelope diff_minx(0.0, 3.0, 2.0, 4.0);
  Envelope diff_maxx(1.0, 4.0, 2.0, 4.0);
  Envelope diff_miny(1.0, 3.0, 1.0, 4.0);
  Envelope diff_maxy(1.0, 3.0, 2.0, 5.0);
  EXPECT_FALSE(a.equals(&diff_minx));
  EXPECT_FALSE(a.equals(&diff_maxx));
  EXPECT_FALSE(a.equals(&diff_miny));
  EXPECT_FALSE(a.equals(&diff_maxy));
}

TEST(EnvelopeTest, Centre) {
  Envelope null_env;
  CoordinateXY c(99.0, 99.0);
  EXPECT_FALSE(null_env.centre(c));
  EXPECT_DOUBLE_EQ(c.x, 99.0);
  EXPECT_DOUBLE_EQ(c.y, 99.0);

  Envelope env(0.0, 10.0, 2.0, 6.0);
  EXPECT_TRUE(env.centre(c));
  EXPECT_DOUBLE_EQ(c.x, 5.0);
  EXPECT_DOUBLE_EQ(c.y, 4.0);

  Envelope neg(-4.0, 2.0, -6.0, -2.0);
  EXPECT_TRUE(neg.centre(c));
  EXPECT_DOUBLE_EQ(c.x, -1.0);
  EXPECT_DOUBLE_EQ(c.y, -4.0);

  Envelope point(CoordinateXY(3.5, -7.25));
  EXPECT_TRUE(point.centre(c));
  EXPECT_DOUBLE_EQ(c.x, 3.5);
  EXPECT_DOUBLE_EQ(c.y, -7.25);
}

TEST(EnvelopeTest, IntersectionByOutputParamAndReturnValue) {
  Envelope a(0.0, 10.0, 0.0, 10.0);
  Envelope b(4.0, 15.0, -2.0, 6.0);
  Envelope disjoint_env(20.0, 30.0, 0.0, 10.0);
  Envelope null_env;

  // Null and disjoint cases leave output parameter untouched and return null.
  Envelope result(99.0, 100.0, 99.0, 100.0);
  EXPECT_FALSE(null_env.intersection(a, result));
  EXPECT_FALSE(a.intersection(null_env, result));
  EXPECT_FALSE(a.intersection(disjoint_env, result));
  EXPECT_EQ(result, Envelope(99.0, 100.0, 99.0, 100.0));

  EXPECT_TRUE(null_env.intersection(a).isNull());
  EXPECT_TRUE(a.intersection(null_env).isNull());
  EXPECT_TRUE(a.intersection(disjoint_env).isNull());

  // Overlapping with mixed min/max dominance.
  EXPECT_TRUE(a.intersection(b, result));
  EXPECT_EQ(result, Envelope(4.0, 10.0, 0.0, 6.0));
  EXPECT_EQ(a.intersection(b), Envelope(4.0, 10.0, 0.0, 6.0));

  EXPECT_TRUE(b.intersection(a, result));
  EXPECT_EQ(result, Envelope(4.0, 10.0, 0.0, 6.0));
  EXPECT_EQ(b.intersection(a), Envelope(4.0, 10.0, 0.0, 6.0));

  // Edge and corner touching.
  Envelope edge_touch(10.0, 12.0, 2.0, 5.0);
  EXPECT_TRUE(a.intersection(edge_touch, result));
  EXPECT_EQ(result, Envelope(10.0, 10.0, 2.0, 5.0));
  EXPECT_EQ(a.intersection(edge_touch), Envelope(10.0, 10.0, 2.0, 5.0));

  Envelope corner_touch(10.0, 12.0, 10.0, 12.0);
  EXPECT_TRUE(a.intersection(corner_touch, result));
  EXPECT_EQ(result, Envelope(10.0, 10.0, 10.0, 10.0));
}

TEST(EnvelopeTest, Translate) {
  Envelope null_env;
  null_env.translate(5.0, -3.0);
  EXPECT_TRUE(null_env.isNull());

  Envelope env(1.0, 4.0, 2.0, 6.0);
  env.translate(3.0, -5.0);
  EXPECT_EQ(env, Envelope(4.0, 7.0, -3.0, 1.0));
  EXPECT_DOUBLE_EQ(env.getWidth(), 3.0);
  EXPECT_DOUBLE_EQ(env.getHeight(), 4.0);

  env.translate(0.0, 0.0);
  EXPECT_EQ(env, Envelope(4.0, 7.0, -3.0, 1.0));
}

TEST(EnvelopeTest, ExpandBy) {
  Envelope null_env;
  null_env.expandBy(5.0, 5.0);
  EXPECT_TRUE(null_env.isNull());

  Envelope env(1.0, 5.0, 2.0, 4.0);
  env.expandBy(2.0, 3.0);
  EXPECT_EQ(env, Envelope(-1.0, 7.0, -1.0, 7.0));

  env.expandBy(-1.0);
  EXPECT_EQ(env, Envelope(0.0, 6.0, 0.0, 6.0));

  // Shrinking exactly to half-width/height collapses to a point (not null).
  env.expandBy(-3.0, -3.0);
  EXPECT_FALSE(env.isNull());
  EXPECT_EQ(env, Envelope(3.0, 3.0, 3.0, 3.0));

  // Over-shrinking in X sets to null.
  Envelope x_vanish(0.0, 4.0, 0.0, 4.0);
  x_vanish.expandBy(-2.5, 0.0);
  EXPECT_TRUE(x_vanish.isNull());

  // Over-shrinking in Y sets to null.
  Envelope y_vanish(0.0, 4.0, 0.0, 4.0);
  y_vanish.expandBy(0.0, -2.5);
  EXPECT_TRUE(y_vanish.isNull());
}

TEST(EnvelopeTest, ExpandToInclude) {
  Envelope env;
  env.expandToInclude(CoordinateXY(2.0, 3.0));
  EXPECT_EQ(env, Envelope(2.0, 2.0, 3.0, 3.0));

  // Expand all 4 sides via points and verify an interior point is a no-op.
  env.expandToInclude(-1.0, 0.0);
  env.expandToInclude(5.0, 7.0);
  env.expandToInclude(2.0, 4.0);
  EXPECT_EQ(env, Envelope(-1.0, 5.0, 0.0, 7.0));

  // Expanding a null envelope to include a non-null envelope copies it.
  Envelope from_null;
  from_null.expandToInclude(env);
  EXPECT_EQ(from_null, env);

  // Expanding with a null or wholly-contained envelope leaves env unchanged.
  Envelope null_env;
  env.expandToInclude(&null_env);
  env.expandToInclude(Envelope(0.0, 4.0, 1.0, 6.0));
  EXPECT_EQ(env, Envelope(-1.0, 5.0, 0.0, 7.0));

  // Expand all 4 sides via another Envelope.
  env.expandToInclude(Envelope(-3.0, 8.0, -2.0, 10.0));
  EXPECT_EQ(env, Envelope(-3.0, 8.0, -2.0, 10.0));
}

TEST(EnvelopeTest, LexicographicLessThanAndSorting) {
  Envelope null_a;
  Envelope null_b;
  Envelope e(1.0, 5.0, 2.0, 6.0);

  EXPECT_FALSE(null_a < null_b);
  EXPECT_TRUE(null_a < e);
  EXPECT_FALSE(e < null_a);
  EXPECT_FALSE(e < e);

  // Compare at each ordinate position (minx, miny, maxx, maxy).
  EXPECT_TRUE(Envelope(0.0, 5.0, 2.0, 6.0) < e);
  EXPECT_FALSE(Envelope(2.0, 5.0, 2.0, 6.0) < e);

  EXPECT_TRUE(Envelope(1.0, 5.0, 1.0, 6.0) < e);
  EXPECT_FALSE(Envelope(1.0, 5.0, 3.0, 6.0) < e);

  EXPECT_TRUE(Envelope(1.0, 4.0, 2.0, 6.0) < e);
  EXPECT_FALSE(Envelope(1.0, 6.0, 2.0, 6.0) < e);

  EXPECT_TRUE(Envelope(1.0, 5.0, 2.0, 5.0) < e);
  EXPECT_FALSE(Envelope(1.0, 5.0, 2.0, 7.0) < e);

  std::vector<Envelope> envs = {
      Envelope(1.0, 5.0, 2.0, 7.0),
      Envelope(1.0, 5.0, 2.0, 6.0),
      Envelope(),
      Envelope(1.0, 4.0, 2.0, 6.0),
      Envelope(0.0, 9.0, 9.0, 9.0),
      Envelope(1.0, 5.0, 1.0, 6.0),
  };
  std::sort(envs.begin(), envs.end());
  EXPECT_TRUE(envs[0].isNull());
  EXPECT_EQ(envs[1], Envelope(0.0, 9.0, 9.0, 9.0));
  EXPECT_EQ(envs[2], Envelope(1.0, 5.0, 1.0, 6.0));
  EXPECT_EQ(envs[3], Envelope(1.0, 4.0, 2.0, 6.0));
  EXPECT_EQ(envs[4], Envelope(1.0, 5.0, 2.0, 6.0));
  EXPECT_EQ(envs[5], Envelope(1.0, 5.0, 2.0, 7.0));
}

TEST(EnvelopeTest, DistancesAndDistanceToCoordinate) {
  Envelope a(0.0, 2.0, 0.0, 2.0);
  Envelope b(5.0, 7.0, 6.0, 8.0);
  Envelope overlapping(1.0, 4.0, 1.0, 4.0);

  EXPECT_DOUBLE_EQ(a.distanceSquared(overlapping), 0.0);
  EXPECT_DOUBLE_EQ(a.distance(overlapping), 0.0);
  EXPECT_DOUBLE_EQ(a.distanceSquared(b), 25.0);
  EXPECT_DOUBLE_EQ(a.distance(b), 5.0);
  EXPECT_DOUBLE_EQ(b.distance(a), 5.0);

  Envelope c(0.0, 3.0, 0.0, 4.0);
  EXPECT_DOUBLE_EQ(c.maxDistance(c), 5.0);
  EXPECT_DOUBLE_EQ(a.maxDistance(b), std::hypot(7.0, 8.0));

  CoordinateXY p0(0.0, 0.0);
  CoordinateXY p1(10.0, 10.0);
  EXPECT_DOUBLE_EQ(
      Envelope::distanceSquaredToCoordinate(CoordinateXY(5.0, 5.0), p0, p1),
      0.0);
  EXPECT_DOUBLE_EQ(
      Envelope::distanceToCoordinate(CoordinateXY(10.0, 10.0), p0, p1), 0.0);
  EXPECT_DOUBLE_EQ(
      Envelope::distanceSquaredToCoordinate(CoordinateXY(13.0, 14.0), p1, p0),
      25.0);
  EXPECT_DOUBLE_EQ(
      Envelope::distanceToCoordinate(CoordinateXY(-3.0, -4.0), p0, p1), 5.0);
}

TEST(EnvelopeTest, HashCodeAndUnorderedSet) {
  Envelope e1(1.0, 2.0, 3.0, 4.0);
  Envelope e2(1.0, 2.0, 3.0, 4.0);
  Envelope e3(1.0, 2.0, 3.0, 5.0);

  Envelope::HashCode hasher;
  EXPECT_EQ(e1.hashCode(), e2.hashCode());
  EXPECT_EQ(hasher(e1), e1.hashCode());

  std::unordered_set<Envelope, Envelope::HashCode> set;
  set.insert(e1);
  set.insert(e2);
  set.insert(e3);
  EXPECT_EQ(set.size(), 2u);
  EXPECT_TRUE(set.contains(e1));
  EXPECT_TRUE(set.contains(e3));
}

}  // namespace
}  // namespace geos_google
