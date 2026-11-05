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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "mitab.h"
#include "mitab_priv.h"
#include "ogr_core.h"
#include "ogr_feature.h"
#include "ogr_geometry.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

namespace autotest2 {
namespace {

using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::Ne;
using ::testing::NotNull;
using ::testing::StrEq;

TEST(TABMAPFileTest, UnopenedAndStaticMethods) {
  // Test static method IsValidObjType for valid and invalid types.
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_NONE));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_SYMBOL));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_SYMBOL_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_LINE));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_LINE_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_PLINE));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_PLINE_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_ARC));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_ARC_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_REGION));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_REGION_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_TEXT));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_TEXT_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_RECT));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_RECT_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_ROUNDRECT));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_ROUNDRECT_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_ELLIPSE));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_ELLIPSE_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_MULTIPLINE));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_MULTIPLINE_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_FONTSYMBOL));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_FONTSYMBOL_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_CUSTOMSYMBOL));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_CUSTOMSYMBOL_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_V450_REGION));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_V450_REGION_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_V450_MULTIPLINE));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_V450_MULTIPLINE_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_MULTIPOINT));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_MULTIPOINT_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_COLLECTION));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_COLLECTION_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_V800_REGION));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_V800_REGION_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_V800_MULTIPLINE));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_V800_MULTIPLINE_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_V800_MULTIPOINT));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_V800_MULTIPOINT_C));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_V800_COLLECTION));
  EXPECT_TRUE(TABMAPFile::IsValidObjType(TAB_GEOM_V800_COLLECTION_C));

  EXPECT_FALSE(TABMAPFile::IsValidObjType(-1));
  EXPECT_FALSE(TABMAPFile::IsValidObjType(0x7f));
  EXPECT_FALSE(TABMAPFile::IsValidObjType(999));

  // Operations on unopened TABMAPFile.
  TABMAPFile mapFile("UTF-8");
  EXPECT_THAT(mapFile.GetEncoding().c_str(), StrEq("UTF-8"));
  mapFile.SetEncoding("ISO-8859-1");
  EXPECT_THAT(mapFile.GetEncoding().c_str(), StrEq("ISO-8859-1"));

  EXPECT_EQ(0U, mapFile.GetFileSize());
  EXPECT_EQ(-1, mapFile.GetMaxObjId());
  EXPECT_EQ(nullptr, mapFile.GetHeaderBlock());
  EXPECT_EQ(nullptr, mapFile.GetIDFileRef());
  EXPECT_EQ(TAB_GEOM_UNSET, mapFile.GetCurObjType());
  EXPECT_EQ(-1, mapFile.GetCurObjId());
  EXPECT_EQ(nullptr, mapFile.GetCurObjBlock());
  EXPECT_EQ(nullptr, mapFile.GetCurCoordBlock());
  EXPECT_EQ(0, mapFile.MoveToObjId(1));
  EXPECT_EQ(TAB_GEOM_NONE, mapFile.GetCurObjType());
  EXPECT_EQ(-1, mapFile.MarkAsDeleted());
  EXPECT_EQ(-1, mapFile.SyncToDisk());
  EXPECT_EQ(300, mapFile.GetMinTABFileVersion());

  double dx = 0.0, dy = 0.0;
  GInt32 nx = 0, ny = 0;
  EXPECT_EQ(-1, mapFile.Int2Coordsys(0, 0, dx, dy));
  EXPECT_EQ(-1, mapFile.Coordsys2Int(0.0, 0.0, nx, ny));
  EXPECT_EQ(-1, mapFile.Int2CoordsysDist(0, 0, dx, dy));
  EXPECT_EQ(-1, mapFile.Coordsys2IntDist(0.0, 0.0, nx, ny));
  EXPECT_EQ(-1, mapFile.SetCoordsysBounds(0.0, 0.0, 10.0, 10.0));

  TABPenDef penDef = {};
  EXPECT_EQ(-1, mapFile.ReadPenDef(1, &penDef));
  EXPECT_EQ(-1, mapFile.WritePenDef(&penDef));
  TABBrushDef brushDef = {};
  EXPECT_EQ(-1, mapFile.ReadBrushDef(1, &brushDef));
  EXPECT_EQ(-1, mapFile.WriteBrushDef(&brushDef));
  TABFontDef fontDef = {};
  EXPECT_EQ(-1, mapFile.ReadFontDef(1, &fontDef));
  EXPECT_EQ(-1, mapFile.WriteFontDef(&fontDef));
  TABSymbolDef symbolDef = {};
  EXPECT_EQ(-1, mapFile.ReadSymbolDef(1, &symbolDef));
  EXPECT_EQ(-1, mapFile.WriteSymbolDef(&symbolDef));

  TABVertex sMin = {1.0, 2.0};
  TABVertex sMax = {3.0, 4.0};
  mapFile.SetCoordFilter(sMin, sMax);
  TABVertex rMin = {}, rMax = {};
  mapFile.GetCoordFilter(rMin, rMax);
  EXPECT_DOUBLE_EQ(1.0, rMin.x);
  EXPECT_DOUBLE_EQ(2.0, rMin.y);
  EXPECT_DOUBLE_EQ(3.0, rMax.x);
  EXPECT_DOUBLE_EQ(4.0, rMax.y);

  // Close unopened file.
  EXPECT_EQ(0, mapFile.Close());
  CPLErrorReset();
}

