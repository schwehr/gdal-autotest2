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

#include "third_party/geos/include/geos/geom/PrecisionModel.h"

#include <cmath>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/geos/include/geos/geom/Coordinate.h"

namespace geos_google {
namespace {

using ::geos::geom::Coordinate;
using ::geos::geom::CoordinateXY;
using ::geos::geom::PrecisionModel;

TEST(PrecisionModelTest, MaximumPreciseValueConstant) {
  EXPECT_EQ(PrecisionModel::maximumPreciseValue, 9007199254740992.0);
  EXPECT_EQ(PrecisionModel::maximumPreciseValue,
            static_cast<double>(1ULL << 53));
}

TEST(PrecisionModelTest, DefaultConstructorIsFloating) {
  PrecisionModel pm;
  EXPECT_EQ(pm.getType(), PrecisionModel::FLOATING);
  EXPECT_TRUE(pm.isFloating());
  EXPECT_EQ(pm.getScale(), 0.0);
  EXPECT_TRUE(std::isnan(pm.getGridSize()));
  EXPECT_EQ(pm.getOffsetX(), 0.0);
  EXPECT_EQ(pm.getOffsetY(), 0.0);
  EXPECT_EQ(pm.getMaximumSignificantDigits(), 16);
  EXPECT_EQ(pm.toString(), "Floating");
}

TEST(PrecisionModelTest, TypeConstructors) {
  PrecisionModel floating(PrecisionModel::FLOATING);
  EXPECT_EQ(floating.getType(), PrecisionModel::FLOATING);
  EXPECT_TRUE(floating.isFloating());
  EXPECT_EQ(floating.getScale(), 1.0);
  EXPECT_TRUE(std::isnan(floating.getGridSize()));
  EXPECT_EQ(floating.getOffsetX(), 0.0);
  EXPECT_EQ(floating.getOffsetY(), 0.0);
  EXPECT_EQ(floating.getMaximumSignificantDigits(), 16);
  EXPECT_EQ(floating.toString(), "Floating");

  PrecisionModel floating_single(PrecisionModel::FLOATING_SINGLE);
  EXPECT_EQ(floating_single.getType(), PrecisionModel::FLOATING_SINGLE);
  EXPECT_TRUE(floating_single.isFloating());
  EXPECT_EQ(floating_single.getScale(), 1.0);
  EXPECT_TRUE(std::isnan(floating_single.getGridSize()));
  EXPECT_EQ(floating_single.getOffsetX(), 0.0);
  EXPECT_EQ(floating_single.getOffsetY(), 0.0);
  EXPECT_EQ(floating_single.getMaximumSignificantDigits(), 6);
  EXPECT_EQ(floating_single.toString(), "Floating-Single");

  PrecisionModel fixed(PrecisionModel::FIXED);
  EXPECT_EQ(fixed.getType(), PrecisionModel::FIXED);
  EXPECT_FALSE(fixed.isFloating());
  EXPECT_EQ(fixed.getScale(), 1.0);
  EXPECT_EQ(fixed.getGridSize(), 1.0);
  EXPECT_EQ(fixed.getOffsetX(), 0.0);
  EXPECT_EQ(fixed.getOffsetY(), 0.0);
  EXPECT_EQ(fixed.getMaximumSignificantDigits(), 0);
  EXPECT_EQ(fixed.toString(), "Fixed (Scale=1 OffsetX=0 OffsetY=0)");
}

TEST(PrecisionModelTest, ScaleConstructorPositiveScales) {
  PrecisionModel unit_scale(1.0);
  EXPECT_EQ(unit_scale.getType(), PrecisionModel::FIXED);
  EXPECT_FALSE(unit_scale.isFloating());
  EXPECT_EQ(unit_scale.getScale(), 1.0);
  EXPECT_EQ(unit_scale.getGridSize(), 1.0);
  EXPECT_EQ(unit_scale.getMaximumSignificantDigits(), 0);
  EXPECT_EQ(unit_scale.toString(), "Fixed (Scale=1 OffsetX=0 OffsetY=0)");

  PrecisionModel scale_10(10.0);
  EXPECT_EQ(scale_10.getScale(), 10.0);
  EXPECT_EQ(scale_10.getGridSize(), 0.1);
  EXPECT_EQ(scale_10.getMaximumSignificantDigits(), 1);
  EXPECT_EQ(scale_10.toString(), "Fixed (Scale=10 OffsetX=0 OffsetY=0)");

  PrecisionModel scale_500(500.0);
  EXPECT_EQ(scale_500.getScale(), 500.0);
  EXPECT_DOUBLE_EQ(scale_500.getGridSize(), 0.002);
  EXPECT_EQ(scale_500.getMaximumSignificantDigits(), 3);
  EXPECT_EQ(scale_500.toString(), "Fixed (Scale=500 OffsetX=0 OffsetY=0)");

  PrecisionModel scale_fractional_gt1(2.5);
  EXPECT_EQ(scale_fractional_gt1.getScale(), 2.5);
  EXPECT_DOUBLE_EQ(scale_fractional_gt1.getGridSize(), 0.4);
  EXPECT_EQ(scale_fractional_gt1.getMaximumSignificantDigits(), 1);

  PrecisionModel scale_lt1(0.05);
  EXPECT_EQ(scale_lt1.getScale(), 0.05);
  EXPECT_EQ(scale_lt1.getGridSize(), 20.0);
  EXPECT_EQ(scale_lt1.getMaximumSignificantDigits(), -2);
}

TEST(PrecisionModelTest, ScaleConstructorSnapsNearIntegralValues) {
  // Scales >= 1 within 1e-5 of an integer snap to that exact integer.
  PrecisionModel snapped_scale(10.0 + 1e-7);
  EXPECT_EQ(snapped_scale.getScale(), 10.0);
  EXPECT_EQ(snapped_scale.getGridSize(), 1.0 / 10.0);

  // Scales >= 1 outside 1e-5 tolerance are preserved.
  const double non_snapped_val = 10.0 + 1e-4;
  PrecisionModel non_snapped_scale(non_snapped_val);
  EXPECT_DOUBLE_EQ(non_snapped_scale.getScale(), non_snapped_val);
  EXPECT_DOUBLE_EQ(non_snapped_scale.getGridSize(), 1.0 / non_snapped_val);

  // Scales < 1 snap gridSize = 1 / scale if within 1e-5 of an integer.
  PrecisionModel snapped_grid(0.1 + 1e-9);
  EXPECT_DOUBLE_EQ(snapped_grid.getScale(), 0.1 + 1e-9);
  EXPECT_EQ(snapped_grid.getGridSize(), 10.0);

  // Scales < 1 whose reciprocal is not near an integer keep exact reciprocal.
  PrecisionModel non_snapped_grid(0.3);
  EXPECT_EQ(non_snapped_grid.getScale(), 0.3);
  EXPECT_DOUBLE_EQ(non_snapped_grid.getGridSize(), 1.0 / 0.3);
}

TEST(PrecisionModelTest, ScaleConstructorNegativeSpecifiesGridSize) {
  PrecisionModel grid_10(-10.0);
  EXPECT_EQ(grid_10.getType(), PrecisionModel::FIXED);
  EXPECT_FALSE(grid_10.isFloating());
  EXPECT_EQ(grid_10.getScale(), 0.1);
  EXPECT_EQ(grid_10.getGridSize(), 10.0);
  EXPECT_EQ(grid_10.toString(), "Fixed (Scale=0.1 OffsetX=0 OffsetY=0)");

  PrecisionModel grid_20(-20.0);
  EXPECT_EQ(grid_20.getScale(), 0.05);
  EXPECT_EQ(grid_20.getGridSize(), 20.0);
  EXPECT_EQ(grid_20.getMaximumSignificantDigits(), -2);

  PrecisionModel grid_half(-0.5);
  EXPECT_EQ(grid_half.getScale(), 2.0);
  EXPECT_EQ(grid_half.getGridSize(), 0.5);
  EXPECT_EQ(grid_half.getMaximumSignificantDigits(), 1);
  EXPECT_EQ(grid_half.toString(), "Fixed (Scale=2 OffsetX=0 OffsetY=0)");

  PrecisionModel grid_2_5(-2.5);
  EXPECT_DOUBLE_EQ(grid_2_5.getScale(), 0.4);
  EXPECT_DOUBLE_EQ(grid_2_5.getGridSize(), 2.5);
  EXPECT_EQ(grid_2_5.getMaximumSignificantDigits(), -1);
}

TEST(PrecisionModelTest, DeprecatedScaleAndOffsetsConstructorIgnoresOffsets) {
  PrecisionModel pm(100.0, 12.5, -34.25);
  EXPECT_EQ(pm.getType(), PrecisionModel::FIXED);
  EXPECT_FALSE(pm.isFloating());
  EXPECT_EQ(pm.getScale(), 100.0);
  EXPECT_EQ(pm.getGridSize(), 0.01);
  EXPECT_EQ(pm.getOffsetX(), 0.0);
  EXPECT_EQ(pm.getOffsetY(), 0.0);
  EXPECT_EQ(pm.toString(), "Fixed (Scale=100 OffsetX=0 OffsetY=0)");

  PrecisionModel negative_scale(-5.0, 10.0, 20.0);
  EXPECT_EQ(negative_scale.getScale(), 0.2);
  EXPECT_EQ(negative_scale.getGridSize(), 5.0);
  EXPECT_EQ(negative_scale.getOffsetX(), 0.0);
  EXPECT_EQ(negative_scale.getOffsetY(), 0.0);
}

TEST(PrecisionModelTest, MakePreciseDoubleFloating) {
  PrecisionModel pm(PrecisionModel::FLOATING);
  const double high_precision = 1.2345678901234567;
  const double neg_precision = -9876.543210987654;
  EXPECT_EQ(pm.makePrecise(high_precision), high_precision);
  EXPECT_EQ(pm.makePrecise(neg_precision), neg_precision);
  EXPECT_EQ(pm.makePrecise(0.0), 0.0);
}

TEST(PrecisionModelTest, MakePreciseDoubleFloatingSingle) {
  PrecisionModel pm(PrecisionModel::FLOATING_SINGLE);
  const double val = 1.2345678901234567;
  const double expected = static_cast<double>(static_cast<float>(val));
  EXPECT_EQ(pm.makePrecise(val), expected);
  EXPECT_NE(pm.makePrecise(val), val);

  const double large_val = 123456789.0;
  EXPECT_EQ(pm.makePrecise(large_val),
            static_cast<double>(static_cast<float>(large_val)));
  EXPECT_NE(pm.makePrecise(large_val), large_val);
}

TEST(PrecisionModelTest, MakePreciseDoubleFixedScaleAtLeastOne) {
  PrecisionModel unit_pm(PrecisionModel::FIXED);
  EXPECT_EQ(unit_pm.makePrecise(1.4), 1.0);
  EXPECT_EQ(unit_pm.makePrecise(1.6), 2.0);
  EXPECT_EQ(unit_pm.makePrecise(1.5), 2.0);
  EXPECT_EQ(unit_pm.makePrecise(2.5), 3.0);
  EXPECT_EQ(unit_pm.makePrecise(-1.4), -1.0);
  EXPECT_EQ(unit_pm.makePrecise(-1.6), -2.0);
  EXPECT_EQ(unit_pm.makePrecise(-1.5), -1.0);
  EXPECT_EQ(unit_pm.makePrecise(-2.5), -2.0);

  PrecisionModel scale_4(4.0);
  EXPECT_EQ(scale_4.makePrecise(1.1), 1.0);
  EXPECT_EQ(scale_4.makePrecise(1.125), 1.25);
  EXPECT_EQ(scale_4.makePrecise(1.375), 1.5);
  EXPECT_EQ(scale_4.makePrecise(-1.125), -1.0);

  PrecisionModel scale_100(100.0);
  EXPECT_DOUBLE_EQ(scale_100.makePrecise(1.234), 1.23);
  EXPECT_DOUBLE_EQ(scale_100.makePrecise(1.236), 1.24);
  EXPECT_DOUBLE_EQ(scale_100.makePrecise(-1.236), -1.24);
}

TEST(PrecisionModelTest, MakePreciseDoubleFixedGridSizeGreaterThanOne) {
  PrecisionModel grid_10(0.1);
  EXPECT_EQ(grid_10.makePrecise(14.0), 10.0);
  EXPECT_EQ(grid_10.makePrecise(16.0), 20.0);
  EXPECT_EQ(grid_10.makePrecise(15.0), 20.0);
  EXPECT_EQ(grid_10.makePrecise(25.0), 30.0);
  EXPECT_EQ(grid_10.makePrecise(-14.0), -10.0);
  EXPECT_EQ(grid_10.makePrecise(-16.0), -20.0);
  EXPECT_EQ(grid_10.makePrecise(-15.0), -10.0);

  PrecisionModel grid_neg_100(-100.0);
  EXPECT_EQ(grid_neg_100.makePrecise(149.0), 100.0);
  EXPECT_EQ(grid_neg_100.makePrecise(151.0), 200.0);
  EXPECT_EQ(grid_neg_100.makePrecise(-249.0), -200.0);

  PrecisionModel grid_2(-2.0);
  EXPECT_EQ(grid_2.makePrecise(3.0), 4.0);
  EXPECT_EQ(grid_2.makePrecise(5.0), 6.0);
  EXPECT_EQ(grid_2.makePrecise(-3.0), -2.0);
}

TEST(PrecisionModelTest, MakePreciseCoordinateOverloads) {
  PrecisionModel floating(PrecisionModel::FLOATING);
  CoordinateXY float_coord(1.2345678901234567, -2.3456789012345678);
  floating.makePrecise(float_coord);
  EXPECT_EQ(float_coord.x, 1.2345678901234567);
  EXPECT_EQ(float_coord.y, -2.3456789012345678);

  PrecisionModel single(PrecisionModel::FLOATING_SINGLE);
  CoordinateXY single_coord(1.2345678901234567, -2.3456789012345678);
  single.makePrecise(&single_coord);
  EXPECT_EQ(single_coord.x,
            static_cast<double>(static_cast<float>(1.2345678901234567)));
  EXPECT_EQ(single_coord.y,
            static_cast<double>(static_cast<float>(-2.3456789012345678)));

  PrecisionModel fixed(4.0);
  CoordinateXY ref_coord(1.125, -2.6);
  fixed.makePrecise(ref_coord);
  EXPECT_EQ(ref_coord.x, 1.25);
  EXPECT_EQ(ref_coord.y, -2.5);

  Coordinate ptr_coord(3.375, 4.9, 9.12345);
  fixed.makePrecise(&ptr_coord);
  EXPECT_EQ(ptr_coord.x, 3.5);
  EXPECT_EQ(ptr_coord.y, 5.0);
  // makePrecise only rounds x and y; z remains unchanged.
  EXPECT_EQ(ptr_coord.z, 9.12345);

  Coordinate no_z_coord(3.375, 4.9);
  fixed.makePrecise(no_z_coord);
  EXPECT_EQ(no_z_coord.x, 3.5);
  EXPECT_EQ(no_z_coord.y, 5.0);
  EXPECT_TRUE(std::isnan(no_z_coord.z));
}

TEST(PrecisionModelTest, CompareToOrdersByMaximumSignificantDigits) {
  PrecisionModel floating_default;
  PrecisionModel floating_explicit(PrecisionModel::FLOATING);
  PrecisionModel floating_single(PrecisionModel::FLOATING_SINGLE);
  PrecisionModel fixed_50(50.0);      // 2 significant digits
  PrecisionModel fixed_50_alt(50.0);  // 2 significant digits
  PrecisionModel fixed_1(1.0);        // 0 significant digits
  PrecisionModel fixed_005(0.05);     // -2 significant digits

  EXPECT_EQ(floating_default.compareTo(&floating_explicit), 0);
  EXPECT_EQ(floating_default.compareTo(&floating_single), 1);
  EXPECT_EQ(floating_single.compareTo(&floating_default), -1);

  EXPECT_EQ(floating_single.compareTo(&fixed_50), 1);
  EXPECT_EQ(fixed_50.compareTo(&floating_single), -1);

  EXPECT_EQ(fixed_50.compareTo(&fixed_50_alt), 0);
  EXPECT_EQ(fixed_50.compareTo(&fixed_1), 1);
  EXPECT_EQ(fixed_1.compareTo(&fixed_50), -1);

  EXPECT_EQ(fixed_1.compareTo(&fixed_005), 1);
  EXPECT_EQ(fixed_005.compareTo(&fixed_1), -1);
}

}  // namespace
}  // namespace geos_google
