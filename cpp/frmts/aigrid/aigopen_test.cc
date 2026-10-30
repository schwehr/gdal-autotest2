// Tests for aigopen:
//   https://github.com/OSGeo/gdal/blob/master/frmts/aigrid/aigopen.c
//
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

#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/gdal/frmts/aigrid/aigrid.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

void WriteBE32(std::vector<GByte>& buf, size_t offset, uint32_t val) {
  buf[offset + 0] = static_cast<GByte>((val >> 24) & 0xFF);
  buf[offset + 1] = static_cast<GByte>((val >> 16) & 0xFF);
  buf[offset + 2] = static_cast<GByte>((val >> 8) & 0xFF);
  buf[offset + 3] = static_cast<GByte>(val & 0xFF);
}

void WriteBEDouble(std::vector<GByte>& buf, size_t offset, double val) {
  uint64_t bits = 0;
  std::memcpy(&bits, &val, sizeof(bits));
  buf[offset + 0] = static_cast<GByte>((bits >> 56) & 0xFF);
  buf[offset + 1] = static_cast<GByte>((bits >> 48) & 0xFF);
  buf[offset + 2] = static_cast<GByte>((bits >> 40) & 0xFF);
  buf[offset + 3] = static_cast<GByte>((bits >> 32) & 0xFF);
  buf[offset + 4] = static_cast<GByte>((bits >> 24) & 0xFF);
  buf[offset + 5] = static_cast<GByte>((bits >> 16) & 0xFF);
  buf[offset + 6] = static_cast<GByte>((bits >> 8) & 0xFF);
  buf[offset + 7] = static_cast<GByte>(bits & 0xFF);
}

void WriteMemFile(const std::string& path, const std::vector<GByte>& data) {
  const size_t alloc_size = data.empty() ? 1 : data.size();
  GByte* buf = static_cast<GByte*>(CPLMalloc(alloc_size));
  if (!data.empty()) {
    std::memcpy(buf, data.data(), data.size());
  }
  VSILFILE* fp = VSIFileFromMemBuffer(path.c_str(), buf, data.size(), TRUE);
  ASSERT_NE(nullptr, fp) << path;
  ASSERT_EQ(0, VSIFCloseL(fp));
}

struct HeaderSpec {
  int32_t cell_type = AIG_CELLTYPE_INT;
  int32_t compressed_hdr_flag = 0;  // 0 -> compressed, 1 -> uncompressed
  double cell_size_x = 1.0;
  double cell_size_y = 1.0;
  int32_t blocks_per_row = 1;
  int32_t blocks_per_col = 1;
  int32_t block_x_size = 2;
  int32_t block_y_size = 2;
};

std::vector<GByte> MakeHdr(const HeaderSpec& spec) {
  std::vector<GByte> hdr(308, 0);
  WriteBE32(hdr, 16, static_cast<uint32_t>(spec.cell_type));
  WriteBE32(hdr, 20, static_cast<uint32_t>(spec.compressed_hdr_flag));
  WriteBEDouble(hdr, 256, spec.cell_size_x);
  WriteBEDouble(hdr, 264, spec.cell_size_y);
  WriteBE32(hdr, 288, static_cast<uint32_t>(spec.blocks_per_row));
  WriteBE32(hdr, 292, static_cast<uint32_t>(spec.blocks_per_col));
  WriteBE32(hdr, 296, static_cast<uint32_t>(spec.block_x_size));
  WriteBE32(hdr, 304, static_cast<uint32_t>(spec.block_y_size));
  return hdr;
}

std::vector<GByte> MakeBounds(double llx, double lly, double urx, double ury) {
  std::vector<GByte> bnd(32, 0);
  WriteBEDouble(bnd, 0, llx);
  WriteBEDouble(bnd, 8, lly);
  WriteBEDouble(bnd, 16, urx);
  WriteBEDouble(bnd, 24, ury);
  return bnd;
}

