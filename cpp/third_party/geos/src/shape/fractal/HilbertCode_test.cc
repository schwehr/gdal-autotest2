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

#include "third_party/geos/include/geos/shape/fractal/HilbertCode.h"

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
using geos::shape::fractal::HilbertCode;
using geos::util::IllegalArgumentException;

TEST(HilbertCodeTest, MaxLevelConstant) {
  EXPECT_EQ(HilbertCode::MAX_LEVEL, 16);
}

TEST(HilbertCodeTest, LevelSizePowersOfFour) {
  for (uint32_t lvl = 0; lvl <= 15; ++lvl) {
    EXPECT_EQ(HilbertCode::levelSize(lvl), 1u << (2 * lvl)) << "level=" << lvl;
  }
}

TEST(HilbertCodeTest, MaxOrdinatePowersOfTwoMinusOne) {
  for (uint32_t lvl = 0; lvl <= 16; ++lvl) {
    EXPECT_EQ(HilbertCode::maxOrdinate(lvl), (1u << lvl) - 1u)
        << "level=" << lvl;
  }
}

TEST(HilbertCodeTest, LevelForNumPointsExactAndIntermediate) {
  // Exact powers of 4 (size == numPoints branch).
  EXPECT_EQ(HilbertCode::level(1), 0u);
  EXPECT_EQ(HilbertCode::level(4), 1u);
  EXPECT_EQ(HilbertCode::level(16), 2u);
  EXPECT_EQ(HilbertCode::level(64), 3u);
  EXPECT_EQ(HilbertCode::level(256), 4u);
  EXPECT_EQ(HilbertCode::level(65536), 8u);
  EXPECT_EQ(HilbertCode::level(1u << 30), 15u);

  // Intermediate values (size < numPoints branch).
  EXPECT_EQ(HilbertCode::level(2), 1u);
  EXPECT_EQ(HilbertCode::level(3), 1u);
  EXPECT_EQ(HilbertCode::level(5), 2u);
  EXPECT_EQ(HilbertCode::level(15), 2u);
  EXPECT_EQ(HilbertCode::level(17), 3u);
  EXPECT_EQ(HilbertCode::level(63), 3u);
  EXPECT_EQ(HilbertCode::level(65), 4u);
  EXPECT_EQ(HilbertCode::level(257), 5u);
  EXPECT_EQ(HilbertCode::level(65535), 8u);
  EXPECT_EQ(HilbertCode::level(65537), 9u);
  EXPECT_EQ(HilbertCode::level((1u << 30) + 1u), 16u);
}

TEST(HilbertCodeTest, OutOfRangeLevelThrowsIllegalArgument) {
  EXPECT_THROW(HilbertCode::encode(17, 0, 0), IllegalArgumentException);
  EXPECT_THROW(HilbertCode::encode(100, 1, 1), IllegalArgumentException);
  EXPECT_THROW(HilbertCode::encode(std::numeric_limits<uint32_t>::max(), 0, 0),
               IllegalArgumentException);

  EXPECT_THROW(HilbertCode::decode(17, 0), IllegalArgumentException);
  EXPECT_THROW(HilbertCode::decode(100, 0), IllegalArgumentException);
  EXPECT_THROW(HilbertCode::decode(std::numeric_limits<uint32_t>::max(), 0),
               IllegalArgumentException);
}

TEST(HilbertCodeTest, Level1ExactCurve) {
  const std::array<std::pair<uint32_t, uint32_t>, 4> expected = {{
      {0, 0},
      {0, 1},
      {1, 1},
      {1, 0},
  }};

  for (uint32_t i = 0; i < expected.size(); ++i) {
    const Coordinate c = HilbertCode::decode(1, i);
    EXPECT_EQ(c.x, expected[i].first) << "index=" << i;
    EXPECT_EQ(c.y, expected[i].second) << "index=" << i;
    EXPECT_TRUE(std::isnan(c.z)) << "index=" << i;
    EXPECT_EQ(HilbertCode::encode(1, expected[i].first, expected[i].second), i)
        << "index=" << i;
  }
}