TEST(TABMAPFileTest, OpenErrorCasesAndSpecialModes) {
  const std::string badPath = "/vsimem/map_bad.map";
  TABMAPFile mapFile("");

  // Invalid access mode string.
  EXPECT_EQ(-1, mapFile.Open(badPath.c_str(), "x"));
  CPLErrorReset();

  // Invalid block size for create (must be multiple of 512).
  EXPECT_EQ(-1, mapFile.Open(badPath.c_str(), "w", FALSE, 1023));
  CPLErrorReset();

  // Missing file in read mode without bNoErrorMsg.
  EXPECT_EQ(-1, mapFile.Open(badPath.c_str(), "r", FALSE));
  CPLErrorReset();

  // Missing file in read mode with bNoErrorMsg=TRUE: returns 1 (virtual empty
  // file).
  EXPECT_EQ(1, mapFile.Open(badPath.c_str(), "r", TRUE));
  EXPECT_EQ(-1, mapFile.GetMaxObjId());
  EXPECT_EQ(0, mapFile.MoveToObjId(1));
  EXPECT_EQ(TAB_GEOM_NONE, mapFile.GetCurObjType());
  EXPECT_EQ(-1, mapFile.GetNextFeatureId(-1));
  EXPECT_EQ(0, mapFile.Close());

  // Corrupt file (less than 512 bytes).
  const std::string corruptPath = "/vsimem/map_corrupt.map";
  absl::Cleanup cleanupCorrupt = [corruptPath]() {
    VSIUnlink(corruptPath.c_str());
  };
  VSILFILE* fp = VSIFOpenL(corruptPath.c_str(), "wb");
  ASSERT_THAT(fp, NotNull());
  const char shortData[100] = {0};
  VSIFWriteL(shortData, 1, sizeof(shortData), fp);
  VSIFCloseL(fp);

  EXPECT_EQ(-1, mapFile.Open(corruptPath.c_str(), "r"));
  CPLErrorReset();

  // Double open on already open file.
  const std::string validPath = "/vsimem/map_double_open.map";
  const std::string validIdPath = "/vsimem/map_double_open.id";
  absl::Cleanup cleanupValid = [validPath, validIdPath]() {
    VSIUnlink(validPath.c_str());
    VSIUnlink(validIdPath.c_str());
  };
  EXPECT_EQ(0, mapFile.Open(validPath.c_str(), "w"));
  EXPECT_EQ(-1, mapFile.Open(validPath.c_str(), "w"));
  CPLErrorReset();
  EXPECT_EQ(0, mapFile.Close());

  // Test Open with non-512 block size (1024) to cover line 207.
  {
    const std::string map1024 = "/vsimem/map1024.map";
    const std::string id1024 = "/vsimem/map1024.id";
    absl::Cleanup cleanup1024 = [map1024, id1024]() {
      VSIUnlink(map1024.c_str());
      VSIUnlink(id1024.c_str());
    };
    TABMAPFile mapFile1024("");
    EXPECT_EQ(0, mapFile1024.Open(map1024.c_str(), "w", FALSE, 1024));
    EXPECT_EQ(0, mapFile1024.Close());
  }

  // Test Open with file that is not a valid header block (line 180).
  {
    const std::string badHdrMap = "/vsimem/bad_hdr.map";
    absl::Cleanup cleanupBadHdr = [badHdrMap]() {
      VSIUnlink(badHdrMap.c_str());
    };
    VSILFILE* fpBad = VSIFOpenL(badHdrMap.c_str(), "wb");
    ASSERT_THAT(fpBad, NotNull());
    GByte junk[512] = {};
    junk[0] = TABMAP_INDEX_BLOCK;  // Block type != header block
    VSIFWriteL(junk, 1, 512, fpBad);
    VSIFCloseL(fpBad);

    TABMAPFile badHdrFile("");
    EXPECT_EQ(-1, badHdrFile.Open(badHdrMap.c_str(), "r"));
    CPLErrorReset();
  }

  // Test Open with failing .id file open (lines 270-271).
  {
    const std::string failIdMap = "/vsimem/fail_id.map";
    const std::string failIdPath = "/vsimem/fail_id.id";
    absl::Cleanup cleanupFailId = [failIdMap, failIdPath]() {
      VSIUnlink(failIdMap.c_str());
      VSIRmdir(failIdPath.c_str());
    };
    // Create a directory at fail_id.id so opening it as a file fails.
    VSIMkdir(failIdPath.c_str(), 0777);
    TABMAPFile failIdFile("");
    EXPECT_EQ(-1, failIdFile.Open(failIdMap.c_str(), "w"));
    CPLErrorReset();
  }
}

