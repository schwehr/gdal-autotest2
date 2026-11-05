// Copyright 2026 Google LLC
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

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

TEST(MitabToolDefTest, DefaultStateAndNulls) {
  TABToolDefTable table;
  EXPECT_EQ(0, table.GetNumPen());
  EXPECT_EQ(0, table.GetNumBrushes());
  EXPECT_EQ(0, table.GetNumFonts());
  EXPECT_EQ(0, table.GetNumSymbols());
  EXPECT_EQ(300, table.GetMinVersionNumber());

  EXPECT_EQ(-1, table.AddPenDefRef(nullptr));
  EXPECT_EQ(-1, table.AddBrushDefRef(nullptr));
  EXPECT_EQ(-1, table.AddFontDefRef(nullptr));
  EXPECT_EQ(-1, table.AddSymbolDefRef(nullptr));

  EXPECT_EQ(nullptr, table.GetPenDefRef(0));
  EXPECT_EQ(nullptr, table.GetPenDefRef(1));
  EXPECT_EQ(nullptr, table.GetBrushDefRef(0));
  EXPECT_EQ(nullptr, table.GetBrushDefRef(1));
  EXPECT_EQ(nullptr, table.GetFontDefRef(0));
  EXPECT_EQ(nullptr, table.GetFontDefRef(1));
  EXPECT_EQ(nullptr, table.GetSymbolDefRef(0));
  EXPECT_EQ(nullptr, table.GetSymbolDefRef(1));
}

TEST(MitabToolDefTest, AddToolsAndRealloc) {
  TABToolDefTable table;

  // Pattern < 1 returns 0 ("none")
  TABPenDef pen_none{};
  pen_none.nLinePattern = 0;
  EXPECT_EQ(0, table.AddPenDefRef(&pen_none));

  TABBrushDef brush_none{};
  brush_none.nFillPattern = 0;
  EXPECT_EQ(0, table.AddBrushDefRef(&brush_none));

  // Add normal pen
  TABPenDef pen1{};
  pen1.nPixelWidth = 2;
  pen1.nLinePattern = 1;
  pen1.nPointWidth = 0;
  pen1.rgbColor = 0x112233;
  EXPECT_EQ(1, table.AddPenDefRef(&pen1));
  // Add identical pen (deduplication)
  EXPECT_EQ(1, table.AddPenDefRef(&pen1));
  EXPECT_EQ(2, table.GetPenDefRef(1)->nRefCount);

  // Add point-width pen (> 255)
  TABPenDef pen_point{};
  pen_point.nPixelWidth = 1;
  pen_point.nLinePattern = 2;
  pen_point.nPointWidth = 300;
  pen_point.rgbColor = 0x445566;
  EXPECT_EQ(2, table.AddPenDefRef(&pen_point));
  EXPECT_EQ(450, table.GetMinVersionNumber());

  // Add 25 more distinct pens to trigger CPLRealloc
  for (int i = 0; i < 25; ++i) {
    TABPenDef p{};
    p.nPixelWidth = 1;
    p.nLinePattern = static_cast<GByte>(i + 3);
    p.rgbColor = static_cast<GInt32>(i * 100);
    EXPECT_EQ(i + 3, table.AddPenDefRef(&p));
  }
  EXPECT_EQ(27, table.GetNumPen());

  // Brushes: add brush 1 and duplicate
  TABBrushDef brush1{};
  brush1.nFillPattern = 2;
  brush1.bTransparentFill = 1;
  brush1.rgbFGColor = 0x123456;
  brush1.rgbBGColor = 0x654321;
  EXPECT_EQ(1, table.AddBrushDefRef(&brush1));
  EXPECT_EQ(1, table.AddBrushDefRef(&brush1));
  EXPECT_EQ(2, table.GetBrushDefRef(1)->nRefCount);

  // Add 25 more distinct brushes to trigger CPLRealloc
  for (int i = 0; i < 25; ++i) {
    TABBrushDef b{};
    b.nFillPattern = static_cast<GByte>(i + 3);
    b.rgbFGColor = static_cast<GInt32>(i * 50);
    EXPECT_EQ(i + 2, table.AddBrushDefRef(&b));
  }
  EXPECT_EQ(26, table.GetNumBrushes());

  // Fonts: add font 1 and duplicate
  TABFontDef font1{};
  strncpy(font1.szFontName, "Arial", sizeof(font1.szFontName) - 1);
  EXPECT_EQ(1, table.AddFontDefRef(&font1));
  EXPECT_EQ(1, table.AddFontDefRef(&font1));
  EXPECT_EQ(2, table.GetFontDefRef(1)->nRefCount);

  // Add 25 more distinct fonts to trigger CPLRealloc
  for (int i = 0; i < 25; ++i) {
    TABFontDef f{};
    std::string name = "Font_" + std::to_string(i);
    strncpy(f.szFontName, name.c_str(), sizeof(f.szFontName) - 1);
    EXPECT_EQ(i + 2, table.AddFontDefRef(&f));
  }
  EXPECT_EQ(26, table.GetNumFonts());

  // Symbols: add symbol 1 and duplicate
  TABSymbolDef sym1{};
  sym1.nSymbolNo = 35;
  sym1.nPointSize = 12;
  sym1._nUnknownValue_ = 0;
  sym1.rgbColor = 0x0000FF;
  EXPECT_EQ(1, table.AddSymbolDefRef(&sym1));
  EXPECT_EQ(1, table.AddSymbolDefRef(&sym1));
  EXPECT_EQ(2, table.GetSymbolDefRef(1)->nRefCount);

  // Add 25 more distinct symbols to trigger CPLRealloc
  for (int i = 0; i < 25; ++i) {
    TABSymbolDef s{};
    s.nSymbolNo = static_cast<GInt16>(i + 40);
    s.nPointSize = 10;
    s.rgbColor = static_cast<GInt32>(i * 20);
    EXPECT_EQ(i + 2, table.AddSymbolDefRef(&s));
  }
  EXPECT_EQ(26, table.GetNumSymbols());
}