TEST(HilbertCodeTest, Level2ExactCurve) {
  const std::array<std::pair<uint32_t, uint32_t>, 16> expected = {{
      {0, 0},
      {1, 0},
      {1, 1},
      {0, 1},
      {0, 2},
      {0, 3},
      {1, 3},
      {1, 2},
      {2, 2},
      {2, 3},
      {3, 3},
      {3, 2},
      {3, 1},
      {2, 1},
      {2, 0},
      {3, 0},
  }};

  for (uint32_t i = 0; i < expected.size(); ++i) {
    const Coordinate c = HilbertCode::decode(2, i);
    EXPECT_EQ(c.x, expected[i].first) << "index=" << i;
    EXPECT_EQ(c.y, expected[i].second) << "index=" << i;
    EXPECT_TRUE(std::isnan(c.z)) << "index=" << i;
    EXPECT_EQ(HilbertCode::encode(2, expected[i].first, expected[i].second), i)
        << "index=" << i;
  }
}

TEST(HilbertCodeTest, ExhaustiveBijectionAndContinuityLevels1To5) {
  for (uint32_t lvl = 1; lvl <= 5; ++lvl) {
    const uint32_t side = 1u << lvl;
    const uint32_t num_points = HilbertCode::levelSize(lvl);
    std::vector<bool> visited(static_cast<std::size_t>(side) * side, false);

    uint32_t prev_x = 0;
    uint32_t prev_y = 0;
    for (uint32_t i = 0; i < num_points; ++i) {
      const Coordinate c = HilbertCode::decode(lvl, i);
      ASSERT_GE(c.x, 0.0);
      ASSERT_GE(c.y, 0.0);
      ASSERT_LT(c.x, static_cast<double>(side));
      ASSERT_LT(c.y, static_cast<double>(side));
      EXPECT_TRUE(std::isnan(c.z));

      const uint32_t x = static_cast<uint32_t>(c.x);
      const uint32_t y = static_cast<uint32_t>(c.y);
      EXPECT_EQ(c.x, static_cast<double>(x));
      EXPECT_EQ(c.y, static_cast<double>(y));

      const std::size_t cell = static_cast<std::size_t>(y) * side + x;
      EXPECT_FALSE(visited[cell]) << "level=" << lvl << " index=" << i;
      visited[cell] = true;

      EXPECT_EQ(HilbertCode::encode(lvl, x, y), i)
          << "level=" << lvl << " index=" << i;

      if (i == 0) {
        EXPECT_EQ(x, 0u);
        EXPECT_EQ(y, 0u);
      } else {
        const int manhattan =
            std::abs(static_cast<int>(x) - static_cast<int>(prev_x)) +
            std::abs(static_cast<int>(y) - static_cast<int>(prev_y));
        EXPECT_EQ(manhattan, 1)
            << "level=" << lvl << " transition=" << (i - 1) << "->" << i;
      }

      if (i + 1 == num_points) {
        EXPECT_EQ(x, HilbertCode::maxOrdinate(lvl));
        EXPECT_EQ(y, 0u);
      }

      prev_x = x;
      prev_y = y;
    }

    for (std::size_t idx = 0; idx < visited.size(); ++idx) {
      EXPECT_TRUE(visited[idx]) << "level=" << lvl << " unvisited cell=" << idx;
    }
  }
}