TEST(TABMAPFileTest, QuickSpatialIndexAndTools) {
  const std::string mapPath = "/vsimem/map_quick.map";
  const std::string idPath = "/vsimem/map_quick.id";
  absl::Cleanup cleanupFiles = [mapPath, idPath]() {
    VSIUnlink(mapPath.c_str());
    VSIUnlink(idPath.c_str());
  };

  TABMAPFile mapFile("");
  ASSERT_EQ(0, mapFile.Open(mapPath.c_str(), "w"));

  // Toggle quick spatial index mode before writing objects.
  EXPECT_EQ(0, mapFile.SetQuickSpatialIndexMode(TRUE));
  EXPECT_EQ(0, mapFile.SetQuickSpatialIndexMode(FALSE));
  EXPECT_EQ(0, mapFile.SetQuickSpatialIndexMode(TRUE));

  // Set coordinate bounds.
  EXPECT_EQ(0, mapFile.SetCoordsysBounds(-1000.0, -1000.0, 1000.0, 1000.0));

  // Coordinate conversions.
  GInt32 nx = 0, ny = 0;
  double dx = 0.0, dy = 0.0;
  EXPECT_EQ(0, mapFile.Coordsys2Int(0.0, 0.0, nx, ny, FALSE));
  EXPECT_EQ(0, mapFile.Int2Coordsys(nx, ny, dx, dy));
  EXPECT_NEAR(0.0, dx, 1e-3);
  EXPECT_NEAR(0.0, dy, 1e-3);

  // Out of bounds with ignore overflow vs not.
  EXPECT_EQ(0, mapFile.Coordsys2Int(5000.0, 5000.0, nx, ny, FALSE));
  EXPECT_EQ(1000000000, nx);
  EXPECT_EQ(1000000000, ny);
  CPLErrorReset();
  EXPECT_EQ(0, mapFile.Coordsys2Int(5000.0, 5000.0, nx, ny, TRUE));

  // Distance conversions.
  EXPECT_EQ(0, mapFile.Coordsys2IntDist(50.0, 50.0, nx, ny));
  EXPECT_EQ(0, mapFile.Int2CoordsysDist(nx, ny, dx, dy));
  EXPECT_NEAR(50.0, dx, 1e-3);
  EXPECT_NEAR(50.0, dy, 1e-3);

  // Write and Read Drawing Tools.
  TABPenDef penDef = {};
  penDef.nPixelWidth = 2;
  penDef.nLinePattern = 1;
  penDef.rgbColor = 0xFF0000;
  EXPECT_EQ(-1, mapFile.WritePenDef(nullptr));
  int penId = mapFile.WritePenDef(&penDef);
  EXPECT_THAT(penId, Gt(0));

  TABBrushDef brushDef = {};
  brushDef.nFillPattern = 2;
  brushDef.rgbFGColor = 0x00FF00;
  brushDef.rgbBGColor = 0x0000FF;
  EXPECT_EQ(-1, mapFile.WriteBrushDef(nullptr));
  int brushId = mapFile.WriteBrushDef(&brushDef);
  EXPECT_THAT(brushId, Gt(0));

  TABFontDef fontDef = {};
  snprintf(fontDef.szFontName, sizeof(fontDef.szFontName), "Arial");
  EXPECT_EQ(-1, mapFile.WriteFontDef(nullptr));
  int fontId = mapFile.WriteFontDef(&fontDef);
  EXPECT_THAT(fontId, Gt(0));

  TABSymbolDef symbolDef = {};
  symbolDef.nSymbolNo = 35;
  symbolDef.nPointSize = 12;
  symbolDef.rgbColor = 0x123456;
  EXPECT_EQ(-1, mapFile.WriteSymbolDef(nullptr));
  int symbolId = mapFile.WriteSymbolDef(&symbolDef);
  EXPECT_THAT(symbolId, Gt(0));

  // Read tools back while still in write mode.
  TABPenDef rPen = {};
  EXPECT_EQ(0, mapFile.ReadPenDef(penId, &rPen));
  EXPECT_EQ(2, rPen.nPixelWidth);
  EXPECT_EQ(0xFF0000, rPen.rgbColor);

  TABBrushDef rBrush = {};
  EXPECT_EQ(0, mapFile.ReadBrushDef(brushId, &rBrush));
  EXPECT_EQ(2, rBrush.nFillPattern);

  TABFontDef rFont = {};
  EXPECT_EQ(0, mapFile.ReadFontDef(fontId, &rFont));
  EXPECT_THAT(rFont.szFontName, StrEq("Arial"));

  TABSymbolDef rSymbol = {};
  EXPECT_EQ(0, mapFile.ReadSymbolDef(symbolId, &rSymbol));
  EXPECT_EQ(35, rSymbol.nSymbolNo);

  // Invalid tool indices return defaults.
  EXPECT_EQ(-1, mapFile.ReadPenDef(999, &rPen));
  EXPECT_EQ(-1, mapFile.ReadBrushDef(999, &rBrush));
  EXPECT_EQ(-1, mapFile.ReadFontDef(999, &rFont));
  EXPECT_EQ(-1, mapFile.ReadSymbolDef(999, &rSymbol));
  EXPECT_EQ(0, mapFile.ReadPenDef(0, nullptr));

  // Exercise UpdateMapHeaderInfo for various geometry types.
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_SYMBOL);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_LINE);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_PLINE);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_ARC);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_REGION);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_TEXT);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_RECT);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_ROUNDRECT);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_ELLIPSE);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_FONTSYMBOL);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_CUSTOMSYMBOL);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_V450_REGION);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_V450_MULTIPLINE);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_MULTIPOINT);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_COLLECTION);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_V800_REGION);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_V800_MULTIPLINE);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_V800_MULTIPOINT);
  mapFile.UpdateMapHeaderInfo(TAB_GEOM_V800_COLLECTION);
  EXPECT_THAT(mapFile.GetMinTABFileVersion(), Ge(800));

  // Write a TAB_GEOM_NONE object (id 1).
  std::unique_ptr<TABMAPObjHdr> noneHdr(TABMAPObjHdr::NewObj(TAB_GEOM_NONE, 1));
  ASSERT_THAT(noneHdr, NotNull());
  EXPECT_EQ(0, mapFile.PrepareNewObj(noneHdr.get()));
  EXPECT_EQ(0, mapFile.CommitNewObj(noneHdr.get()));

  // Write 35 objects (TABMAPObjLine) to overflow the first 512-byte object
  // block and trigger PrepareNewObjViaObjBlock block allocation & chaining.
  for (int i = 2; i <= 36; ++i) {
    std::unique_ptr<TABMAPObjHdr> lineHdr(
        TABMAPObjHdr::NewObj(TAB_GEOM_LINE, i));
    ASSERT_THAT(lineHdr, NotNull());
    auto* line = cpl::down_cast<TABMAPObjLine*>(lineHdr.get());
    line->m_nX1 = i * 10;
    line->m_nY1 = i * 10;
    line->m_nX2 = i * 10 + 5;
    line->m_nY2 = i * 10 + 5;
    line->m_nPenId = 1;
    line->SetMBR(line->m_nX1, line->m_nY1, line->m_nX2, line->m_nY2);

    ASSERT_EQ(0, mapFile.PrepareNewObj(line));
    ASSERT_EQ(0, mapFile.CommitNewObj(line));
  }

  // Writing objects locks quick spatial index mode.
  EXPECT_EQ(-1, mapFile.SetQuickSpatialIndexMode(FALSE));
  CPLErrorReset();

  EXPECT_THAT(mapFile.GetFileSize(), Gt(0U));
  EXPECT_EQ(0, mapFile.SyncToDisk());
  EXPECT_EQ(0, mapFile.Close());

  // Reopen in read mode.
  ASSERT_EQ(0, mapFile.Open(mapPath.c_str(), "r"));
  EXPECT_EQ(-1, mapFile.SetQuickSpatialIndexMode(TRUE));
  CPLErrorReset();

  EXPECT_EQ(36, mapFile.GetMaxObjId());
  EXPECT_EQ(0, mapFile.MoveToObjId(1));
  EXPECT_EQ(TAB_GEOM_NONE, mapFile.GetCurObjType());
  EXPECT_EQ(1, mapFile.GetCurObjId());

  EXPECT_EQ(0, mapFile.MoveToObjId(10));
  EXPECT_EQ(TAB_GEOM_LINE, mapFile.GetCurObjType());
  EXPECT_EQ(10, mapFile.GetCurObjId());

  // Read tools back in read mode.
  TABPenDef penRead = {};
  EXPECT_EQ(0, mapFile.ReadPenDef(penId, &penRead));
  EXPECT_EQ(2, penRead.nPixelWidth);

  EXPECT_EQ(0, mapFile.Close());
}

