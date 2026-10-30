// Tests for gridlib:
//   https://github.com/OSGeo/gdal/blob/master/frmts/aigrid/gridlib.c
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
#include "third_party/gdal/frmts/aigrid/aigrid.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

void WriteBE16(std::vector<GByte>& buf, size_t offset, uint16_t val) {
  buf[offset + 0] = static_cast<GByte>((val >> 8) & 0xFF);
  buf[offset + 1] = static_cast<GByte>(val & 0xFF);
}

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

void WriteLEDouble(std::vector<GByte>& buf, size_t offset, double val) {
  uint64_t bits = 0;
  std::memcpy(&bits, &val, sizeof(bits));
  for (int i = 0; i < 8; ++i) {
    buf[offset + i] = static_cast<GByte>((bits >> (i * 8)) & 0xFF);
  }
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

// Wraps a block payload (excluding the 2-byte size prefix) in an in-memory
// VSILFILE at offset 0 with the 2-byte word-count prefix prepended.
// Note: AIGReadBlock requires nBlockSize to be even because the 2-byte prefix
// stores nBlockSize / 2.
class MemBlockFile {
 public:
  explicit MemBlockFile(const std::string& path,
                        const std::vector<GByte>& payload)
      : path_(path) {
    std::vector<GByte> raw(2 + payload.size(), 0);
    const uint16_t words = static_cast<uint16_t>(payload.size() / 2);
    WriteBE16(raw, 0, words);
    if (!payload.empty()) {
      std::memcpy(raw.data() + 2, payload.data(), payload.size());
    }
    WriteMemFile(path_, raw);
    fp_ = VSIFOpenL(path_.c_str(), "rb");
  }

  ~MemBlockFile() {
    if (fp_ != nullptr) {
      VSIFCloseL(fp_);
    }
    VSIUnlink(path_.c_str());
  }

  VSILFILE* fp() const { return fp_; }

 private:
  std::string path_;
  VSILFILE* fp_ = nullptr;
};

CPLErr ReadCompressedBlock(const std::vector<GByte>& payload, int block_x,
                           int block_y, std::vector<GInt32>* out) {
  MemBlockFile mf("/vsimem/gridlib_blk.adf", payload);
  EXPECT_NE(nullptr, mf.fp());
  return AIGReadBlock(mf.fp(), 0, static_cast<int>(payload.size()), block_x,
                      block_y, out->data(), AIG_CELLTYPE_INT, TRUE);
}

TEST(GridlibTest, ReadHeaderBoundsAndStatistics) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  VsiDirGuard dir("/vsimem/gridlib_meta");
  AIGInfo_t info;
  std::memset(&info, 0, sizeof(info));

  // Missing files.
  EXPECT_EQ(CE_Failure, AIGReadHeader(dir.path().c_str(), &info));
  EXPECT_EQ(CE_Failure, AIGReadBounds(dir.path().c_str(), &info));
  EXPECT_EQ(CE_Failure, AIGReadStatistics(dir.path().c_str(), &info));

  // Short hdr.adf (< 308 bytes).
  WriteMemFile(dir.path() + "/hdr.adf", std::vector<GByte>(100, 0));
  EXPECT_EQ(CE_Failure, AIGReadHeader(dir.path().c_str(), &info));

  // Valid 308-byte hdr.adf (compressed flag 0 -> bCompressed = 1).
  std::vector<GByte> hdr(308, 0);
  WriteBE32(hdr, 16, AIG_CELLTYPE_INT);
  WriteBE32(hdr, 20, 0);
  WriteBEDouble(hdr, 256, 2.5);
  WriteBEDouble(hdr, 264, 5.0);
  WriteBE32(hdr, 288, 3);
  WriteBE32(hdr, 292, 4);
  WriteBE32(hdr, 296, 256);
  WriteBE32(hdr, 304, 16);
  WriteMemFile(dir.path() + "/hdr.adf", hdr);
  ASSERT_EQ(CE_None, AIGReadHeader(dir.path().c_str(), &info));
  EXPECT_EQ(AIG_CELLTYPE_INT, info.nCellType);
  EXPECT_EQ(1, info.bCompressed);
  EXPECT_DOUBLE_EQ(2.5, info.dfCellSizeX);
  EXPECT_DOUBLE_EQ(5.0, info.dfCellSizeY);
  EXPECT_EQ(3, info.nBlocksPerRow);
  EXPECT_EQ(4, info.nBlocksPerColumn);
  EXPECT_EQ(256, info.nBlockXSize);
  EXPECT_EQ(16, info.nBlockYSize);

  // Uncompressed flag 1 -> bCompressed = 0.
  WriteBE32(hdr, 20, 1);
  WriteMemFile(dir.path() + "/hdr.adf", hdr);
  ASSERT_EQ(CE_None, AIGReadHeader(dir.path().c_str(), &info));
  EXPECT_EQ(0, info.bCompressed);

  // Short dblbnd.adf (< 32 bytes).
  WriteMemFile(dir.path() + "/dblbnd.adf", std::vector<GByte>(16, 0));
  EXPECT_EQ(CE_Failure, AIGReadBounds(dir.path().c_str(), &info));

  // Valid 32-byte dblbnd.adf.
  std::vector<GByte> bnd(32, 0);
  WriteBEDouble(bnd, 0, -10.0);
  WriteBEDouble(bnd, 8, -20.0);
  WriteBEDouble(bnd, 16, 30.0);
  WriteBEDouble(bnd, 24, 40.0);
  WriteMemFile(dir.path() + "/dblbnd.adf", bnd);
  ASSERT_EQ(CE_None, AIGReadBounds(dir.path().c_str(), &info));
  EXPECT_DOUBLE_EQ(-10.0, info.dfLLX);
  EXPECT_DOUBLE_EQ(-20.0, info.dfLLY);
  EXPECT_DOUBLE_EQ(30.0, info.dfURX);
  EXPECT_DOUBLE_EQ(40.0, info.dfURY);

  // Invalid sta.adf size (10 bytes).
  WriteMemFile(dir.path() + "/sta.adf", std::vector<GByte>(10, 0));
  EXPECT_EQ(CE_Failure, AIGReadStatistics(dir.path().c_str(), &info));

  // 24-byte little-endian sta.adf (min, max, mean only; stddev = -1.0).
  std::vector<GByte> sta24(24, 0);
  WriteLEDouble(sta24, 0, 1.0);
  WriteLEDouble(sta24, 8, 99.0);
  WriteLEDouble(sta24, 16, 45.5);
  WriteMemFile(dir.path() + "/sta.adf", sta24);
  ASSERT_EQ(CE_None, AIGReadStatistics(dir.path().c_str(), &info));
  EXPECT_DOUBLE_EQ(1.0, info.dfMin);
  EXPECT_DOUBLE_EQ(99.0, info.dfMax);
  EXPECT_DOUBLE_EQ(45.5, info.dfMean);
  EXPECT_DOUBLE_EQ(-1.0, info.dfStdDev);

  // 32-byte big-endian sta.adf (min, max, mean, stddev).
  std::vector<GByte> sta32(32, 0);
  WriteBEDouble(sta32, 0, -5.0);
  WriteBEDouble(sta32, 8, 50.0);
  WriteBEDouble(sta32, 16, 22.5);
  WriteBEDouble(sta32, 24, 12.25);
  WriteMemFile(dir.path() + "/sta.adf", sta32);
  ASSERT_EQ(CE_None, AIGReadStatistics(dir.path().c_str(), &info));
  EXPECT_DOUBLE_EQ(-5.0, info.dfMin);
  EXPECT_DOUBLE_EQ(50.0, info.dfMax);
  EXPECT_DOUBLE_EQ(22.5, info.dfMean);
  EXPECT_DOUBLE_EQ(12.25, info.dfStdDev);
}

