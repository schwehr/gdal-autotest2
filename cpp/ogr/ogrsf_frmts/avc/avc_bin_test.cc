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
//
// Tests for Arc/Info vector coverage binary file parsing (avc_bin.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/avc/avc_bin.cpp

#include <cstdint>
#include <cstring>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogrsf_frmts/avc/avc.h"
#include "third_party/gdal/ogr/ogrsf_frmts/shape/shapelib/shapefil.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

void _AVCBinReadRepairDBFFieldName(char* pszFieldName);

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

void WriteBinHeader(AVCRawBinFile* file, AVCCoverType cover_type,
                    int32_t signature, int32_t precision, int32_t record_size,
                    int32_t length_words) {
  if (cover_type == AVCCoverPC) {
    AVCRawBinWriteZeros(file, 256);
  }
  AVCRawBinWriteInt32(file, signature);
  AVCRawBinWriteInt32(file, precision);
  AVCRawBinWriteInt32(file, record_size);
  AVCRawBinWriteZeros(file, 12);
  AVCRawBinWriteInt32(file, length_words);
  AVCRawBinWriteZeros(file, 72);
}

TEST(AvcBinTest, OpenErrorAndHeaderEdgeCases) {
  const std::string dir = "/vsimem/avc_bin_open_err_test/";
  ASSERT_EQ(0, VSIMkdir("/vsimem/avc_bin_open_err_test", 0777));
  absl::Cleanup dir_cleaner = [] {
    VSIRmdirRecursive("/vsimem/avc_bin_open_err_test");
  };

  // 1. Non-existent file returns nullptr.
  EXPECT_EQ(nullptr, AVCBinReadOpen(dir.c_str(), "missing.adf", AVCCoverV7,
                                    AVCFileARC, nullptr));

  // 2. Unsupported file type returns nullptr with CE_Failure.
  const std::string arc_file = dir + "arc.adf";
  {
    AVCRawBinFile* raw =
        AVCRawBinOpen(arc_file.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, raw);
    WriteBinHeader(raw, AVCCoverV7, 9994, 0, 10, 100);
    AVCRawBinClose(raw);
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, AVCBinReadOpen(dir.c_str(), "arc.adf", AVCCoverV7,
                                      AVCFileUnknown, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 3. Invalid signature in header returns nullptr with CE_Warning.
  const std::string bad_sig_file = dir + "bad_sig.adf";
  {
    AVCRawBinFile* raw =
        AVCRawBinOpen(bad_sig_file.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, raw);
    WriteBinHeader(raw, AVCCoverV7, 1234, 0, 10, 100);
    AVCRawBinClose(raw);
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, AVCBinReadOpen(dir.c_str(), "bad_sig.adf", AVCCoverV7,
                                      AVCFileARC, nullptr));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("invalid file header"));
  }

  // 4. Truncated header (EOF encountered during header read).
  const std::string trunc_file = dir + "trunc.adf";
  {
    AVCRawBinFile* raw =
        AVCRawBinOpen(trunc_file.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, raw);
    AVCRawBinWriteInt32(raw, 9994);
    AVCRawBinClose(raw);
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, AVCBinReadOpen(dir.c_str(), "trunc.adf", AVCCoverV7,
                                      AVCFileARC, nullptr));
  }

  // 5. Negative or overflow nLength in header.
  const std::string neg_len_file = dir + "neg_len.adf";
  {
    AVCRawBinFile* raw =
        AVCRawBinOpen(neg_len_file.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, raw);
    WriteBinHeader(raw, AVCCoverV7, 9994, 0, 10, -5);
    AVCRawBinClose(raw);
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, AVCBinReadOpen(dir.c_str(), "neg_len.adf", AVCCoverV7,
                                      AVCFileARC, nullptr));
  }

  // 6. AVCBinReadClose and AVCBinReadNextObject with unsupported file type.
  AVCBinFile* dummy =
      static_cast<AVCBinFile*>(CPLCalloc(1, sizeof(AVCBinFile)));
  dummy->eFileType = AVCFileUnknown;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, AVCBinReadNextObject(dummy));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    AVCBinReadClose(dummy);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST(AvcBinTest, ReadArcSingleDoublePcAndIndexed) {
  const std::string dir = "/vsimem/avc_bin_arc_test/";
  ASSERT_EQ(0, VSIMkdir("/vsimem/avc_bin_arc_test", 0777));
  absl::Cleanup dir_cleaner = [] {
    VSIRmdirRecursive("/vsimem/avc_bin_arc_test");
  };

  // 1. Single-precision V7 ARC + ARX index.
  const std::string arc_path = dir + "arc.adf";
  const std::string arx_path = dir + "arx.adf";

  // Total words: 50 (header = 100 bytes = 50 words) + Record 1 (24 words) +
  // Record 2 (30 words)
  constexpr int32_t kTotalWords = 50 + 24 + 30;
  {
    AVCRawBinFile* arc =
        AVCRawBinOpen(arc_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, arc);
    WriteBinHeader(arc, AVCCoverV7, 9994, 0, 0, kTotalWords);

    // Record 1: 2 vertices (numVertices = 2), nRecordSize = 20 words (40 bytes
    // data)
    AVCRawBinWriteInt32(arc, 1);  // nArcId
    AVCRawBinWriteInt32(
        arc, 20);  // nRecordSize (in 16-bit words: 40 bytes after nRecordSize)
    AVCRawBinWriteInt32(arc, 101);  // nUserId
    AVCRawBinWriteInt32(arc, 10);   // nFNode
    AVCRawBinWriteInt32(arc, 20);   // nTNode
    AVCRawBinWriteInt32(arc, 1);    // nLPoly
    AVCRawBinWriteInt32(arc, 2);    // nRPoly
    AVCRawBinWriteInt32(arc, 2);    // numVertices
    AVCRawBinWriteFloat(arc, 1.0f);
    AVCRawBinWriteFloat(arc, 2.0f);
    AVCRawBinWriteFloat(arc, 3.0f);
    AVCRawBinWriteFloat(arc, 4.0f);

    // Record 2: 3 vertices, nRecordSize = 26 words (52 bytes, 48 bytes data + 4
    // bytes padding)
    AVCRawBinWriteInt32(arc, 2);    // nArcId
    AVCRawBinWriteInt32(arc, 26);   // nRecordSize (52 bytes > 48 bytes data)
    AVCRawBinWriteInt32(arc, 102);  // nUserId
    AVCRawBinWriteInt32(arc, 20);   // nFNode
    AVCRawBinWriteInt32(arc, 30);   // nTNode
    AVCRawBinWriteInt32(arc, 2);    // nLPoly
    AVCRawBinWriteInt32(arc, 3);    // nRPoly
    AVCRawBinWriteInt32(arc, 3);    // numVertices
    AVCRawBinWriteFloat(arc, 5.0f);
    AVCRawBinWriteFloat(arc, 6.0f);
    AVCRawBinWriteFloat(arc, 7.0f);
    AVCRawBinWriteFloat(arc, 8.0f);
    AVCRawBinWriteFloat(arc, 9.0f);
    AVCRawBinWriteFloat(arc, 10.0f);
    AVCRawBinWriteZeros(arc, 4);  // Extra 4 bytes of padding

    AVCRawBinClose(arc);
  }

  // Create ARX index file: 100-byte header + index entries (8 bytes per entry)
  {
    AVCRawBinFile* arx =
        AVCRawBinOpen(arx_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, arx);
    WriteBinHeader(arx, AVCCoverV7, 9994, 0, 4, 50 + 8);
    // Entry 1: offset in 16-bit words from file start (100 bytes = 50 words)
    AVCRawBinWriteInt32(arx, 50);
    AVCRawBinWriteInt32(arx, 24);
    // Entry 2: offset 50 + 24 = 74 words
    AVCRawBinWriteInt32(arx, 74);
    AVCRawBinWriteInt32(arx, 30);
    AVCRawBinClose(arx);
  }

  // Open and read sequentially
  AVCBinFile* psFile =
      AVCBinReadOpen(dir.c_str(), "arc.adf", AVCCoverV7, AVCFileARC, nullptr);
  ASSERT_NE(nullptr, psFile);
  absl::Cleanup closer = [psFile] { AVCBinReadClose(psFile); };

  EXPECT_EQ(AVC_SINGLE_PREC, psFile->nPrecision);

  // Read Record 1
  AVCArc* arc1 = AVCBinReadNextArc(psFile);
  ASSERT_NE(nullptr, arc1);
  EXPECT_EQ(1, arc1->nArcId);
  EXPECT_EQ(101, arc1->nUserId);
  EXPECT_EQ(10, arc1->nFNode);
  EXPECT_EQ(20, arc1->nTNode);
  EXPECT_EQ(1, arc1->nLPoly);
  EXPECT_EQ(2, arc1->nRPoly);
  EXPECT_EQ(2, arc1->numVertices);
  EXPECT_DOUBLE_EQ(1.0, arc1->pasVertices[0].x);
  EXPECT_DOUBLE_EQ(2.0, arc1->pasVertices[0].y);

  // Read Record 2 via generic AVCBinReadNextObject
  void* obj2 = AVCBinReadNextObject(psFile);
  ASSERT_NE(nullptr, obj2);
  AVCArc* arc2 = static_cast<AVCArc*>(obj2);
  EXPECT_EQ(2, arc2->nArcId);
  EXPECT_EQ(102, arc2->nUserId);
  EXPECT_EQ(3, arc2->numVertices);
  EXPECT_DOUBLE_EQ(9.0, arc2->pasVertices[2].x);
  EXPECT_DOUBLE_EQ(10.0, arc2->pasVertices[2].y);

  // EOF
  EXPECT_EQ(nullptr, AVCBinReadNextArc(psFile));

  // Rewind and re-read
  EXPECT_EQ(0, AVCBinReadRewind(psFile));
  arc1 = AVCBinReadNextArc(psFile);
  ASSERT_NE(nullptr, arc1);
  EXPECT_EQ(1, arc1->nArcId);

  // Read indexed via AVCBinReadObject
  AVCArc* indexed2 = static_cast<AVCArc*>(AVCBinReadObject(psFile, 2));
  ASSERT_NE(nullptr, indexed2);
  EXPECT_EQ(2, indexed2->nArcId);

  AVCArc* indexed1 = static_cast<AVCArc*>(AVCBinReadObject(psFile, 1));
  ASSERT_NE(nullptr, indexed1);
  EXPECT_EQ(1, indexed1->nArcId);

  // Negative index, out of range index
  EXPECT_EQ(nullptr, AVCBinReadObject(psFile, -1));
  EXPECT_EQ(nullptr, AVCBinReadObject(psFile, 99));

  // 2. Double-precision ARC (nPrecision = -1) and uppercase filename extension
  const std::string dbl_arc_path = dir + "DBL.ARC";
  {
    AVCRawBinFile* dbl_arc =
        AVCRawBinOpen(dbl_arc_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, dbl_arc);
    WriteBinHeader(dbl_arc, AVCCoverV7, 9994, -1, 0, 100);
    AVCRawBinWriteInt32(dbl_arc, 10);   // nArcId
    AVCRawBinWriteInt32(dbl_arc, 16);   // nRecordSize
    AVCRawBinWriteInt32(dbl_arc, 500);  // nUserId
    AVCRawBinWriteInt32(dbl_arc, 1);    // nFNode
    AVCRawBinWriteInt32(dbl_arc, 2);    // nTNode
    AVCRawBinWriteInt32(dbl_arc, 0);    // nLPoly
    AVCRawBinWriteInt32(dbl_arc, 0);    // nRPoly
    AVCRawBinWriteInt32(dbl_arc, 1);    // numVertices
    AVCRawBinWriteDouble(dbl_arc, 123.456789);
    AVCRawBinWriteDouble(dbl_arc, 987.654321);
    AVCRawBinClose(dbl_arc);
  }

  AVCBinFile* psDblFile =
      AVCBinReadOpen(dir.c_str(), "DBL.ARC", AVCCoverV7, AVCFileARC, nullptr);
  ASSERT_NE(nullptr, psDblFile);
  absl::Cleanup dbl_closer = [psDblFile] { AVCBinReadClose(psDblFile); };

  EXPECT_EQ(AVC_DOUBLE_PREC, psDblFile->nPrecision);
  AVCArc* dbl_arc = AVCBinReadNextArc(psDblFile);
  ASSERT_NE(nullptr, dbl_arc);
  EXPECT_EQ(10, dbl_arc->nArcId);
  EXPECT_DOUBLE_EQ(123.456789, dbl_arc->pasVertices[0].x);
  EXPECT_DOUBLE_EQ(987.654321, dbl_arc->pasVertices[0].y);

  // AVCBinReadObject when index file does not exist (DBL.ARX missing)
  EXPECT_EQ(nullptr, AVCBinReadObject(psDblFile, 1));

  // 3. PC Coverage ARC (AVCCoverPC with 256-byte prefix)
  const std::string pc_arc_path = dir + "arc";
  const std::string pc_arx_path = dir + "arx";
  {
    AVCRawBinFile* pc_arc =
        AVCRawBinOpen(pc_arc_path.c_str(), "w", AVCLittleEndian, nullptr);
    ASSERT_NE(nullptr, pc_arc);
    WriteBinHeader(pc_arc, AVCCoverPC, 9993, 0, 0, 100);
    AVCRawBinWriteInt32(pc_arc, 1);
    AVCRawBinWriteInt32(pc_arc, 10);
    AVCRawBinWriteInt32(pc_arc, 101);
    AVCRawBinWriteInt32(pc_arc, 1);
    AVCRawBinWriteInt32(pc_arc, 2);
    AVCRawBinWriteInt32(pc_arc, 0);
    AVCRawBinWriteInt32(pc_arc, 0);
    AVCRawBinWriteInt32(pc_arc, 1);
    AVCRawBinWriteFloat(pc_arc, 10.0f);
    AVCRawBinWriteFloat(pc_arc, 20.0f);
    AVCRawBinClose(pc_arc);
  }

  // Create PC Coverage ARX index file (356-byte header + index entries)
  {
    AVCRawBinFile* pc_arx =
        AVCRawBinOpen(pc_arx_path.c_str(), "w", AVCLittleEndian, nullptr);
    ASSERT_NE(nullptr, pc_arx);
    WriteBinHeader(pc_arx, AVCCoverPC, 9993, 0, 4, 100);
    // Entry 1: word offset from start of data (after 256-byte prefix):
    // 100 bytes header = 50 words
    AVCRawBinWriteInt32(pc_arx, 50);
    AVCRawBinWriteInt32(pc_arx, 14);
    AVCRawBinClose(pc_arx);
  }

  AVCBinFile* psPcFile =
      AVCBinReadOpen(dir.c_str(), "arc", AVCCoverPC, AVCFileARC, nullptr);
  ASSERT_NE(nullptr, psPcFile);
  absl::Cleanup pc_closer = [psPcFile] { AVCBinReadClose(psPcFile); };

  AVCArc* pc_arc = AVCBinReadNextArc(psPcFile);
  ASSERT_NE(nullptr, pc_arc);
  EXPECT_EQ(1, pc_arc->nArcId);
  EXPECT_DOUBLE_EQ(10.0, pc_arc->pasVertices[0].x);

  // Indexed read on PC Coverage ARC
  AVCArc* pc_indexed = static_cast<AVCArc*>(AVCBinReadObject(psPcFile, 1));
  ASSERT_NE(nullptr, pc_indexed);
  EXPECT_EQ(1, pc_indexed->nArcId);
}