TEST(TABMAPFileTest, OptimizedSpatialIndexSplitAndFeatures) {
  const std::string mapPath = "/vsimem/map_opt.map";
  const std::string idPath = "/vsimem/map_opt.id";
  absl::Cleanup cleanupFiles = [mapPath, idPath]() {
    VSIUnlink(mapPath.c_str());
    VSIUnlink(idPath.c_str());
  };

  TABMAPFile mapFile("");
  ASSERT_EQ(0, mapFile.Open(mapPath.c_str(), "w"));
  ASSERT_EQ(0, mapFile.SetQuickSpatialIndexMode(FALSE));
  EXPECT_EQ(0, mapFile.SetCoordsysBounds(0.0, 0.0, 10000.0, 10000.0));

  OGRFeatureDefn* defn = new OGRFeatureDefn("layer");
  defn->Reference();
  absl::Cleanup cleanupDefn = [defn]() { defn->Release(); };

  // Write 60 features using TABPolyline with coordinate data.
  // Coordinates are written to m_poCurCoordBlock via WriteGeometryToMAPFile.
  // This exercises PrepareNewObjViaSpatialIndex, SplitObjBlock, MoveObjToBlock,
  // coordinate copying, and garbage block reclamation!
  for (int i = 1; i <= 60; ++i) {
    TABPolyline feature(defn);
    OGRLineString ls;
    const double baseX = (i % 8) * 1000.0 + 100.0;
    const double baseY = (i / 8) * 1000.0 + 100.0;
    for (int pt = 0; pt < 70; ++pt) {
      ls.addPoint(baseX + pt * 10.0, baseY + (pt % 5) * 10.0);
    }
    feature.SetGeometry(&ls);

    const TABGeomType geomType = feature.ValidateMapInfoType(&mapFile);
    std::unique_ptr<TABMAPObjHdr> objHdr(TABMAPObjHdr::NewObj(geomType, i));
    ASSERT_THAT(objHdr, NotNull());
    feature.GetIntMBR(objHdr->m_nMinX, objHdr->m_nMinY, objHdr->m_nMaxX,
                      objHdr->m_nMaxY);

    ASSERT_EQ(0, mapFile.PrepareNewObj(objHdr.get()));
    ASSERT_EQ(0, feature.WriteGeometryToMAPFile(&mapFile, objHdr.get()));
    ASSERT_EQ(0, mapFile.CommitNewObj(objHdr.get()));

    if (i == 1) {
      auto* objHdrCoord = cpl::down_cast<TABMAPObjHdrWithCoord*>(objHdr.get());
      GInt32 firstCoord = objHdrCoord->m_nCoordBlockPtr;
      // Exercise GetCoordBlock on block boundary (lines 2427-2430) and
      // move coordinate pointer to first block so that the next feature's
      // PrepareCoordBlock reloads the last coordinate block (lines 2293-2302).
      EXPECT_NE(nullptr, mapFile.GetCoordBlock(firstCoord));
    }
  }

  // Also write various geometry types to exercise UpdateMBR and coordinate
  // blocks: Point
  {
    TABPoint pt(defn);
    OGRPoint p(500.0, 500.0);
    pt.SetGeometry(&p);
    const TABGeomType ptType = pt.ValidateMapInfoType(&mapFile);
    std::unique_ptr<TABMAPObjHdr> objHdr(TABMAPObjHdr::NewObj(ptType, 61));
    ASSERT_THAT(objHdr, NotNull());
    pt.GetIntMBR(objHdr->m_nMinX, objHdr->m_nMinY, objHdr->m_nMaxX,
                 objHdr->m_nMaxY);
    ASSERT_EQ(0, mapFile.PrepareNewObj(objHdr.get()));
    ASSERT_EQ(0, pt.WriteGeometryToMAPFile(&mapFile, objHdr.get()));
    ASSERT_EQ(0, mapFile.CommitNewObj(objHdr.get()));
  }

  // Test calling read methods before ResetReading while last op was write.
  EXPECT_EQ(-1, mapFile.GetNextFeatureId(-1));
  CPLErrorReset();
  EXPECT_EQ(-1, mapFile.MoveToObjId(1));
  CPLErrorReset();

  // While in TABWrite mode, test calling ResetReading() and GetNextFeatureId(),
  // which exercises ReOpenReadWrite()!
  mapFile.ResetReading();
  int nextId = mapFile.GetNextFeatureId(0);
  EXPECT_THAT(nextId, Gt(0));

  // Sequence check: calling GetNextFeatureId with wrong prevId returns -1.
  EXPECT_EQ(-1, mapFile.GetNextFeatureId(9999));
  CPLErrorReset();

  EXPECT_EQ(0, mapFile.Close());
}

