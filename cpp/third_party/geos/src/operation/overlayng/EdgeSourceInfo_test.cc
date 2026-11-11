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

#include "third_party/geos/include/geos/operation/overlayng/EdgeSourceInfo.h"

#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Dimension.h"

namespace geos_google {
namespace {

using geos::geom::Dimension;
using geos::operation::overlayng::EdgeSourceInfo;

TEST(EdgeSourceInfoTest, LineConstructorSetsLinearDefaults) {
  const EdgeSourceInfo info0(uint8_t{0});
  EXPECT_EQ(info0.getIndex(), 0);
  EXPECT_EQ(info0.getDimension(), static_cast<int>(Dimension::L));
  EXPECT_EQ(info0.getDepthDelta(), 0);
  EXPECT_FALSE(info0.isHole());

  const EdgeSourceInfo info1(uint8_t{1});
  EXPECT_EQ(info1.getIndex(), 1);
  EXPECT_EQ(info1.getDimension(), static_cast<int>(Dimension::L));
  EXPECT_EQ(info1.getDepthDelta(), 0);
  EXPECT_FALSE(info1.isHole());
}

TEST(EdgeSourceInfoTest, AreaConstructorShellProperties) {
  const EdgeSourceInfo shell_pos(uint8_t{0}, 1, false);
  EXPECT_EQ(shell_pos.getIndex(), 0);
  EXPECT_EQ(shell_pos.getDimension(), static_cast<int>(Dimension::A));
  EXPECT_EQ(shell_pos.getDepthDelta(), 1);
  EXPECT_FALSE(shell_pos.isHole());

  const EdgeSourceInfo shell_neg(uint8_t{1}, -1, false);
  EXPECT_EQ(shell_neg.getIndex(), 1);
  EXPECT_EQ(shell_neg.getDimension(), static_cast<int>(Dimension::A));
  EXPECT_EQ(shell_neg.getDepthDelta(), -1);
  EXPECT_FALSE(shell_neg.isHole());
}

TEST(EdgeSourceInfoTest, AreaConstructorHoleProperties) {
  const EdgeSourceInfo hole_pos(uint8_t{0}, 1, true);
  EXPECT_EQ(hole_pos.getIndex(), 0);
  EXPECT_EQ(hole_pos.getDimension(), static_cast<int>(Dimension::A));
  EXPECT_EQ(hole_pos.getDepthDelta(), 1);
  EXPECT_TRUE(hole_pos.isHole());

  const EdgeSourceInfo hole_neg(uint8_t{1}, -1, true);
  EXPECT_EQ(hole_neg.getIndex(), 1);
  EXPECT_EQ(hole_neg.getDimension(), static_cast<int>(Dimension::A));
  EXPECT_EQ(hole_neg.getDepthDelta(), -1);
  EXPECT_TRUE(hole_neg.isHole());
}

TEST(EdgeSourceInfoTest, AreaConstructorPreservesZeroAndMultiDepthDelta) {
  const EdgeSourceInfo zero_delta(uint8_t{0}, 0, false);
  EXPECT_EQ(zero_delta.getIndex(), 0);
  EXPECT_EQ(zero_delta.getDimension(), static_cast<int>(Dimension::A));
  EXPECT_EQ(zero_delta.getDepthDelta(), 0);
  EXPECT_FALSE(zero_delta.isHole());

  const EdgeSourceInfo multi_delta(uint8_t{1}, -2, true);
  EXPECT_EQ(multi_delta.getIndex(), 1);
  EXPECT_EQ(multi_delta.getDimension(), static_cast<int>(Dimension::A));
  EXPECT_EQ(multi_delta.getDepthDelta(), -2);
  EXPECT_TRUE(multi_delta.isHole());
}

}  // namespace
}  // namespace geos_google