std::vector<GByte> MakeStats(double min_v, double max_v, double mean_v,
                             double stddev_v) {
  std::vector<GByte> sta(32, 0);
  WriteBEDouble(sta, 0, min_v);
  WriteBEDouble(sta, 8, max_v);
  WriteBEDouble(sta, 16, mean_v);
  WriteBEDouble(sta, 24, stddev_v);
  return sta;
}

// Builds a single-block tile data file + index file for uncompressed 32-bit
// int or 32-bit float blocks.
void WriteSingleBlockTile(const std::string& dir, const std::string& basename,
                          const std::vector<uint32_t>& be_words) {
  const uint32_t payload_bytes = static_cast<uint32_t>(be_words.size() * 4);
  // Tile file: 100-byte header + 2-byte block size (in words) + payload.
  std::vector<GByte> tile(100 + 2 + payload_bytes, 0);
  tile[100] = static_cast<GByte>(((payload_bytes / 2) >> 8) & 0xFF);
  tile[101] = static_cast<GByte>((payload_bytes / 2) & 0xFF);
  for (size_t i = 0; i < be_words.size(); ++i) {
    WriteBE32(tile, 102 + i * 4, be_words[i]);
  }
  WriteMemFile(absl::StrFormat("%s/%s.adf", dir, basename), tile);

  // Index file: 100-byte header + 8-byte entry (offset=50 words, size in
  // words).
  std::vector<GByte> idx(108, 0);
  idx[2] = 0x27;
  idx[3] = 0x0A;
  idx[4] = 0xFF;
  idx[5] = 0xFF;
  WriteBE32(idx, 24, 108 / 2);
  WriteBE32(idx, 100, 100 / 2);
  WriteBE32(idx, 104, payload_bytes / 2);
  WriteMemFile(absl::StrFormat("%s/%sx.adf", dir, basename), idx);
}

class VsiDirGuard {
 public:
  explicit VsiDirGuard(std::string path) : path_(std::move(path)) {
    VSIMkdir(path_.c_str(), 0777);
  }
  ~VsiDirGuard() { CPLUnlinkTree(path_.c_str()); }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

TEST(AigopenTest, OpenPathStrippingAndMissingFiles) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  // Bare .adf filename without directory separator resolves to ".".
  EXPECT_EQ(nullptr, AIGOpen("nonexistent_aigopen_probe.adf", "r"));

  VsiDirGuard dir("/vsimem/aigopen_strip");
  // Missing hdr.adf.
  EXPECT_EQ(nullptr, AIGOpen(dir.path().c_str(), "r"));

  WriteMemFile(dir.path() + "/hdr.adf", MakeHdr({}));
  // Missing dblbnd.adf.
  EXPECT_EQ(nullptr, AIGOpen(dir.path().c_str(), "r"));

  WriteMemFile(dir.path() + "/dblbnd.adf", MakeBounds(0.0, 0.0, 2.0, 2.0));
  // Missing sta.adf.
  EXPECT_EQ(nullptr, AIGOpen(dir.path().c_str(), "r"));

  WriteMemFile(dir.path() + "/sta.adf", MakeStats(1.0, 4.0, 2.5, 1.0));

  // Open via .adf file path with forward slash.
  AIGInfo_t* info = AIGOpen((dir.path() + "/hdr.adf").c_str(), "r");
  ASSERT_NE(nullptr, info);
  EXPECT_STREQ(dir.path().c_str(), info->pszCoverName);
  EXPECT_EQ(2, info->nPixels);
  EXPECT_EQ(2, info->nLines);
  AIGClose(info);

  // Open via .adf file path with backslash separator.
  AIGInfo_t* info_bs = AIGOpen((dir.path() + "\\hdr.adf").c_str(), "r");
  ASSERT_NE(nullptr, info_bs);
  EXPECT_STREQ(dir.path().c_str(), info_bs->pszCoverName);
  AIGClose(info_bs);
}