TEST(TABMAPFileTest, ReadWriteModeDeletionAndCompaction) {
  const std::string mapPath = "/vsimem/map_compact.map";
  const std::string idPath = "/vsimem/map_compact.id";
  absl::Cleanup cleanupFiles = [mapPath, idPath]() {
    VSIUnlink(mapPath.c_str());
    VSIUnlink(idPath.c_str());
  };

  // Create a dataset with 30 point objects located in the same small area.
  {
    TABMAPFile mapFile("");
    ASSERT_EQ(0, mapFile.Open(mapPath.c_str(), "w"));
    ASSERT_EQ(0, mapFile.SetCoordsysBounds(0.0, 0.0, 1000.0, 1000.0));

    for (int i = 1; i <= 30; ++i) {
      std::unique_ptr<TABMAPObjHdr> objHdr(
          TABMAPObjHdr::NewObj(TAB_GEOM_SYMBOL, i));
      ASSERT_THAT(objHdr, NotNull());
      auto* pt = cpl::down_cast<TABMAPObjPoint*>(objHdr.get());
      pt->m_nX = 100 + i;
      pt->m_nY = 100 + i;
      pt->SetMBR(pt->m_nX, pt->m_nY, pt->m_nX, pt->m_nY);

      ASSERT_EQ(0, mapFile.PrepareNewObj(pt));
      ASSERT_EQ(0, mapFile.CommitNewObj(pt));
    }
    ASSERT_EQ(0, mapFile.Close());
  }

  // Reopen in TABReadWrite mode.
  {
    TABMAPFile rwFile("");
    ASSERT_EQ(0, rwFile.Open(mapPath.c_str(), TABReadWrite));
    EXPECT_EQ(30, rwFile.GetMaxObjId());

    // Mark 10 objects as deleted.
    GInt32 oldOffset5 = rwFile.GetIDFileRef()->GetObjPtr(5);
    for (int i = 5; i <= 14; ++i) {
      ASSERT_EQ(0, rwFile.MoveToObjId(i));
      EXPECT_EQ(i, rwFile.GetCurObjId());
      ASSERT_EQ(0, rwFile.MarkAsDeleted());
    }

    // Test MoveToObjId consistency check errors while last op was NOT write:
    // 1. Deleted object check in .MAP file (lines 1100-1107).
    rwFile.GetIDFileRef()->SetObjPtr(5, oldOffset5);
    EXPECT_EQ(-1, rwFile.MoveToObjId(5));
    CPLErrorReset();

    // 2. Mismatched object ID in .MAP file (lines 1109-1115).
    GInt32 obj1Offset = rwFile.GetIDFileRef()->GetObjPtr(1);
    rwFile.GetIDFileRef()->SetObjPtr(5, obj1Offset);
    EXPECT_EQ(-1, rwFile.MoveToObjId(5));
    CPLErrorReset();

    // Restore ID index entry for object 5.
    rwFile.GetIDFileRef()->SetObjPtr(5, 0);

    // Now insert new objects in the same block to fill it up and trigger
    // the in-place dead object compaction loop (lines 1518-1572)!
    for (int i = 31; i <= 55; ++i) {
      std::unique_ptr<TABMAPObjHdr> objHdr(
          TABMAPObjHdr::NewObj(TAB_GEOM_SYMBOL, i));
      ASSERT_THAT(objHdr, NotNull());
      auto* pt = cpl::down_cast<TABMAPObjPoint*>(objHdr.get());
      pt->m_nX = 100 + (i % 20);
      pt->m_nY = 100 + (i % 20);
      pt->SetMBR(pt->m_nX, pt->m_nY, pt->m_nX, pt->m_nY);

      ASSERT_EQ(0, rwFile.PrepareNewObj(pt));
      ASSERT_EQ(0, rwFile.CommitNewObj(pt));
    }

    // Test inverted filter for ORDER_MIN_MAX coverage.
    TABVertex invertedMin = {800.0, 800.0};
    TABVertex invertedMax = {100.0, 100.0};
    rwFile.SetCoordFilter(invertedMin, invertedMax);

    // Direct access to GetIndexObjectBlock and GetCoordBlock.
    TABRawBinBlock* idxBlock = rwFile.GetIndexObjectBlock(512);
    if (idxBlock != nullptr) {
      delete idxBlock;
    }
    EXPECT_EQ(nullptr, rwFile.GetIndexObjectBlock(999999));
    CPLErrorReset();

    TABMAPCoordBlock* coordBlock = rwFile.GetCoordBlock(512);
    (void)coordBlock;

    ASSERT_EQ(0, rwFile.Close());

    // Corrupt object type in .MAP file to test unsupported type (lines
    // 1087-1093).
    TABIDFile idFile;
    ASSERT_EQ(0, idFile.Open(idPath.c_str(), "r"));
    const GInt32 curObj1Offset = idFile.GetObjPtr(1);
    ASSERT_EQ(0, idFile.Close());

    VSILFILE* fpCorruptObj = VSIFOpenL(mapPath.c_str(), "rb+");
    ASSERT_THAT(fpCorruptObj, NotNull());
    VSIFSeekL(fpCorruptObj, curObj1Offset, SEEK_SET);
    GByte badType = 0xFF;
    VSIFWriteL(&badType, 1, 1, fpCorruptObj);
    VSIFCloseL(fpCorruptObj);

    TABMAPFile roFile("");
    ASSERT_EQ(0, roFile.Open(mapPath.c_str(), "r"));
    EXPECT_EQ(0, roFile.MoveToObjId(1));
    EXPECT_EQ(TAB_GEOM_NONE, roFile.GetCurObjType());
    CPLErrorReset();
    EXPECT_EQ(0, roFile.Close());
  }

  // Test opening a .map file in TABReadWrite where m_nFirstIndexBlock points
  // directly to a TABMAP_OBJECT_BLOCK (lines 317-323 and 1398-1418).
  const std::string directObjMap = "/vsimem/map_direct_obj.map";
  const std::string directObjId = "/vsimem/map_direct_obj.id";
  absl::Cleanup cleanupDirect = [directObjMap, directObjId]() {
    VSIUnlink(directObjMap.c_str());
    VSIUnlink(directObjId.c_str());
  };
  {
    TABMAPFile createMap("");
    ASSERT_EQ(0, createMap.Open(directObjMap.c_str(), "w"));
    ASSERT_EQ(0, createMap.SetCoordsysBounds(0.0, 0.0, 1000.0, 1000.0));
    std::unique_ptr<TABMAPObjHdr> ptHdr(
        TABMAPObjHdr::NewObj(TAB_GEOM_SYMBOL, 1));
    auto* pt = cpl::down_cast<TABMAPObjPoint*>(ptHdr.get());
    pt->m_nX = 100;
    pt->m_nY = 100;
    pt->SetMBR(100, 100, 100, 100);
    ASSERT_EQ(0, createMap.PrepareNewObj(pt));
    ASSERT_EQ(0, createMap.CommitNewObj(pt));
    ASSERT_EQ(0, createMap.Close());

    // Patch offset 0x130 (m_nFirstIndexBlock) in .map to point directly to
    // the first object block (offset 1024 instead of index block at 512).
    VSILFILE* fp = VSIFOpenL(directObjMap.c_str(), "rb+");
    ASSERT_THAT(fp, NotNull());
    VSIFSeekL(fp, 0x130, SEEK_SET);
    GInt32 newFirstBlock = 1024;
    CPL_LSBPTR32(&newFirstBlock);
    VSIFWriteL(&newFirstBlock, 4, 1, fp);
    VSIFCloseL(fp);

    TABMAPFile rwDirect("");
    ASSERT_EQ(0, rwDirect.Open(directObjMap.c_str(), TABReadWrite));
    // Writing new object in optimized spatial index mode triggers
    // lines 1398-1418 where the object block is converted into a spatial index.
    std::unique_ptr<TABMAPObjHdr> pt2Hdr(
        TABMAPObjHdr::NewObj(TAB_GEOM_SYMBOL, 2));
    auto* pt2 = cpl::down_cast<TABMAPObjPoint*>(pt2Hdr.get());
    pt2->m_nX = 200;
    pt2->m_nY = 200;
    pt2->SetMBR(200, 200, 200, 200);
    ASSERT_EQ(0, rwDirect.PrepareNewObj(pt2));
    ASSERT_EQ(0, rwDirect.CommitNewObj(pt2));
    ASSERT_EQ(0, rwDirect.Close());

    // Test Open(TABReadWrite) with invalid first index block offset (lines
    // 306-310, 389-390).
    VSILFILE* fpBadIdx = VSIFOpenL(directObjMap.c_str(), "rb+");
    ASSERT_THAT(fpBadIdx, NotNull());
    VSIFSeekL(fpBadIdx, 0x130, SEEK_SET);
    GInt32 badOffset = 999999;
    CPL_LSBPTR32(&badOffset);
    VSIFWriteL(&badOffset, 4, 1, fpBadIdx);
    VSIFCloseL(fpBadIdx);

    TABMAPFile badIdxFile("");
    EXPECT_EQ(-1, badIdxFile.Open(directObjMap.c_str(), TABReadWrite));
    CPLErrorReset();
  }

  // Test reading chain of garbage blocks on Open(TABReadWrite) (lines 353-380).
  const std::string garbMap = "/vsimem/map_garb.map";
  const std::string garbId = "/vsimem/map_garb.id";
  absl::Cleanup cleanupGarb = [garbMap, garbId]() {
    VSIUnlink(garbMap.c_str());
    VSIUnlink(garbId.c_str());
  };
  {
    TABMAPFile createMap("");
    ASSERT_EQ(0, createMap.Open(garbMap.c_str(), "w"));
    ASSERT_EQ(0, createMap.SetCoordsysBounds(0.0, 0.0, 1000.0, 1000.0));
    std::unique_ptr<TABMAPObjHdr> ptHdr(
        TABMAPObjHdr::NewObj(TAB_GEOM_SYMBOL, 1));
    auto* pt = cpl::down_cast<TABMAPObjPoint*>(ptHdr.get());
    pt->m_nX = 100;
    pt->m_nY = 100;
    pt->SetMBR(100, 100, 100, 100);
    ASSERT_EQ(0, createMap.PrepareNewObj(pt));
    ASSERT_EQ(0, createMap.CommitNewObj(pt));
    ASSERT_EQ(0, createMap.Close());

    // Append two 512-byte garbage blocks at end of file.
    // Make block 2 have an invalid type (lines 371-374) and next pointer past
    // EOF (lines 364-367).
    VSILFILE* fp = VSIFOpenL(garbMap.c_str(), "rb+");
    ASSERT_THAT(fp, NotNull());
    VSIFSeekL(fp, 0, SEEK_END);
    vsi_l_offset garbOffset1 = VSIFTellL(fp);
    GByte garbBlock1[512] = {};
    GUInt16 bType = TABMAP_GARB_BLOCK;  // 4
    GInt32 nextPtr = static_cast<GInt32>(garbOffset1 + 512);
    CPL_LSBPTR16(&bType);
    CPL_LSBPTR32(&nextPtr);
    memcpy(garbBlock1, &bType, 2);
    memcpy(garbBlock1 + 2, &nextPtr, 4);
    VSIFWriteL(garbBlock1, 1, 512, fp);

    GByte garbBlock2[512] = {};
    bType = 99;        // invalid block type
    nextPtr = 999999;  // past EOF
    CPL_LSBPTR16(&bType);
    CPL_LSBPTR32(&nextPtr);
    memcpy(garbBlock2, &bType, 2);
    memcpy(garbBlock2 + 2, &nextPtr, 4);
    VSIFWriteL(garbBlock2, 1, 512, fp);

    // Patch m_nFirstGarbageBlock in header at offset 0x134.
    VSIFSeekL(fp, 0x134, SEEK_SET);
    GInt32 firstGarb = static_cast<GInt32>(garbOffset1);
    CPL_LSBPTR32(&firstGarb);
    VSIFWriteL(&firstGarb, 4, 1, fp);
    VSIFCloseL(fp);

    TABMAPFile rwGarb("");
    EXPECT_EQ(-1, rwGarb.Open(garbMap.c_str(), TABReadWrite));
    CPLErrorReset();
  }
}