TEST(GridlibTest, ReadBlockIndexValidationAndSuccess) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  VsiDirGuard dir("/vsimem/gridlib_idx");
  AIGInfo_t info;
  std::memset(&info, 0, sizeof(info));
  info.pszCoverName = const_cast<char*>(dir.path().c_str());

  AIGTileInfo tinfo;
  std::memset(&tinfo, 0, sizeof(tinfo));

  // Missing index file.
  EXPECT_EQ(CE_Failure, AIGReadBlockIndex(&info, &tinfo, "w001001"));

  // Truncated header (< 8 bytes).
  WriteMemFile(dir.path() + "/w001001x.adf", std::vector<GByte>(4, 0));
  EXPECT_EQ(CE_Failure, AIGReadBlockIndex(&info, &tinfo, "w001001"));

  // CR/LF corrupted header (abyHeader[3] == 0x0D && abyHeader[4] == 0x0A).
  std::vector<GByte> crlf(100, 0);
  crlf[2] = 0x27;
  crlf[3] = 0x0D;
  crlf[4] = 0x0A;
  WriteMemFile(dir.path() + "/w001001x.adf", crlf);
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, AIGReadBlockIndex(&info, &tinfo, "w001001"));
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("unix to dos"));

  // Bad magic number.
  std::vector<GByte> bad_magic(100, 0);
  bad_magic[2] = 0x27;
  bad_magic[3] = 0x0B;
  WriteMemFile(dir.path() + "/w001001x.adf", bad_magic);
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, AIGReadBlockIndex(&info, &tinfo, "w001001"));
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("magic number is corrupt"));

  // Truncated before offset 24 (16 bytes total with valid magic).
  std::vector<GByte> short24(16, 0);
  short24[2] = 0x27;
  short24[3] = 0x0A;
  short24[4] = 0xFF;
  short24[5] = 0xFF;
  WriteMemFile(dir.path() + "/w001001x.adf", short24);
  EXPECT_EQ(CE_Failure, AIGReadBlockIndex(&info, &tinfo, "w001001"));

  // Base valid 100-byte header template.
  auto make_idx_header = []() {
    std::vector<GByte> h(100, 0);
    h[2] = 0x27;
    h[3] = 0x0A;
    h[4] = 0xFF;
    h[5] = 0xFF;
    return h;
  };

  // nValue > INT_MAX -> Bad length.
  {
    auto h = make_idx_header();
    WriteBE32(h, 24, 0x80000000u);
    WriteMemFile(dir.path() + "/w001001x.adf", h);
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, AIGReadBlockIndex(&info, &tinfo, "w001001"));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Bad length"));
  }

  // nLength <= 100 (nValue = 50 -> nLength = 100) -> Bad length.
  {
    auto h = make_idx_header();
    WriteBE32(h, 24, 50);
    WriteMemFile(dir.path() + "/w001001x.adf", h);
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, AIGReadBlockIndex(&info, &tinfo, "w001001"));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Bad length"));
  }

  // nBlocks >= 1000000 but file is small -> rejects excessive allocation.
  {
    auto h = make_idx_header();
    WriteBE32(h, 24, 50 + 4 * 1000000);
    WriteMemFile(dir.path() + "/w001001x.adf", h);
    EXPECT_EQ(CE_Failure, AIGReadBlockIndex(&info, &tinfo, "w001001"));
  }

  // nBlocks = 2, but file only has 1 block entry (108 bytes < 116) -> read err.
  {
    auto h = make_idx_header();
    h.resize(108, 0);
    WriteBE32(h, 24, 116 / 2);
    WriteMemFile(dir.path() + "/w001001x.adf", h);
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, AIGReadBlockIndex(&info, &tinfo, "w001001"));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot read block info"));
  }

  // Bad block offset (>= INT_MAX).
  {
    auto h = make_idx_header();
    h.resize(108, 0);
    WriteBE32(h, 24, 108 / 2);
    WriteBE32(h, 100, 0x80000000u);
    WriteBE32(h, 104, 4);
    WriteMemFile(dir.path() + "/w001001x.adf", h);
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, AIGReadBlockIndex(&info, &tinfo, "w001001"));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Bad offset"));
  }

  // Bad block size (>= INT_MAX / 2).
  {
    auto h = make_idx_header();
    h.resize(108, 0);
    WriteBE32(h, 24, 108 / 2);
    WriteBE32(h, 100, 50);
    WriteBE32(h, 104, 0x40000000u);
    WriteMemFile(dir.path() + "/w001001x.adf", h);
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, AIGReadBlockIndex(&info, &tinfo, "w001001"));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Bad size"));
  }

  // Valid 2-block index.
  {
    auto h = make_idx_header();
    h.resize(116, 0);
    WriteBE32(h, 24, 116 / 2);
    WriteBE32(h, 100, 50);
    WriteBE32(h, 104, 10);
    WriteBE32(h, 108, 61);
    WriteBE32(h, 112, 0);
    WriteMemFile(dir.path() + "/w001001x.adf", h);
    ASSERT_EQ(CE_None, AIGReadBlockIndex(&info, &tinfo, "w001001"));
    EXPECT_EQ(2, tinfo.nBlocks);
    EXPECT_EQ(100u, tinfo.panBlockOffset[0]);
    EXPECT_EQ(20, tinfo.panBlockSize[0]);
    EXPECT_EQ(122u, tinfo.panBlockOffset[1]);
    EXPECT_EQ(0, tinfo.panBlockSize[1]);
    CPLFree(tinfo.panBlockOffset);
    CPLFree(tinfo.panBlockSize);
  }
}

