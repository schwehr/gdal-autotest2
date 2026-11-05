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

#include <cmath>
#include <memory>
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

using ::testing::DoubleNear;

TEST(MitabMapHeaderBlockTest, UninitializedOperations) {
  delete new TABMAPHeaderBlock(TABRead);

  TABMAPHeaderBlock block(TABRead);
  double dX = 0.0;
  double dY = 0.0;
  GInt32 nX = 0;
  GInt32 nY = 0;
  TABProjInfo proj{};

  EXPECT_EQ(-1, block.CommitToFile());
  EXPECT_EQ(-1, block.Int2Coordsys(10, 10, dX, dY));
  EXPECT_EQ(-1, block.Coordsys2Int(10.0, 10.0, nX, nY));
  EXPECT_EQ(-1, block.ComprInt2Coordsys(10, 10, 1, 1, dX, dY));
  EXPECT_EQ(-1, block.Int2CoordsysDist(10, 10, dX, dY));
  EXPECT_EQ(-1, block.Coordsys2IntDist(10.0, 10.0, nX, nY));
  EXPECT_EQ(-1, block.GetProjInfo(&proj));
  EXPECT_EQ(-1, block.SetProjInfo(&proj));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, block.GetMapObjectSize(1));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
    EXPECT_FALSE(block.MapObjectUsesCoordBlock(1));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST(MitabMapHeaderBlockTest, InitBlockErrorsAndVersions) {
  // Invalid magic cookie
  {
    std::vector<GByte> buf(512, 0);
    TABMAPHeaderBlock block(TABRead);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, block.InitBlockFromData(buf.data(), 512, 512));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Invalid block size (< 512)
  {
    std::vector<GByte> buf(512, 0);
    // Magic cookie 42424242 (0x0287474a) at offset 0x100
    GInt32 cookie = 42424242;
    memcpy(buf.data() + 0x100, &cookie, 4);
    GInt16 version = 500;
    memcpy(buf.data() + 0x104, &version, 2);
    GInt16 blk_size = 256;  // invalid
    memcpy(buf.data() + 0x106, &blk_size, 2);

    TABMAPHeaderBlock block(TABRead);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, block.InitBlockFromData(buf.data(), 512, 512));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Null XScale/YScale
  {
    std::vector<GByte> buf(512, 0);
    GInt32 cookie = 42424242;
    memcpy(buf.data() + 0x100, &cookie, 4);
    GInt16 version = 500;
    memcpy(buf.data() + 0x104, &version, 2);
    GInt16 blk_size = 512;
    memcpy(buf.data() + 0x106, &blk_size, 2);

    TABMAPHeaderBlock block(TABRead);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, block.InitBlockFromData(buf.data(), 512, 512));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Version <= 100 with corrupted MBR
  {
    std::vector<GByte> buf(512, 0);
    GInt32 cookie = 42424242;
    memcpy(buf.data() + 0x100, &cookie, 4);
    GInt16 version = 100;
    memcpy(buf.data() + 0x104, &version, 2);
    GInt16 blk_size = 512;
    memcpy(buf.data() + 0x106, &blk_size, 2);
    // Corrupted MBR: MinX > MaxX
    GInt32 min_val = 1000;
    GInt32 max_val = 100;
    memcpy(buf.data() + 0x110, &min_val, 4);
    memcpy(buf.data() + 0x118, &max_val, 4);
    // Set scale at 0x170 and 0x178 to non-zero so validation passes
    double scale = 1.0;
    memcpy(buf.data() + 0x170, &scale, 8);
    memcpy(buf.data() + 0x178, &scale, 8);
    buf[0x160] = 3;  // m_nCoordPrecision

    TABMAPHeaderBlock block(TABRead);
    EXPECT_EQ(0, block.InitBlockFromData(buf.data(), 512, 512));
    EXPECT_EQ(100, block.m_nMAPVersionNumber);
    // Precision 3 -> scale pow(10, 3) = 1000
    EXPECT_EQ(1000.0, block.m_XScale);
    EXPECT_EQ(1000.0, block.m_YScale);
  }

  // Version 200 (datum params cleared)
  {
    std::vector<GByte> buf(512, 0);
    GInt32 cookie = 42424242;
    memcpy(buf.data() + 0x100, &cookie, 4);
    GInt16 version = 200;
    memcpy(buf.data() + 0x104, &version, 2);
    GInt16 blk_size = 512;
    memcpy(buf.data() + 0x106, &blk_size, 2);
    double scale = 1.0;
    memcpy(buf.data() + 0x170, &scale, 8);
    memcpy(buf.data() + 0x178, &scale, 8);

    TABMAPHeaderBlock block(TABRead);
    EXPECT_EQ(0, block.InitBlockFromData(buf.data(), 512, 512));
    EXPECT_EQ(200, block.m_nMAPVersionNumber);
    TABProjInfo proj{};
    EXPECT_EQ(0, block.GetProjInfo(&proj));
    EXPECT_EQ(0.0, proj.adDatumParams[0]);
  }
}