TEST(MitabToolDefTest, WriteAndReadAllToolDefs) {
  CPLErrorReset();
  constexpr char kPath[] = "/vsimem/tooldef_test/all_tools.bin";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/tooldef_test"); };
  VSIMkdir("/vsimem/tooldef_test", 0777);

  VSILFILE* fp = VSIFOpenL(kPath, "w+b");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };

  // Write 512-byte dummy block at offset 0 (reserved header block space)
  std::vector<GByte> dummy_header(512, 0);
  ASSERT_EQ(1, VSIFWriteL(dummy_header.data(), 512, 1, fp));

  TABBinBlockManager mgr;
  mgr.SetBlockSize(512);
  mgr.SetLastPtr(512);

  // Build table with pen, brush, font, symbol
  TABToolDefTable write_table;
  TABPenDef p1{};
  p1.nPixelWidth = 3;
  p1.nLinePattern = 2;
  p1.rgbColor = 0xAABBCC;
  write_table.AddPenDefRef(&p1);

  TABPenDef p_pt{};
  p_pt.nPixelWidth = 1;
  p_pt.nLinePattern = 3;
  p_pt.nPointWidth = 300;
  p_pt.rgbColor = 0x112233;
  write_table.AddPenDefRef(&p_pt);

  TABBrushDef b1{};
  b1.nFillPattern = 5;
  b1.bTransparentFill = 0;
  b1.rgbFGColor = 0x111111;
  b1.rgbBGColor = 0x222222;
  write_table.AddBrushDefRef(&b1);

  TABFontDef f1{};
  strncpy(f1.szFontName, "Helvetica", sizeof(f1.szFontName) - 1);
  write_table.AddFontDefRef(&f1);

  TABSymbolDef s1{};
  s1.nSymbolNo = 40;
  s1.nPointSize = 14;
  s1._nUnknownValue_ = 1;
  s1.rgbColor = 0xFF00FF;
  write_table.AddSymbolDefRef(&s1);

  // Add multiple tools to cross 512-byte block boundary
  for (int i = 0; i < 20; ++i) {
    TABFontDef f{};
    std::string name = "CustomFont" + std::to_string(i);
    strncpy(f.szFontName, name.c_str(), sizeof(f.szFontName) - 1);
    write_table.AddFontDefRef(&f);
  }

  // Write all tool defs starting at offset 512
  TABMAPToolBlock write_block(TABWrite);
  EXPECT_EQ(0, write_block.InitNewBlock(fp, 512, 512));
  write_block.SetMAPBlockManagerRef(&mgr);
  EXPECT_EQ(0, write_table.WriteAllToolDefs(&write_block));

  VSIFFlushL(fp);

  // Read all tool defs back from offset 512
  TABMAPToolBlock read_block(TABRead);
  EXPECT_EQ(0, read_block.ReadFromFile(fp, 512, 512));

  TABToolDefTable read_table;
  EXPECT_EQ(0, read_table.ReadAllToolDefs(&read_block));

  EXPECT_EQ(write_table.GetNumPen(), read_table.GetNumPen());
  EXPECT_EQ(write_table.GetNumBrushes(), read_table.GetNumBrushes());
  EXPECT_EQ(write_table.GetNumFonts(), read_table.GetNumFonts());
  EXPECT_EQ(write_table.GetNumSymbols(), read_table.GetNumSymbols());

  // Check point width decoding (> 255)
  TABPenDef* read_pen_pt = read_table.GetPenDefRef(2);
  ASSERT_NE(nullptr, read_pen_pt);
  EXPECT_EQ(300, read_pen_pt->nPointWidth);

  // Check font name
  TABFontDef* read_font = read_table.GetFontDefRef(1);
  ASSERT_NE(nullptr, read_font);
  EXPECT_STREQ("Helvetica", read_font->szFontName);
}

TEST(MitabToolDefTest, ReadUnsupportedToolType) {
  // Construct a tool block in memory with invalid tool type 99 at offset 512
  std::vector<GByte> buf(512, 0);
  buf[0] = TABMAP_TOOL_BLOCK;  // type
  buf[2] = 10;                 // data bytes = 10
  buf[8] = 99;                 // invalid tool type at data start

  TABMAPToolBlock block(TABRead);
  EXPECT_EQ(0,
            block.InitBlockFromData(buf.data(), 512, 512, TRUE, nullptr, 512));

  TABToolDefTable table;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, table.ReadAllToolDefs(&block));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unsupported drawing tool type"));
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