TEST(AvcBinTest, ReadPalAndRplSingleAndDoubleAndIndexed) {
  const std::string dir = "/vsimem/avc_bin_pal_test/";
  ASSERT_EQ(0, VSIMkdir("/vsimem/avc_bin_pal_test", 0777));
  absl::Cleanup dir_cleaner = [] {
    VSIRmdirRecursive("/vsimem/avc_bin_pal_test");
  };

  // 1. Single-precision PAL + PAX index
  const std::string pal_path = dir + "pal.adf";
  const std::string pax_path = dir + "pax.adf";

  {
    AVCRawBinFile* pal =
        AVCRawBinOpen(pal_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, pal);
    WriteBinHeader(pal, AVCCoverV7, 9994, 0, 0, 100);

    // Record 1: 1 arc
    AVCRawBinWriteInt32(pal, 1);   // nPolyId
    AVCRawBinWriteInt32(pal, 16);  // nRecordSize (32 bytes data)
    AVCRawBinWriteFloat(pal, 0.0f);
    AVCRawBinWriteFloat(pal, 0.0f);
    AVCRawBinWriteFloat(pal, 10.0f);
    AVCRawBinWriteFloat(pal, 10.0f);
    AVCRawBinWriteInt32(pal, 1);   // numArcs
    AVCRawBinWriteInt32(pal, 10);  // nArcId
    AVCRawBinWriteInt32(pal, 1);   // nFNode
    AVCRawBinWriteInt32(pal, 2);   // nAdjPoly

    // Record 2: 2 arcs (growing arcs buffer + padding to test nBytesRead <
    // nRecordSize)
    AVCRawBinWriteInt32(pal, 2);  // nPolyId
    AVCRawBinWriteInt32(
        pal, 24);  // nRecordSize = 24 words (48 bytes > 44 bytes data)
    AVCRawBinWriteFloat(pal, 10.0f);
    AVCRawBinWriteFloat(pal, 10.0f);
    AVCRawBinWriteFloat(pal, 20.0f);
    AVCRawBinWriteFloat(pal, 20.0f);
    AVCRawBinWriteInt32(pal, 2);  // numArcs
    AVCRawBinWriteInt32(pal, 11);
    AVCRawBinWriteInt32(pal, 2);
    AVCRawBinWriteInt32(pal, 3);
    AVCRawBinWriteInt32(pal, 12);
    AVCRawBinWriteInt32(pal, 3);
    AVCRawBinWriteInt32(pal, 4);
    AVCRawBinWriteZeros(pal, 4);  // 4 bytes padding

    AVCRawBinClose(pal);
  }

  {
    AVCRawBinFile* pax =
        AVCRawBinOpen(pax_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, pax);
    WriteBinHeader(pax, AVCCoverV7, 9994, 0, 4, 100);
    AVCRawBinWriteInt32(pax, 50);
    AVCRawBinWriteInt32(pax, 16);
    AVCRawBinWriteInt32(pax, 70);
    AVCRawBinWriteInt32(pax, 24);
    AVCRawBinClose(pax);
  }

  AVCBinFile* psFile =
      AVCBinReadOpen(dir.c_str(), "pal.adf", AVCCoverV7, AVCFilePAL, nullptr);
  ASSERT_NE(nullptr, psFile);
  absl::Cleanup closer = [psFile] { AVCBinReadClose(psFile); };

  AVCPal* pal1 = AVCBinReadNextPal(psFile);
  ASSERT_NE(nullptr, pal1);
  EXPECT_EQ(1, pal1->nPolyId);
  EXPECT_FLOAT_EQ(0.0f, pal1->sMin.x);
  EXPECT_FLOAT_EQ(10.0f, pal1->sMax.y);
  EXPECT_EQ(1, pal1->numArcs);
  EXPECT_EQ(10, pal1->pasArcs[0].nArcId);

  AVCPal* pal2 = static_cast<AVCPal*>(AVCBinReadNextObject(psFile));
  ASSERT_NE(nullptr, pal2);
  EXPECT_EQ(2, pal2->nPolyId);
  EXPECT_EQ(2, pal2->numArcs);

  EXPECT_EQ(nullptr, AVCBinReadNextPal(psFile));

  // Indexed read
  AVCPal* indexed1 = static_cast<AVCPal*>(AVCBinReadObject(psFile, 1));
  ASSERT_NE(nullptr, indexed1);
  EXPECT_EQ(1, indexed1->nPolyId);

  // 2. Double-precision RPL (nPrecision = 1011)
  const std::string rpl_path = dir + "rpl.adf";
  {
    AVCRawBinFile* rpl =
        AVCRawBinOpen(rpl_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, rpl);
    WriteBinHeader(rpl, AVCCoverV7, 9994, 1011, 0, 100);
    AVCRawBinWriteInt32(rpl, 5);
    AVCRawBinWriteInt32(rpl, 22);
    AVCRawBinWriteDouble(rpl, 1.0);
    AVCRawBinWriteDouble(rpl, 2.0);
    AVCRawBinWriteDouble(rpl, 3.0);
    AVCRawBinWriteDouble(rpl, 4.0);
    AVCRawBinWriteInt32(rpl, 1);
    AVCRawBinWriteInt32(rpl, 100);
    AVCRawBinWriteInt32(rpl, 1);
    AVCRawBinWriteInt32(rpl, 2);
    AVCRawBinClose(rpl);
  }

  AVCBinFile* psRplFile =
      AVCBinReadOpen(dir.c_str(), "rpl.adf", AVCCoverV7, AVCFileRPL, nullptr);
  ASSERT_NE(nullptr, psRplFile);
  absl::Cleanup rpl_closer = [psRplFile] { AVCBinReadClose(psRplFile); };

  EXPECT_EQ(AVC_DOUBLE_PREC, psRplFile->nPrecision);
  AVCPal* rpl = AVCBinReadNextPal(psRplFile);
  ASSERT_NE(nullptr, rpl);
  EXPECT_EQ(5, rpl->nPolyId);
  EXPECT_DOUBLE_EQ(1.0, rpl->sMin.x);
  EXPECT_DOUBLE_EQ(4.0, rpl->sMax.y);
}