TEST(TABMAPFileTest, SpatialFilterTraversalAndDump) {
  const std::string mapPath = "/vsimem/map_filter.map";
  const std::string idPath = "/vsimem/map_filter.id";
  absl::Cleanup cleanupFiles = [mapPath, idPath]() {
    VSIUnlink(mapPath.c_str());
    VSIUnlink(idPath.c_str());
  };

  {
    TABMAPFile mapFile("");
    ASSERT_EQ(0, mapFile.Open(mapPath.c_str(), "w"));
    ASSERT_EQ(0, mapFile.SetCoordsysBounds(0.0, 0.0, 10000.0, 10000.0));

    for (int i = 1; i <= 550; ++i) {
      std::unique_ptr<TABMAPObjHdr> objHdr(
          TABMAPObjHdr::NewObj(TAB_GEOM_LINE, i));
      ASSERT_THAT(objHdr, NotNull());
      auto* line = cpl::down_cast<TABMAPObjLine*>(objHdr.get());
      line->m_nX1 = (i % 50) * 150;
      line->m_nY1 = (i / 50) * 150;
      line->m_nX2 = line->m_nX1 + 50;
      line->m_nY2 = line->m_nY1 + 50;
      line->m_nPenId = 1;
      line->SetMBR(line->m_nX1, line->m_nY1, line->m_nX2, line->m_nY2);

      ASSERT_EQ(0, mapFile.PrepareNewObj(line));
      ASSERT_EQ(0, mapFile.CommitNewObj(line));
    }
    ASSERT_EQ(0, mapFile.Close());
  }

  // Open in read mode.
  TABMAPFile readFile("");
  ASSERT_EQ(0, readFile.Open(mapPath.c_str(), "r"));

  // Traverse all features with spatial index.
  readFile.ResetReading();
  int count = 0;
  int featId = readFile.GetNextFeatureId(-1);
  while (featId > 0) {
    count++;
    featId = readFile.GetNextFeatureId(featId);
  }
  EXPECT_EQ(550, count);

  // Set tight coordinate filter.
  double minX = 0, minY = 0, maxX = 0, maxY = 0;
  ASSERT_EQ(0, readFile.Int2Coordsys(200, 200, minX, minY));
  ASSERT_EQ(0, readFile.Int2Coordsys(1200, 1200, maxX, maxY));
  TABVertex sMin = {minX, minY};
  TABVertex sMax = {maxX, maxY};
  readFile.SetCoordFilter(sMin, sMax);
  readFile.ResetReading();

  int filteredCount = 0;
  featId = readFile.GetNextFeatureId(-1);
  while (featId > 0) {
    filteredCount++;
    featId = readFile.GetNextFeatureId(featId);
  }
  EXPECT_THAT(filteredCount, Gt(0));
  EXPECT_THAT(filteredCount, ::testing::Lt(550));

  // Reset coordinate filter.
  readFile.ResetCoordFilter();
  readFile.ResetReading();
  count = 0;
  featId = readFile.GetNextFeatureId(-1);
  while (featId > 0) {
    count++;
    featId = readFile.GetNextFeatureId(featId);
  }
  EXPECT_EQ(550, count);

  // Test MoveToObjId error and edge cases.
  EXPECT_EQ(-1, readFile.MoveToObjId(0));
  CPLErrorReset();
  EXPECT_EQ(-1, readFile.MoveToObjId(-5));
  CPLErrorReset();
  EXPECT_EQ(-1, readFile.MoveToObjId(9999));
  CPLErrorReset();

  // Alternating MoveToObjId calls.
  EXPECT_EQ(0, readFile.MoveToObjId(5));
  EXPECT_EQ(5, readFile.GetCurObjId());
  EXPECT_EQ(0, readFile.MoveToObjId(25));
  EXPECT_EQ(25, readFile.GetCurObjId());
  EXPECT_EQ(0, readFile.MoveToObjId(5));
  EXPECT_EQ(5, readFile.GetCurObjId());

#ifdef DEBUG
  FILE* devNull = fopen("/dev/null", "w");
  if (devNull) {
    readFile.Dump(devNull);
    readFile.DumpSpatialIndexToMIF(nullptr, devNull, devNull);
    fclose(devNull);
  }
#endif

  EXPECT_EQ(0, readFile.Close());
}