TEST(GridlibTest, ReadBlockHeaderErrorsAndRawFormats) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  // 1. nBlockSize == 0 -> all nodata.
  {
    std::vector<GInt32> data(4, 0);
    EXPECT_EQ(CE_None, AIGReadBlock(nullptr, 0, 0, 2, 2, data.data(),
                                    AIG_CELLTYPE_INT, TRUE));
    EXPECT_THAT(data, ElementsAre(ESRI_GRID_NO_DATA, ESRI_GRID_NO_DATA,
                                  ESRI_GRID_NO_DATA, ESRI_GRID_NO_DATA));
  }

  // 2. Invalid block sizes (< 0 and > 65535 * 2).
  {
    std::vector<GInt32> data(4, 0);
    EXPECT_EQ(CE_Failure, AIGReadBlock(nullptr, 0, -2, 2, 2, data.data(),
                                       AIG_CELLTYPE_INT, TRUE));
    EXPECT_EQ(CE_Failure, AIGReadBlock(nullptr, 0, 65535 * 2 + 2, 2, 2,
                                       data.data(), AIG_CELLTYPE_INT, TRUE));
  }

  // 3. Truncated read / offset past EOF and block size mismatch.
  {
    MemBlockFile mf("/vsimem/gridlib_trunc.adf", {0x00, 0x00});
    std::vector<GInt32> data(4, 99);
    // Offset 100 is past EOF -> read fails.
    EXPECT_EQ(CE_Failure, AIGReadBlock(mf.fp(), 100, 2, 2, 2, data.data(),
                                       AIG_CELLTYPE_INT, TRUE));
    EXPECT_THAT(data, ElementsAre(0, 0, 0, 0));

    // Prefix says 2 bytes, pass nBlockSize = 4 -> mismatch (file has 4 bytes,
    // wait: create 6-byte file whose prefix says 2 bytes while passing 4).
    std::vector<GByte> mismatch_raw(6, 0);
    WriteBE16(mismatch_raw, 0, 1);  // says 2 bytes
    WriteMemFile("/vsimem/gridlib_mm.adf", mismatch_raw);
    VSILFILE* fp_mm = VSIFOpenL("/vsimem/gridlib_mm.adf", "rb");
    ASSERT_NE(nullptr, fp_mm);
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, AIGReadBlock(fp_mm, 0, 4, 2, 2, data.data(),
                                       AIG_CELLTYPE_INT, TRUE));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Block is corrupt"));
    VSIFCloseL(fp_mm);
    VSIUnlink("/vsimem/gridlib_mm.adf");
  }

  // 4. Float block (valid and too small).
  {
    std::vector<GByte> payload(8, 0);
    const float f0 = 1.25f;
    const float f1 = -3.5f;
    uint32_t b0 = 0;
    uint32_t b1 = 0;
    std::memcpy(&b0, &f0, 4);
    std::memcpy(&b1, &f1, 4);
    WriteBE32(payload, 0, b0);
    WriteBE32(payload, 4, b1);
    MemBlockFile mf("/vsimem/gridlib_flt.adf", payload);
    float out[2] = {0.0f, 0.0f};
    EXPECT_EQ(CE_None,
              AIGReadBlock(mf.fp(), 0, 8, 2, 1, reinterpret_cast<GInt32*>(out),
                           AIG_CELLTYPE_FLOAT, FALSE));
    EXPECT_FLOAT_EQ(1.25f, out[0]);
    EXPECT_FLOAT_EQ(-3.5f, out[1]);

    // Too small for 2x2 float block (8 < 16).
    float out4[4] = {};
    CPLErrorReset();
    EXPECT_EQ(CE_None,
              AIGReadBlock(mf.fp(), 0, 8, 2, 2, reinterpret_cast<GInt32*>(out4),
                           AIG_CELLTYPE_FLOAT, FALSE));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Block too small"));
  }

  // 5. Uncompressed 32-bit int block (valid and too small).
  {
    std::vector<GByte> payload(8, 0);
    WriteBE32(payload, 0, 100);
    WriteBE32(payload, 4, static_cast<uint32_t>(-50));
    MemBlockFile mf("/vsimem/gridlib_raw32.adf", payload);
    GInt32 out[2] = {};
    EXPECT_EQ(CE_None,
              AIGReadBlock(mf.fp(), 0, 8, 2, 1, out, AIG_CELLTYPE_INT, FALSE));
    EXPECT_EQ(100, out[0]);
    EXPECT_EQ(-50, out[1]);

    GInt32 out4[4] = {};
    CPLErrorReset();
    EXPECT_EQ(CE_None,
              AIGReadBlock(mf.fp(), 0, 8, 2, 2, out4, AIG_CELLTYPE_INT, FALSE));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Block too small"));
  }

  // 6. Compressed header errors: nDataSize < nMinSize and nMinSize > 4.
  {
    std::vector<GInt32> out(4, 0);
    // nBlockSize = 2 -> nMagic = 0x00, nMinSize = 4 -> nDataSize = 0 < 4.
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ReadCompressedBlock({0x00, 0x04}, 2, 2, &out));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Need 4 bytes to read nMin"));

    // nMinSize = 5 > 4.
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              ReadCompressedBlock({0x00, 0x05, 0, 0, 0, 0, 0, 0}, 2, 2, &out));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Corrupt 'minsize'"));
  }
}

