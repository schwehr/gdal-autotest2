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

#include "third_party/geos/include/geos/index/quadtree/Key.h"

#include <cmath>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/geom/Envelope.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::Envelope;
using ::geos::index::quadtree::Key;

TEST(KeyTest, ComputeQuadLevelUsesLargerExtent) {
  // dx > dy
  EXPECT_EQ(Key::computeQuadLevel(Envelope(0.0, 4.0, 0.0, 1.0)), 3);
  // dy > dx
  EXPECT_EQ(Key::computeQuadLevel(Envelope(0.0, 1.0, 0.0, 4.0)), 3);
  // dx == dy
  EXPECT_EQ(Key::computeQuadLevel(Envelope(0.0, 1.0, 0.0, 1.0)), 1);
  EXPECT_EQ(Key::computeQuadLevel(Envelope(0.0, 0.5, 0.0, 0.5)), 0);
  EXPECT_EQ(Key::computeQuadLevel(Envelope(0.0, 0.25, 0.0, 0.25)), -1);
  EXPECT_EQ(Key::computeQuadLevel(Envelope(0.0, 0.75, 0.0, 0.25)), 0);
  EXPECT_EQ(Key::computeQuadLevel(Envelope(0.0, 1.5, 0.0, 1.5)), 1);
  EXPECT_EQ(Key::computeQuadLevel(Envelope(0.0, 2.0, 0.0, 2.0)), 2);
  EXPECT_EQ(Key::computeQuadLevel(Envelope(0.0, 1024.0, 0.0, 10.0)), 11);

  // Zero-area and null envelopes yield level 0 (frexp(0.0) == 0).
  EXPECT_EQ(Key::computeQuadLevel(Envelope(3.5, 3.5, 7.25, 7.25)), 0);
  EXPECT_EQ(Key::computeQuadLevel(Envelope()), 0);
  // computeQuadLevel does not iterate, so origin-straddling envelopes are safe.
  EXPECT_EQ(Key::computeQuadLevel(Envelope(-1.0, 1.0, -0.5, 0.5)), 2);
}

TEST(KeyTest, ConstructsAlignedKeyWithoutExpandingLevel) {
  const Envelope item(0.25, 1.25, 2.25, 3.25);
  const Key key(item);

  EXPECT_EQ(key.getLevel(), 1);
  EXPECT_EQ(key.getPoint(), Coordinate(0.0, 2.0));
  EXPECT_TRUE(std::isnan(key.getPoint().z));
  EXPECT_EQ(key.getEnvelope(), Envelope(0.0, 2.0, 2.0, 4.0));
  EXPECT_TRUE(key.getEnvelope().contains(item));

  std::unique_ptr<Coordinate> centre(key.getCentre());
  ASSERT_NE(centre, nullptr);
  EXPECT_EQ(*centre, Coordinate(1.0, 3.0));
  EXPECT_TRUE(std::isnan(centre->z));
}

TEST(KeyTest, ExpandsLevelWhenEnvelopeStraddlesGridBoundary) {
  {
    const Envelope item(1.5, 2.5, 1.5, 2.5);
    const Key key(item);
    EXPECT_EQ(key.getLevel(), 2);
    EXPECT_EQ(key.getPoint(), Coordinate(0.0, 0.0));
    EXPECT_EQ(key.getEnvelope(), Envelope(0.0, 4.0, 0.0, 4.0));
    EXPECT_TRUE(key.getEnvelope().contains(item));
    std::unique_ptr<Coordinate> centre(key.getCentre());
    EXPECT_EQ(*centre, Coordinate(2.0, 2.0));
  }

  {
    // Starts at level -1 (quadSize 0.5) and steps up to level 5 (quadSize 32).
    const Envelope item(15.875, 16.125, 0.125, 0.375);
    const Key key(item);
    EXPECT_EQ(key.getLevel(), 5);
    EXPECT_EQ(key.getPoint(), Coordinate(0.0, 0.0));
    EXPECT_EQ(key.getEnvelope(), Envelope(0.0, 32.0, 0.0, 32.0));
    EXPECT_TRUE(key.getEnvelope().contains(item));
    std::unique_ptr<Coordinate> centre(key.getCentre());
    EXPECT_EQ(*centre, Coordinate(16.0, 16.0));
  }

  {
    // Aligned in X, straddles y = 4.0, forcing level up to 3 (quadSize 8).
    const Envelope item(0.125, 0.25, 3.875, 4.125);
    const Key key(item);
    EXPECT_EQ(key.getLevel(), 3);
    EXPECT_EQ(key.getPoint(), Coordinate(0.0, 0.0));
    EXPECT_EQ(key.getEnvelope(), Envelope(0.0, 8.0, 0.0, 8.0));
    EXPECT_TRUE(key.getEnvelope().contains(item));
  }
}