TEST(MitabMapHeaderBlockTest, InitNewBlockAndCommitRoundTrip) {
  CPLErrorReset();
  constexpr char kPath[] = "/vsimem/maphdr_test/header.map";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/maphdr_test"); };
  VSIMkdir("/vsimem/maphdr_test", 0777);

  VSILFILE* fp = VSIFOpenL(kPath, "w+b");
  ASSERT_NE(nullptr, fp);
  absl::Cleanup fp_closer = [fp] { EXPECT_EQ(0, VSIFCloseL(fp)); };

  // Error: affine params with version < 500
  {
    TABMAPHeaderBlock write_block(TABWrite);
    EXPECT_EQ(0, write_block.InitNewBlock(fp, 512, 0));
    write_block.m_nMAPVersionNumber = 400;
    TABProjInfo proj{};
    write_block.GetProjInfo(&proj);
    proj.nAffineFlag = 1;
    write_block.SetProjInfo(&proj);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, write_block.CommitToFile());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Write valid header with version 500, affine params, and projId 35
  {
    TABMAPHeaderBlock write_block(TABWrite);
    EXPECT_EQ(0, write_block.InitNewBlock(fp, 512, 0));
    write_block.m_nMAPVersionNumber = 500;
    TABProjInfo proj{};
    write_block.GetProjInfo(&proj);
    proj.nDatumId = 100;
    proj.nProjId = 35;
    proj.adProjParams[6] = 12345.678;

    // Affine params
    proj.nAffineFlag = 1;
    proj.nAffineUnits = 7;
    proj.dAffineParamA = 1.1;
    proj.dAffineParamB = 2.2;
    proj.dAffineParamC = 3.3;
    proj.dAffineParamD = 4.4;
    proj.dAffineParamE = 5.5;
    proj.dAffineParamF = 6.6;
    write_block.SetProjInfo(&proj);

    EXPECT_EQ(0, write_block.SetCoordsysBounds(-180.0, -90.0, 180.0, 90.0));
    EXPECT_EQ(0, write_block.CommitToFile());
  }

  VSIFFlushL(fp);

  // Read back and verify
  {
    TABMAPHeaderBlock read_block(TABRead);
    EXPECT_EQ(0, read_block.ReadFromFile(fp, 0, 1024));
    EXPECT_EQ(500, read_block.m_nMAPVersionNumber);
    TABProjInfo proj{};
    EXPECT_EQ(0, read_block.GetProjInfo(&proj));
    EXPECT_EQ(100, proj.nDatumId);
    EXPECT_EQ(35, proj.nProjId);
    EXPECT_DOUBLE_EQ(12345.678, proj.adProjParams[6]);
    EXPECT_EQ(1, proj.nAffineFlag);
    EXPECT_DOUBLE_EQ(1.1, proj.dAffineParamA);
    EXPECT_DOUBLE_EQ(6.6, proj.dAffineParamF);
  }

  // Also test ProjId 35 without affine params (triggers m_nCurPos == 512 zero
  // write)
  {
    TABMAPHeaderBlock write_block(TABWrite);
    EXPECT_EQ(0, write_block.InitNewBlock(fp, 512, 0));
    write_block.m_nMAPVersionNumber = 500;
    TABProjInfo proj{};
    write_block.GetProjInfo(&proj);
    proj.nProjId = 35;
    proj.nAffineFlag = 0;
    proj.adProjParams[6] = 999.0;
    write_block.SetProjInfo(&proj);
    EXPECT_EQ(0, write_block.CommitToFile());

    TABMAPHeaderBlock read_block(TABRead);
    EXPECT_EQ(0, read_block.ReadFromFile(fp, 0, 1024));
    EXPECT_EQ(0, read_block.GetProjInfo(&proj));
    EXPECT_DOUBLE_EQ(999.0, proj.adProjParams[6]);
  }
}