TEST(AigopenTest, InvalidHeaderAndBoundsValidation) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  VsiDirGuard dir("/vsimem/aigopen_invalid");
  WriteMemFile(dir.path() + "/dblbnd.adf", MakeBounds(0.0, 0.0, 4.0, 4.0));
  WriteMemFile(dir.path() + "/sta.adf", MakeStats(0.0, 1.0, 0.5, 0.5));

  // 1. Illegal cell size (X <= 0 and Y <= 0).
  {
    HeaderSpec spec;
    spec.cell_size_x = 0.0;
    WriteMemFile(dir.path() + "/hdr.adf", MakeHdr(spec));
    CPLErrorReset();
    EXPECT_EQ(nullptr, AIGOpen(dir.path().c_str(), "r"));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Illegal cell size"));
  }
  {
    HeaderSpec spec;
    spec.cell_size_y = -2.0;
    WriteMemFile(dir.path() + "/hdr.adf", MakeHdr(spec));
    CPLErrorReset();
    EXPECT_EQ(nullptr, AIGOpen(dir.path().c_str(), "r"));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Illegal cell size"));
  }

  // 2. Invalid raster dimensions (nPixels <= 0).
  {
    HeaderSpec spec;
    WriteMemFile(dir.path() + "/hdr.adf", MakeHdr(spec));
    WriteMemFile(dir.path() + "/dblbnd.adf", MakeBounds(10.0, 0.0, 2.0, 4.0));
    CPLErrorReset();
    EXPECT_EQ(nullptr, AIGOpen(dir.path().c_str(), "r"));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid raster dimensions"));
    WriteMemFile(dir.path() + "/dblbnd.adf", MakeBounds(0.0, 0.0, 4.0, 4.0));
  }

  // 3. Invalid block characteristics (zero and INT_MAX overflow).
  {
    HeaderSpec spec;
    spec.block_x_size = 0;
    WriteMemFile(dir.path() + "/hdr.adf", MakeHdr(spec));
    CPLErrorReset();
    EXPECT_EQ(nullptr, AIGOpen(dir.path().c_str(), "r"));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid block characteristics"));
  }
  {
    HeaderSpec spec;
    spec.block_x_size = INT_MAX;
    spec.blocks_per_row = 2;
    WriteMemFile(dir.path() + "/hdr.adf", MakeHdr(spec));
    CPLErrorReset();
    EXPECT_EQ(nullptr, AIGOpen(dir.path().c_str(), "r"));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid block characteristics"));
  }

  // 4. Too many blocks (nBlocksPerRow > INT_MAX / nBlocksPerColumn).
  {
    HeaderSpec spec;
    spec.block_x_size = 1;
    spec.block_y_size = 1;
    spec.blocks_per_row = 65536;
    spec.blocks_per_col = 65536;
    WriteMemFile(dir.path() + "/hdr.adf", MakeHdr(spec));
    CPLErrorReset();
    EXPECT_EQ(nullptr, AIGOpen(dir.path().c_str(), "r"));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too many blocks"));
  }

  // 5. Too many tiles (nTilesPerRow > 1000 * 1000 / nTilesPerColumn).
  {
    HeaderSpec spec;
    spec.block_x_size = 1;
    spec.block_y_size = 1;
    spec.blocks_per_row = 1;
    spec.blocks_per_col = 1;
    WriteMemFile(dir.path() + "/hdr.adf", MakeHdr(spec));
    WriteMemFile(dir.path() + "/dblbnd.adf",
                 MakeBounds(0.0, 0.0, 2000.0, 2000.0));
    CPLErrorReset();
    EXPECT_EQ(nullptr, AIGOpen(dir.path().c_str(), "r"));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too many tiles"));
  }
}

