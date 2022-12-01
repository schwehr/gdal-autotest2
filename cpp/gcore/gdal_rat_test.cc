// Copyright 2018 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests gcore/gdal_rat.cpp.
//
// RAT == Raster Attribute Table

#include <memory>

#include "gmock.h"
#include "gunit.h"
#include "third_party/absl/memory/memory.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "gcore/gdal_rat.h"
#include "port/cpl_error.h"

namespace {

TEST(GdalRatTest, DefaultInstance) {
  auto rat = std::make_unique<GDALDefaultRasterAttributeTable>();
  EXPECT_EQ(0, rat->GetColumnCount());
  EXPECT_EQ(0, rat->GetRowCount());
  EXPECT_FALSE(rat->ChangesAreWrittenToFile());
}

TEST(GdalRatTest, QueryNonExistentEntries) {
  auto rat = std::make_unique<GDALDefaultRasterAttributeTable>();

  CPLErrorReset();
  EXPECT_STREQ("", rat->GetValueAsString(0, 0));
  EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(1, CPLGetErrorCounter());
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("out of range"));

  CPLErrorReset();
  EXPECT_STREQ("", rat->GetValueAsString(-1, -2));
  EXPECT_EQ(1, CPLGetErrorCounter());
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("out of range"));
}

TEST(GdalRatTest, IntTableWriteTooFarForward) {
  auto rat = std::make_unique<GDALDefaultRasterAttributeTable>();
  rat->CreateColumn("value", GFT_Integer, GFU_Generic);

  // Try to set a row beyond the start.
  CPLErrorReset();
  rat->SetValue(10, 0, 999);
  EXPECT_EQ(0, rat->GetRowCount());
  EXPECT_EQ(1, CPLGetErrorCounter());
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("iRow (10)"));
}

TEST(GdalRatTest, StringTable) {
  auto rat = std::make_unique<GDALDefaultRasterAttributeTable>();
  rat->CreateColumn("value", GFT_String, GFU_Generic);
  EXPECT_EQ(1, rat->GetColumnCount());
  EXPECT_EQ(GFU_Generic, rat->GetUsageOfCol(0));

  EXPECT_EQ(0, rat->GetRowCount());

  constexpr int row = 0;
  constexpr int col = 0;
  // Read before write.
  EXPECT_STREQ("", rat->GetValueAsString(row, col));
  rat->SetValue(row, col, "a string");
  EXPECT_STREQ("a string", rat->GetValueAsString(row, col));

  rat->SetValue(row, col, "2 3");
  EXPECT_EQ(1, rat->GetRowCount());
  EXPECT_STREQ("2 3", rat->GetValueAsString(row, col));
  EXPECT_EQ(2, rat->GetValueAsInt(row, col));
  EXPECT_DOUBLE_EQ(2, rat->GetValueAsDouble(row, col));
}

TEST(GdalRatTest, IntTable) {
  auto rat = std::make_unique<GDALDefaultRasterAttributeTable>();
  rat->CreateColumn("value", GFT_Integer, GFU_Generic);
  rat->CreateColumn("alpha", GFT_Integer, GFU_AlphaMax);
  EXPECT_EQ(2, rat->GetColumnCount());
  EXPECT_EQ(GFU_Generic, rat->GetUsageOfCol(0));
  EXPECT_EQ(GFU_AlphaMax, rat->GetUsageOfCol(1));

  EXPECT_EQ(0, rat->GetRowCount());

  constexpr int row = 0;
  // Read before write.
  EXPECT_EQ(0, rat->GetValueAsInt(row, 0));
  rat->SetValue(row, 0, 123);
  EXPECT_EQ(123, rat->GetValueAsInt(row, 0));

  constexpr int col = 1;
  rat->SetValue(row, 1, -456);
  EXPECT_EQ(1, rat->GetRowCount());
  EXPECT_STREQ("-456", rat->GetValueAsString(row, col));
  EXPECT_EQ(-456, rat->GetValueAsInt(row, col));
  EXPECT_DOUBLE_EQ(-456.0, rat->GetValueAsDouble(row, col));
}

TEST(GdalRatTest, DoubleTable) {
  auto rat = std::make_unique<GDALDefaultRasterAttributeTable>();
  rat->CreateColumn("value", GFT_Real, GFU_Generic);
  EXPECT_EQ(1, rat->GetColumnCount());

  constexpr int row = 0;
  constexpr int col = 0;
  // Read before write.
  EXPECT_EQ(0, rat->GetValueAsDouble(row, col));
  rat->SetValue(row, col, 12.3);
  EXPECT_DOUBLE_EQ(12.3, rat->GetValueAsDouble(row, col));

  rat->SetValue(1, col, -4.5);
  EXPECT_EQ(2, rat->GetRowCount());
  EXPECT_STREQ("-4.5", rat->GetValueAsString(1, col));
  EXPECT_EQ(-4, rat->GetValueAsInt(1, col));
  EXPECT_DOUBLE_EQ(-4.5, rat->GetValueAsDouble(1, col));
}

// TODO(schwehr): Flush out testing the rest of the RAT functionality.

}  // namespace
