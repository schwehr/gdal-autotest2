// Copyright 2018 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests gcore/gdalcolortable.cpp.

#include <cstdint>

#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"

namespace {

std::vector<int16_t> ToVector(const GDALColorEntry &color) {
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

void WriteVsiMemFile(const char* filename, const char* content) {
  VSILFILE* fp = VSIFOpenL(filename, "wb");
  ASSERT_NE(nullptr, fp);
  VSIFWriteL(content, 1, strlen(content), fp);
  VSIFCloseL(fp);
}

TEST(GdalcolortableTest, IsIdentity) {
  GDALColorTable ct;
  EXPECT_TRUE(ct.IsIdentity());

  for (int i = 0; i < 4; ++i) {
    const GDALColorEntry entry{static_cast<short>(i), static_cast<short>(i),
                               static_cast<short>(i), 255};
    ct.SetColorEntry(i, &entry);
  }
  EXPECT_TRUE(ct.IsIdentity());

  const GDALColorEntry non_id{0, 1, 0, 255};
  ct.SetColorEntry(0, &non_id);
  EXPECT_FALSE(ct.IsIdentity());
}

TEST(GdalcolortableTest, LoadFromFileQmlAndQlr) {
  // 1. Valid paletted .qml
  WriteVsiMemFile("/vsimem/valid_paletted.qml",
                  "<qgis><pipe><rasterrenderer type=\"paletted\">"
                  "<colorPalette>"
                  "<paletteEntry value=\"0\" color=\"#000000\" alpha=\"255\"/>"
                  "<paletteEntry value=\"2\" color=\"#112233\"/>"
                  "</colorPalette>"
                  "</rasterrenderer></pipe></qgis>");
  auto ct1 = GDALColorTable::LoadFromFile("/vsimem/valid_paletted.qml");
  ASSERT_NE(nullptr, ct1);
  EXPECT_EQ(3, ct1->GetColorEntryCount());
  EXPECT_THAT(ToVector(*ct1->GetColorEntry(2)),
              testing::ElementsAre(0x11, 0x22, 0x33, 255));
  VSIUnlink("/vsimem/valid_paletted.qml");

  // 2. Valid singlebandpseudocolor .qlr
  WriteVsiMemFile("/vsimem/valid_pseudo.qlr",
                  "<qlr><rasterrenderer type=\"singlebandpseudocolor\">"
                  "<item value=\"1\" color=\"#abcdef\" alpha=\"128\"/>"
                  "</rasterrenderer></qlr>");
  auto ct2 = GDALColorTable::LoadFromFile("/vsimem/valid_pseudo.qlr");
  ASSERT_NE(nullptr, ct2);
  EXPECT_EQ(2, ct2->GetColorEntryCount());
  VSIUnlink("/vsimem/valid_pseudo.qlr");

  // 3. Error cases in .qml: nonexistent file, multiple renderers, no renderer,
  // missing entry, invalid value, invalid color.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(nullptr, GDALColorTable::LoadFromFile("/vsimem/nonexistent.qml"));

  WriteVsiMemFile("/vsimem/two_renderers.qml",
                  "<qgis><rasterrenderer type=\"paletted\"/>"
                  "<rasterrenderer type=\"paletted\"/></qgis>");
  EXPECT_EQ(nullptr, GDALColorTable::LoadFromFile("/vsimem/two_renderers.qml"));
  VSIUnlink("/vsimem/two_renderers.qml");

  WriteVsiMemFile("/vsimem/no_renderer.qml", "<qgis><other/></qgis>");
  EXPECT_EQ(nullptr, GDALColorTable::LoadFromFile("/vsimem/no_renderer.qml"));
  VSIUnlink("/vsimem/no_renderer.qml");

  WriteVsiMemFile("/vsimem/no_entry.qml",
                  "<qgis><rasterrenderer type=\"paletted\"/></qgis>");
  EXPECT_EQ(nullptr, GDALColorTable::LoadFromFile("/vsimem/no_entry.qml"));
  VSIUnlink("/vsimem/no_entry.qml");

  WriteVsiMemFile("/vsimem/bad_val.qml",
                  "<qgis><rasterrenderer type=\"paletted\">"
                  "<paletteEntry value=\"-5\" color=\"#000000\"/>"
                  "</rasterrenderer></qgis>");
  EXPECT_EQ(nullptr, GDALColorTable::LoadFromFile("/vsimem/bad_val.qml"));
  VSIUnlink("/vsimem/bad_val.qml");

  WriteVsiMemFile("/vsimem/bad_color.qml",
                  "<qgis><rasterrenderer type=\"paletted\">"
                  "<paletteEntry value=\"1\" color=\"invalid\"/>"
                  "</rasterrenderer></qgis>");
  EXPECT_EQ(nullptr, GDALColorTable::LoadFromFile("/vsimem/bad_color.qml"));
  VSIUnlink("/vsimem/bad_color.qml");
  CPLPopErrorHandler();
}

TEST(GdalcolortableTest, LoadTextColorMapAndLoadFromFileText) {
  GDALAllRegister();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);
  std::unique_ptr<GDALDataset> ds(
      mem_driver->Create("", 4, 1, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  GByte pixels[4] = {0, 10, 20, 100};
  ASSERT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, 4, 1, pixels, 4, 1,
                                    GDT_Byte, 0, 0, nullptr));
  band->SetNoDataValue(0.0);

  // 1. GRASS/text format with numeric RGB, RGBA, named colors, percentages, nv.
  WriteVsiMemFile("/vsimem/grass_colors.txt",
                  "# Comment line\n"
                  "/ Another comment\n"
                  "\n"
                  "nv white\n"
                  "0% black\n"
                  "50% 10 20 30 200\n"
                  "100% red 128\n"
                  "5 100 110 120\n");
  auto assoc = GDALLoadTextColorMap("/vsimem/grass_colors.txt", band);
  EXPECT_EQ(5u, assoc.size());

  auto ct_from_txt = GDALColorTable::LoadFromFile("/vsimem/grass_colors.txt");
  // Fails because without band, 0% is not supported.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(nullptr, GDALColorTable::LoadFromFile("/vsimem/grass_colors.txt"));
  CPLPopErrorHandler();
  VSIUnlink("/vsimem/grass_colors.txt");

  // 2. Integer text color map via LoadFromFile (and non-integer rejection).
  WriteVsiMemFile("/vsimem/int_colors.txt",
                  "1 10 20 30\n"
                  "2 green\n");
  auto ct_int = GDALColorTable::LoadFromFile("/vsimem/int_colors.txt");
  ASSERT_NE(nullptr, ct_int);
  EXPECT_EQ(3, ct_int->GetColorEntryCount());
  VSIUnlink("/vsimem/int_colors.txt");

  WriteVsiMemFile("/vsimem/float_colors.txt", "1.5 10 20 30\n");
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(nullptr, GDALColorTable::LoadFromFile("/vsimem/float_colors.txt"));
  CPLPopErrorHandler();
  VSIUnlink("/vsimem/float_colors.txt");

  // 3. GMT CPT format with COLOR_MODEL = RGB, 8-token lines, and 4-token N
  // line.
  WriteVsiMemFile("/vsimem/gmt.cpt",
                  "# COLOR_MODEL = RGB\n"
                  "0 10 20 30 10 40 50 60\n"
                  "N 255 0 0\n"
                  "B 0 0 0\n");
  auto gmt_assoc = GDALLoadTextColorMap("/vsimem/gmt.cpt", band);
  EXPECT_EQ(3u, gmt_assoc.size());
  VSIUnlink("/vsimem/gmt.cpt");

  // 4. Error cases: unsupported COLOR_MODEL, nv without nodata, bad percentage,
  // unknown named color, empty file, missing file.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_TRUE(GDALLoadTextColorMap("/vsimem/missing.txt", nullptr).empty());

  WriteVsiMemFile("/vsimem/gmt_hsv.cpt", "# COLOR_MODEL = HSV\n");
  EXPECT_TRUE(GDALLoadTextColorMap("/vsimem/gmt_hsv.cpt", nullptr).empty());
  VSIUnlink("/vsimem/gmt_hsv.cpt");

  WriteVsiMemFile("/vsimem/bad_pct.txt", "150% 0 0 0\n");
  EXPECT_TRUE(GDALLoadTextColorMap("/vsimem/bad_pct.txt", band).empty());
  VSIUnlink("/vsimem/bad_pct.txt");

  WriteVsiMemFile("/vsimem/nv_and_bad_color.txt",
                  "nv 0 0 0\n"
                  "1 not_a_color\n");
  EXPECT_TRUE(
      GDALLoadTextColorMap("/vsimem/nv_and_bad_color.txt", nullptr).empty());
  VSIUnlink("/vsimem/nv_and_bad_color.txt");

  WriteVsiMemFile("/vsimem/empty.txt", "# only comment\n");
  EXPECT_TRUE(GDALLoadTextColorMap("/vsimem/empty.txt", nullptr).empty());
  VSIUnlink("/vsimem/empty.txt");
  CPLPopErrorHandler();
}

}  // namespace
