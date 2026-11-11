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

#include "third_party/geos/include/geos/shape/fractal/MortonCode.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::geom::Coordinate;
using geos::shape::fractal::MortonCode;
using geos::util::IllegalArgumentException;

TEST(MortonCodeTest, MaxLevelConstant) { EXPECT_EQ(MortonCode::MAX_LEVEL, 16); }

TEST(MortonCodeTest, LevelSizePowersOfFourAndBoundsCheck) {
  for (uint32_t lvl = 0; lvl <= 15; ++lvl) {
    EXPECT_EQ(MortonCode::levelSize(lvl), 1u << (2 * lvl)) << "level=" << lvl;
  }

  EXPECT_THROW(MortonCode::levelSize(17), IllegalArgumentException);
  EXPECT_THROW(MortonCode::levelSize(100), IllegalArgumentException);
  EXPECT_THROW(MortonCode::levelSize(std::numeric_limits<uint32_t>::max()),
               IllegalArgumentException);
}

TEST(MortonCodeTest, MaxOrdinatePowersOfTwoMinusOneAndBoundsCheck) {
  for (uint32_t lvl = 0; lvl <= 16; ++lvl) {
    EXPECT_EQ(MortonCode::maxOrdinate(lvl), (1u << lvl) - 1u)
        << "level=" << lvl;
  }

  EXPECT_THROW(MortonCode::maxOrdinate(17), IllegalArgumentException);
  EXPECT_THROW(MortonCode::maxOrdinate(100), IllegalArgumentException);
  EXPECT_THROW(MortonCode::maxOrdinate(std::numeric_limits<uint32_t>::max()),
               IllegalArgumentException);
}

TEST(MortonCodeTest, LevelForNumPointsExactAndIntermediate) {
  // Exact powers of 4 (sz == numPoints branch).
  EXPECT_EQ(MortonCode::level(1), 0u);
  EXPECT_EQ(MortonCode::level(4), 1u);
  EXPECT_EQ(MortonCode::level(16), 2u);
  EXPECT_EQ(MortonCode::level(64), 3u);
  EXPECT_EQ(MortonCode::level(256), 4u);
  EXPECT_EQ(MortonCode::level(65536), 8u);
  EXPECT_EQ(MortonCode::level(1u << 30), 15u);

  // Intermediate values (sz < numPoints branch).
  EXPECT_EQ(MortonCode::level(2), 1u);
  EXPECT_EQ(MortonCode::level(3), 1u);
  EXPECT_EQ(MortonCode::level(5), 2u);
  EXPECT_EQ(MortonCode::level(15), 2u);
  EXPECT_EQ(MortonCode::level(17), 3u);
  EXPECT_EQ(MortonCode::level(63), 3u);
  EXPECT_EQ(MortonCode::level(65), 4u);
  EXPECT_EQ(MortonCode::level(257), 5u);
  EXPECT_EQ(MortonCode::level(65535), 8u);
  EXPECT_EQ(MortonCode::level(65537), 9u);
  EXPECT_EQ(MortonCode::level((1u << 30) + 1u), 16u);
}

TEST(MortonCodeTest, Level1AndLevel2ExactZOrder) {
  const std::array<std::pair<int, int>, 16> expected = {{
      {0, 0},
      {1, 0},
      {0, 1},
      {1, 1},
      {2, 0},
      {3, 0},
      {2, 1},
      {3, 1},
      {0, 2},
      {1, 2},
      {0, 3},
      {1, 3},
      {2, 2},
      {3, 2},
      {2, 3},
      {3, 3},
  }};

  for (uint32_t i = 0; i < expected.size(); ++i) {
    const Coordinate c = MortonCode::decode(i);
    EXPECT_EQ(c.x, static_cast<double>(expected[i].first)) << "index=" << i;
    EXPECT_EQ(c.y, static_cast<double>(expected[i].second)) << "index=" << i;
    EXPECT_TRUE(std::isnan(c.z)) << "index=" << i;
    EXPECT_EQ(MortonCode::encode(expected[i].first, expected[i].second), i)
        << "index=" << i;
  }
}

