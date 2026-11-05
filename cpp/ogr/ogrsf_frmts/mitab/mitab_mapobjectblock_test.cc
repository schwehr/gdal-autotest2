// Copyright 2024 Google LLC
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

#include <climits>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr const char* kTestDir = "/vsimem/mitab_mapobjectblock_test";

class TABMAPObjectBlockTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CPLErrorReset();
    VSIMkdir(kTestDir, 0777);
  }

  void TearDown() override {
    VSIRmdirRecursive(kTestDir);
    CPLErrorReset();
  }
};

TEST_F(TABMAPObjectBlockTest, UninitializedAndBasicOperations) {
  TABMAPObjectBlock block(TABRead);
  EXPECT_EQ(TABMAP_OBJECT_BLOCK, block.GetBlockClass());

  // Commit on uninitialized block fails.
  EXPECT_EQ(-1, block.CommitToFile());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // InitBlockFromData with wrong block type at non-zero offset.
  GByte badTypeBuf[512] = {0};
  badTypeBuf[0] =
      static_cast<GByte>(TABMAP_COORD_BLOCK);  // Expected 3 (OBJECT_BLOCK)
  EXPECT_EQ(-1,
            block.InitBlockFromData(badTypeBuf, 512, 512, TRUE, nullptr, 512));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // Invalid m_numDataBytes (< 0 or > 512 - 20)
  badTypeBuf[0] = static_cast<GByte>(TABMAP_OBJECT_BLOCK);
  badTypeBuf[2] = 0xFF;  // -1 as int16
  badTypeBuf[3] = 0xFF;
  EXPECT_EQ(-1,
            block.InitBlockFromData(badTypeBuf, 512, 512, TRUE, nullptr, 512));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  // m_numDataBytes + 20 > 512 (e.g. 500 + 20 = 520)
  badTypeBuf[2] = static_cast<GByte>(500 & 0xFF);
  badTypeBuf[3] = static_cast<GByte>((500 >> 8) & 0xFF);
  EXPECT_EQ(-1,
            block.InitBlockFromData(badTypeBuf, 512, 512, TRUE, nullptr, 512));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();
}

TEST_F(TABMAPObjectBlockTest, LifecycleHelpersAndMBRTracking) {
  const std::string path = std::string(kTestDir) + "/lifecycle.map";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);

  TABMAPObjectBlock block(TABReadWrite);
  ASSERT_EQ(0, block.InitNewBlock(fp, 512, 512));

  // MBR update and LockCenter
  EXPECT_EQ(0, block.UpdateMBR(100, 200));
  EXPECT_EQ(0, block.UpdateMBR(300, 400));
  GInt32 minX = 0, minY = 0, maxX = 0, maxY = 0;
  block.GetMBR(minX, minY, maxX, maxY);
  EXPECT_EQ(100, minX);
  EXPECT_EQ(200, minY);
  EXPECT_EQ(300, maxX);
  EXPECT_EQ(400, maxY);

  block.LockCenter();
  block.SetMBR(50, 60, 70, 80);
  block.GetMBR(minX, minY, maxX, maxY);
  EXPECT_EQ(50, minX);
  EXPECT_EQ(80, maxY);

  TABMAPObjectBlock otherBlock(TABReadWrite);
  ASSERT_EQ(0, otherBlock.InitNewBlock(fp, 512, 1024));
  otherBlock.UpdateMBR(1000, 2000);
  block.SetCenterFromOtherBlock(&otherBlock);

  // AddCoordBlockRef
  block.AddCoordBlockRef(2048);
  block.AddCoordBlockRef(4096);

  // Write and Read IntCoord (uncompressed & compressed)
  block.GotoByteInBlock(20);
  EXPECT_EQ(0, block.WriteIntCoord(500, 600, FALSE));
  EXPECT_EQ(0, block.WriteIntCoord(1020, 2030, TRUE));
  EXPECT_EQ(0, block.WriteIntMBRCoord(10, 20, 30, 40, FALSE));
  EXPECT_EQ(0, block.WriteIntMBRCoord(1010, 2010, 1020, 2020, TRUE));

  block.GotoByteInBlock(20);
  GInt32 rx = 0, ry = 0;
  EXPECT_EQ(0, block.ReadIntCoord(FALSE, rx, ry));
  EXPECT_EQ(500, rx);
  EXPECT_EQ(600, ry);

  EXPECT_EQ(0, block.ReadIntCoord(TRUE, rx, ry));
  EXPECT_EQ(1020, rx);
  EXPECT_EQ(2030, ry);

  // Commit and clear
  EXPECT_EQ(0, block.CommitToFile());
  block.ClearObjects();
  block.Rewind();

  VSIFCloseL(fp);
}

