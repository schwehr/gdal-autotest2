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
// Tests gcore/gdalcolortable.cpp.

#include <memory>
#include <vector>

#include "gmock.h"
#include "gunit.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"

namespace {

const std::vector<short> ToVector(const GDALColorEntry &color) {
  return {color.c1, color.c2, color.c3, color.c4};
}

TEST(GdalcolortableTest, TryMostCallsOnADefaultInstance) {
  GDALColorTable color_table;
  EXPECT_EQ(GPI_RGB, color_table.GetPaletteInterpretation());

  {
    auto clone_table = std::unique_ptr<GDALColorTable>(color_table.Clone());
    ASSERT_TRUE(clone_table);

    EXPECT_TRUE(color_table.IsSame(&color_table));
    EXPECT_TRUE(color_table.IsSame(clone_table.get()));
  }

  EXPECT_EQ(0, color_table.GetColorEntryCount());
  EXPECT_EQ(nullptr, color_table.GetColorEntry(0));
  EXPECT_EQ(nullptr, color_table.GetColorEntry(1));
  EXPECT_EQ(nullptr, color_table.GetColorEntry(-1));

  const GDALColorEntry kBlack{0, 0, 0, 255};
  const GDALColorEntry kWhite{255, 255, 255, 255};

  color_table.SetColorEntry(0, &kBlack);
  color_table.SetColorEntry(1, &kWhite);
  EXPECT_EQ(2, color_table.GetColorEntryCount());

  const GDALColorEntry *result_color = color_table.GetColorEntry(0);
  EXPECT_THAT(ToVector(*result_color), testing::ElementsAre(0, 0, 0, 255));

  result_color = color_table.GetColorEntry(1);
  EXPECT_THAT(ToVector(*result_color),
              testing::ElementsAre(255, 255, 255, 255));

  GDALColorEntry color{-1, -1, -1, -1};
  EXPECT_FALSE(color_table.GetColorEntryAsRGB(-1, &color));
  EXPECT_FALSE(color_table.GetColorEntryAsRGB(2, &color));
  EXPECT_THAT(ToVector(color), testing::ElementsAre(-1, -1, -1, -1));

  EXPECT_TRUE(color_table.GetColorEntryAsRGB(0, &color));
  EXPECT_THAT(ToVector(color), testing::ElementsAre(0, 0, 0, 255));

  EXPECT_TRUE(color_table.GetColorEntryAsRGB(1, &color));
  EXPECT_THAT(ToVector(color), testing::ElementsAre(255, 255, 255, 255));

  EXPECT_EQ(5, color_table.CreateColorRamp(2, &kBlack, 4, &kWhite));
  EXPECT_THAT(ToVector(*color_table.GetColorEntry(2)),
              testing::ElementsAre(0, 0, 0, 255));
  EXPECT_THAT(ToVector(*color_table.GetColorEntry(3)),
              testing::ElementsAre(127, 127, 127, 255));
  EXPECT_THAT(ToVector(*color_table.GetColorEntry(4)),
              testing::ElementsAre(255, 255, 255, 255));

  GDALColorTableH handle = GDALColorTable::ToHandle(&color_table);
  ASSERT_NE(nullptr, handle);
  EXPECT_EQ(&color_table, GDALColorTable::FromHandle(handle));
}

TEST(GdalcolortableTest, GraySpecificPaletteBehavior) {
  GDALColorTable color_table(GPI_Gray);
  EXPECT_EQ(GPI_Gray, color_table.GetPaletteInterpretation());

  const GDALColorEntry kBlack{0, 0, 0, 255};

  color_table.SetColorEntry(0, &kBlack);
  EXPECT_EQ(1, color_table.GetColorEntryCount());

  // Does not know how to convert from gray to rgb.
  GDALColorEntry color{-1, -1, -1, -1};
  EXPECT_FALSE(color_table.GetColorEntryAsRGB(0, &color));
}

TEST(GdalcolortableTest, CmykSpecificPaletteBehavior) {
  GDALColorTable color_table(GPI_CMYK);
  EXPECT_EQ(GPI_CMYK, color_table.GetPaletteInterpretation());

  const GDALColorEntry kBlack{0, 0, 0, 255};

  color_table.SetColorEntry(0, &kBlack);
  EXPECT_EQ(1, color_table.GetColorEntryCount());

  // Does not know how to convert from gray to rgb.
  GDALColorEntry color{-1, -1, -1, -1};
  EXPECT_FALSE(color_table.GetColorEntryAsRGB(0, &color));
}

TEST(GdalcolortableTest, HlsSpecificPaletteBehavior) {
  GDALColorTable color_table(GPI_HLS);
  EXPECT_EQ(GPI_HLS, color_table.GetPaletteInterpretation());

  const GDALColorEntry kBlack{0, 0, 0, 0};

  color_table.SetColorEntry(0, &kBlack);
  EXPECT_EQ(1, color_table.GetColorEntryCount());

  // Does not know how to convert from HLS to RGB.
  GDALColorEntry color{-1, -1, -1, -1};
  EXPECT_FALSE(color_table.GetColorEntryAsRGB(0, &color));
}

TEST(GdalcolortableTest, SetColorEntryNegativeIndex) {
  GDALColorTable color_table;

  const GDALColorEntry kBlack{0, 0, 0, 0};

  color_table.SetColorEntry(-1, &kBlack);
  EXPECT_EQ(0, color_table.GetColorEntryCount());
}

TEST(GdalcolortableTest, SetColorEntryLargeIndex) {
  // While some of the functions limit the size of the table, some do not.
  GDALColorTable color_table;

  const GDALColorEntry kBlack{0, 0, 0, 42};

  constexpr int kIndex = 12345;
  color_table.SetColorEntry(kIndex, &kBlack);
  EXPECT_EQ(kIndex + 1, color_table.GetColorEntryCount());

  EXPECT_THAT(ToVector(*color_table.GetColorEntry(kIndex)),
              testing::ElementsAre(0, 0, 0, 42));
}

TEST(GdalcolortableTest, RampWithBadArgs) {
  GDALColorTable color_table;
  EXPECT_EQ(GPI_RGB, color_table.GetPaletteInterpretation());

  const GDALColorEntry kBlack{0, 0, 0, 255};
  const GDALColorEntry kWhite{255, 255, 255, 255};

  // Bad indices.
  EXPECT_EQ(-1, color_table.CreateColorRamp(-1, &kBlack, 4, &kWhite));
  EXPECT_EQ(-1, color_table.CreateColorRamp(0, &kBlack, 256, &kWhite));
  EXPECT_EQ(-1, color_table.CreateColorRamp(99, &kBlack, 0, &kWhite));

  // nullptr for color.
  EXPECT_EQ(-1, color_table.CreateColorRamp(0, nullptr, 10, &kWhite));
  EXPECT_EQ(-1, color_table.CreateColorRamp(0, &kBlack, 10, nullptr));

  EXPECT_EQ(0, color_table.GetColorEntryCount());
}

TEST(GdalcolortableTest, RampsWithOneEntry) {
  GDALColorTable color_table;
  EXPECT_EQ(GPI_RGB, color_table.GetPaletteInterpretation());

  const GDALColorEntry kBlack{0, 0, 0, 255};
  const GDALColorEntry kWhite{255, 255, 255, 255};

  EXPECT_EQ(1, color_table.CreateColorRamp(0, &kBlack, 0, &kWhite));
  EXPECT_EQ(3, color_table.CreateColorRamp(2, &kBlack, 2, &kWhite));
  EXPECT_EQ(3, color_table.CreateColorRamp(1, &kBlack, 1, &kWhite));

  EXPECT_EQ(3, color_table.GetColorEntryCount());
}

TEST(GdalcolortableTest, OneEntryAtPosition255) {
  GDALColorTable color_table;
  EXPECT_EQ(GPI_RGB, color_table.GetPaletteInterpretation());

  const GDALColorEntry kWhite{255, 255, 255, 255};
  color_table.SetColorEntry(255, &kWhite);
  EXPECT_EQ(256, color_table.GetColorEntryCount());

  EXPECT_THAT(ToVector(*color_table.GetColorEntry(0)),
              testing::ElementsAre(0, 0, 0, 0));
  EXPECT_THAT(ToVector(*color_table.GetColorEntry(254)),
              testing::ElementsAre(0, 0, 0, 0));
  EXPECT_THAT(ToVector(*color_table.GetColorEntry(255)),
              testing::ElementsAre(255, 255, 255, 255));

  EXPECT_EQ(256, color_table.GetColorEntryCount());
}

TEST(GdalcolortableTest, NegativeColors) {
  GDALColorTable color_table;
  EXPECT_EQ(GPI_RGB, color_table.GetPaletteInterpretation());

  const GDALColorEntry kColor{-1, -2, -3, -32768};
  color_table.SetColorEntry(0, &kColor);

  EXPECT_THAT(ToVector(*color_table.GetColorEntry(0)),
              testing::ElementsAre(-1, -2, -3, -32768));
}

TEST(GdalcolortableTest, IsSamePaletteInterpretation) {
  const GDALColorTable cmyk(GPI_CMYK);
  const GDALColorTable hls(GPI_HLS);
  // Sad that it doesn't check the palette type.
  EXPECT_TRUE(cmyk.IsSame(&hls));
}

TEST(GdalcolortableTest, IsSameOneEntry) {
  GDALColorTable a;
  const GDALColorEntry kColorA{1, 2, 3, 4};
  a.SetColorEntry(0, &kColorA);

  GDALColorTable b;
  b.SetColorEntry(0, &kColorA);
  EXPECT_TRUE(a.IsSame(&b));

  const GDALColorEntry kColorB1{0, 2, 3, 4};
  b.SetColorEntry(0, &kColorB1);
  EXPECT_FALSE(a.IsSame(&b));

  const GDALColorEntry kColorB2{1, 0, 3, 4};
  b.SetColorEntry(0, &kColorB2);
  EXPECT_FALSE(a.IsSame(&b));

  const GDALColorEntry kColorB3{1, 2, 0, 4};
  b.SetColorEntry(0, &kColorB3);
  EXPECT_FALSE(a.IsSame(&b));

  const GDALColorEntry kColorB4{1, 2, 3, 0};
  b.SetColorEntry(0, &kColorB4);
  EXPECT_FALSE(a.IsSame(&b));

  b.SetColorEntry(0, &kColorA);
  EXPECT_TRUE(a.IsSame(&b));
}

TEST(GdalcolortableTest, CApi) {
  GDALColorTableH table = GDALCreateColorTable(GPI_RGB);
  ASSERT_NE(nullptr, table);
  EXPECT_EQ(GPI_RGB, GDALGetPaletteInterpretation(table));
  const GDALColorEntry kColor{1, 2, 3, 4};
  GDALSetColorEntry(table, 1, &kColor);
  EXPECT_EQ(2, GDALGetColorEntryCount(table));

  GDALColorEntry color{-1, -1, -1, -1};
  GDALGetColorEntryAsRGB(table, 1, &color);
  EXPECT_THAT(ToVector(color), testing::ElementsAre(1, 2, 3, 4));

  const GDALColorEntry kBlack{0, 0, 0, 255};
  const GDALColorEntry kWhite{255, 255, 255, 255};
  // C API swallows the CreateColorRamp return value.
  GDALCreateColorRamp(table, 1, &kWhite, 11, &kBlack);
  EXPECT_EQ(12, GDALGetColorEntryCount(table));

  EXPECT_THAT(ToVector(*GDALGetColorEntry(table, 11)),
              testing::ElementsAre(0, 0, 0, 255));

  GDALColorTableH table2 = GDALCloneColorTable(table);
  ASSERT_NE(nullptr, table2);

  GDALDestroyColorTable(table);
  GDALDestroyColorTable(table2);
}

}  // namespace