TEST(KeyTest, HandlesPointAndSubUnitEnvelopes) {
  {
    const Envelope pt_env(5.75, 5.75, 2.25, 2.25);
    const Key key(pt_env);
    EXPECT_EQ(key.getLevel(), 0);
    EXPECT_EQ(key.getPoint(), Coordinate(5.0, 2.0));
    EXPECT_EQ(key.getEnvelope(), Envelope(5.0, 6.0, 2.0, 3.0));
    std::unique_ptr<Coordinate> centre(key.getCentre());
    EXPECT_EQ(*centre, Coordinate(5.5, 2.5));
  }

  {
    const Envelope sub_env(0.125, 0.25, 0.375, 0.5);
    const Key key(sub_env);
    EXPECT_EQ(key.getLevel(), -2);
    EXPECT_EQ(key.getPoint(), Coordinate(0.0, 0.25));
    EXPECT_EQ(key.getEnvelope(), Envelope(0.0, 0.25, 0.25, 0.5));
    std::unique_ptr<Coordinate> centre(key.getCentre());
    EXPECT_EQ(*centre, Coordinate(0.125, 0.375));
  }
}

TEST(KeyTest, HandlesNegativeQuadrantsAndOriginBoundaries) {
  {
    const Envelope item(-3.0, -1.5, -5.0, -4.0);
    const Key key(item);
    EXPECT_EQ(key.getLevel(), 2);
    EXPECT_EQ(key.getPoint(), Coordinate(-4.0, -8.0));
    EXPECT_EQ(key.getEnvelope(), Envelope(-4.0, 0.0, -8.0, -4.0));
    EXPECT_TRUE(key.getEnvelope().contains(item));
  }

  {
    const Envelope neg_touch(-1.0, 0.0, -1.0, 0.0);
    const Key key(neg_touch);
    EXPECT_EQ(key.getLevel(), 1);
    EXPECT_EQ(key.getPoint(), Coordinate(-2.0, -2.0));
    EXPECT_EQ(key.getEnvelope(), Envelope(-2.0, 0.0, -2.0, 0.0));
    EXPECT_TRUE(key.getEnvelope().contains(neg_touch));
  }

  {
    const Envelope pos_touch(0.0, 1.0, 0.0, 1.0);
    const Key key(pos_touch);
    EXPECT_EQ(key.getLevel(), 1);
    EXPECT_EQ(key.getPoint(), Coordinate(0.0, 0.0));
    EXPECT_EQ(key.getEnvelope(), Envelope(0.0, 2.0, 0.0, 2.0));
    EXPECT_TRUE(key.getEnvelope().contains(pos_touch));
  }

  {
    const Envelope origin_pt(0.0, 0.0, 0.0, 0.0);
    const Key key(origin_pt);
    EXPECT_EQ(key.getLevel(), 0);
    EXPECT_EQ(key.getPoint(), Coordinate(0.0, 0.0));
    EXPECT_EQ(key.getEnvelope(), Envelope(0.0, 1.0, 0.0, 1.0));
  }
}

TEST(KeyTest, RecomputesKeyForLargerAndSmallerEnvelopes) {
  Key key(Envelope(0.125, 0.25, 0.125, 0.25));
  EXPECT_EQ(key.getLevel(), -2);
  EXPECT_EQ(key.getEnvelope(), Envelope(0.0, 0.25, 0.0, 0.25));

  const Envelope larger(10.0, 12.0, 5.0, 7.0);
  key.computeKey(larger);
  EXPECT_EQ(key.getLevel(), 2);
  EXPECT_EQ(key.getPoint(), Coordinate(8.0, 4.0));
  EXPECT_EQ(key.getEnvelope(), Envelope(8.0, 12.0, 4.0, 8.0));
  EXPECT_TRUE(key.getEnvelope().contains(larger));
  {
    std::unique_ptr<Coordinate> centre(key.getCentre());
    EXPECT_EQ(*centre, Coordinate(10.0, 6.0));
  }

  const Envelope smaller(0.25, 0.296875, 0.25, 0.296875);
  key.computeKey(smaller);
  EXPECT_EQ(key.getLevel(), -4);
  EXPECT_EQ(key.getPoint(), Coordinate(0.25, 0.25));
  EXPECT_EQ(key.getEnvelope(), Envelope(0.25, 0.3125, 0.25, 0.3125));
  EXPECT_TRUE(key.getEnvelope().contains(smaller));
  {
    std::unique_ptr<Coordinate> centre(key.getCentre());
    EXPECT_EQ(*centre, Coordinate(0.28125, 0.28125));
  }
}

TEST(KeyTest, KeyEnvelopeInvariantsAcrossQuadrants) {
  const std::vector<Envelope> samples = {
      Envelope(1.0, 3.0, 2.0, 7.0),
      Envelope(-7.5, -2.25, 1.5, 6.5),
      Envelope(-12.0, -11.5, -20.0, -19.25),
      Envelope(3.25, 3.5, -9.0, -8.5),
  };

  for (const Envelope& item : samples) {
    const Key key(item);
    const double quad_size = std::exp2(key.getLevel());
    const Envelope& env = key.getEnvelope();
    EXPECT_TRUE(env.contains(item));
    EXPECT_DOUBLE_EQ(env.getWidth(), quad_size);
    EXPECT_DOUBLE_EQ(env.getHeight(), quad_size);
    EXPECT_DOUBLE_EQ(env.getMinX(), key.getPoint().x);
    EXPECT_DOUBLE_EQ(env.getMinY(), key.getPoint().y);

    std::unique_ptr<Coordinate> centre(key.getCentre());
    EXPECT_DOUBLE_EQ(centre->x, key.getPoint().x + 0.5 * quad_size);
    EXPECT_DOUBLE_EQ(centre->y, key.getPoint().y + 0.5 * quad_size);
  }
}

}  // namespace
}  // namespace geos_google