TEST(AvcBinTest, ReadCntLabTolPrjRxp) {
  const std::string dir = "/vsimem/avc_bin_cnt_lab_test/";
  ASSERT_EQ(0, VSIMkdir("/vsimem/avc_bin_cnt_lab_test", 0777));
  absl::Cleanup dir_cleaner = [] {
    VSIRmdirRecursive("/vsimem/avc_bin_cnt_lab_test");
  };

  // 1. CNT file: single precision and double precision records
  const std::string cnt_path = dir + "cnt.adf";
  {
    AVCRawBinFile* cnt =
        AVCRawBinOpen(cnt_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, cnt);
    WriteBinHeader(cnt, AVCCoverV7, 9994, 0, 0, 100);
    // Record 1: 1 label
    AVCRawBinWriteInt32(cnt, 1);  // nPolyId
    AVCRawBinWriteInt32(cnt, 8);  // nRecordSize
    AVCRawBinWriteFloat(cnt, 5.0f);
    AVCRawBinWriteFloat(cnt, 6.0f);
    AVCRawBinWriteInt32(cnt, 1);    // numLabels
    AVCRawBinWriteInt32(cnt, 501);  // panLabelIds[0]

    // Record 2: 2 labels (growth test + padding to test nBytesRead <
    // nRecordSize)
    AVCRawBinWriteInt32(cnt, 2);
    AVCRawBinWriteInt32(cnt, 12);  // 12 words = 24 bytes > 20 bytes data
    AVCRawBinWriteFloat(cnt, 7.0f);
    AVCRawBinWriteFloat(cnt, 8.0f);
    AVCRawBinWriteInt32(cnt, 2);
    AVCRawBinWriteInt32(cnt, 502);
    AVCRawBinWriteInt32(cnt, 503);
    AVCRawBinWriteZeros(cnt, 4);

    // Record 3: double precision record
    AVCRawBinWriteInt32(cnt, 3);
    AVCRawBinWriteInt32(cnt, 14);  // 14 words = 28 bytes
    AVCRawBinWriteDouble(cnt, 9.0);
    AVCRawBinWriteDouble(cnt, 10.0);
    AVCRawBinWriteInt32(cnt, 2);
    AVCRawBinWriteInt32(cnt, 504);
    AVCRawBinWriteInt32(cnt, 505);

    AVCRawBinClose(cnt);
  }

  AVCBinFile* psCntFile =
      AVCBinReadOpen(dir.c_str(), "cnt.adf", AVCCoverV7, AVCFileCNT, nullptr);
  ASSERT_NE(nullptr, psCntFile);
  absl::Cleanup cnt_closer = [psCntFile] { AVCBinReadClose(psCntFile); };

  AVCCnt* cnt1 = AVCBinReadNextCnt(psCntFile);
  ASSERT_NE(nullptr, cnt1);
  EXPECT_EQ(1, cnt1->nPolyId);
  EXPECT_FLOAT_EQ(5.0f, cnt1->sCoord.x);
  EXPECT_EQ(1, cnt1->numLabels);
  EXPECT_EQ(501, cnt1->panLabelIds[0]);

  AVCCnt* cnt2 = static_cast<AVCCnt*>(AVCBinReadNextObject(psCntFile));
  ASSERT_NE(nullptr, cnt2);
  EXPECT_EQ(2, cnt2->nPolyId);
  EXPECT_EQ(2, cnt2->numLabels);
  EXPECT_EQ(503, cnt2->panLabelIds[1]);

  // Read Record 3 with double precision
  psCntFile->nPrecision = AVC_DOUBLE_PREC;
  AVCCnt* cnt3 = AVCBinReadNextCnt(psCntFile);
  ASSERT_NE(nullptr, cnt3);
  EXPECT_EQ(3, cnt3->nPolyId);
  EXPECT_DOUBLE_EQ(9.0, cnt3->sCoord.x);
  EXPECT_DOUBLE_EQ(10.0, cnt3->sCoord.y);

  // 2. LAB file: double precision and single precision
  const std::string lab_path = dir + "lab.adf";
  {
    AVCRawBinFile* lab =
        AVCRawBinOpen(lab_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, lab);
    WriteBinHeader(lab, AVCCoverV7, 9994, -1, 0, 100);
    // Record 1
    AVCRawBinWriteInt32(lab, 42);  // nValue
    AVCRawBinWriteInt32(lab, 7);   // nPolyId
    AVCRawBinWriteDouble(lab, 1.0);
    AVCRawBinWriteDouble(lab, 2.0);
    AVCRawBinWriteDouble(lab, 3.0);
    AVCRawBinWriteDouble(lab, 4.0);
    AVCRawBinWriteDouble(lab, 5.0);
    AVCRawBinWriteDouble(lab, 6.0);

    // Record 2
    AVCRawBinWriteInt32(lab, 43);  // nValue
    AVCRawBinWriteInt32(lab, 8);   // nPolyId
    AVCRawBinWriteDouble(lab, 10.0);
    AVCRawBinWriteDouble(lab, 20.0);
    AVCRawBinWriteDouble(lab, 30.0);
    AVCRawBinWriteDouble(lab, 40.0);
    AVCRawBinWriteDouble(lab, 50.0);
    AVCRawBinWriteDouble(lab, 60.0);

    AVCRawBinClose(lab);
  }

  AVCBinFile* psLabFile =
      AVCBinReadOpen(dir.c_str(), "lab.adf", AVCCoverV7, AVCFileLAB, nullptr);
  ASSERT_NE(nullptr, psLabFile);
  absl::Cleanup lab_closer = [psLabFile] { AVCBinReadClose(psLabFile); };

  AVCLab* lab1 = AVCBinReadNextLab(psLabFile);
  ASSERT_NE(nullptr, lab1);
  EXPECT_EQ(42, lab1->nValue);
  EXPECT_EQ(7, lab1->nPolyId);
  EXPECT_DOUBLE_EQ(1.0, lab1->sCoord1.x);
  EXPECT_DOUBLE_EQ(6.0, lab1->sCoord3.y);

  // Read Record 2 via generic AVCBinReadNextObject
  void* lab2_obj = AVCBinReadNextObject(psLabFile);
  ASSERT_NE(nullptr, lab2_obj);
  AVCLab* lab2 = static_cast<AVCLab*>(lab2_obj);
  EXPECT_EQ(43, lab2->nValue);
  EXPECT_EQ(8, lab2->nPolyId);
  EXPECT_DOUBLE_EQ(10.0, lab2->sCoord1.x);

  // AVCBinReadObject on unsupported type returns nullptr
  EXPECT_EQ(nullptr, AVCBinReadObject(psLabFile, 1));

  // Single-precision LAB file
  const std::string lab_sp_path = dir + "lab_sp.adf";
  {
    AVCRawBinFile* lab_sp =
        AVCRawBinOpen(lab_sp_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, lab_sp);
    WriteBinHeader(lab_sp, AVCCoverV7, 9994, 0, 0, 100);
    AVCRawBinWriteInt32(lab_sp, 99);
    AVCRawBinWriteInt32(lab_sp, 100);
    AVCRawBinWriteFloat(lab_sp, 11.0f);
    AVCRawBinWriteFloat(lab_sp, 22.0f);
    AVCRawBinWriteFloat(lab_sp, 33.0f);
    AVCRawBinWriteFloat(lab_sp, 44.0f);
    AVCRawBinWriteFloat(lab_sp, 55.0f);
    AVCRawBinWriteFloat(lab_sp, 66.0f);
    AVCRawBinClose(lab_sp);
  }

  AVCBinFile* psLabSpFile = AVCBinReadOpen(dir.c_str(), "lab_sp.adf",
                                           AVCCoverV7, AVCFileLAB, nullptr);
  ASSERT_NE(nullptr, psLabSpFile);
  absl::Cleanup lab_sp_closer = [psLabSpFile] { AVCBinReadClose(psLabSpFile); };

  EXPECT_EQ(AVC_SINGLE_PREC, psLabSpFile->nPrecision);
  AVCLab* lab_sp = AVCBinReadNextLab(psLabSpFile);
  ASSERT_NE(nullptr, lab_sp);
  EXPECT_EQ(99, lab_sp->nValue);
  EXPECT_DOUBLE_EQ(11.0, lab_sp->sCoord1.x);
  EXPECT_DOUBLE_EQ(66.0, lab_sp->sCoord3.y);

  // 3. TOL file: single precision (no header) and double precision (starts with
  // 9993 header)
  const std::string tol_path = dir + "tol.adf";
  {
    AVCRawBinFile* tol =
        AVCRawBinOpen(tol_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, tol);
    AVCRawBinWriteInt32(tol, 1);     // nIndex
    AVCRawBinWriteInt32(tol, 2);     // nFlag
    AVCRawBinWriteFloat(tol, 0.5f);  // dValue
    AVCRawBinClose(tol);
  }

  AVCBinFile* psTolFile =
      AVCBinReadOpen(dir.c_str(), "tol.adf", AVCCoverV7, AVCFileTOL, nullptr);
  ASSERT_NE(nullptr, psTolFile);
  absl::Cleanup tol_closer = [psTolFile] { AVCBinReadClose(psTolFile); };

  EXPECT_EQ(AVC_SINGLE_PREC, psTolFile->nPrecision);
  AVCTol* tol = AVCBinReadNextTol(psTolFile);
  ASSERT_NE(nullptr, tol);
  EXPECT_EQ(1, tol->nIndex);
  EXPECT_EQ(2, tol->nFlag);
  EXPECT_FLOAT_EQ(0.5f, static_cast<float>(tol->dValue));

  const std::string par_path = dir + "par.adf";
  {
    AVCRawBinFile* par =
        AVCRawBinOpen(par_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, par);
    WriteBinHeader(par, AVCCoverV7, 9993, 1, 0, 100);
    AVCRawBinWriteInt32(par, 3);
    AVCRawBinWriteInt32(par, 4);
    AVCRawBinWriteDouble(par, 0.001);
    AVCRawBinClose(par);
  }

  AVCBinFile* psParFile =
      AVCBinReadOpen(dir.c_str(), "par.adf", AVCCoverV7, AVCFileTOL, nullptr);
  ASSERT_NE(nullptr, psParFile);
  absl::Cleanup par_closer = [psParFile] { AVCBinReadClose(psParFile); };

  EXPECT_EQ(AVC_DOUBLE_PREC, psParFile->nPrecision);
  AVCTol* par = static_cast<AVCTol*>(AVCBinReadNextObject(psParFile));
  ASSERT_NE(nullptr, par);
  EXPECT_EQ(3, par->nIndex);
  EXPECT_DOUBLE_EQ(0.001, par->dValue);

  // 4. PRJ file
  const std::string prj_path = dir + "prj.adf";
  VsiMemTempWrapper prj_wrapper(prj_path, "Projection UTM\nZone 10\n");

  AVCBinFile* psPrjFile =
      AVCBinReadOpen(dir.c_str(), "prj.adf", AVCCoverV7, AVCFilePRJ, nullptr);
  ASSERT_NE(nullptr, psPrjFile);
  absl::Cleanup prj_closer = [psPrjFile] { AVCBinReadClose(psPrjFile); };

  char** prj_lines = AVCBinReadNextPrj(psPrjFile);
  ASSERT_NE(nullptr, prj_lines);
  EXPECT_EQ(2, CSLCount(prj_lines));
  EXPECT_STREQ("Projection UTM", prj_lines[0]);
  EXPECT_STREQ("Zone 10", prj_lines[1]);

  // 5. RXP file (no header, pairs of Int32)
  const std::string rxp_path = dir + "rxp.adf";
  {
    AVCRawBinFile* rxp =
        AVCRawBinOpen(rxp_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, rxp);
    AVCRawBinWriteInt32(rxp, 100);
    AVCRawBinWriteInt32(rxp, 200);
    AVCRawBinClose(rxp);
  }

  AVCBinFile* psRxpFile =
      AVCBinReadOpen(dir.c_str(), "rxp.adf", AVCCoverV7, AVCFileRXP, nullptr);
  ASSERT_NE(nullptr, psRxpFile);
  absl::Cleanup rxp_closer = [psRxpFile] { AVCBinReadClose(psRxpFile); };

  AVCRxp* rxp = static_cast<AVCRxp*>(AVCBinReadNextObject(psRxpFile));
  ASSERT_NE(nullptr, rxp);
  EXPECT_EQ(100, rxp->n1);
  EXPECT_EQ(200, rxp->n2);
  EXPECT_EQ(nullptr, AVCBinReadNextRxp(psRxpFile));
}