TEST_F(TABMAPObjectBlockTest, AllGeometryTypesRoundTrip) {
  const std::string path = std::string(kTestDir) + "/geoms.map";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);

  // Initialize header block at offset 0 so object sizes can be computed.
  TABMAPHeaderBlock header(TABReadWrite);
  ASSERT_EQ(0, header.InitNewBlock(fp, 512, 0));
  ASSERT_EQ(0, header.CommitToFile());

  // Helper lambda to write an object, rewind, and read it back.
  auto roundTripTest = [&](TABGeomType geomType, auto setupFn, auto verifyFn) {
    TABMAPObjectBlock block(TABReadWrite);
    ASSERT_EQ(0, block.InitNewBlock(fp, 512, 512));

    std::unique_ptr<TABMAPObjHdr> obj(TABMAPObjHdr::NewObj(geomType, 101));
    ASSERT_NE(nullptr, obj);
    setupFn(obj.get());

    int startAddr = block.PrepareNewObject(obj.get());
    EXPECT_GE(startAddr, 512);
    EXPECT_EQ(0, block.CommitNewObject(obj.get()));
    EXPECT_EQ(0, block.CommitToFile());

    TABMAPObjectBlock readBlock(TABRead);
    ASSERT_EQ(0, readBlock.ReadFromFile(fp, 512, 512));

    std::unique_ptr<TABMAPObjHdr> readObj(
        TABMAPObjHdr::ReadNextObj(&readBlock, &header));
    ASSERT_NE(nullptr, readObj);
    EXPECT_EQ(101, readObj->m_nId);
    EXPECT_EQ(geomType, readObj->m_nType);
    verifyFn(readObj.get());
  };

  // 1. None
  {
    std::unique_ptr<TABMAPObjHdr> noneObj(
        TABMAPObjHdr::NewObj(TAB_GEOM_NONE, 1));
    ASSERT_NE(nullptr, noneObj);
    TABMAPObjectBlock block(TABReadWrite);
    ASSERT_EQ(0, block.InitNewBlock(fp, 512, 512));
    EXPECT_EQ(0, block.PrepareNewObject(noneObj.get()));
  }

  // 2. Point (Symbol) - uncompressed and compressed
  roundTripTest(
      TAB_GEOM_SYMBOL,
      [](TABMAPObjHdr* o) {
        auto* p = static_cast<TABMAPObjPoint*>(o);
        p->m_nX = 100;
        p->m_nY = 200;
        p->m_nSymbolId = 5;
        p->SetMBR(100, 200, 100, 200);
      },
      [](TABMAPObjHdr* o) {
        auto* p = static_cast<TABMAPObjPoint*>(o);
        EXPECT_EQ(100, p->m_nX);
        EXPECT_EQ(200, p->m_nY);
        EXPECT_EQ(5, p->m_nSymbolId);
      });

  roundTripTest(
      TAB_GEOM_SYMBOL_C,
      [](TABMAPObjHdr* o) {
        auto* p = static_cast<TABMAPObjPoint*>(o);
        p->m_nX = 150;
        p->m_nY = 250;
        p->m_nSymbolId = 7;
        p->SetMBR(150, 250, 150, 250);
      },
      [](TABMAPObjHdr* o) {
        auto* p = static_cast<TABMAPObjPoint*>(o);
        EXPECT_EQ(150, p->m_nX);
        EXPECT_EQ(250, p->m_nY);
        EXPECT_EQ(7, p->m_nSymbolId);
      });

  // 3. Font Point
  roundTripTest(
      TAB_GEOM_FONTSYMBOL,
      [](TABMAPObjHdr* o) {
        auto* p = static_cast<TABMAPObjFontPoint*>(o);
        p->m_nX = 300;
        p->m_nY = 400;
        p->m_nSymbolId = 12;
        p->m_nPointSize = 10;
        p->m_nFontStyle = 1;
        p->m_nR = 255;
        p->m_nG = 0;
        p->m_nB = 128;
        p->m_nAngle = 450;
        p->m_nFontId = 2;
        p->SetMBR(300, 400, 300, 400);
      },
      [](TABMAPObjHdr* o) {
        auto* p = static_cast<TABMAPObjFontPoint*>(o);
        EXPECT_EQ(300, p->m_nX);
        EXPECT_EQ(400, p->m_nY);
        EXPECT_EQ(12, p->m_nSymbolId);
        EXPECT_EQ(450, p->m_nAngle);
      });

  // 4. Custom Point
  roundTripTest(
      TAB_GEOM_CUSTOMSYMBOL,
      [](TABMAPObjHdr* o) {
        auto* p = static_cast<TABMAPObjCustomPoint*>(o);
        p->m_nX = 500;
        p->m_nY = 600;
        p->m_nUnknown_ = 0;
        p->m_nCustomStyle = 1;
        p->m_nSymbolId = 3;
        p->m_nFontId = 4;
        p->SetMBR(500, 600, 500, 600);
      },
      [](TABMAPObjHdr* o) {
        auto* p = static_cast<TABMAPObjCustomPoint*>(o);
        EXPECT_EQ(500, p->m_nX);
        EXPECT_EQ(600, p->m_nY);
        EXPECT_EQ(1, p->m_nCustomStyle);
        EXPECT_EQ(3, p->m_nSymbolId);
      });

  // 5. Line
  roundTripTest(
      TAB_GEOM_LINE,
      [](TABMAPObjHdr* o) {
        auto* l = static_cast<TABMAPObjLine*>(o);
        l->m_nX1 = 10;
        l->m_nY1 = 20;
        l->m_nX2 = 30;
        l->m_nY2 = 40;
        l->m_nPenId = 8;
        l->SetMBR(10, 20, 30, 40);
      },
      [](TABMAPObjHdr* o) {
        auto* l = static_cast<TABMAPObjLine*>(o);
        EXPECT_EQ(10, l->m_nX1);
        EXPECT_EQ(20, l->m_nY1);
        EXPECT_EQ(30, l->m_nX2);
        EXPECT_EQ(40, l->m_nY2);
        EXPECT_EQ(8, l->m_nPenId);
      });

  // 6. PLine, Region, MultiLine, V450, V800
  const TABGeomType plineTypes[] = {
      TAB_GEOM_PLINE,           TAB_GEOM_PLINE_C,
      TAB_GEOM_REGION,          TAB_GEOM_REGION_C,
      TAB_GEOM_MULTIPLINE,      TAB_GEOM_MULTIPLINE_C,
      TAB_GEOM_V450_REGION,     TAB_GEOM_V450_REGION_C,
      TAB_GEOM_V450_MULTIPLINE, TAB_GEOM_V450_MULTIPLINE_C,
      TAB_GEOM_V800_REGION,     TAB_GEOM_V800_REGION_C,
      TAB_GEOM_V800_MULTIPLINE, TAB_GEOM_V800_MULTIPLINE_C};

  for (TABGeomType t : plineTypes) {
    roundTripTest(
        t,
        [](TABMAPObjHdr* o) {
          auto* pl = static_cast<TABMAPObjPLine*>(o);
          pl->m_nCoordBlockPtr = 1024;
          pl->m_nCoordDataSize = 100;
          pl->m_bSmooth = TRUE;
          pl->m_numLineSections = 2;
          pl->m_nLabelX = 50;
          pl->m_nLabelY = 60;
          pl->m_nComprOrgX = 10;
          pl->m_nComprOrgY = 20;
          pl->m_nMinX = 10;
          pl->m_nMinY = 20;
          pl->m_nMaxX = 90;
          pl->m_nMaxY = 100;
          pl->m_nPenId = 3;
          pl->m_nBrushId = 4;
          pl->SetMBR(10, 20, 90, 100);
        },
        [](TABMAPObjHdr* o) {
          auto* pl = static_cast<TABMAPObjPLine*>(o);
          EXPECT_EQ(1024, pl->m_nCoordBlockPtr);
          EXPECT_EQ(100, pl->m_nCoordDataSize);
          EXPECT_TRUE(pl->m_bSmooth);
          EXPECT_EQ(3, pl->m_nPenId);
        });
  }

  // 7. Arc
  roundTripTest(
      TAB_GEOM_ARC,
      [](TABMAPObjHdr* o) {
        auto* a = static_cast<TABMAPObjArc*>(o);
        a->m_nStartAngle = 0;
        a->m_nEndAngle = 90;
        a->m_nArcEllipseMinX = 10;
        a->m_nArcEllipseMinY = 20;
        a->m_nArcEllipseMaxX = 100;
        a->m_nArcEllipseMaxY = 200;
        a->m_nMinX = 10;
        a->m_nMinY = 20;
        a->m_nMaxX = 100;
        a->m_nMaxY = 200;
        a->m_nPenId = 5;
        a->SetMBR(10, 20, 100, 200);
      },
      [](TABMAPObjHdr* o) {
        auto* a = static_cast<TABMAPObjArc*>(o);
        EXPECT_EQ(0, a->m_nStartAngle);
        EXPECT_EQ(90, a->m_nEndAngle);
        EXPECT_EQ(5, a->m_nPenId);
      });

  // 8. Rect, RoundRect, Ellipse
  const TABGeomType rectTypes[] = {TAB_GEOM_RECT,      TAB_GEOM_RECT_C,
                                   TAB_GEOM_ROUNDRECT, TAB_GEOM_ROUNDRECT_C,
                                   TAB_GEOM_ELLIPSE,   TAB_GEOM_ELLIPSE_C};

  for (TABGeomType t : rectTypes) {
    roundTripTest(
        t,
        [](TABMAPObjHdr* o) {
          auto* r = static_cast<TABMAPObjRectEllipse*>(o);
          r->m_nCornerWidth = 15;
          r->m_nCornerHeight = 25;
          r->m_nMinX = 10;
          r->m_nMinY = 20;
          r->m_nMaxX = 100;
          r->m_nMaxY = 200;
          r->m_nPenId = 2;
          r->m_nBrushId = 3;
          r->SetMBR(10, 20, 100, 200);
        },
        [](TABMAPObjHdr* o) {
          auto* r = static_cast<TABMAPObjRectEllipse*>(o);
          EXPECT_EQ(10, r->m_nMinX);
          EXPECT_EQ(200, r->m_nMaxY);
          EXPECT_EQ(2, r->m_nPenId);
        });
  }

  // 9. Text
  roundTripTest(
      TAB_GEOM_TEXT,
      [](TABMAPObjHdr* o) {
        auto* txt = static_cast<TABMAPObjText*>(o);
        txt->m_nCoordBlockPtr = 1024;
        txt->m_nCoordDataSize = 12;
        txt->m_nTextAlignment = 1;
        txt->m_nAngle = 180;
        txt->m_nFontStyle = 0;
        txt->m_nFGColorR = 0;
        txt->m_nFGColorG = 0;
        txt->m_nFGColorB = 0;
        txt->m_nBGColorR = 255;
        txt->m_nBGColorG = 255;
        txt->m_nBGColorB = 255;
        txt->m_nLineEndX = 50;
        txt->m_nLineEndY = 60;
        txt->m_nHeight = 14;
        txt->m_nFontId = 1;
        txt->m_nMinX = 10;
        txt->m_nMinY = 20;
        txt->m_nMaxX = 80;
        txt->m_nMaxY = 90;
        txt->m_nPenId = 1;
        txt->SetMBR(10, 20, 80, 90);
      },
      [](TABMAPObjHdr* o) {
        auto* txt = static_cast<TABMAPObjText*>(o);
        EXPECT_EQ(1024, txt->m_nCoordBlockPtr);
        EXPECT_EQ(12, txt->m_nCoordDataSize);
        EXPECT_EQ(180, txt->m_nAngle);
        EXPECT_EQ(14, txt->m_nHeight);
      });

  // 10. MultiPoint
  const TABGeomType mpointTypes[] = {TAB_GEOM_MULTIPOINT, TAB_GEOM_MULTIPOINT_C,
                                     TAB_GEOM_V800_MULTIPOINT,
                                     TAB_GEOM_V800_MULTIPOINT_C};

  for (TABGeomType t : mpointTypes) {
    roundTripTest(
        t,
        [](TABMAPObjHdr* o) {
          auto* mp = static_cast<TABMAPObjMultiPoint*>(o);
          mp->m_nCoordBlockPtr = 2048;
          mp->m_nNumPoints = 5;
          mp->m_nSymbolId = 3;
          mp->m_nLabelX = 100;
          mp->m_nLabelY = 200;
          mp->m_nComprOrgX = 50;
          mp->m_nComprOrgY = 50;
          mp->m_nMinX = 50;
          mp->m_nMinY = 60;
          mp->m_nMaxX = 150;
          mp->m_nMaxY = 250;
          mp->SetMBR(50, 60, 150, 250);
        },
        [](TABMAPObjHdr* o) {
          auto* mp = static_cast<TABMAPObjMultiPoint*>(o);
          EXPECT_EQ(2048, mp->m_nCoordBlockPtr);
          EXPECT_EQ(5, mp->m_nNumPoints);
          EXPECT_EQ(3, mp->m_nSymbolId);
        });
  }

  // 11. Collection
  const TABGeomType collTypes[] = {TAB_GEOM_COLLECTION, TAB_GEOM_COLLECTION_C,
                                   TAB_GEOM_V800_COLLECTION,
                                   TAB_GEOM_V800_COLLECTION_C};

  for (TABGeomType t : collTypes) {
    roundTripTest(
        t,
        [](TABMAPObjHdr* o) {
          auto* c = static_cast<TABMAPObjCollection*>(o);
          c->m_nCoordBlockPtr = 4096;
          c->m_nNumMultiPoints = 2;
          c->m_nRegionDataSize = 10;
          c->m_nPolylineDataSize = 10;
          c->m_nNumRegSections = 1;
          c->m_nNumPLineSections = 1;
          c->m_nMultiPointSymbolId = 7;
          c->m_nRegionPenId = 2;
          c->m_nPolylinePenId = 3;
          c->m_nRegionBrushId = 4;
          c->m_nComprOrgX = 10;
          c->m_nComprOrgY = 20;
          c->m_nMinX = 10;
          c->m_nMinY = 20;
          c->m_nMaxX = 200;
          c->m_nMaxY = 300;
          c->SetMBR(10, 20, 200, 300);
        },
        [](TABMAPObjHdr* o) {
          auto* c = static_cast<TABMAPObjCollection*>(o);
          EXPECT_EQ(4096, c->m_nCoordBlockPtr);
          EXPECT_EQ(2, c->m_nNumMultiPoints);
          EXPECT_EQ(10, c->m_nRegionDataSize);
          EXPECT_EQ(10, c->m_nPolylineDataSize);
          EXPECT_EQ(1, c->m_nNumRegSections);
          EXPECT_EQ(1, c->m_nNumPLineSections);
          EXPECT_EQ(7, c->m_nMultiPointSymbolId);
        });
  }

  VSIFCloseL(fp);
}