TEST(AigopenTest, TileNamingCachingAndIntReadTile) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  VsiDirGuard dir("/vsimem/aigopen_tiles");
  // 1x1 pixels per block, 2 blocks per row, 1 block per col -> 2x1 px per tile.
  // Extent 2 x 4 -> 1 tile per row, 4 tiles per col (iTileY = 0, 1, 2, 3).
  HeaderSpec spec;
  spec.cell_type = AIG_CELLTYPE_INT;
  spec.compressed_hdr_flag = 1;  // uncompressed 32-bit int
  spec.block_x_size = 1;
  spec.block_y_size = 1;
  spec.blocks_per_row = 2;
  spec.blocks_per_col = 1;
  WriteMemFile(dir.path() + "/hdr.adf", MakeHdr(spec));
  WriteMemFile(dir.path() + "/dblbnd.adf", MakeBounds(0.0, 0.0, 2.0, 4.0));
  WriteMemFile(dir.path() + "/sta.adf", MakeStats(10.0, 30.0, 20.0, 5.0));

  // iTileY == 0 -> w001001.adf (only 1 block in index, even though
  // nBlocksPerRow * nBlocksPerColumn == 2, to test nBlockID >= nBlocks).
  WriteSingleBlockTile(dir.path(), "w001001", {10});
  // iTileY == 1 -> w001000.adf.
  WriteSingleBlockTile(dir.path(), "w001000", {20});
  // iTileY == 2 -> z001001.adf (data file present, index missing -> failure).
  WriteMemFile(dir.path() + "/z001001.adf", std::vector<GByte>(106, 0));
  // iTileY == 3 -> z001002.adf (completely missing -> warning + nodata).

  AIGInfo_t* info = AIGOpen(dir.path().c_str(), "r");
  ASSERT_NE(nullptr, info);
  EXPECT_EQ(1, info->nTilesPerRow);
  EXPECT_EQ(4, info->nTilesPerColumn);

  // Tile (0, 0) -> w001001: first access loads, second hits fpGrid != NULL.
  EXPECT_EQ(CE_None, AIGAccessTile(info, 0, 0));
  EXPECT_EQ(CE_None, AIGAccessTile(info, 0, 0));

  GInt32 pixel = 0;
  EXPECT_EQ(CE_None, AIGReadTile(info, 0, 0, &pixel));
  EXPECT_EQ(10, pixel);

  // Block 1 in tile (0, 0) is beyond nBlocks (1) -> nodata.
  EXPECT_EQ(CE_None, AIGReadTile(info, 1, 0, &pixel));
  EXPECT_EQ(ESRI_GRID_NO_DATA, pixel);

  // Tile (0, 1) -> w001000.
  EXPECT_EQ(CE_None, AIGReadTile(info, 0, 1, &pixel));
  EXPECT_EQ(20, pixel);

  // Tile (0, 2) -> z001001 (missing index): first fails, second hits cached
  // fpGrid != NULL && panBlockOffset == NULL -> CE_Failure.
  EXPECT_EQ(CE_Failure, AIGAccessTile(info, 0, 2));
  EXPECT_EQ(CE_Failure, AIGReadTile(info, 0, 2, &pixel));

  // Tile (0, 3) -> z001002 (missing tile): first warns, second hits
  // bTriedToLoad -> CE_None; AIGReadTile returns nodata.
  EXPECT_EQ(CE_Warning, AIGAccessTile(info, 0, 3));
  EXPECT_EQ(CE_None, AIGAccessTile(info, 0, 3));
  EXPECT_EQ(CE_None, AIGReadTile(info, 0, 3, &pixel));
  EXPECT_EQ(ESRI_GRID_NO_DATA, pixel);

  // Illegal block ID (nBlockXOff = -1 -> iTileX = 0, nBlockID = -1 < 0).
  EXPECT_EQ(CE_Failure, AIGReadTile(info, -1, 0, &pixel));

  // Convert int tile to float via AIGReadFloatTile.
  float fpixel = 0.0f;
  EXPECT_EQ(CE_None, AIGReadFloatTile(info, 0, 0, &fpixel));
  EXPECT_FLOAT_EQ(10.0f, fpixel);

  AIGClose(info);
}

