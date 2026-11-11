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

#include "third_party/geos/include/geos/algorithm/RobustDeterminant.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/util/IllegalArgumentException.h"

namespace geos_google {
namespace {

using geos::algorithm::RobustDeterminant;
using geos::util::IllegalArgumentException;

struct DetCase {
  double x1;
  double y1;
  double x2;
  double y2;
  int expected;
};

TEST(RobustDeterminantTest, ThrowsOnNonFiniteInputs) {
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInf = std::numeric_limits<double>::infinity();
  constexpr double kNegInf = -std::numeric_limits<double>::infinity();

  for (const double bad : {kNan, kInf, kNegInf}) {
    EXPECT_THROW(RobustDeterminant::signOfDet2x2(bad, 1.0, 2.0, 3.0),
                 IllegalArgumentException);
    EXPECT_THROW(RobustDeterminant::signOfDet2x2(1.0, bad, 2.0, 3.0),
                 IllegalArgumentException);
    EXPECT_THROW(RobustDeterminant::signOfDet2x2(1.0, 2.0, bad, 3.0),
                 IllegalArgumentException);
    EXPECT_THROW(RobustDeterminant::signOfDet2x2(1.0, 2.0, 3.0, bad),
                 IllegalArgumentException);
  }
}

TEST(RobustDeterminantTest, ZeroEntryShortcuts) {
  const std::vector<DetCase> cases = {
      // Both (x1 == 0 || y2 == 0) and (y1 == 0 || x2 == 0) -> 0.
      {0.0, 0.0, 0.0, 0.0, 0},
      {-0.0, -0.0, -0.0, -0.0, 0},
      {0.0, 0.0, 5.0, 7.0, 0},
      {0.0, 5.0, 0.0, 7.0, 0},
      {5.0, 0.0, 7.0, 0.0, 0},
      {5.0, 7.0, 0.0, 0.0, 0},
      {-0.0, 3.0, -0.0, 4.0, 0},

      // x1 == 0 or y2 == 0, with y1 != 0 and x2 != 0 -> sign(-y1 * x2).
      {0.0, 2.0, 3.0, 4.0, -1},
      {5.0, 2.0, 3.0, 0.0, -1},
      {0.0, 2.0, -3.0, 4.0, 1},
      {-5.0, 2.0, -3.0, -0.0, 1},
      {0.0, -2.0, 3.0, 4.0, 1},
      {5.0, -2.0, 3.0, 0.0, 1},
      {0.0, -2.0, -3.0, 4.0, -1},
      {-5.0, -2.0, -3.0, 0.0, -1},

      // y1 == 0 or x2 == 0, with x1 != 0 and y2 != 0 -> sign(x1 * y2).
      {2.0, 0.0, 3.0, 4.0, 1},
      {2.0, 3.0, 0.0, 4.0, 1},
      {-2.0, -0.0, 3.0, 4.0, -1},
      {-2.0, 3.0, -0.0, 4.0, -1},
      {2.0, 0.0, 3.0, -4.0, -1},
      {2.0, 3.0, 0.0, -4.0, -1},
      {-2.0, 0.0, 3.0, -4.0, 1},
      {-2.0, -3.0, 0.0, -4.0, 1},
  };

  for (std::size_t i = 0; i < cases.size(); ++i) {
    SCOPED_TRACE(i);
    const DetCase& c = cases[i];
    EXPECT_EQ(RobustDeterminant::signOfDet2x2(c.x1, c.y1, c.x2, c.y2),
              c.expected);
  }
}

TEST(RobustDeterminantTest, YNormalizationAndPermutationBranches) {
  const std::vector<DetCase> cases = {
      // y1 > 0, y2 > 0, y1 <= y2.
      {1.0, 2.0, 3.0, 5.0, -1},
      {1.0, 3.0, 2.0, 3.0, -1},
      {2.0, 3.0, 1.0, 3.0, 1},
      {2.0, 3.0, 2.0, 3.0, 0},

      // y1 > 0, y2 > 0, y1 > y2.
      {1.0, 5.0, 2.0, 2.0, -1},
      {2.0, 5.0, 1.0, 3.0, 1},

      // y1 > 0, y2 < 0, y1 <= -y2.
      {1.0, 2.0, -3.0, -5.0, 1},
      {1.0, 4.0, -3.0, -4.0, 1},
      {3.0, 2.0, -1.0, -5.0, -1},

      // y1 > 0, y2 < 0, y1 > -y2.
      {2.0, 5.0, -1.0, -3.0, -1},
      {1.0, 5.0, -2.0, -3.0, 1},
      {-2.0, 5.0, -1.0, -3.0, 1},

      // y1 < 0, y2 > 0, -y1 <= y2.
      {-1.0, -2.0, 3.0, 5.0, 1},
      {1.0, -2.0, -3.0, 5.0, -1},
      {-1.0, -3.0, 2.0, 3.0, 1},

      // y1 < 0, y2 > 0, -y1 > y2.
      {-3.0, -5.0, 1.0, 2.0, -1},
      {-2.0, -5.0, 2.0, 3.0, 1},

      // y1 < 0, y2 < 0, y1 >= y2 (-y1 <= -y2).
      {-1.0, -2.0, -3.0, -5.0, -1},
      {-2.0, -3.0, -1.0, -3.0, 1},

      // y1 < 0, y2 < 0, y1 < y2 (-y1 > -y2).
      {-3.0, -5.0, -1.0, -2.0, 1},
      {-1.0, -5.0, -3.0, -2.0, -1},
  };

  for (std::size_t i = 0; i < cases.size(); ++i) {
    SCOPED_TRACE(i);
    const DetCase& c = cases[i];
    EXPECT_EQ(RobustDeterminant::signOfDet2x2(c.x1, c.y1, c.x2, c.y2),
              c.expected);
  }
}

TEST(RobustDeterminantTest, XNormalizationBranches) {
  const std::vector<DetCase> cases = {
      // x1 > 0, x2 > 0, x1 <= x2.
      {2.0, 1.0, 2.0, 3.0, 1},
      {2.0, 1.0, 5.0, 3.0, 1},

      // x1 > 0, x2 > 0, x1 > x2 (early return).
      {5.0, 1.0, 2.0, 3.0, 1},
      {2.0, 3.0, 5.0, 1.0, -1},

      // x1 > 0, x2 < 0 (early return).
      {2.0, 1.0, -3.0, 4.0, 1},

      // x1 < 0, x2 > 0 (early return).
      {-2.0, 1.0, 3.0, 4.0, -1},

      // x1 < 0, x2 < 0, x1 >= x2 (-x1 <= -x2).
      {-2.0, 1.0, -2.0, 3.0, -1},
      {-2.0, 1.0, -5.0, 3.0, -1},
      {-2.0, 2.0, -5.0, 3.0, 1},

      // x1 < 0, x2 < 0, x1 < x2 (-x1 > -x2, early return).
      {-5.0, 1.0, -2.0, 3.0, -1},
  };

  for (std::size_t i = 0; i < cases.size(); ++i) {
    SCOPED_TRACE(i);
    const DetCase& c = cases[i];
    EXPECT_EQ(RobustDeterminant::signOfDet2x2(c.x1, c.y1, c.x2, c.y2),
              c.expected);
  }
}

TEST(RobustDeterminantTest, LoopFirstHalfBranches) {
  const std::vector<DetCase> cases = {
      // y2 < 0 after subtracting k * y1.
      {2.0, 3.0, 5.0, 4.0, -1},

      // y2 > y1 after subtracting k * y1.
      {2.0, 1.0, 3.0, 5.0, 1},

      // x1 > 2 * x2 and y1 < 2 * y2.
      {10.0, 10.0, 13.0, 16.0, 1},

      // x1 <= 2 * x2 and y1 > 2 * y2.
      {10.0, 10.0, 16.0, 13.0, -1},

      // x1 <= 2 * x2 and y1 <= 2 * y2 with y2 == y1 -> flipped y2 == 0, x2 !=
      // 0.
      {10.0, 3.0, 16.0, 6.0, 1},

      // x1 <= 2 * x2 and y1 <= 2 * y2, continuing to second half.
      {10.0, 10.0, 16.0, 17.0, 1},

      // x1 > 2 * x2, y1 >= 2 * y2, y2 == 0 and x2 == 0 -> 0.
      {2.0, 3.0, 4.0, 6.0, 0},

      // x1 > 2 * x2, y1 >= 2 * y2, y2 == 0 and x2 != 0.
      {5.0, 2.0, 6.0, 2.0, -1},

      // x1 > 2 * x2, y1 >= 2 * y2, y2 != 0 and x2 == 0.
      {5.0, 4.0, 5.0, 5.0, 1},
      {5.0, 4.0, 5.0, 6.0, 1},
  };

  for (std::size_t i = 0; i < cases.size(); ++i) {
    SCOPED_TRACE(i);
    const DetCase& c = cases[i];
    EXPECT_EQ(RobustDeterminant::signOfDet2x2(c.x1, c.y1, c.x2, c.y2),
              c.expected);
  }
}

TEST(RobustDeterminantTest, LoopSecondHalfAndMultiIterationBranches) {
  const std::vector<DetCase> cases = {
      // Second half: y1 < 0 after subtracting k * y2.
      {10.0, 4.0, 13.0, 6.0, 1},

      // Second half: y1 > y2 after subtracting k * y2.
      {7.0, 10.0, 10.0, 12.0, -1},

      // Second half: x2 > 2 * x1 and y2 < 2 * y1.
      {33.0, 36.0, 43.0, 46.0, -1},

      // Second half: x2 <= 2 * x1 and y2 > 2 * y1.
      {26.0, 23.0, 36.0, 33.0, 1},

      // Second half: x2 <= 2 * x1 and y2 <= 2 * y1 with y1 == y2 -> y1 == 0.
      {26.0, 12.0, 36.0, 16.0, -1},

      // Second half: x2 <= 2 * x1 and y2 <= 2 * y1, looping back to start.
      {26.0, 27.0, 36.0, 37.0, -1},

      // Second half: x2 > 2 * x1, y2 >= 2 * y1, y1 == 0 and x1 == 0 -> 0.
      {6.0, 9.0, 8.0, 12.0, 0},

      // Second half: x2 > 2 * x1, y2 >= 2 * y1, y1 == 0 and x1 != 0.
      {16.0, 6.0, 21.0, 8.0, 1},

      // Second half: x2 > 2 * x1, y2 >= 2 * y1, y1 != 0 and x1 == 0.
      {15.0, 13.0, 20.0, 17.0, -1},

      // Second half: x2 > 2 * x1, y2 >= 2 * y1, both nonzero -> next iteration.
      {16.0, 17.0, 21.0, 22.0, -1},
  };

  for (std::size_t i = 0; i < cases.size(); ++i) {
    SCOPED_TRACE(i);
    const DetCase& c = cases[i];
    EXPECT_EQ(RobustDeterminant::signOfDet2x2(c.x1, c.y1, c.x2, c.y2),
              c.expected);
  }
}

TEST(RobustDeterminantTest, FibonacciCassiniIdentityMultiIteration) {
  std::int64_t f_prev = 1;
  std::int64_t f_curr = 1;
  for (int n = 2; n <= 45; ++n) {
    const std::int64_t f_next = f_prev + f_curr;
    const int expected = (n % 2 == 0) ? -1 : 1;
    SCOPED_TRACE(n);
    EXPECT_EQ(RobustDeterminant::signOfDet2x2(
                  static_cast<double>(f_curr), static_cast<double>(f_prev),
                  static_cast<double>(f_next), static_cast<double>(f_curr)),
              expected);
    f_prev = f_curr;
    f_curr = f_next;
  }
}

TEST(RobustDeterminantTest, PrecisionNearCollinearOverflowAndSubnormal) {
  // Naive (1e8 + 1)*(1e8 - 1) - 1e16 rounds to 0 in IEEE 754 double.
  EXPECT_EQ(RobustDeterminant::signOfDet2x2(1e8 + 1.0, 1e8, 1e8, 1e8 - 1.0),
            -1);

  // Exact integers up to 1e15 < 2^50 where naive product overflows 53-bit
  // significand.
  EXPECT_EQ(
      RobustDeterminant::signOfDet2x2(1e15 + 1.0, 1e15, 1e15 + 2.0, 1e15 + 1.0),
      1);
  EXPECT_EQ(
      RobustDeterminant::signOfDet2x2(1e15 + 3.0, 1e15 + 1.0, 1e15 + 2.0, 1e15),
      -1);
  EXPECT_EQ(RobustDeterminant::signOfDet2x2(
                1e15 + 1.0, 1e15 + 3.0, 2.0 * (1e15 + 1.0), 2.0 * (1e15 + 3.0)),
            0);

  // 1-ULP perturbations near 1.0 whose exact determinant is +/- 2^-104.
  const double eps = std::ldexp(1.0, -52);
  EXPECT_EQ(RobustDeterminant::signOfDet2x2(1.0, 1.0 + eps, 1.0 + eps,
                                            1.0 + 2.0 * eps),
            -1);
  EXPECT_EQ(RobustDeterminant::signOfDet2x2(1.0, 1.0 + eps, 1.0 + 2.0 * eps,
                                            1.0 + 3.0 * eps),
            -1);
  EXPECT_EQ(RobustDeterminant::signOfDet2x2(1.0 + eps, 1.0, 1.0 + 2.0 * eps,
                                            1.0 + eps),
            1);
  EXPECT_EQ(
      RobustDeterminant::signOfDet2x2(1.0 + eps, 1.0, 2.0 + 2.0 * eps, 2.0), 0);
  EXPECT_EQ(
      RobustDeterminant::signOfDet2x2(1.0, 1.0, 1.0, std::nextafter(1.0, 2.0)),
      1);
  EXPECT_EQ(
      RobustDeterminant::signOfDet2x2(1.0, 1.0, 1.0, std::nextafter(1.0, 0.0)),
      -1);

  // Large entries where naive x1 * y2 overflows to infinity.
  EXPECT_EQ(RobustDeterminant::signOfDet2x2(1e200, 2e200, 3e200, 7e200), 1);
  EXPECT_EQ(RobustDeterminant::signOfDet2x2(1e200, 2e200, 3e200, 5e200), -1);
  EXPECT_EQ(RobustDeterminant::signOfDet2x2(1e200, 2e200, 3e200, 6e200), 0);

  // Subnormal entries where naive x1 * y2 underflows to zero.
  constexpr double d = std::numeric_limits<double>::denorm_min();
  EXPECT_EQ(RobustDeterminant::signOfDet2x2(d, 2.0 * d, 3.0 * d, 7.0 * d), 1);
  EXPECT_EQ(RobustDeterminant::signOfDet2x2(d, 2.0 * d, 3.0 * d, 5.0 * d), -1);
  EXPECT_EQ(RobustDeterminant::signOfDet2x2(d, 2.0 * d, 3.0 * d, 6.0 * d), 0);

  constexpr double m = std::numeric_limits<double>::min();
  EXPECT_EQ(RobustDeterminant::signOfDet2x2(m, m, m * 1.5, m), -1);
}

TEST(RobustDeterminantTest, AlgebraicSymmetriesAndExactScaling) {
  const std::vector<DetCase> cases = {
      {0.0, 0.0, 1.0, 2.0, 0},        {0.0, 3.0, -4.0, 5.0, 1},
      {3.0, 0.0, 4.0, -5.0, -1},      {1.0, 2.0, 3.0, 4.0, -1},
      {3.0, 7.0, 5.0, 12.0, 1},       {6.0, 9.0, 8.0, 12.0, 0},
      {-16.0, 17.0, 21.0, -22.0, -1}, {26.0, -27.0, -36.0, 37.0, -1},
  };

  const double scales[] = {
      std::ldexp(1.0, -100),
      0.5,
      4.0,
      std::ldexp(1.0, 100),
  };

  for (std::size_t i = 0; i < cases.size(); ++i) {
    SCOPED_TRACE(i);
    const DetCase& c = cases[i];
    const int d = RobustDeterminant::signOfDet2x2(c.x1, c.y1, c.x2, c.y2);
    ASSERT_EQ(d, c.expected);

    // Antisymmetry under row swap, column swap, and single-row negation.
    EXPECT_EQ(RobustDeterminant::signOfDet2x2(c.x2, c.y2, c.x1, c.y1), -d);
    EXPECT_EQ(RobustDeterminant::signOfDet2x2(c.y1, c.x1, c.y2, c.x2), -d);
    EXPECT_EQ(RobustDeterminant::signOfDet2x2(-c.x1, -c.y1, c.x2, c.y2), -d);
    EXPECT_EQ(RobustDeterminant::signOfDet2x2(c.x1, c.y1, -c.x2, -c.y2), -d);

    // Invariance under transpose and negating both rows.
    EXPECT_EQ(RobustDeterminant::signOfDet2x2(c.x1, c.x2, c.y1, c.y2), d);
    EXPECT_EQ(RobustDeterminant::signOfDet2x2(-c.x1, -c.y1, -c.x2, -c.y2), d);

    // Positive power-of-two scaling of each row preserves determinant sign.
    for (const double s : scales) {
      EXPECT_EQ(RobustDeterminant::signOfDet2x2(s * c.x1, s * c.y1, c.x2, c.y2),
                d);
      EXPECT_EQ(RobustDeterminant::signOfDet2x2(c.x1, c.y1, s * c.x2, s * c.y2),
                d);
    }
  }
}

}  // namespace
}  // namespace geos_google