TEST(AvcBinTest, ReadTxtV7PcAndWeird) {
  const std::string dir = "/vsimem/avc_bin_txt_test/";
  ASSERT_EQ(0, VSIMkdir("/vsimem/avc_bin_txt_test", 0777));
  absl::Cleanup dir_cleaner = [] {
    VSIRmdirRecursive("/vsimem/avc_bin_txt_test");
  };

  // 1. V7 single precision TXT / TX6
  const std::string txt_path = dir + "txt.adf";
  {
    AVCRawBinFile* txt =
        AVCRawBinOpen(txt_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, txt);
    WriteBinHeader(txt, AVCCoverV7, 9994, 0, 0, 200);

    AVCRawBinWriteInt32(txt, 1);     // nTxtId
    AVCRawBinWriteInt32(txt, 90);    // nRecordSize (words)
    AVCRawBinWriteInt32(txt, 101);   // nUserId
    AVCRawBinWriteInt32(txt, 2);     // nLevel
    AVCRawBinWriteFloat(txt, 1.5f);  // f_1e2
    AVCRawBinWriteInt32(txt, 10);    // nSymbol
    AVCRawBinWriteInt32(txt, 2);     // numVerticesLine
    AVCRawBinWriteInt32(txt, 0);     // n28
    AVCRawBinWriteInt32(txt, 5);     // numChars -> numCharsToRead = 8
    AVCRawBinWriteInt32(txt, 1);     // numVerticesArrow
    for (int i = 0; i < 20; ++i)
      AVCRawBinWriteInt16(txt, static_cast<int16_t>(i));
    for (int i = 0; i < 20; ++i)
      AVCRawBinWriteInt16(txt, static_cast<int16_t>(i * 2));
    AVCRawBinWriteFloat(txt, 12.0f);  // dHeight
    AVCRawBinWriteFloat(txt, 0.0f);   // dV2
    AVCRawBinWriteFloat(txt, 0.0f);   // dV3
    AVCRawBinWritePaddedString(txt, 8, reinterpret_cast<const GByte*>("LABEL"));
    // 3 vertices (2 line + 1 arrow)
    for (int i = 0; i < 3; ++i) {
      AVCRawBinWriteFloat(txt, static_cast<float>(i * 10));
      AVCRawBinWriteFloat(txt, static_cast<float>(i * 20));
    }
    // Trailing padding to reach nRecordSize
    AVCRawBinWriteZeros(txt, 40);

    AVCRawBinClose(txt);
  }

  AVCBinFile* psTxtFile =
      AVCBinReadOpen(dir.c_str(), "txt.adf", AVCCoverV7, AVCFileTXT, nullptr);
  ASSERT_NE(nullptr, psTxtFile);
  absl::Cleanup txt_closer = [psTxtFile] { AVCBinReadClose(psTxtFile); };

  AVCTxt* txt = AVCBinReadNextTxt(psTxtFile);
  ASSERT_NE(nullptr, txt);
  EXPECT_EQ(1, txt->nTxtId);
  EXPECT_EQ(101, txt->nUserId);
  EXPECT_EQ(2, txt->nLevel);
  EXPECT_FLOAT_EQ(1.5f, txt->f_1e2);
  EXPECT_EQ(10, txt->nSymbol);
  EXPECT_EQ(2, txt->numVerticesLine);
  EXPECT_EQ(1, txt->numVerticesArrow);
  EXPECT_EQ(5, txt->numChars);
  EXPECT_STREQ("LABEL", reinterpret_cast<char*>(txt->pszText));
  EXPECT_DOUBLE_EQ(12.0, txt->dHeight);
  EXPECT_DOUBLE_EQ(20.0, txt->pasVertices[2].x);
  EXPECT_DOUBLE_EQ(40.0, txt->pasVertices[2].y);

  // 2. Weird Coverage TXT with nPrecision = 67 (converted to AVCFileTX6)
  const std::string weird_path = dir + "weird_txt.adf";
  {
    AVCRawBinFile* wtxt =
        AVCRawBinOpen(weird_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, wtxt);
    WriteBinHeader(wtxt, AVCCoverWeird, 9994, 67, 0, 200);
    AVCRawBinWriteInt32(wtxt, 2);
    AVCRawBinWriteInt32(wtxt, 80);
    AVCRawBinWriteInt32(wtxt, 102);
    AVCRawBinWriteInt32(wtxt, 1);
    AVCRawBinWriteFloat(wtxt, 1.0f);
    AVCRawBinWriteInt32(wtxt, 5);
    AVCRawBinWriteInt32(wtxt, 1);
    AVCRawBinWriteInt32(wtxt, 0);
    AVCRawBinWriteInt32(wtxt, 4);
    AVCRawBinWriteInt32(wtxt, 0);
    for (int i = 0; i < 20; ++i) AVCRawBinWriteInt16(wtxt, 0);
    for (int i = 0; i < 20; ++i) AVCRawBinWriteInt16(wtxt, 0);
    AVCRawBinWriteFloat(wtxt, 10.0f);
    AVCRawBinWriteFloat(wtxt, 0.0f);
    AVCRawBinWriteFloat(wtxt, 0.0f);
    AVCRawBinWritePaddedString(wtxt, 4, reinterpret_cast<const GByte*>("TEST"));
    AVCRawBinWriteFloat(wtxt, 100.0f);
    AVCRawBinWriteFloat(wtxt, 200.0f);
    AVCRawBinWriteZeros(wtxt, 30);
    AVCRawBinClose(wtxt);
  }

  AVCBinFile* psWeirdFile = AVCBinReadOpen(dir.c_str(), "weird_txt.adf",
                                           AVCCoverWeird, AVCFileTXT, nullptr);
  ASSERT_NE(nullptr, psWeirdFile);
  absl::Cleanup weird_closer = [psWeirdFile] { AVCBinReadClose(psWeirdFile); };

  EXPECT_EQ(AVCFileTX6, psWeirdFile->eFileType);
  AVCTxt* wtxt = static_cast<AVCTxt*>(AVCBinReadNextObject(psWeirdFile));
  ASSERT_NE(nullptr, wtxt);
  EXPECT_EQ(2, wtxt->nTxtId);
  EXPECT_STREQ("TEST", reinterpret_cast<char*>(wtxt->pszText));

  // 3. V7 double precision TXT
  const std::string txt_dbl_path = dir + "txt_dbl.adf";
  {
    AVCRawBinFile* dtxt =
        AVCRawBinOpen(txt_dbl_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, dtxt);
    WriteBinHeader(dtxt, AVCCoverV7, 9994, -1, 0, 200);

    AVCRawBinWriteInt32(dtxt, 3);     // nTxtId
    AVCRawBinWriteInt32(dtxt, 110);   // nRecordSize (words)
    AVCRawBinWriteInt32(dtxt, 103);   // nUserId
    AVCRawBinWriteInt32(dtxt, 1);     // nLevel
    AVCRawBinWriteFloat(dtxt, 2.0f);  // f_1e2
    AVCRawBinWriteInt32(dtxt, 5);     // nSymbol
    AVCRawBinWriteInt32(dtxt, 1);     // numVerticesLine
    AVCRawBinWriteInt32(dtxt, 0);     // n28
    AVCRawBinWriteInt32(dtxt, 4);     // numChars -> numCharsToRead = 4
    AVCRawBinWriteInt32(dtxt, 0);     // numVerticesArrow
    for (int i = 0; i < 20; ++i) AVCRawBinWriteInt16(dtxt, 0);
    for (int i = 0; i < 20; ++i) AVCRawBinWriteInt16(dtxt, 0);
    AVCRawBinWriteDouble(dtxt, 15.0);  // dHeight
    AVCRawBinWriteDouble(dtxt, 1.0);   // dV2
    AVCRawBinWriteDouble(dtxt, 2.0);   // dV3
    AVCRawBinWritePaddedString(dtxt, 4, reinterpret_cast<const GByte*>("DBLT"));
    // 1 vertex (double precision)
    AVCRawBinWriteDouble(dtxt, 123.456);
    AVCRawBinWriteDouble(dtxt, 654.321);
    AVCRawBinWriteZeros(dtxt, 40);

    AVCRawBinClose(dtxt);
  }

  AVCBinFile* psDblTxtFile = AVCBinReadOpen(dir.c_str(), "txt_dbl.adf",
                                            AVCCoverV7, AVCFileTXT, nullptr);
  ASSERT_NE(nullptr, psDblTxtFile);
  absl::Cleanup dbl_txt_closer = [psDblTxtFile] {
    AVCBinReadClose(psDblTxtFile);
  };

  EXPECT_EQ(AVC_DOUBLE_PREC, psDblTxtFile->nPrecision);
  AVCTxt* dtxt = AVCBinReadNextTxt(psDblTxtFile);
  ASSERT_NE(nullptr, dtxt);
  EXPECT_EQ(3, dtxt->nTxtId);
  EXPECT_DOUBLE_EQ(15.0, dtxt->dHeight);
  EXPECT_DOUBLE_EQ(1.0, dtxt->dV2);
  EXPECT_DOUBLE_EQ(2.0, dtxt->dV3);
  EXPECT_STREQ("DBLT", reinterpret_cast<char*>(dtxt->pszText));
  EXPECT_DOUBLE_EQ(123.456, dtxt->pasVertices[0].x);

  // 4. PC Arc/Info TXT file (AVCCoverPC with 256-byte header + single precision
  // structure)
  const std::string pc_txt_path = dir + "pctxt";
  {
    AVCRawBinFile* pctxt =
        AVCRawBinOpen(pc_txt_path.c_str(), "w", AVCLittleEndian, nullptr);
    ASSERT_NE(nullptr, pctxt);
    WriteBinHeader(pctxt, AVCCoverPC, 9993, 0, 0, 200);

    AVCRawBinWriteInt32(pctxt, 10);  // nTxtId
    AVCRawBinWriteInt32(pctxt,
                        46);        // nRecordSize (words): 46*2 + 8 = 100 bytes
    AVCRawBinWriteInt32(pctxt, 3);  // nLevel
    AVCRawBinWriteInt32(
        pctxt, 1);  // numVerticesLine: MIN(1, 4) = 1 -> numVertices = 2
    // 1 vertex coord written: x, y (subsequent 13 unused floats skipped)
    AVCRawBinWriteFloat(pctxt, 55.0f);
    AVCRawBinWriteFloat(pctxt, 66.0f);
    AVCRawBinWriteZeros(pctxt, 13 * 4);
    AVCRawBinWriteFloat(pctxt, 8.0f);  // dHeight
    AVCRawBinWriteFloat(pctxt, 1.0f);  // f_1e2
    AVCRawBinWriteInt32(pctxt, 4);     // nSymbol
    AVCRawBinWriteInt32(pctxt, 4);     // numChars
    // numCharsToRead = 100 - (28 + 16*4) = 8 bytes
    AVCRawBinWritePaddedString(pctxt, 8,
                               reinterpret_cast<const GByte*>("PCAN"));

    AVCRawBinClose(pctxt);
  }

  AVCBinFile* psPcTxtFile =
      AVCBinReadOpen(dir.c_str(), "pctxt", AVCCoverPC, AVCFileTXT, nullptr);
  ASSERT_NE(nullptr, psPcTxtFile);
  absl::Cleanup pctxt_closer = [psPcTxtFile] { AVCBinReadClose(psPcTxtFile); };

  AVCTxt* pctxt = AVCBinReadNextTxt(psPcTxtFile);
  ASSERT_NE(nullptr, pctxt);
  EXPECT_EQ(10, pctxt->nTxtId);
  EXPECT_EQ(3, pctxt->nLevel);
  EXPECT_EQ(2, pctxt->numVerticesLine);
  EXPECT_DOUBLE_EQ(55.0, pctxt->pasVertices[0].x);
  EXPECT_DOUBLE_EQ(55.0, pctxt->pasVertices[1].x);
  EXPECT_STREQ("PCAN", reinterpret_cast<char*>(pctxt->pszText));

  // 5. PC Arc/Info double precision TXT file
  const std::string pc_dbl_txt_path = dir + "pctxt_dbl";
  {
    AVCRawBinFile* pdtxt =
        AVCRawBinOpen(pc_dbl_txt_path.c_str(), "w", AVCLittleEndian, nullptr);
    ASSERT_NE(nullptr, pdtxt);
    WriteBinHeader(pdtxt, AVCCoverPC, 9993, -1, 0, 200);

    AVCRawBinWriteInt32(pdtxt, 20);  // nTxtId
    AVCRawBinWriteInt32(pdtxt, 78);  // nRecordSize = 78 words
    AVCRawBinWriteInt32(pdtxt, 2);   // nLevel
    AVCRawBinWriteInt32(
        pdtxt, 1);  // numVerticesLine: MIN(1, 4) = 1 -> numVertices = 2
    // 1 vertex coord written: x, y (subsequent 13 unused double pairs skipped)
    AVCRawBinWriteDouble(pdtxt, 111.0);
    AVCRawBinWriteDouble(pdtxt, 222.0);
    AVCRawBinWriteZeros(pdtxt, 13 * 8);
    AVCRawBinWriteDouble(pdtxt, 14.0);  // dHeight
    AVCRawBinWriteFloat(pdtxt, 1.0f);   // f_1e2
    AVCRawBinWriteInt32(pdtxt, 7);      // nSymbol
    AVCRawBinWriteInt32(pdtxt, 4);      // numChars
    AVCRawBinWritePaddedString(pdtxt, 8,
                               reinterpret_cast<const GByte*>("PCDL"));

    AVCRawBinClose(pdtxt);
  }

  AVCBinFile* psPcDblTxtFile =
      AVCBinReadOpen(dir.c_str(), "pctxt_dbl", AVCCoverPC, AVCFileTXT, nullptr);
  ASSERT_NE(nullptr, psPcDblTxtFile);
  absl::Cleanup pcdbl_closer = [psPcDblTxtFile] {
    AVCBinReadClose(psPcDblTxtFile);
  };

  EXPECT_EQ(AVC_SINGLE_PREC, psPcDblTxtFile->nPrecision);
  psPcDblTxtFile->nPrecision = AVC_DOUBLE_PREC;
  AVCTxt* pcdbl = AVCBinReadNextTxt(psPcDblTxtFile);
  ASSERT_NE(nullptr, pcdbl);
  EXPECT_EQ(20, pcdbl->nTxtId);
  EXPECT_DOUBLE_EQ(14.0, pcdbl->dHeight);
  EXPECT_DOUBLE_EQ(111.0, pcdbl->pasVertices[0].x);
  EXPECT_STREQ("PCDL", reinterpret_cast<char*>(pcdbl->pszText));
}