TEST(MortonCodeTest, ExhaustiveBijectionLevel4) {
  constexpr uint32_t kSide = 16;
  constexpr uint32_t kTotal = kSide * kSide;
  std::vector<bool> visited(kTotal, false);

  for (uint32_t i = 0; i < kTotal; ++i) {
    const Coordinate c = MortonCode::decode(i);
    ASSERT_GE(c.x, 0.0);
    ASSERT_GE(c.y, 0.0);
    ASSERT_LT(c.x, static_cast<double>(kSide));
    ASSERT_LT(c.y, static_cast<double>(kSide));
    EXPECT_TRUE(std::isnan(c.z));

    const uint32_t x = static_cast<uint32_t>(c.x);
    const uint32_t y = static_cast<uint32_t>(c.y);
    EXPECT_EQ(c.x, static_cast<double>(x));
    EXPECT_EQ(c.y, static_cast<double>(y));

    const std::size_t cell = static_cast<std::size_t>(y) * kSide + x;
    EXPECT_FALSE(visited[cell]) << "index=" << i;
    visited[cell] = true;

    EXPECT_EQ(MortonCode::encode(static_cast<int>(x), static_cast<int>(y)), i);
  }

  for (std::size_t idx = 0; idx < visited.size(); ++idx) {
    EXPECT_TRUE(visited[idx]) << "unvisited cell=" << idx;
  }
}

TEST(MortonCodeTest, SingleBitPowersOfTwoInterleaving) {
  for (uint32_t bit = 0; bit < 16; ++bit) {
    const int ord = 1 << bit;

    const uint32_t x_code = MortonCode::encode(ord, 0);
    EXPECT_EQ(x_code, 1u << (2 * bit)) << "bit=" << bit;
    const Coordinate cx = MortonCode::decode(x_code);
    EXPECT_EQ(cx.x, static_cast<double>(ord));
    EXPECT_EQ(cx.y, 0.0);
    EXPECT_TRUE(std::isnan(cx.z));

    const uint32_t y_code = MortonCode::encode(0, ord);
    EXPECT_EQ(y_code, 1u << (2 * bit + 1)) << "bit=" << bit;
    const Coordinate cy = MortonCode::decode(y_code);
    EXPECT_EQ(cy.x, 0.0);
    EXPECT_EQ(cy.y, static_cast<double>(ord));
    EXPECT_TRUE(std::isnan(cy.z));

    const uint32_t xy_code = MortonCode::encode(ord, ord);
    EXPECT_EQ(xy_code, 3u << (2 * bit)) << "bit=" << bit;
    const Coordinate cxy = MortonCode::decode(xy_code);
    EXPECT_EQ(cxy.x, static_cast<double>(ord));
    EXPECT_EQ(cxy.y, static_cast<double>(ord));
    EXPECT_TRUE(std::isnan(cxy.z));
  }
}

TEST(MortonCodeTest, Extremes16BitAndMasking) {
  EXPECT_EQ(MortonCode::encode(65535, 0), 0x55555555u);
  Coordinate cx = MortonCode::decode(0x55555555u);
  EXPECT_EQ(cx.x, 65535.0);
  EXPECT_EQ(cx.y, 0.0);
  EXPECT_TRUE(std::isnan(cx.z));

  EXPECT_EQ(MortonCode::encode(0, 65535), 0xAAAAAAAAu);
  Coordinate cy = MortonCode::decode(0xAAAAAAAAu);
  EXPECT_EQ(cy.x, 0.0);
  EXPECT_EQ(cy.y, 65535.0);
  EXPECT_TRUE(std::isnan(cy.z));

  EXPECT_EQ(MortonCode::encode(65535, 65535), 0xFFFFFFFFu);
  Coordinate cmax = MortonCode::decode(0xFFFFFFFFu);
  EXPECT_EQ(cmax.x, 65535.0);
  EXPECT_EQ(cmax.y, 65535.0);
  EXPECT_TRUE(std::isnan(cmax.z));

  const uint32_t arbitrary_code = MortonCode::encode(0x1234, 0xABCD);
  const Coordinate carb = MortonCode::decode(arbitrary_code);
  EXPECT_EQ(carb.x, static_cast<double>(0x1234));
  EXPECT_EQ(carb.y, static_cast<double>(0xABCD));
  EXPECT_TRUE(std::isnan(carb.z));

  // Only the low 16 bits of each ordinate are interleaved.
  EXPECT_EQ(MortonCode::encode(0x10005, 0x20003), MortonCode::encode(5, 3));
  EXPECT_EQ(MortonCode::encode(-1, -1), 0xFFFFFFFFu);
}

}  // namespace
}  // namespace geos_google