TEST(AigopenTest, FloatTileAndConversions) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  VsiDirGuard dir("/vsimem/aigopen_float");
  HeaderSpec spec;
  spec.cell_type = AIG_CELLTYPE_FLOAT;
  spec.compressed_hdr_flag = 0;
  spec.block_x_size = 1;
  spec.block_y_size = 1;
  spec.blocks_per_row = 2;
  spec.blocks_per_col = 1;
  WriteMemFile(dir.path() + "/hdr.adf", MakeHdr(spec));
  WriteMemFile(dir.path() + "/dblbnd.adf", MakeBounds(0.0, 0.0, 2.0, 3.0));
  WriteMemFile(dir.path() + "/sta.adf", MakeStats(1.5, 42.75, 20.0, 10.0));

  const float val = 42.75f;
  uint32_t bits = 0;
  std::memcpy(&bits, &val, sizeof(bits));
  WriteSingleBlockTile(dir.path(), "w001001", {bits});
  // w001000 missing -> nodata.
  // z001001 missing index -> failure.
  WriteMemFile(dir.path() + "/z001001.adf", std::vector<GByte>(106, 0));

  AIGInfo_t* info = AIGOpen(dir.path().c_str(), "r");
  ASSERT_NE(nullptr, info);

  float fpixel = 0.0f;
  EXPECT_EQ(CE_None, AIGReadFloatTile(info, 0, 0, &fpixel));
  EXPECT_FLOAT_EQ(42.75f, fpixel);

  // Block beyond nBlocks -> float nodata.
  EXPECT_EQ(CE_None, AIGReadFloatTile(info, 1, 0, &fpixel));
  EXPECT_FLOAT_EQ(static_cast<float>(ESRI_GRID_FLOAT_NO_DATA), fpixel);

  // Missing tile -> float nodata.
  EXPECT_EQ(CE_None, AIGReadFloatTile(info, 0, 1, &fpixel));
  EXPECT_FLOAT_EQ(static_cast<float>(ESRI_GRID_FLOAT_NO_DATA), fpixel);

  // Missing index -> failure.
  EXPECT_EQ(CE_Failure, AIGReadFloatTile(info, 0, 2, &fpixel));

  // Illegal block ID in AIGReadFloatTile.
  EXPECT_EQ(CE_Failure, AIGReadFloatTile(info, -1, 0, &fpixel));

  // Float-to-int post-processing in AIGReadTile.
  GInt32 ipixel = 0;
  EXPECT_EQ(CE_None, AIGReadTile(info, 0, 0, &ipixel));
  EXPECT_EQ(42, ipixel);

  AIGClose(info);
}

TEST(AigopenTest, UppercaseFilenameFallbackAndManyMissingTiles) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  VsiDirGuard dir("/vsimem/aigopen_uc");
  // Uppercase HDR.ADF, DBLBND.ADF, STA.ADF tested via AIGLLOpen fallback.
  HeaderSpec spec;
  spec.block_x_size = 1;
  spec.block_y_size = 1;
  spec.blocks_per_row = 1;
  spec.blocks_per_col = 1;
  WriteMemFile(dir.path() + "/HDR.ADF", MakeHdr(spec));
  WriteMemFile(dir.path() + "/DBLBND.ADF", MakeBounds(0.0, 0.0, 105.0, 1.0));
  WriteMemFile(dir.path() + "/STA.ADF", MakeStats(0.0, 1.0, 0.5, 0.5));

  AIGInfo_t* info = AIGOpen(dir.path().c_str(), "r");
  ASSERT_NE(nullptr, info);
  EXPECT_EQ(105, info->nTilesPerRow);

  // Access 105 missing tiles to exercise nFailedOpenings >= 100 warning cap.
  for (int tx = 0; tx < 105; ++tx) {
    EXPECT_EQ(CE_Warning, AIGAccessTile(info, tx, 0));
  }
  EXPECT_EQ(105, info->nFailedOpenings);

  AIGClose(info);
}

}  // namespace
}  // namespace autotest2