TEST(AvcBinTest, ReadNativeV7AndWeirdTables) {
  const std::string dir = "/vsimem/avc_bin_table_test";
  const std::string info_dir = dir + "/info/";
  ASSERT_EQ(0, VSIMkdir(dir.c_str(), 0777));
  ASSERT_EQ(0, VSIMkdir(info_dir.c_str(), 0777));
  absl::Cleanup dir_cleaner = [&dir] { VSIRmdirRecursive(dir.c_str()); };

  // 1. Create arc.dir with 4 entries:
  // - Entry 1: deleted table TEST.DEL (arc9999) with real files
  // - Entry 2: TEST.AAT (internal data file ARC0000.DAT)
  // - Entry 3: TEST.PAT (external data file ARC0001.DAT containing relative
  // path)
  // - Entry 4: TEST.EMT (empty table, numRecords = 0)
  const std::string arcdir_path = info_dir + "arc.dir";
  {
    AVCRawBinFile* arcdir =
        AVCRawBinOpen(arcdir_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, arcdir);

    // Entry 1: Deleted table (name matches "TEST." prefix; excluded solely by
    // bDeletedFlag)
    AVCRawBinWritePaddedString(arcdir, 32,
                               reinterpret_cast<const GByte*>("TEST.DEL"));
    AVCRawBinWritePaddedString(arcdir, 8,
                               reinterpret_cast<const GByte*>("ARC9999"));
    AVCRawBinWriteInt16(arcdir, 1);
    AVCRawBinWriteInt16(arcdir, 4);
    AVCRawBinWriteZeros(arcdir, 18);
    AVCRawBinWriteInt16(arcdir, 1);  // bDeletedFlag = 1
    AVCRawBinWriteInt32(arcdir, 1);
    AVCRawBinWriteZeros(arcdir, 10);
    AVCRawBinWriteBytes(arcdir, 2, reinterpret_cast<const GByte*>("  "));
    AVCRawBinWriteZeros(arcdir, 300);

    // Entry 2: TEST.AAT (internal)
    AVCRawBinWritePaddedString(arcdir, 32,
                               reinterpret_cast<const GByte*>("TEST.AAT"));
    AVCRawBinWritePaddedString(arcdir, 8,
                               reinterpret_cast<const GByte*>("ARC0000"));
    AVCRawBinWriteInt16(arcdir, 6);   // 6 fields
    AVCRawBinWriteInt16(arcdir, 42);  // nRecSize = 42 (padded from 40)
    AVCRawBinWriteZeros(arcdir, 18);
    AVCRawBinWriteInt16(arcdir, 0);  // bDeletedFlag = 0
    AVCRawBinWriteInt32(arcdir,
                        10);  // numRecords = 10 (will be auto-corrected to 2)
    AVCRawBinWriteZeros(arcdir, 10);
    AVCRawBinWriteBytes(arcdir, 2, reinterpret_cast<const GByte*>("  "));
    AVCRawBinWriteZeros(arcdir, 300);

    // Entry 3: TEST.PAT (external "XX")
    AVCRawBinWritePaddedString(arcdir, 32,
                               reinterpret_cast<const GByte*>("TEST.PAT"));
    AVCRawBinWritePaddedString(arcdir, 8,
                               reinterpret_cast<const GByte*>("ARC0001"));
    AVCRawBinWriteInt16(arcdir, 1);  // 1 field
    AVCRawBinWriteInt16(arcdir, 4);  // nRecSize = 4
    AVCRawBinWriteZeros(arcdir, 18);
    AVCRawBinWriteInt16(arcdir, 0);
    AVCRawBinWriteInt32(arcdir, 1);
    AVCRawBinWriteZeros(arcdir, 10);
    AVCRawBinWriteBytes(arcdir, 2, reinterpret_cast<const GByte*>("XX"));
    AVCRawBinWriteZeros(arcdir, 300);

    // Entry 4: TEST.EMT (empty)
    AVCRawBinWritePaddedString(arcdir, 32,
                               reinterpret_cast<const GByte*>("TEST.EMT"));
    AVCRawBinWritePaddedString(arcdir, 8,
                               reinterpret_cast<const GByte*>("ARC0002"));
    AVCRawBinWriteInt16(arcdir, 1);
    AVCRawBinWriteInt16(arcdir, 4);
    AVCRawBinWriteZeros(arcdir, 18);
    AVCRawBinWriteInt16(arcdir, 0);
    AVCRawBinWriteInt32(arcdir, 0);  // numRecords = 0
    AVCRawBinWriteZeros(arcdir, 10);
    AVCRawBinWriteBytes(arcdir, 2, reinterpret_cast<const GByte*>("  "));
    AVCRawBinWriteZeros(arcdir, 300);

    AVCRawBinClose(arcdir);
  }

  // Create real files for deleted table TEST.DEL (ARC9999) so
  // _AVCBinReadInfoFileExists passes.
  const std::string nit9_path = info_dir + "arc9999.nit";
  {
    AVCRawBinFile* nit9 =
        AVCRawBinOpen(nit9_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, nit9);
    AVCRawBinWriteZeros(nit9, 144);
    AVCRawBinClose(nit9);
  }
  const std::string dat9_path = info_dir + "arc9999.dat";
  {
    AVCRawBinFile* dat9 =
        AVCRawBinOpen(dat9_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, dat9);
    AVCRawBinClose(dat9);
  }

  // 2. Create ARC0000.NIT (field definitions for TEST.AAT):
  // Include 1 deleted field (nIndex = -1) and 6 active fields (nIndex = 1..6):
  // - Field 1: DATE (nType1=1, size=8)
  // - Field 2: CHAR (nType1=2, size=10)
  // - Field 3: BININT (nType1=5, size=4)
  // - Field 4: BININT (nType1=5, size=2)
  // - Field 5: BINFLOAT (nType1=6, size=4)
  // - Field 6: BINFLOAT (nType1=6, size=8)
  // Sum sizes = 8 + 10 + 4 + 2 + 4 + 8 = 36 bytes (+ 6 padding bytes in record
  // = 42)
  const std::string nit0_path = info_dir + "arc0000.nit";
  {
    AVCRawBinFile* nit =
        AVCRawBinOpen(nit0_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, nit);

    // Deleted field (nIndex = -1)
    AVCRawBinWritePaddedString(nit, 16,
                               reinterpret_cast<const GByte*>("DELETED_FLD"));
    AVCRawBinWriteInt16(nit, 4);
    AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWriteInt16(nit, 1);
    AVCRawBinWriteInt16(nit, 4);
    AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWriteInt16(nit, 4);
    AVCRawBinWriteInt16(nit, 0);
    AVCRawBinWriteInt16(nit, 5);
    AVCRawBinWriteInt16(nit, 0);
    for (int i = 0; i < 4; ++i) AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWritePaddedString(nit, 16, reinterpret_cast<const GByte*>(""));
    AVCRawBinWriteZeros(nit, 56);
    AVCRawBinWriteInt16(nit, -1);  // nIndex = -1
    AVCRawBinWriteZeros(nit, 28);

    // Field 1: DATE
    AVCRawBinWritePaddedString(nit, 16,
                               reinterpret_cast<const GByte*>("DATE_COL"));
    AVCRawBinWriteInt16(nit, 8);
    AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWriteInt16(nit, 1);
    AVCRawBinWriteInt16(nit, 4);
    AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWriteInt16(nit, 8);
    AVCRawBinWriteInt16(nit, 0);
    AVCRawBinWriteInt16(nit, 1);  // nType1 = 1 (DATE)
    AVCRawBinWriteInt16(nit, 0);
    for (int i = 0; i < 4; ++i) AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWritePaddedString(nit, 16, reinterpret_cast<const GByte*>(""));
    AVCRawBinWriteZeros(nit, 56);
    AVCRawBinWriteInt16(nit, 1);  // nIndex = 1
    AVCRawBinWriteZeros(nit, 28);

    // Field 2: CHAR
    AVCRawBinWritePaddedString(nit, 16,
                               reinterpret_cast<const GByte*>("NAME_COL"));
    AVCRawBinWriteInt16(nit, 10);
    AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWriteInt16(nit, 9);
    AVCRawBinWriteInt16(nit, 4);
    AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWriteInt16(nit, 10);
    AVCRawBinWriteInt16(nit, 0);
    AVCRawBinWriteInt16(nit, 2);  // nType1 = 2 (CHAR)
    AVCRawBinWriteInt16(nit, 0);
    for (int i = 0; i < 4; ++i) AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWritePaddedString(nit, 16, reinterpret_cast<const GByte*>(""));
    AVCRawBinWriteZeros(nit, 56);
    AVCRawBinWriteInt16(nit, 2);
    AVCRawBinWriteZeros(nit, 28);

    // Field 3: BININT (size 4)
    AVCRawBinWritePaddedString(nit, 16,
                               reinterpret_cast<const GByte*>("INT32_COL"));
    AVCRawBinWriteInt16(nit, 4);
    AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWriteInt16(nit, 19);
    AVCRawBinWriteInt16(nit, 4);
    AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWriteInt16(nit, 5);
    AVCRawBinWriteInt16(nit, 0);
    AVCRawBinWriteInt16(nit, 5);  // nType1 = 5
    AVCRawBinWriteInt16(nit, 0);
    for (int i = 0; i < 4; ++i) AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWritePaddedString(nit, 16, reinterpret_cast<const GByte*>(""));
    AVCRawBinWriteZeros(nit, 56);
    AVCRawBinWriteInt16(nit, 3);
    AVCRawBinWriteZeros(nit, 28);

    // Field 4: BININT (size 2)
    AVCRawBinWritePaddedString(nit, 16,
                               reinterpret_cast<const GByte*>("INT16_COL"));
    AVCRawBinWriteInt16(nit, 2);
    AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWriteInt16(nit, 23);
    AVCRawBinWriteInt16(nit, 4);
    AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWriteInt16(nit, 5);
    AVCRawBinWriteInt16(nit, 0);
    AVCRawBinWriteInt16(nit, 5);  // nType1 = 5
    AVCRawBinWriteInt16(nit, 0);
    for (int i = 0; i < 4; ++i) AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWritePaddedString(nit, 16, reinterpret_cast<const GByte*>(""));
    AVCRawBinWriteZeros(nit, 56);
    AVCRawBinWriteInt16(nit, 4);
    AVCRawBinWriteZeros(nit, 28);

    // Field 5: BINFLOAT (size 4)
    AVCRawBinWritePaddedString(nit, 16,
                               reinterpret_cast<const GByte*>("FLT_COL"));
    AVCRawBinWriteInt16(nit, 4);
    AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWriteInt16(nit, 25);
    AVCRawBinWriteInt16(nit, 4);
    AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWriteInt16(nit, 12);
    AVCRawBinWriteInt16(nit, 3);
    AVCRawBinWriteInt16(nit, 6);  // nType1 = 6
    AVCRawBinWriteInt16(nit, 0);
    for (int i = 0; i < 4; ++i) AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWritePaddedString(nit, 16, reinterpret_cast<const GByte*>(""));
    AVCRawBinWriteZeros(nit, 56);
    AVCRawBinWriteInt16(nit, 5);
    AVCRawBinWriteZeros(nit, 28);

    // Field 6: BINFLOAT (size 8)
    AVCRawBinWritePaddedString(nit, 16,
                               reinterpret_cast<const GByte*>("DBL_COL"));
    AVCRawBinWriteInt16(nit, 8);
    AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWriteInt16(nit, 29);
    AVCRawBinWriteInt16(nit, 4);
    AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWriteInt16(nit, 18);
    AVCRawBinWriteInt16(nit, 5);
    AVCRawBinWriteInt16(nit, 6);  // nType1 = 6
    AVCRawBinWriteInt16(nit, 0);
    for (int i = 0; i < 4; ++i) AVCRawBinWriteInt16(nit, -1);
    AVCRawBinWritePaddedString(nit, 16, reinterpret_cast<const GByte*>(""));
    AVCRawBinWriteZeros(nit, 56);
    AVCRawBinWriteInt16(nit, 6);
    AVCRawBinWriteZeros(nit, 28);

    AVCRawBinClose(nit);
  }

  // 3. Create ARC0000.DAT (2 records, each 42 bytes):
  const std::string dat0_path = info_dir + "arc0000.dat";
  {
    AVCRawBinFile* dat =
        AVCRawBinOpen(dat0_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, dat);

    // Record 1
    AVCRawBinWritePaddedString(dat, 8,
                               reinterpret_cast<const GByte*>("20260101"));
    AVCRawBinWritePaddedString(dat, 10,
                               reinterpret_cast<const GByte*>("Alpha"));
    AVCRawBinWriteInt32(dat, 12345);
    AVCRawBinWriteInt16(dat, 42);
    AVCRawBinWriteFloat(dat, 3.14f);
    AVCRawBinWriteDouble(dat, 2.718281828);
    AVCRawBinWriteZeros(dat, 6);  // Pad to 42 bytes

    // Record 2
    AVCRawBinWritePaddedString(dat, 8,
                               reinterpret_cast<const GByte*>("20261231"));
    AVCRawBinWritePaddedString(dat, 10, reinterpret_cast<const GByte*>("Beta"));
    AVCRawBinWriteInt32(dat, 67890);
    AVCRawBinWriteInt16(dat, 99);
    AVCRawBinWriteFloat(dat, 6.28f);
    AVCRawBinWriteDouble(dat, 1.41421356);
    AVCRawBinWriteZeros(dat, 6);

    AVCRawBinClose(dat);
  }

  // 4. Create ARC0001.NIT + ARC0001.DAT (External table TEST.PAT):
  const std::string nit1_path = info_dir + "arc0001.nit";
  {
    AVCRawBinFile* nit1 =
        AVCRawBinOpen(nit1_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, nit1);
    AVCRawBinWritePaddedString(nit1, 16,
                               reinterpret_cast<const GByte*>("EXT_ID"));
    AVCRawBinWriteInt16(nit1, 4);
    AVCRawBinWriteInt16(nit1, -1);
    AVCRawBinWriteInt16(nit1, 1);
    AVCRawBinWriteInt16(nit1, 4);
    AVCRawBinWriteInt16(nit1, -1);
    AVCRawBinWriteInt16(nit1, 5);
    AVCRawBinWriteInt16(nit1, 0);
    AVCRawBinWriteInt16(nit1, 5);
    AVCRawBinWriteInt16(nit1, 0);
    for (int i = 0; i < 4; ++i) AVCRawBinWriteInt16(nit1, -1);
    AVCRawBinWritePaddedString(nit1, 16, reinterpret_cast<const GByte*>(""));
    AVCRawBinWriteZeros(nit1, 56);
    AVCRawBinWriteInt16(nit1, 1);
    AVCRawBinWriteZeros(nit1, 28);
    AVCRawBinClose(nit1);
  }

  // In ARC0001.DAT, write 80 bytes containing relative path "ext_data.dat"
  const std::string dat1_path = info_dir + "arc0001.dat";
  {
    AVCRawBinFile* dat1 =
        AVCRawBinOpen(dat1_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, dat1);
    char rel_path[80];
    std::memset(rel_path, ' ', sizeof(rel_path));
    std::memcpy(rel_path, "ext_data.dat", 12);
    AVCRawBinWriteBytes(dat1, 80, reinterpret_cast<const GByte*>(rel_path));
    AVCRawBinClose(dat1);
  }

  // Create external data file in info_dir:
  const std::string ext_data_path = info_dir + "ext_data.dat";
  {
    AVCRawBinFile* ext =
        AVCRawBinOpen(ext_data_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, ext);
    AVCRawBinWriteInt32(ext, 9999);
    AVCRawBinClose(ext);
  }

  // 5. Create ARC0002.NIT + ARC0002.DAT (Empty table TEST.EMT):
  const std::string nit2_path = info_dir + "arc0002.nit";
  {
    AVCRawBinFile* nit2 =
        AVCRawBinOpen(nit2_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, nit2);
    AVCRawBinWritePaddedString(nit2, 16,
                               reinterpret_cast<const GByte*>("EMPTY_ID"));
    AVCRawBinWriteInt16(nit2, 4);
    AVCRawBinWriteInt16(nit2, -1);
    AVCRawBinWriteInt16(nit2, 1);
    AVCRawBinWriteInt16(nit2, 4);
    AVCRawBinWriteInt16(nit2, -1);
    AVCRawBinWriteInt16(nit2, 5);
    AVCRawBinWriteInt16(nit2, 0);
    AVCRawBinWriteInt16(nit2, 5);
    AVCRawBinWriteInt16(nit2, 0);
    for (int i = 0; i < 4; ++i) AVCRawBinWriteInt16(nit2, -1);
    AVCRawBinWritePaddedString(nit2, 16, reinterpret_cast<const GByte*>(""));
    AVCRawBinWriteZeros(nit2, 56);
    AVCRawBinWriteInt16(nit2, 1);
    AVCRawBinWriteZeros(nit2, 28);
    AVCRawBinClose(nit2);
  }
  const std::string dat2_path = info_dir + "arc0002.dat";
  {
    AVCRawBinFile* dat2 =
        AVCRawBinOpen(dat2_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, dat2);
    AVCRawBinClose(dat2);
  }

  // Test AVCBinReadListTables
  char** dat_files = nullptr;
  char** tables = AVCBinReadListTables(info_dir.c_str(), "TEST", &dat_files,
                                       AVCCoverV7, nullptr);
  ASSERT_NE(nullptr, tables);
  absl::Cleanup tables_cleaner = [tables, dat_files] {
    CSLDestroy(tables);
    CSLDestroy(dat_files);
  };
  EXPECT_EQ(
      3, CSLCount(tables));  // TEST.AAT, TEST.PAT, TEST.EMT (DELETED skipped)
  EXPECT_THAT(tables[0], HasSubstr("TEST.AAT"));
  ASSERT_NE(nullptr, dat_files);
  EXPECT_STREQ("ARC0000", dat_files[0]);

  // Also test AVCBinReadListTables with AVCCoverV7Tables (returns all tables
  // regardless of cover name)
  char** all_tables = AVCBinReadListTables(info_dir.c_str(), "NOSUCHCOVER",
                                           nullptr, AVCCoverV7Tables, nullptr);
  ASSERT_NE(nullptr, all_tables);
  absl::Cleanup all_cleaner = [all_tables] { CSLDestroy(all_tables); };
  EXPECT_EQ(3, CSLCount(all_tables));

  // Open TEST.AAT
  AVCBinFile* psTableFile = AVCBinReadOpen(info_dir.c_str(), "TEST.AAT",
                                           AVCCoverV7, AVCFileTABLE, nullptr);
  ASSERT_NE(nullptr, psTableFile);
  absl::Cleanup table_closer = [psTableFile] { AVCBinReadClose(psTableFile); };

  // Verify numRecords corrected from 10 to 2
  ASSERT_NE(nullptr, psTableFile->hdr.psTableDef);
  EXPECT_EQ(2, psTableFile->hdr.psTableDef->numRecords);
  EXPECT_EQ(6, psTableFile->hdr.psTableDef->numFields);

  // Read Record 1
  AVCField* f1 = AVCBinReadNextTableRec(psTableFile);
  ASSERT_NE(nullptr, f1);
  EXPECT_STREQ("20260101", reinterpret_cast<char*>(f1[0].pszStr));
  EXPECT_STREQ("Alpha     ", reinterpret_cast<char*>(f1[1].pszStr));
  EXPECT_EQ(12345, f1[2].nInt32);
  EXPECT_EQ(42, f1[3].nInt16);
  EXPECT_FLOAT_EQ(3.14f, f1[4].fFloat);
  EXPECT_DOUBLE_EQ(2.718281828, f1[5].dDouble);

  // Read Record 2 via generic AVCBinReadNextObject
  void* f2_obj = AVCBinReadNextObject(psTableFile);
  ASSERT_NE(nullptr, f2_obj);
  AVCField* f2 = static_cast<AVCField*>(f2_obj);
  EXPECT_STREQ("20261231", reinterpret_cast<char*>(f2[0].pszStr));
  EXPECT_EQ(67890, f2[2].nInt32);
  EXPECT_EQ(99, f2[3].nInt16);

  // EOF
  EXPECT_EQ(nullptr, AVCBinReadNextTableRec(psTableFile));

  // Direct random access on TABLE via AVCBinReadObject (record 1, offset 0)
  AVCField* f_idx1 = static_cast<AVCField*>(AVCBinReadObject(psTableFile, 1));
  ASSERT_NE(nullptr, f_idx1);
  EXPECT_EQ(12345, f_idx1[2].nInt32);

  // Stride test: record 2 (tests nRecSize * 1 stride)
  AVCField* f_idx2 = static_cast<AVCField*>(AVCBinReadObject(psTableFile, 2));
  ASSERT_NE(nullptr, f_idx2);
  EXPECT_EQ(67890, f_idx2[2].nInt32);

  // Record 3 is EOF
  EXPECT_EQ(nullptr, AVCBinReadObject(psTableFile, 3));

  // Open external table TEST.PAT
  AVCBinFile* psPatFile = AVCBinReadOpen(info_dir.c_str(), "TEST.PAT",
                                         AVCCoverV7, AVCFileTABLE, nullptr);
  ASSERT_NE(nullptr, psPatFile);
  absl::Cleanup pat_closer = [psPatFile] { AVCBinReadClose(psPatFile); };
  AVCField* pat_f = AVCBinReadNextTableRec(psPatFile);
  ASSERT_NE(nullptr, pat_f);
  EXPECT_EQ(9999, pat_f[0].nInt32);

  // Open empty table TEST.EMT
  AVCBinFile* psEmtFile = AVCBinReadOpen(info_dir.c_str(), "TEST.EMT",
                                         AVCCoverV7, AVCFileTABLE, nullptr);
  ASSERT_NE(nullptr, psEmtFile);
  absl::Cleanup emt_closer = [psEmtFile] { AVCBinReadClose(psEmtFile); };
  EXPECT_EQ(0, psEmtFile->hdr.psTableDef->numRecords);
  EXPECT_EQ(nullptr, AVCBinReadNextTableRec(psEmtFile));

  // Open non-existent table fails with CE_Failure
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, AVCBinReadOpen(info_dir.c_str(), "NON_EXISTENT.TAB",
                                      AVCCoverV7, AVCFileTABLE, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 6. AVCCoverWeird coverage tables (arcdr9, ARC000DA / ARC000NI truncation
  // check)
  const std::string arcdr9_path = info_dir + "arcdr9";
  {
    AVCRawBinFile* arcdr9 =
        AVCRawBinOpen(arcdr9_path.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, arcdr9);
    AVCRawBinWritePaddedString(arcdr9, 32,
                               reinterpret_cast<const GByte*>("WEIRD.TAB"));
    AVCRawBinWritePaddedString(arcdr9, 8,
                               reinterpret_cast<const GByte*>("ARC000"));
    AVCRawBinWriteInt16(arcdr9, 1);
    AVCRawBinWriteInt16(arcdr9, 4);
    AVCRawBinWriteZeros(arcdr9, 18);
    AVCRawBinWriteInt16(arcdr9, 0);
    AVCRawBinWriteInt32(arcdr9, 1);
    AVCRawBinWriteZeros(arcdr9, 10);
    AVCRawBinWriteBytes(arcdr9, 2, reinterpret_cast<const GByte*>("  "));
    AVCRawBinWriteZeros(arcdr9, 300);
    AVCRawBinClose(arcdr9);
  }

  // Create truncated 8-char filenames ARC000DA and ARC000NI
  const std::string weird_dat = info_dir + "ARC000DA";
  {
    AVCRawBinFile* wdat =
        AVCRawBinOpen(weird_dat.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, wdat);
    AVCRawBinWriteInt32(wdat, 7777);
    AVCRawBinClose(wdat);
  }
  const std::string weird_nit = info_dir + "ARC000NI";
  {
    AVCRawBinFile* wnit =
        AVCRawBinOpen(weird_nit.c_str(), "w", AVCBigEndian, nullptr);
    ASSERT_NE(nullptr, wnit);
    AVCRawBinWritePaddedString(wnit, 16,
                               reinterpret_cast<const GByte*>("W_ID"));
    AVCRawBinWriteInt16(wnit, 4);
    AVCRawBinWriteInt16(wnit, -1);
    AVCRawBinWriteInt16(wnit, 1);
    AVCRawBinWriteInt16(wnit, 4);
    AVCRawBinWriteInt16(wnit, -1);
    AVCRawBinWriteInt16(wnit, 5);
    AVCRawBinWriteInt16(wnit, 0);
    AVCRawBinWriteInt16(wnit, 5);
    AVCRawBinWriteInt16(wnit, 0);
    for (int i = 0; i < 4; ++i) AVCRawBinWriteInt16(wnit, -1);
    AVCRawBinWritePaddedString(wnit, 16, reinterpret_cast<const GByte*>(""));
    AVCRawBinWriteZeros(wnit, 56);
    AVCRawBinWriteInt16(wnit, 1);
    AVCRawBinWriteZeros(wnit, 28);
    AVCRawBinClose(wnit);
  }

  AVCBinFile* psWeirdTab = AVCBinReadOpen(info_dir.c_str(), "WEIRD.TAB",
                                          AVCCoverWeird, AVCFileTABLE, nullptr);
  ASSERT_NE(nullptr, psWeirdTab);
  absl::Cleanup weird_tab_closer = [psWeirdTab] {
    AVCBinReadClose(psWeirdTab);
  };

  AVCField* wf = AVCBinReadNextTableRec(psWeirdTab);
  ASSERT_NE(nullptr, wf);
  EXPECT_EQ(7777, wf[0].nInt32);
}

TEST(AvcBinTest, ReadPcDbfTablesAndRepairFieldNames) {
  const std::string dir = "/vsimem/avc_bin_dbf_test";
  ASSERT_EQ(0, VSIMkdir(dir.c_str(), 0777));
  absl::Cleanup dir_cleaner = [&dir] { VSIRmdirRecursive(dir.c_str()); };

  // Create a DBF table using Shapelib
  const std::string dbf_path = dir + "/test.dbf";
  {
    DBFHandle hDBF = DBFCreate(dbf_path.c_str());
    ASSERT_NE(nullptr, hDBF);
    absl::Cleanup dbf_create_cleaner = [hDBF] { DBFClose(hDBF); };

    // Fields to test name repair and all types
    EXPECT_NE(-1, DBFAddField(hDBF, "COVER_", FTInteger, 5, 0));
    EXPECT_NE(-1, DBFAddField(hDBF, "COVER_ID", FTInteger, 5, 0));
    EXPECT_NE(-1, DBFAddField(hDBF, "USER_VAL", FTInteger, 5, 0));
    EXPECT_NE(-1, DBFAddField(hDBF, "PLAIN", FTInteger, 5, 0));
    EXPECT_NE(-1, DBFAddField(hDBF, "AREA", FTDouble, 12, 3));
    EXPECT_NE(-1, DBFAddNativeFieldType(hDBF, "F_FLOAT", 'F', 12, 3));
    EXPECT_NE(-1, DBFAddNativeFieldType(hDBF, "REC_DATE", 'D', 8, 0));
    EXPECT_NE(-1, DBFAddField(hDBF, "NAME", FTString, 10, 0));

    // Record 0
    EXPECT_TRUE(DBFWriteIntegerAttribute(hDBF, 0, 0, 100));
    EXPECT_TRUE(DBFWriteIntegerAttribute(hDBF, 0, 1, 101));
    EXPECT_TRUE(DBFWriteIntegerAttribute(hDBF, 0, 2, 102));
    EXPECT_TRUE(DBFWriteIntegerAttribute(hDBF, 0, 3, 103));
    EXPECT_TRUE(DBFWriteDoubleAttribute(hDBF, 0, 4, 12.345));
    EXPECT_TRUE(DBFWriteDoubleAttribute(hDBF, 0, 5, 67.89));
    EXPECT_TRUE(DBFWriteAttributeDirectly(hDBF, 0, 6, "20260101"));
    EXPECT_TRUE(DBFWriteStringAttribute(hDBF, 0, 7, "Hello"));

    // Record 1
    EXPECT_TRUE(DBFWriteIntegerAttribute(hDBF, 1, 0, 200));
    EXPECT_TRUE(DBFWriteIntegerAttribute(hDBF, 1, 1, 201));
    EXPECT_TRUE(DBFWriteIntegerAttribute(hDBF, 1, 2, 202));
    EXPECT_TRUE(DBFWriteIntegerAttribute(hDBF, 1, 3, 203));
    EXPECT_TRUE(DBFWriteDoubleAttribute(hDBF, 1, 4, 99.999));
    EXPECT_TRUE(DBFWriteDoubleAttribute(hDBF, 1, 5, 11.11));
    EXPECT_TRUE(DBFWriteAttributeDirectly(hDBF, 1, 6, "20261231"));
    EXPECT_TRUE(DBFWriteStringAttribute(hDBF, 1, 7, "World"));
  }

  // Open through AVCBinReadOpen with AVCCoverPC and AVCFileTABLE
  AVCBinFile* psFile = AVCBinReadOpen(dbf_path.c_str(), "TEST.AAT", AVCCoverPC,
                                      AVCFileTABLE, nullptr);
  ASSERT_NE(nullptr, psFile);
  absl::Cleanup dbf_closer = [psFile] { AVCBinReadClose(psFile); };

  ASSERT_NE(nullptr, psFile->hdr.psTableDef);
  EXPECT_EQ(8, psFile->hdr.psTableDef->numFields);
  EXPECT_EQ(2, psFile->hdr.psTableDef->numRecords);

  // Verify repaired field names
  EXPECT_STREQ("COVER#", psFile->hdr.psTableDef->pasFieldDef[0].szName);
  EXPECT_STREQ("COVER-ID", psFile->hdr.psTableDef->pasFieldDef[1].szName);
  EXPECT_STREQ("USER_VAL", psFile->hdr.psTableDef->pasFieldDef[2].szName);
  EXPECT_STREQ("PLAIN", psFile->hdr.psTableDef->pasFieldDef[3].szName);

  // Read Record 0
  AVCField* f0 = AVCBinReadNextTableRec(psFile);
  ASSERT_NE(nullptr, f0);
  EXPECT_EQ(100, f0[0].nInt32);
  EXPECT_EQ(101, f0[1].nInt32);
  EXPECT_EQ(102, f0[2].nInt32);
  EXPECT_EQ(103, f0[3].nInt32);
  EXPECT_FLOAT_EQ(12.345f, f0[4].fFloat);
  EXPECT_FLOAT_EQ(67.89f, f0[5].fFloat);
  EXPECT_STREQ("20260101", reinterpret_cast<char*>(f0[6].pszStr));
  EXPECT_STREQ("Hello", reinterpret_cast<char*>(f0[7].pszStr));

  // Before reading Record 1, mutate field 2 to size 2 (16-bit int) and field 5
  // to size 8 (double) to exercise those branches in _AVCBinReadNextDBFTableRec
  psFile->hdr.psTableDef->pasFieldDef[2].nSize = 2;
  psFile->hdr.psTableDef->pasFieldDef[5].nSize = 8;

  AVCField* f1 = AVCBinReadNextTableRec(psFile);
  ASSERT_NE(nullptr, f1);
  EXPECT_EQ(202, f1[2].nInt16);
  EXPECT_DOUBLE_EQ(11.11, f1[5].dDouble);

  // Record 2 -> EOF
  EXPECT_EQ(nullptr, AVCBinReadNextTableRec(psFile));

  // Test unsupported field type in _AVCBinReadNextDBFTableRec
  psFile->nCurDBFRecord = -1;
  psFile->hdr.psTableDef->pasFieldDef[0].nType1 = 99;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, AVCBinReadNextTableRec(psFile));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
  }

  // Test non-existent DBF file fails cleanly
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              AVCBinReadOpen((dir + "/missing.dbf").c_str(), "MISSING.AAT",
                             AVCCoverPC, AVCFileTABLE, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Direct test of _AVCBinReadRepairDBFFieldName edge cases
  char no_underscore[] = "NOUNDERSCORE";
  _AVCBinReadRepairDBFFieldName(no_underscore);
  EXPECT_STREQ("NOUNDERSCORE", no_underscore);

  char ends_id[] = "PREFIX_ID";
  _AVCBinReadRepairDBFFieldName(ends_id);
  EXPECT_STREQ("PREFIX-ID", ends_id);

  char ends_under[] = "PREFIX_";
  _AVCBinReadRepairDBFFieldName(ends_under);
  EXPECT_STREQ("PREFIX#", ends_under);
}

}  // namespace
}  // namespace autotest2