TEST(MitabMapHeaderBlockTest, CoordinateConversionsAndQuadrants) {
  CPLErrorReset();
  TABMAPHeaderBlock block(TABRead);
  std::vector<GByte> buf(512, 0);
  GInt32 cookie = 42424242;
  memcpy(buf.data() + 0x100, &cookie, 4);
  GInt16 version = 500;
  memcpy(buf.data() + 0x104, &version, 2);
  GInt16 blk_size = 512;
  memcpy(buf.data() + 0x106, &blk_size, 2);
  double scale = 1000.0;
  memcpy(buf.data() + 0x170, &scale, 8);
  memcpy(buf.data() + 0x178, &scale, 8);
  buf[1] = 0x0a;
  buf[7] = 0xa2;  // 0x80 | 0x22

  EXPECT_EQ(0, block.InitBlockFromData(buf.data(), 512, 512));

  // Test SetCoordsysBounds with 0-width and 0-height
  EXPECT_EQ(0, block.SetCoordsysBounds(100.0, 200.0, 100.0, 200.0));

  // Test SetCoordsysBounds normal
  EXPECT_EQ(0, block.SetCoordsysBounds(-1000.0, -1000.0, 1000.0, 1000.0));

  // Test all quadrants: 1, 2, 3, 4, 0
  for (GByte q : {1, 2, 3, 4, 0}) {
    block.m_nCoordOriginQuadrant = q;
    GInt32 nX = 0;
    GInt32 nY = 0;
    EXPECT_EQ(0, block.Coordsys2Int(500.0, 500.0, nX, nY));
    double dX = 0.0;
    double dY = 0.0;
    EXPECT_EQ(0, block.Int2Coordsys(nX, nY, dX, dY));
    EXPECT_NEAR(500.0, dX, 0.01);
    EXPECT_NEAR(500.0, dY, 0.01);
  }

  // Precision rounding
  block.m_nCoordOriginQuadrant = 1;
  block.m_XPrecision = 10.0;
  block.m_YPrecision = 10.0;
  double dX = 0.0;
  double dY = 0.0;
  EXPECT_EQ(0, block.Int2Coordsys(1234, 5678, dX, dY));

  // Compressed coordinates
  EXPECT_EQ(0, block.ComprInt2Coordsys(1000, 1000, 50, 50, dX, dY));

  // Distance conversions
  EXPECT_EQ(0, block.Int2CoordsysDist(1000000, 1000000, dX, dY));
  GInt32 nDistX = 0;
  GInt32 nDistY = 0;
  EXPECT_EQ(0, block.Coordsys2IntDist(dX, dY, nDistX, nDistY));
  EXPECT_EQ(1000000, nDistX);
  EXPECT_EQ(1000000, nDistY);

  // Clamping / bounds overflow
  GInt32 nClampedX = 0;
  GInt32 nClampedY = 0;
  EXPECT_EQ(0, block.Coordsys2Int(1e12, 1e12, nClampedX, nClampedY, FALSE));
  EXPECT_EQ(1000000000, nClampedX);
  EXPECT_EQ(1000000000, nClampedY);
  EXPECT_TRUE(block.m_bIntBoundsOverflow);

  EXPECT_EQ(0, block.Coordsys2Int(-1e12, -1e12, nClampedX, nClampedY, TRUE));
  EXPECT_EQ(-1000000000, nClampedX);
  EXPECT_EQ(-1000000000, nClampedY);

  // Object sizes and coord block flags
  EXPECT_EQ(0x0a, block.GetMapObjectSize(1));
  EXPECT_FALSE(block.MapObjectUsesCoordBlock(1));
  EXPECT_TRUE(block.MapObjectUsesCoordBlock(7));  // 0xa2 has 0x80 bit set

  // Invalid object types (< 0 and > 255)
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, block.GetMapObjectSize(-1));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
    EXPECT_EQ(-1, block.GetMapObjectSize(256));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
    EXPECT_FALSE(block.MapObjectUsesCoordBlock(-1));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
    EXPECT_FALSE(block.MapObjectUsesCoordBlock(256));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // GetProjInfo and SetProjInfo
  TABProjInfo proj{};
  EXPECT_EQ(0, block.GetProjInfo(nullptr));
  EXPECT_EQ(0, block.SetProjInfo(nullptr));
  EXPECT_EQ(0, block.GetProjInfo(&proj));
  EXPECT_EQ(0, block.SetProjInfo(&proj));
}

}  // namespace
}  // namespace autotest2