TEST(TABMAPFileTest, MoveToObjIdReopenInWriteMode) {
  const std::string mapReopen = "/vsimem/map_reopen.map";
  const std::string idReopen = "/vsimem/map_reopen.id";
  absl::Cleanup cleanupReopen = [mapReopen, idReopen]() {
    VSIUnlink(mapReopen.c_str());
    VSIUnlink(idReopen.c_str());
  };
  TABMAPFile mFile("");
  ASSERT_EQ(0, mFile.Open(mapReopen.c_str(), "w"));
  ASSERT_EQ(0, mFile.SetCoordsysBounds(0.0, 0.0, 1000.0, 1000.0));
  std::unique_ptr<TABMAPObjHdr> ptHdr(TABMAPObjHdr::NewObj(TAB_GEOM_SYMBOL, 1));
  auto* pt = cpl::down_cast<TABMAPObjPoint*>(ptHdr.get());
  pt->m_nX = 100;
  pt->m_nY = 100;
  pt->SetMBR(100, 100, 100, 100);
  ASSERT_EQ(0, mFile.PrepareNewObj(pt));
  ASSERT_EQ(0, mFile.CommitNewObj(pt));

  mFile.ResetReading();
  // In TABWrite mode, MoveToObjId calls ReOpenReadWrite() (lines 1017-1019).
  EXPECT_EQ(0, mFile.MoveToObjId(1));
  EXPECT_EQ(1, mFile.GetCurObjId());
  EXPECT_EQ(0, mFile.Close());
}