TEST(HilbertCodeTest, HighLevelsAndMaxLevelRoundTrip) {
  const std::array<uint32_t, 4> levels = {8u, 12u, 15u, 16u};
  for (uint32_t lvl : levels) {
    const uint32_t max_ord = HilbertCode::maxOrdinate(lvl);
    const uint32_t max_code = (lvl == 16) ? std::numeric_limits<uint32_t>::max()
                                          : (1u << (2 * lvl)) - 1u;

    const Coordinate start = HilbertCode::decode(lvl, 0);
    EXPECT_EQ(start.x, 0.0);
    EXPECT_EQ(start.y, 0.0);
    EXPECT_TRUE(std::isnan(start.z));
    EXPECT_EQ(HilbertCode::encode(lvl, 0, 0), 0u);

    const Coordinate end = HilbertCode::decode(lvl, max_code);
    EXPECT_EQ(end.x, static_cast<double>(max_ord));
    EXPECT_EQ(end.y, 0.0);
    EXPECT_TRUE(std::isnan(end.z));
    EXPECT_EQ(HilbertCode::encode(lvl, max_ord, 0), max_code);

    const std::vector<std::pair<uint32_t, uint32_t>> samples = {
        {0, max_ord},
        {max_ord, max_ord},
        {max_ord / 2, max_ord / 3},
        {1, 0},
        {0, 1},
        {123u & max_ord, 456u & max_ord},
        {0x5555u & max_ord, 0xAAAAu & max_ord},
        {max_ord - 1u, max_ord / 2},
    };
    for (const auto& [x, y] : samples) {
      const uint32_t code = HilbertCode::encode(lvl, x, y);
      const Coordinate decoded = HilbertCode::decode(lvl, code);
      EXPECT_EQ(decoded.x, static_cast<double>(x)) << "level=" << lvl;
      EXPECT_EQ(decoded.y, static_cast<double>(y)) << "level=" << lvl;
      EXPECT_TRUE(std::isnan(decoded.z));
    }
  }
}

TEST(HilbertCodeTest, Level16BitPatternsAndQuadrantContinuity) {
  const std::array<uint32_t, 5> boundary_prev = {
      0x3FFFFFFFu, 0x7FFFFFFFu, 0xBFFFFFFFu, 0x55555555u, 0xFFFFFFFEu,
  };
  for (uint32_t idx : boundary_prev) {
    const Coordinate a = HilbertCode::decode(16, idx);
    const Coordinate b = HilbertCode::decode(16, idx + 1u);
    const int manhattan =
        std::abs(static_cast<int>(a.x) - static_cast<int>(b.x)) +
        std::abs(static_cast<int>(a.y) - static_cast<int>(b.y));
    EXPECT_EQ(manhattan, 1) << "idx=" << idx;
    EXPECT_EQ(HilbertCode::encode(16, static_cast<uint32_t>(a.x),
                                  static_cast<uint32_t>(a.y)),
              idx);
    EXPECT_EQ(HilbertCode::encode(16, static_cast<uint32_t>(b.x),
                                  static_cast<uint32_t>(b.y)),
              idx + 1u);
  }

  const std::array<uint32_t, 4> index_samples = {
      0x12345678u,
      0x89ABCDEFu,
      0xA5A55A5Au,
      0xF0F00F0Fu,
  };
  for (uint32_t idx : index_samples) {
    const Coordinate c = HilbertCode::decode(16, idx);
    EXPECT_GE(c.x, 0.0);
    EXPECT_LE(c.x, 65535.0);
    EXPECT_GE(c.y, 0.0);
    EXPECT_LE(c.y, 65535.0);
    EXPECT_TRUE(std::isnan(c.z));
    EXPECT_EQ(HilbertCode::encode(16, static_cast<uint32_t>(c.x),
                                  static_cast<uint32_t>(c.y)),
              idx);
  }
}

TEST(HilbertCodeTest, CodeDependsOnLevel) {
  EXPECT_EQ(HilbertCode::encode(1, 1, 0), 3u);
  EXPECT_EQ(HilbertCode::encode(2, 1, 0), 1u);
  EXPECT_EQ(HilbertCode::encode(3, 1, 0), 3u);
}

}  // namespace
}  // namespace geos_google