TEST(GridlibTest, MinSizeVariantsAndFixedBitFormats) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  std::vector<GInt32> out(4, 0);

  // nMinSize == 0 (0x00 constant block -> 0).
  ASSERT_EQ(CE_None, ReadCompressedBlock({0x00, 0x00}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(0, 0, 0, 0));

  // nMinSize == 1 positive (42) and negative (0xFE -> -2).
  ASSERT_EQ(CE_None, ReadCompressedBlock({0x00, 0x01, 42, 0x00}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(42, 42, 42, 42));
  ASSERT_EQ(CE_None, ReadCompressedBlock({0x00, 0x01, 0xFE, 0x00}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(-2, -2, -2, -2));

  // nMinSize == 2 positive (0x0102 = 258) and negative (0xFFFE = -2).
  ASSERT_EQ(CE_None, ReadCompressedBlock({0x00, 0x02, 0x01, 0x02}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(258, 258, 258, 258));
  ASSERT_EQ(CE_None, ReadCompressedBlock({0x00, 0x02, 0xFF, 0xFE}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(-2, -2, -2, -2));

  // nMinSize == 3 positive (0x010203) and negative (0xFFFFFE = -2).
  ASSERT_EQ(CE_None,
            ReadCompressedBlock({0x00, 0x03, 0x01, 0x02, 0x03, 0}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(0x010203, 0x010203, 0x010203, 0x010203));
  ASSERT_EQ(CE_None,
            ReadCompressedBlock({0x00, 0x03, 0xFF, 0xFF, 0xFE, 0}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(-2, -2, -2, -2));

  // nMinSize == 4 (-100000 = 0xFFFE7960).
  ASSERT_EQ(CE_None, ReadCompressedBlock({0x00, 0x04, 0xFF, 0xFE, 0x79, 0x60},
                                         2, 2, &out));
  EXPECT_THAT(out, ElementsAre(-100000, -100000, -100000, -100000));

  // nMagic == 0x01 (1-bit raw): nMin = 10, bits = 0b1010_0000 -> 11, 10,
  // 11, 10.
  ASSERT_EQ(CE_None, ReadCompressedBlock({0x01, 0x01, 10, 0xA0}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(11, 10, 11, 10));
  // 0x01 block too small (nDataSize = 0 < 1).
  CPLErrorReset();
  EXPECT_EQ(CE_None, ReadCompressedBlock({0x01, 0x02, 0, 10}, 2, 2, &out));
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Block too small"));

  // nMagic == 0x04 (4-bit raw): nMin = 5, nibbles = 0x12, 0x34 -> 6, 7, 8, 9.
  ASSERT_EQ(CE_None,
            ReadCompressedBlock({0x04, 0x01, 5, 0x12, 0x34, 0x00}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(6, 7, 8, 9));
  // 0x04 block too small (nDataSize = 1 < 2).
  CPLErrorReset();
  EXPECT_EQ(CE_None, ReadCompressedBlock({0x04, 0x01, 5, 0x12}, 2, 2, &out));
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Block too small"));

  // nMagic == 0x08 (8-bit raw): nMin = 100, bytes = 1, 2, 3, 4.
  ASSERT_EQ(CE_None,
            ReadCompressedBlock({0x08, 0x01, 100, 1, 2, 3, 4, 0}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(101, 102, 103, 104));
  // 0x08 block too small.
  CPLErrorReset();
  EXPECT_EQ(CE_None, ReadCompressedBlock({0x08, 0x01, 100, 1}, 2, 2, &out));
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Block too small"));

  // nMagic == 0x10 (16-bit raw): nMin = 1000 (2 bytes), 4x2 bytes.
  ASSERT_EQ(CE_None,
            ReadCompressedBlock(
                {0x10, 0x02, 0x03, 0xE8, 0, 1, 0, 2, 0, 3, 0, 4}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(1001, 1002, 1003, 1004));
  // 0x10 block too small.
  CPLErrorReset();
  EXPECT_EQ(CE_None,
            ReadCompressedBlock({0x10, 0x02, 0x03, 0xE8, 0, 1}, 2, 2, &out));
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Block too small"));

  // nMagic == 0x20 (32-bit raw): nMin = 10 (1 byte + 1 pad? Wait: 2 + 1 + 16 =
  // 19, pad to even 20 -> nDataSize = 18 >= 16).
  ASSERT_EQ(CE_None, ReadCompressedBlock({0x20, 0x01, 10, 0, 0, 0, 1, 0, 0, 0,
                                          2,    0,    0,  0, 3, 0, 0, 0, 4, 0},
                                         2, 2, &out));
  EXPECT_THAT(out, ElementsAre(11, 12, 13, 14));
  // 0x20 block too small.
  CPLErrorReset();
  EXPECT_EQ(CE_None, ReadCompressedBlock({0x20, 0x01, 10, 0}, 2, 2, &out));
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Block too small"));
}

TEST(GridlibTest, CcittFFBlockAndRleCompressionFormats) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  // 1. nMagic == 0xFF (CCITT RLE): 8x1 block, nMin = 20.
  // 3 white (idx=1, w=4 -> MSB 1000) + 5 black (idx=12, w=4 -> MSB 0011)
  // = 0b10000011 = 0x83, padded with 0x00 to even payload size (6 bytes total).
  {
    std::vector<GInt32> out8(8, 0);
    ASSERT_EQ(CE_None, ReadCompressedBlock({0xFF, 0x01, 20, 0x83, 0x00, 0x00},
                                           8, 1, &out8));
    EXPECT_THAT(out8, ElementsAre(20, 20, 20, 21, 21, 21, 21, 21));

    // Truncated CCITT data -> DecompressCCITTRLETile fails -> CE_Failure.
    EXPECT_EQ(CE_Failure,
              ReadCompressedBlock({0xFF, 0x01, 20, 0x00}, 8, 2, &out8));
  }

  std::vector<GInt32> out(4, 0);

  // 2. nMagic == 0xE0 (4-byte repeat): 4 pixels of (nMin=10 + 5 = 15).
  ASSERT_EQ(CE_None,
            ReadCompressedBlock({0xE0, 0x01, 10, 4, 0, 0, 0, 5}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(15, 15, 15, 15));
  // 0xE0 errors: run too long & block too small.
  EXPECT_EQ(CE_Failure,
            ReadCompressedBlock({0xE0, 0x01, 10, 5, 0, 0, 0, 1}, 2, 2, &out));
  EXPECT_EQ(CE_Failure,
            ReadCompressedBlock({0xE0, 0x01, 10, 2, 0, 1}, 2, 2, &out));

  // 3. nMagic == 0xF0 (2-byte repeat): 4 pixels of (nMin=10 + 0x0102 = 268).
  ASSERT_EQ(CE_None,
            ReadCompressedBlock({0xF0, 0x01, 10, 4, 0x01, 0x02}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(268, 268, 268, 268));
  // 0xF0 errors: run too long & block too small.
  EXPECT_EQ(CE_Failure,
            ReadCompressedBlock({0xF0, 0x01, 10, 5, 0, 1}, 2, 2, &out));
  EXPECT_EQ(CE_Failure, ReadCompressedBlock({0xF0, 0x01, 10, 2}, 2, 2, &out));

  // 4. nMagic == 0xFC and 0xF8 (1-byte repeat): 4 pixels of (10 + 7 = 17).
  ASSERT_EQ(CE_None,
            ReadCompressedBlock({0xFC, 0x01, 10, 4, 7, 0}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(17, 17, 17, 17));
  ASSERT_EQ(CE_None,
            ReadCompressedBlock({0xF8, 0x01, 10, 4, 7, 0}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(17, 17, 17, 17));
  // 0xFC errors: run too long & block too small (nDataSize == 0 after marker).
  EXPECT_EQ(CE_Failure,
            ReadCompressedBlock({0xFC, 0x01, 10, 5, 7, 0}, 2, 2, &out));
  EXPECT_EQ(CE_Failure,
            ReadCompressedBlock({0xFC, 0x02, 0, 10, 2, 7}, 2, 2, &out));

  // 5. nMagic == 0xDF (repeat nMin) + nodata repeat (nMarker > 128).
  // 2 pixels of nMin (42) + (256 - 254 = 2) pixels of ESRI_GRID_NO_DATA.
  ASSERT_EQ(CE_None,
            ReadCompressedBlock({0xDF, 0x01, 42, 2, 254, 0}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(42, 42, ESRI_GRID_NO_DATA, ESRI_GRID_NO_DATA));
  // 0xDF run too long, and nodata run too long.
  EXPECT_EQ(CE_Failure,
            ReadCompressedBlock({0xDF, 0x01, 42, 5, 0, 0}, 2, 2, &out));
  EXPECT_EQ(CE_Failure,
            ReadCompressedBlock({0xDF, 0x01, 42, 250, 0, 0}, 2, 2, &out));

  // 6. nMagic == 0xD7 (8-bit literals): nMin = 10, 4 literals 1, 2, 3, 4.
  ASSERT_EQ(CE_None,
            ReadCompressedBlock({0xD7, 0x01, 10, 4, 1, 2, 3, 4}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(11, 12, 13, 14));
  // 0xD7 run too long.
  EXPECT_EQ(CE_Failure,
            ReadCompressedBlock({0xD7, 0x01, 10, 5, 1, 2}, 2, 2, &out));

  // 7. nMagic == 0xCF (16-bit literals): nMin = 100, 4 literals 1, 2, 3, 4.
  ASSERT_EQ(CE_None,
            ReadCompressedBlock({0xCF, 0x01, 100, 4, 0, 1, 0, 2, 0, 3, 0, 4}, 2,
                                2, &out));
  EXPECT_THAT(out, ElementsAre(101, 102, 103, 104));
  // 0xCF run too long.
  EXPECT_EQ(CE_Failure,
            ReadCompressedBlock({0xCF, 0x01, 100, 5, 0, 1}, 2, 2, &out));

  // 8. Ran out of data (only 1 pixel decoded out of 4).
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, ReadCompressedBlock({0xDF, 0x01, 10, 1}, 2, 2, &out));
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Ran out of data"));

  // 9. Unknown magic code (0xAA) -> fills nodata and returns CE_Failure.
  std::fill(out.begin(), out.end(), 123);
  EXPECT_EQ(CE_Failure, ReadCompressedBlock({0xAA, 0x01, 10, 1}, 2, 2, &out));
  EXPECT_THAT(out, ElementsAre(ESRI_GRID_NO_DATA, ESRI_GRID_NO_DATA,
                               ESRI_GRID_NO_DATA, ESRI_GRID_NO_DATA));
}

}  // namespace
}  // namespace autotest2