TEST(TABMAPFileTest, ClusteredGuttmanSplit) {
  const std::string mapPath = "/vsimem/map_cluster.map";
  const std::string idPath = "/vsimem/map_cluster.id";
  absl::Cleanup cleanupFiles = [mapPath, idPath]() {
    VSIUnlink(mapPath.c_str());
    VSIUnlink(idPath.c_str());
  };

  TABMAPFile mapFile("");
  ASSERT_EQ(0, mapFile.Open(mapPath.c_str(), "w"));
  ASSERT_EQ(0, mapFile.SetQuickSpatialIndexMode(FALSE));
  EXPECT_EQ(0, mapFile.SetCoordsysBounds(0.0, 0.0, 100000.0, 100000.0));

  // Write 18 lines clustered at (100, 100).
  for (int i = 1; i <= 18; ++i) {
    std::unique_ptr<TABMAPObjHdr> objHdr(
        TABMAPObjHdr::NewObj(TAB_GEOM_LINE, i));
    ASSERT_THAT(objHdr, NotNull());
    auto* line = cpl::down_cast<TABMAPObjLine*>(objHdr.get());
    line->m_nX1 = 100 + i;
    line->m_nY1 = 100 + i;
    line->m_nX2 = 100 + i + 1;
    line->m_nY2 = 100 + i + 1;
    line->m_nPenId = 1;
    line->SetMBR(line->m_nX1, line->m_nY1, line->m_nX2, line->m_nY2);
    ASSERT_EQ(0, mapFile.PrepareNewObj(line));
    ASSERT_EQ(0, mapFile.CommitNewObj(line));
  }

  // Now write 1 line far away at (90000, 90000) that forces a split.
  // One seed will be at (100, 100) and the other at (90000, 90000).
  // All 17 other entries at (100, 100) prefer seed 1 and fill it up (lines
  // 2066-2069)!
  {
    std::unique_ptr<TABMAPObjHdr> objHdr(
        TABMAPObjHdr::NewObj(TAB_GEOM_LINE, 19));
    ASSERT_THAT(objHdr, NotNull());
    auto* line = cpl::down_cast<TABMAPObjLine*>(objHdr.get());
    line->m_nX1 = 90000;
    line->m_nY1 = 90000;
    line->m_nX2 = 90001;
    line->m_nY2 = 90001;
    line->m_nPenId = 1;
    line->SetMBR(line->m_nX1, line->m_nY1, line->m_nX2, line->m_nY2);
    ASSERT_EQ(0, mapFile.PrepareNewObj(line));
    ASSERT_EQ(0, mapFile.CommitNewObj(line));
  }

  // Write another cluster at (90000, 90000) to trigger the opposite block fill
  // (lines 2074-2077).
  for (int i = 20; i <= 38; ++i) {
    std::unique_ptr<TABMAPObjHdr> objHdr(
        TABMAPObjHdr::NewObj(TAB_GEOM_LINE, i));
    ASSERT_THAT(objHdr, NotNull());
    auto* line = cpl::down_cast<TABMAPObjLine*>(objHdr.get());
    line->m_nX1 = 90000 + (i - 20);
    line->m_nY1 = 90000 + (i - 20);
    line->m_nX2 = 90000 + (i - 20) + 1;
    line->m_nY2 = 90000 + (i - 20) + 1;
    line->m_nPenId = 1;
    line->SetMBR(line->m_nX1, line->m_nY1, line->m_nX2, line->m_nY2);
    ASSERT_EQ(0, mapFile.PrepareNewObj(line));
    ASSERT_EQ(0, mapFile.CommitNewObj(line));
  }

  EXPECT_EQ(0, mapFile.Close());
}

}  // namespace
}  // namespace autotest2