TEST_F(TABMAPObjectBlockTest, DeletedObjectAndErrorBranches) {
  const std::string path = std::string(kTestDir) + "/deleted.map";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);

  TABMAPHeaderBlock header(TABReadWrite);
  ASSERT_EQ(0, header.InitNewBlock(fp, 512, 0));
  ASSERT_EQ(0, header.CommitToFile());

  // 1. AdvanceToNextObject skips deleted object (bit 0x40000000)
  TABMAPObjectBlock block(TABReadWrite);
  ASSERT_EQ(0, block.InitNewBlock(fp, 512, 512));

  // Write a deleted object: ID with 0x40000000
  std::unique_ptr<TABMAPObjPoint> delPoint(static_cast<TABMAPObjPoint*>(
      TABMAPObjHdr::NewObj(TAB_GEOM_SYMBOL, 1 | 0x40000000)));
  delPoint->m_nX = 10;
  delPoint->m_nY = 20;
  delPoint->m_nSymbolId = 1;
  delPoint->SetMBR(10, 20, 10, 20);
  block.PrepareNewObject(delPoint.get());
  EXPECT_EQ(0, block.CommitNewObject(delPoint.get()));

  // Followed by an active object
  std::unique_ptr<TABMAPObjPoint> activePoint(
      static_cast<TABMAPObjPoint*>(TABMAPObjHdr::NewObj(TAB_GEOM_SYMBOL, 2)));
  activePoint->m_nX = 30;
  activePoint->m_nY = 40;
  activePoint->m_nSymbolId = 2;
  activePoint->SetMBR(30, 40, 30, 40);
  block.PrepareNewObject(activePoint.get());
  EXPECT_EQ(0, block.CommitNewObject(activePoint.get()));

  EXPECT_EQ(0, block.CommitToFile());

  // Read back: first AdvanceToNextObject automatically skips object 1 and
  // returns object 2!
  TABMAPObjectBlock readBlock(TABRead);
  ASSERT_EQ(0, readBlock.ReadFromFile(fp, 512, 512));
  std::unique_ptr<TABMAPObjHdr> readObj(
      TABMAPObjHdr::ReadNextObj(&readBlock, &header));
  ASSERT_NE(nullptr, readObj);
  EXPECT_EQ(2, readObj->m_nId);

  // 2. Unsupported object type in NewObj
  EXPECT_EQ(nullptr, TABMAPObjHdr::NewObj(static_cast<TABGeomType>(3)));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  CPLErrorReset();

  VSIFCloseL(fp);
}

TEST_F(TABMAPObjectBlockTest, DumpBlock) {
#ifdef DEBUG
  const std::string path = std::string(kTestDir) + "/dump.map";
  VSILFILE* fp = VSIFOpenL(path.c_str(), "wb+");
  ASSERT_NE(nullptr, fp);

  TABMAPHeaderBlock header(TABReadWrite);
  header.InitNewBlock(fp, 512, 0);
  header.CommitToFile();

  TABMAPObjectBlock block(TABReadWrite);
  block.Dump(nullptr, FALSE);

  block.InitNewBlock(fp, 512, 512);
  block.Dump(nullptr, FALSE);
  block.Dump(nullptr, TRUE);

  VSIFCloseL(fp);
#endif
}

}  // namespace
}  // namespace autotest2
