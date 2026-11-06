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
// White-box tests for HFABand block I/O, compression, overviews, and color
// tables (hfa_p.h / hfaband.cpp).
//
// Note: White-box tests depend on hfa_p.h internals and assume little-endian
// host byte order (HFAStandard is a no-op on CPL_LSB).
//
// See also:
//   https://gdal.org/drivers/raster/hfa.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/hfa/hfaband.cpp

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <span>  // NOLINT
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/hfa/hfa.h"
#include "third_party/gdal/frmts/hfa/hfa_p.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/hfaband_test";

// Compressed block header in hfaband.cpp (UncompressBlock):
//   0..3   nDataMin (GUInt32)
//   4..7   nNumRuns (GInt32; -1 signals reduced-precision mode)
//   8..11  nDataOffset (GInt32)
//   12     nNumBits (GByte)
constexpr int kCompressedHeaderBytes = 13;
constexpr int kIntMax = std::numeric_limits<int>::max();

constexpr std::array<EPTType, 13> kAllEptTypes = {
    EPT_u1,  EPT_u2,  EPT_u4,  EPT_u8,  EPT_s8,  EPT_u16, EPT_s16,
    EPT_u32, EPT_s32, EPT_f32, EPT_f64, EPT_c64, EPT_c128};
static_assert(kAllEptTypes.size() == EPT_MAX + 1);

struct HfaCloser {
  void operator()(HFAHandle hfa) const {
    if (hfa != nullptr) {
      EXPECT_EQ(CE_None, HFAClose(hfa));
    }
  }
};

using HfaPtr = std::unique_ptr<hfainfo, HfaCloser>;

template <typename T>
void WriteHfaLe(GByte* dst, size_t offset, T val) {
  HFAStandard(static_cast<int>(sizeof(T)), &val);
  std::memcpy(dst + offset, &val, sizeof(T));
}

std::vector<GByte> ReadVsiMemBytes(const char* path) {
  vsi_l_offset length = 0;
  GByte* raw = VSIGetMemFileBuffer(path, &length, FALSE);
  if (raw == nullptr || length == 0) {
    return {};
  }
  return std::vector<GByte>(raw, raw + static_cast<size_t>(length));
}

void WriteVsiMemBytes(const char* path, std::span<const GByte> bytes) {
  std::unique_ptr<GByte, decltype(&VSIFree)> owned(
      static_cast<GByte*>(VSIMalloc(bytes.size())), &VSIFree);
  ASSERT_NE(nullptr, owned);
  std::memcpy(owned.get(), bytes.data(), bytes.size());
  EXPECT_EQ(0, VSIFCloseL(VSIFileFromMemBuffer(path, owned.release(),
                                               bytes.size(), TRUE)));
}

void SetLayerDims(HFAEntry* layer, int width, int height, int block_width,
                  int block_height) {
  ASSERT_NE(nullptr, layer);
  EXPECT_EQ(CE_None, layer->SetIntField("width", width));
  EXPECT_EQ(CE_None, layer->SetIntField("height", height));
  EXPECT_EQ(CE_None, layer->SetIntField("blockWidth", block_width));
  EXPECT_EQ(CE_None, layer->SetIntField("blockHeight", block_height));
}

void RegisterCustomTypes(HFAHandle hfa, const char* defn_text) {
  const char* psz = defn_text;
  while (psz != nullptr && *psz != '\0' && *psz != '.') {
    auto t = std::make_unique<HFAType>();
    const char* next = t->Initialize(psz);
    ASSERT_NE(nullptr, next);
    ASSERT_TRUE(t->CompleteDefn(hfa->poDictionary));
    hfa->poDictionary->AddType(t.release());
    psz = next;
  }
  if (!hfa->poDictionary->osDictionaryText.empty() &&
      hfa->poDictionary->osDictionaryText.back() == '.') {
    hfa->poDictionary->osDictionaryText.pop_back();
  }
  hfa->poDictionary->osDictionaryText += defn_text;
  if (!hfa->poDictionary->osDictionaryText.ends_with('.')) {
    hfa->poDictionary->osDictionaryText += '.';
  }
  hfa->poDictionary->bDictionaryTextDirty = true;
}

// Writes a raw compressed block into band 0's first block slot and invokes
// HFABand::GetRasterBlock() to exercise UncompressBlock().
CPLErr RunUncompressWithPayload(EPTType eDataType,
                                std::span<const GByte> compressed_payload,
                                std::vector<GByte>* out_pixels) {
  constexpr char kPath[] = "/vsimem/hfaband_test/uncomp_helper.img";
  absl::Cleanup cleanup = [kPath] { VSIUnlink(kPath); };

  CPLStringList options({"COMPRESSED=YES", "BLOCKSIZE=32"});
  HfaPtr hfa(HFACreate(kPath, 32, 32, 1, eDataType, options.List()));
  if (hfa == nullptr) {
    return CE_Failure;
  }

  HFABand* band = hfa->papoBand[0];
  const int nBlockBytes =
      (band->nBlockXSize * band->nBlockYSize * HFAGetDataTypeBits(eDataType) +
       7) /
      8;
  const int nBufBytes =
      std::max(nBlockBytes, band->nBlockXSize * band->nBlockYSize);

  const GUInt32 block_offset = HFAAllocateSpace(
      hfa.get(), static_cast<GUInt32>(compressed_payload.size()));
  if (VSIFSeekL(hfa->fp, block_offset, SEEK_SET) != 0 ||
      VSIFWriteL(compressed_payload.data(), 1, compressed_payload.size(),
                 hfa->fp) != compressed_payload.size()) {
    return CE_Failure;
  }

  HFAEntry* dms = band->poNode->GetNamedChild("RasterDMS");
  if (dms == nullptr) {
    return CE_Failure;
  }
  dms->SetStringField("blockinfo[0].logvalid", "true");
  dms->SetIntField("blockinfo[0].offset", static_cast<int>(block_offset));
  dms->SetIntField("blockinfo[0].size",
                   static_cast<int>(compressed_payload.size()));
  if (HFAClose(hfa.release()) != CE_None) {
    return CE_Failure;
  }

  hfa.reset(HFAOpen(kPath, "rb"));
  if (hfa == nullptr || hfa->papoBand == nullptr) {
    return CE_Failure;
  }

  out_pixels->assign(nBufBytes, 0);
  return hfa->papoBand[0]->GetRasterBlock(0, 0, out_pixels->data(),
                                          nBlockBytes);
}

std::vector<GByte> MakeCompressedHeader(GUInt32 data_min, GInt32 num_runs,
                                        GInt32 data_offset, GByte num_bits) {
  std::vector<GByte> hdr(kCompressedHeaderBytes, 0);
  WriteHfaLe(hdr.data(), 0, data_min);
  WriteHfaLe(hdr.data(), 4, num_runs);
  WriteHfaLe(hdr.data(), 8, data_offset);
  hdr[12] = num_bits;
  return hdr;
}

TEST(HfaBandTest, ConstructorValidationAndNoDataReload) {
  constexpr char kPath[] = "/vsimem/hfaband_test/ctor.img";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  // 1. Set nodata, close, and reopen to exercise constructor nodata loading.
  HfaPtr hfa(HFACreate(kPath, 32, 32, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);
  EXPECT_EQ(CE_None, hfa->papoBand[0]->SetNoDataValue(77.5));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  EXPECT_TRUE(hfa->papoBand[0]->bNoDataSet);
  EXPECT_DOUBLE_EQ(77.5, hfa->papoBand[0]->dfNoData);

  // 2. Construct HFABand with invalid dimensions, invalid pixelType (>
  // EPT_MAX),
  //    and block-count overflow.
  HFAEntry* bad_dims =
      HFAEntry::New(hfa.get(), "BadDims", "Eimg_Layer", hfa->poRoot);
  ASSERT_NO_FATAL_FAILURE(SetLayerDims(bad_dims, 0, 32, 32, 32));

  HFAEntry* bad_type =
      HFAEntry::New(hfa.get(), "BadType", "Eimg_Layer", hfa->poRoot);
  ASSERT_NO_FATAL_FAILURE(SetLayerDims(bad_type, 32, 32, 32, 32));
  ASSERT_NE(nullptr, bad_type->MakeData());
  EXPECT_EQ(CE_None, bad_type->SetIntField("pixelType", 99));

  HFAEntry* huge_dims =
      HFAEntry::New(hfa.get(), "HugeDims", "Eimg_Layer", hfa->poRoot);
  ASSERT_NO_FATAL_FAILURE(SetLayerDims(huge_dims, 100000, 100000, 1, 1));

  {
    WithQuietHandler quiet;
    CPLErrorReset();

    HFABand band_bad_dims(hfa.get(), bad_dims);
    EXPECT_EQ(0, band_bad_dims.nWidth);
    std::vector<GByte> dummy(32, 0);
    EXPECT_EQ(CE_Failure, band_bad_dims.GetRasterBlock(0, 0, dummy.data(), 32));

    HFABand band_bad_type(hfa.get(), bad_type);
    EXPECT_EQ(0, band_bad_type.nWidth);

    HFABand band_huge(hfa.get(), huge_dims);
    EXPECT_EQ(0, band_huge.nWidth);
    CPLErrorReset();
  }

  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
}

TEST(HfaBandTest, NullBlockAcrossAllDataTypes) {
  constexpr char kPath[] = "/vsimem/hfaband_test/nullblock.img";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  CPLStringList options({"BLOCKSIZE=32"});

  for (EPTType dt : kAllEptTypes) {
    HfaPtr hfa(HFACreate(kPath, 32, 32, 1, dt, options.List()));
    ASSERT_NE(nullptr, hfa);
    HFABand* band = hfa->papoBand[0];

    const int nBytes =
        (band->nBlockXSize * band->nBlockYSize * HFAGetDataTypeBits(dt) + 7) /
        8;
    const int nBufBytes =
        std::max(nBytes, band->nBlockXSize * band->nBlockYSize);
    std::vector<GByte> buf(nBufBytes, 0xab);

    // 1. Unset nodata fills with 0.
    EXPECT_EQ(CE_None, band->GetRasterBlock(0, 0, buf.data(), nBytes));
    EXPECT_EQ(0, buf[0]);

    // 2. Sub-byte nodata packing (u1, u2) and standard nodata across all types.
    if (dt == EPT_u1) {
      EXPECT_EQ(CE_None, band->SetNoDataValue(0.0));
      EXPECT_EQ(CE_None, band->GetRasterBlock(0, 0, buf.data(), nBytes));
      EXPECT_EQ(0x00, buf[0]);
      EXPECT_EQ(CE_None, band->SetNoDataValue(1.0));
      EXPECT_EQ(CE_None, band->GetRasterBlock(0, 0, buf.data(), nBytes));
      EXPECT_EQ(0xff, buf[0]);
    } else if (dt == EPT_u2) {
      constexpr GByte kExpectedU2Bytes[] = {0x00, 0x55, 0xaa, 0xff};
      for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(CE_None, band->SetNoDataValue(static_cast<double>(i)));
        EXPECT_EQ(CE_None, band->GetRasterBlock(0, 0, buf.data(), nBytes));
        EXPECT_EQ(kExpectedU2Bytes[i], buf[0]);
      }
    } else {
      EXPECT_EQ(CE_None, band->SetNoDataValue(5.0));
      EXPECT_EQ(CE_None, band->GetRasterBlock(0, 0, buf.data(), nBytes));
      EXPECT_NE(0xab, buf[0]);
    }

    ASSERT_EQ(CE_None, HFAClose(hfa.release()));
  }
}

TEST(HfaBandTest, CompressedBlockRoundTripAcrossDataTypes) {
  constexpr char kPath[] = "/vsimem/hfaband_test/comp_types.img";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  CPLStringList comp_opts({"COMPRESSED=YES", "BLOCKSIZE=32"});

  // RLE compression supports all 10 non-double/non-complex EPT types.
  for (EPTType dt : std::span<const EPTType>(kAllEptTypes).first(10)) {
    HfaPtr chfa(HFACreate(kPath, 32, 32, 1, dt, comp_opts.List()));
    ASSERT_NE(nullptr, chfa);
    HFABand* cband = chfa->papoBand[0];
    const int nPixels = cband->nBlockXSize * cband->nBlockYSize;
    const int nBytes = (nPixels * HFAGetDataTypeBits(dt) + 7) / 8;
    std::vector<GByte> write_buf(nBytes, 0);
    if (dt == EPT_u1 || dt == EPT_u2 || dt == EPT_u4 || dt == EPT_u8 ||
        dt == EPT_s8) {
      std::memset(write_buf.data(), 0x55, nBytes / 2);
    } else if (dt == EPT_u16 || dt == EPT_s16) {
      for (int i = 0; i < nPixels / 2; ++i) {
        const GUInt16 val = 42;
        std::memcpy(write_buf.data() + i * sizeof(GUInt16), &val, sizeof(val));
      }
    } else if (dt == EPT_u32 || dt == EPT_s32) {
      for (int i = 0; i < nPixels / 2; ++i) {
        const GUInt32 val = 1000;
        std::memcpy(write_buf.data() + i * sizeof(GUInt32), &val, sizeof(val));
      }
    } else if (dt == EPT_f32) {
      for (int i = 0; i < nPixels / 2; ++i) {
        const float val = 2.5f;
        std::memcpy(write_buf.data() + i * sizeof(float), &val, sizeof(val));
      }
    }
    EXPECT_EQ(CE_None, cband->SetRasterBlock(0, 0, write_buf.data()));
    std::vector<GByte> read_buf(nBytes, 0);
    EXPECT_EQ(CE_None, cband->GetRasterBlock(0, 0, read_buf.data(), nBytes));
    EXPECT_EQ(write_buf, read_buf);
    ASSERT_EQ(CE_None, HFAClose(chfa.release()));
  }
}

TEST(HfaBandTest, CompressedReAllocAndReadOnlyErrors) {
  constexpr char kPath[] = "/vsimem/hfaband_test/rw.img";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  CPLStringList comp_opts({"COMPRESSED=YES", "BLOCKSIZE=32"});

  // 1. ReAllocBlock reuse, growth, incompressible fallback, and buffer-size
  // error.
  HfaPtr hfa(HFACreate(kPath, 64, 32, 1, EPT_u8, comp_opts.List()));
  ASSERT_NE(nullptr, hfa);
  HFABand* band = hfa->papoBand[0];

  std::vector<GByte> blk(1024, 10);
  std::fill_n(blk.begin(), 256, 20);
  EXPECT_EQ(CE_None, band->SetRasterBlock(0, 0, blk.data()));

  // Smaller compressed size reuses existing block.
  std::fill(blk.begin(), blk.end(), 10);
  EXPECT_EQ(CE_None, band->SetRasterBlock(0, 0, blk.data()));

  // Larger compressed size allocates new block.
  for (int i = 0; i < 1024; ++i) {
    blk[i] = static_cast<GByte>(i / 16);
  }
  EXPECT_EQ(CE_None, band->SetRasterBlock(0, 0, blk.data()));

  // Incompressible block falls back to uncompressed storage.
  for (int i = 0; i < 1024; ++i) {
    blk[i] = (i % 2 == 0) ? 0 : 255;
  }
  EXPECT_EQ(CE_None, band->SetRasterBlock(0, 0, blk.data()));

  std::vector<GByte> out(1024, 0);
  EXPECT_EQ(CE_None, band->GetRasterBlock(0, 0, out.data(), 1024));
  EXPECT_EQ(blk, out);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->GetRasterBlock(0, 0, out.data(), 10));
    CPLErrorReset();
  }

  HFAEntry* dms = band->poNode->GetNamedChild("RasterDMS");
  ASSERT_NE(nullptr, dms);
  dms->SetStringField("blockinfo[1].logvalid", "true");
  dms->SetIntField("blockinfo[1].offset", 999999);
  dms->SetIntField("blockinfo[1].size", 32);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  // 2. Out-of-bounds offset in r+b vs rb mode and read-only write errors.
  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  EXPECT_EQ(CE_None, hfa->papoBand[0]->GetRasterBlock(1, 0, out.data(), 1024));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "rb"));
  ASSERT_NE(nullptr, hfa);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              hfa->papoBand[0]->GetRasterBlock(1, 0, out.data(), 1024));
    EXPECT_EQ(CE_Failure, hfa->papoBand[0]->SetRasterBlock(0, 0, blk.data()));
    EXPECT_EQ(CE_Failure, hfa->papoBand[0]->SetNoDataValue(1.0));
    CPLErrorReset();
  }
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
}

TEST(HfaBandTest, UncompressedBlockValidityAndCorruptMetadata) {
  constexpr char kUncompPath[] = "/vsimem/hfaband_test/uncomp_rw.img";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  CPLStringList uncomp_opts({"BLOCKSIZE=32"});

  std::vector<GByte> blk(1024, 42);
  std::vector<GByte> out(1024, 0);

  // 1. Write block 0, mark logvalid="false", and verify read zero-fills.
  HfaPtr uhfa(HFACreate(kUncompPath, 32, 32, 1, EPT_u8, uncomp_opts.List()));
  ASSERT_NE(nullptr, uhfa);
  ASSERT_EQ(CE_None, HFAClose(uhfa.release()));

  uhfa.reset(HFAOpen(kUncompPath, "r+b"));
  ASSERT_NE(nullptr, uhfa);
  EXPECT_EQ(CE_None, uhfa->papoBand[0]->SetRasterBlock(0, 0, blk.data()));

  HFAEntry* udms = uhfa->papoBand[0]->poNode->GetNamedChild("RasterDMS");
  ASSERT_NE(nullptr, udms);
  udms->SetStringField("blockinfo[0].logvalid", "false");
  ASSERT_EQ(CE_None, HFAClose(uhfa.release()));

  // 2. Missing RasterDMS causes SetRasterBlock to fail.
  uhfa.reset(HFAOpen(kUncompPath, "r+b"));
  ASSERT_NE(nullptr, uhfa);
  EXPECT_EQ(CE_None, uhfa->papoBand[0]->GetRasterBlock(0, 0, out.data(), 1024));
  udms = uhfa->papoBand[0]->poNode->GetNamedChild("RasterDMS");
  ASSERT_NE(nullptr, udms);
  ASSERT_EQ(CE_None, udms->RemoveAndDestroy());
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, uhfa->papoBand[0]->SetRasterBlock(0, 0, blk.data()));
    CPLErrorReset();
  }
  ASSERT_EQ(CE_None, HFAClose(uhfa.release()));

  // 3. logvalid="false" with offset=0 fails write; negative block size fails
  // read.
  uhfa.reset(HFACreate(kUncompPath, 32, 32, 1, EPT_u8, uncomp_opts.List()));
  ASSERT_NE(nullptr, uhfa);
  udms = uhfa->papoBand[0]->poNode->GetNamedChild("RasterDMS");
  ASSERT_NE(nullptr, udms);
  udms->SetStringField("blockinfo[0].logvalid", "false");
  udms->SetIntField("blockinfo[0].offset", 0);
  ASSERT_EQ(CE_None, HFAClose(uhfa.release()));

  uhfa.reset(HFAOpen(kUncompPath, "r+b"));
  ASSERT_NE(nullptr, uhfa);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, uhfa->papoBand[0]->SetRasterBlock(0, 0, blk.data()));
    CPLErrorReset();
  }
  udms = uhfa->papoBand[0]->poNode->GetNamedChild("RasterDMS");
  ASSERT_NE(nullptr, udms);
  udms->SetStringField("blockinfo[0].logvalid", "true");
  udms->SetIntField("blockinfo[0].offset", 999999);
  ASSERT_EQ(CE_None, HFAClose(uhfa.release()));

  uhfa.reset(HFAOpen(kUncompPath, "r+b"));
  ASSERT_NE(nullptr, uhfa);
  EXPECT_EQ(CE_None, uhfa->papoBand[0]->GetRasterBlock(0, 0, out.data(), 1024));
  udms = uhfa->papoBand[0]->poNode->GetNamedChild("RasterDMS");
  ASSERT_NE(nullptr, udms);
  udms->SetIntField("blockinfo[0].size", -5);
  ASSERT_EQ(CE_None, HFAClose(uhfa.release()));

  uhfa.reset(HFAOpen(kUncompPath, "rb"));
  ASSERT_NE(nullptr, uhfa);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              uhfa->papoBand[0]->GetRasterBlock(0, 0, out.data(), 1024));
    CPLErrorReset();
  }
  ASSERT_EQ(CE_None, HFAClose(uhfa.release()));
}

TEST(HfaBandTest, UncompressBlockReducedPrecision) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  std::vector<GByte> out;
  constexpr int kMaxPixels = 32 * 32;

  // 1. Short (< 13B) payload fails immediately.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, RunUncompressWithPayload(
                              EPT_u8, std::to_array<GByte>({1, 2, 3}), &out));
    CPLErrorReset();
  }

  // 2. Reduced-precision bit widths 0, 1, 2, 4, 8, 16, 32 on EPT_u8.
  for (int nbits : {0, 1, 2, 4, 8, 16, 32}) {
    const int payload_bytes = (nbits * kMaxPixels + 7) / 8;
    std::vector<GByte> payload =
        MakeCompressedHeader(1, -1, kCompressedHeaderBytes, nbits);
    payload.resize(kCompressedHeaderBytes + payload_bytes, 0x05);
    EXPECT_EQ(CE_None, RunUncompressWithPayload(EPT_u8, payload, &out))
        << "nbits=" << nbits;
  }

  // 3. Reduced-precision across remaining supported integer and float32 types.
  for (EPTType dt : {EPT_u1, EPT_u2, EPT_u4, EPT_s8, EPT_u16, EPT_s16, EPT_s32,
                     EPT_u32, EPT_f32}) {
    std::vector<GByte> payload =
        MakeCompressedHeader(0, -1, kCompressedHeaderBytes, 8);
    payload.resize(kCompressedHeaderBytes + kMaxPixels, 0x01);
    EXPECT_EQ(CE_None, RunUncompressWithPayload(dt, payload, &out));
  }

  // 4. Unsupported nbits (3) and unsupported EPT_f64 fail cleanly.
  {
    WithQuietHandler quiet;
    CPLErrorReset();

    std::vector<GByte> bad_nbits =
        MakeCompressedHeader(0, -1, kCompressedHeaderBytes, 3);
    bad_nbits.resize(kCompressedHeaderBytes + 512, 0);
    EXPECT_EQ(CE_Failure, RunUncompressWithPayload(EPT_u8, bad_nbits, &out));

    std::vector<GByte> f64_payload =
        MakeCompressedHeader(0, -1, kCompressedHeaderBytes, 8);
    f64_payload.resize(kCompressedHeaderBytes + kMaxPixels, 0x01);
    EXPECT_EQ(CE_Failure, RunUncompressWithPayload(EPT_f64, f64_payload, &out));
    CPLErrorReset();
  }
}

TEST(HfaBandTest, UncompressBlockRleEdgeCases) {
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  std::vector<GByte> out;

  // 1. Sub-byte EPT_u1 RLE runs across bit boundaries.
  {
    std::vector<GByte> rle_u1 =
        MakeCompressedHeader(0, 2, kCompressedHeaderBytes + 2, 8);
    rle_u1.push_back(4);
    rle_u1.push_back(4);
    rle_u1.push_back(1);
    rle_u1.push_back(0);
    EXPECT_EQ(CE_None, RunUncompressWithPayload(EPT_u1, rle_u1, &out));
    EXPECT_EQ(0x0f, out[0]);
  }

  // 2. Corrupt negative / zero / overflowing repeat-count and size headers.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(
        CE_Failure,
        RunUncompressWithPayload(
            EPT_u8, MakeCompressedHeader(0, -2, kCompressedHeaderBytes, 8),
            &out));
    EXPECT_EQ(CE_Failure, RunUncompressWithPayload(
                              EPT_u8, MakeCompressedHeader(0, 1, -1, 8), &out));
    EXPECT_EQ(
        CE_Failure,
        RunUncompressWithPayload(
            EPT_u8, MakeCompressedHeader(0, kIntMax, kCompressedHeaderBytes, 8),
            &out));
    CPLErrorReset();
  }

  // 3. All 4 repeat-count encodings (1-, 2-, 3-, and 4-byte counts) + clamping.
  {
    std::vector<GByte> rle =
        MakeCompressedHeader(5, 4, kCompressedHeaderBytes + 10, 8);
    rle.push_back(0x02);
    rle.push_back(0x40);
    rle.push_back(0x02);
    rle.push_back(0x80);
    rle.push_back(0x00);
    rle.push_back(0x02);
    rle.push_back(0xc0);
    rle.push_back(0x00);
    rle.push_back(0x07);
    rle.push_back(0xd0);
    rle.push_back(1);
    rle.push_back(2);
    rle.push_back(3);
    rle.push_back(4);
    EXPECT_EQ(CE_None, RunUncompressWithPayload(EPT_u8, rle, &out));
    EXPECT_EQ(6, out[0]);
    EXPECT_EQ(7, out[2]);
    EXPECT_EQ(8, out[4]);
    EXPECT_EQ(9, out[6]);
  }

  // 4. RLE payloads across supported bit widths (0, 1, 2, 4, 16, 32).
  for (int nbits : {0, 1, 2, 4, 16, 32}) {
    std::vector<GByte> rle = MakeCompressedHeader(0, 1, 15, nbits);
    rle.push_back(0x44);
    rle.push_back(0x00);
    rle.resize(15 + (nbits + 7) / 8 + 4, 0x01);
    EXPECT_EQ(CE_None, RunUncompressWithPayload(EPT_u8, rle, &out))
        << "rle nbits=" << nbits;
  }

  // 5. Truncated RLE bit stream and unsupported EPT_f64 RLE decompression.
  {
    WithQuietHandler quiet;
    CPLErrorReset();

    std::vector<GByte> bad_rle = MakeCompressedHeader(0, 1, 14, 5);
    bad_rle.push_back(16);
    bad_rle.resize(20, 0);
    EXPECT_EQ(CE_Failure, RunUncompressWithPayload(EPT_u8, bad_rle, &out));

    std::vector<GByte> f64_rle = MakeCompressedHeader(0, 1, 15, 8);
    f64_rle.push_back(0x44);
    f64_rle.push_back(0x00);
    f64_rle.push_back(0x01);
    EXPECT_EQ(CE_Failure, RunUncompressWithPayload(EPT_f64, f64_rle, &out));
    CPLErrorReset();
  }
}

TEST(HfaBandTest, ExternalSpillFileReadWriteAndErrors) {
  constexpr char kPath[] = "/vsimem/hfaband_test/spill.img";
  constexpr char kIgePath[] = "/vsimem/hfaband_test/spill.ige";
  constexpr char kSubIgePath[] = "/vsimem/hfaband_test/spill_sub.ige";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  // 1. Create external spill file and verify valid vs cleared-bitmap block
  // reads.
  CPLStringList options({"USE_SPILL=YES", "BLOCKSIZE=32"});
  HfaPtr hfa(HFACreate(kPath, 64, 32, 1, EPT_u8, options.List()));
  ASSERT_NE(nullptr, hfa);

  std::vector<GByte> blk0(1024, 123);
  EXPECT_EQ(CE_None, hfa->papoBand[0]->SetRasterBlock(0, 0, blk0.data()));
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  const std::vector<GByte> orig_ige_bytes = ReadVsiMemBytes(kIgePath);
  ASSERT_GT(orig_ige_bytes.size(), 70u);
  std::vector<GByte> patched_ige_bytes = orig_ige_bytes;
  patched_ige_bytes[69] &= ~0x02;
  WriteVsiMemBytes(kIgePath, patched_ige_bytes);

  // Read valid block (0, 0) and invalid block (1, 0) -> zeroed buffer.
  hfa.reset(HFAOpen(kPath, "rb"));
  ASSERT_NE(nullptr, hfa);
  std::vector<GByte> out0(1024, 0);
  std::vector<GByte> out1(1024, 0xff);
  EXPECT_EQ(CE_None, hfa->papoBand[0]->GetRasterBlock(0, 0, out0.data(), 1024));
  EXPECT_EQ(blk0, out0);
  EXPECT_EQ(CE_None, hfa->papoBand[0]->GetRasterBlock(1, 0, out1.data(), 1024));
  EXPECT_EQ(0, out1[0]);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  // 2. Test /vsisubfile/ read and negative 64-bit layerStackDataOffset seek
  //    failure.
  std::vector<GByte> sub_ige_bytes(16, 0);
  sub_ige_bytes.insert(sub_ige_bytes.end(), orig_ige_bytes.begin(),
                       orig_ige_bytes.end());
  WriteVsiMemBytes(kSubIgePath, sub_ige_bytes);
  VSIUnlink(kIgePath);

  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  HFAEntry* ext_dms =
      hfa->papoBand[0]->poNode->GetNamedChild("ExternalRasterDMS");
  ASSERT_NE(nullptr, ext_dms);
  // ImgExternalRaster holds 4-byte scalars plus fileName string (~64 bytes).
  constexpr size_t kExtDmsSize = 120;
  GByte* dms_data = ext_dms->MakeData(kExtDmsSize);
  ASSERT_NE(nullptr, dms_data);
  std::memset(dms_data, 0, kExtDmsSize);
  ext_dms->SetStringField(
      "fileName.string",
      "/vsisubfile/16_5000,/vsimem/hfaband_test/spill_sub.ige");
  ext_dms->SetIntField("layerStackValidFlagsOffset[0]", 49);
  ext_dms->SetIntField("layerStackValidFlagsOffset[1]", 0);
  ext_dms->SetIntField("layerStackDataOffset[0]", -1);
  ext_dms->SetIntField("layerStackDataOffset[1]", -1);
  ext_dms->SetIntField("layerStackCount", 1);
  ext_dms->SetIntField("layerStackIndex", 0);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  CPLFree(hfa->pszPath);
  hfa->pszPath = CPLStrdup("");
  EXPECT_EQ(CE_None, hfa->papoBand[0]->GetRasterBlock(0, 0, out0.data(), 1024));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, hfa->papoBand[0]->SetRasterBlock(0, 0, blk0.data()));
    CPLErrorReset();
  }
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  // 3. Empty pszPath failure, path traversal rejection, corrupt header, and
  //    missing .ige file.
  hfa.reset(HFAOpen(kPath, "rb"));
  ASSERT_NE(nullptr, hfa);
  CPLFree(hfa->pszPath);
  hfa->pszPath = CPLStrdup("");
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              hfa->papoBand[0]->GetRasterBlock(0, 0, out0.data(), 1024));
    CPLErrorReset();
  }
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  ext_dms = hfa->papoBand[0]->poNode->GetNamedChild("ExternalRasterDMS");
  ASSERT_NE(nullptr, ext_dms);
  ext_dms->SetStringField("fileName.string", "../traversal.ige");
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "rb"));
  ASSERT_NE(nullptr, hfa);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              hfa->papoBand[0]->GetRasterBlock(0, 0, out0.data(), 1024));
    CPLErrorReset();
  }
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  ext_dms = hfa->papoBand[0]->poNode->GetNamedChild("ExternalRasterDMS");
  ASSERT_NE(nullptr, ext_dms);
  ext_dms->SetStringField("fileName.string", "spill.ige");
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  std::vector<GByte> corrupt_ige_bytes = orig_ige_bytes;
  std::memcpy(corrupt_ige_bytes.data(), "CORRUPT_HEADER_DATA", 19);
  WriteVsiMemBytes(kIgePath, corrupt_ige_bytes);

  hfa.reset(HFAOpen(kPath, "rb"));
  ASSERT_NE(nullptr, hfa);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              hfa->papoBand[0]->GetRasterBlock(0, 0, out0.data(), 1024));
    CPLErrorReset();
  }
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  VSIUnlink(kIgePath);
  hfa.reset(HFAOpen(kPath, "rb"));
  ASSERT_NE(nullptr, hfa);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              hfa->papoBand[0]->GetRasterBlock(0, 0, out0.data(), 1024));
    CPLErrorReset();
  }
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
}

TEST(HfaBandTest, BandNameGetAndSet) {
  constexpr char kPath[] = "/vsimem/hfaband_test/bandname.img";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  HfaPtr hfa(HFACreate(kPath, 32, 32, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);
  HFABand* band = hfa->papoBand[0];

  EXPECT_STREQ("Layer_1", band->GetBandName());
  band->SetBandName("CustomBand");
  EXPECT_STREQ("CustomBand", band->GetBandName());
  band->SetBandName("");
  EXPECT_STREQ("Layer_1", band->GetBandName());

  HFAEntry* extra_layer =
      HFAEntry::New(hfa.get(), "", "Eimg_Layer", hfa->poRoot);
  ASSERT_NO_FATAL_FAILURE(SetLayerDims(extra_layer, 32, 32, 32, 32));
  HFABand extra_band(hfa.get(), extra_layer);
  EXPECT_TRUE(std::string_view(extra_band.GetBandName()).starts_with("Layer_"));
}

TEST(HfaBandTest, PctColorTableAndBFUniqueBins) {
  constexpr char kPath[] = "/vsimem/hfaband_test/pct.img";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  // 1. Set, clear, and overwrite PCT color table entries on a new band.
  HfaPtr hfa(HFACreate(kPath, 32, 32, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);
  HFABand* band = hfa->papoBand[0];

  int nColors = 0;
  double* r = nullptr;
  double* g = nullptr;
  double* b = nullptr;
  double* a = nullptr;
  double* bins = nullptr;
  EXPECT_EQ(CE_None, band->SetPCT(0, nullptr, nullptr, nullptr, nullptr));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, band->GetPCT(&nColors, &r, &g, &b, &a, &bins));
    CPLErrorReset();
  }

  constexpr double kRed[3] = {0.1, 0.2, 0.3};
  constexpr double kGreen[3] = {0.4, 0.5, 0.6};
  constexpr double kBlue[3] = {0.7, 0.8, 0.9};
  constexpr double kAlpha[3] = {1.0, 0.5, 1.0};
  EXPECT_EQ(CE_None, band->SetPCT(3, kRed, kGreen, kBlue, kAlpha));
  EXPECT_EQ(CE_None, band->SetPCT(0, nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_None, band->SetPCT(3, kRed, kGreen, kBlue, kAlpha));

  // 2. Attach an embedded BFUnique #Bin_Function840# descriptor node.
  constexpr char kBfDefn[] = "{1:oEmif_MIFObject,binFunction,}TestBF840,.";
  ASSERT_NO_FATAL_FAILURE(RegisterCustomTypes(hfa.get(), kBfDefn));

  HFAEntry* desc_table = band->poNode->GetNamedChild("Descriptor_Table");
  ASSERT_NE(nullptr, desc_table);
  HFAEntry* bf_node =
      HFAEntry::New(hfa.get(), "#Bin_Function840#", "TestBF840", desc_table);
  ASSERT_NE(nullptr, bf_node);
  // Layout after setting type="BFUnique" (8B header + 9B "BFUnique\0" = 17B)
  // and MIFDictionary (8B header + 25B = 33B): MIFObject pointer header at
  // offset 50 (size at 50, offset 58 at 54); BASEDATA header (24B) at 58..81; 3
  // doubles (24B) at 82..105. Total = 106 bytes; 160 bytes gives ample slack.
  constexpr size_t kBfDataSize = 160;
  constexpr size_t kMifHeaderOffset = 50;
  constexpr size_t kBasedataOffset = 58;
  constexpr int kBasedataPayloadSize =
      24 + 3 * static_cast<int>(sizeof(double));
  GByte* bf_data = bf_node->MakeData(kBfDataSize);
  ASSERT_NE(nullptr, bf_data);
  std::memset(bf_data, 0, kBfDataSize);
  bf_node->SetPosition();
  ASSERT_EQ(CE_None,
            bf_node->SetStringField("binFunction.type.string", "BFUnique"));
  ASSERT_EQ(CE_None, bf_node->SetStringField("binFunction.MIFDictionary.string",
                                             "{1:*bvalueBD,}BFUnique,."));
  WriteHfaLe(bf_data, kMifHeaderOffset, GInt32{kBasedataPayloadSize});
  WriteHfaLe(bf_data, kMifHeaderOffset + 4, GInt32{kBasedataOffset});
  // Upstream HFAReadBFUniqueBins (hfaband.cpp) checks BASEDATA header bytes
  // 20..21 for LE int16 EPT_f64.
  WriteHfaLe(bf_data, kBasedataOffset + 20, GInt16{EPT_f64});
  constexpr double kBinVals[3] = {10.0, 20.0, 30.0};
  for (size_t i = 0; i < 3; ++i) {
    WriteHfaLe(bf_data, kBasedataOffset + 24 + i * sizeof(double), kBinVals[i]);
  }
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  // 3. Reopen and verify GetPCT loads colors and BFUnique bins, plus
  //    HFAReadBFUniqueBins error cases and default alpha when Opacity is
  //    absent.
  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  EXPECT_EQ(CE_None, hfa->papoBand[0]->GetPCT(&nColors, &r, &g, &b, &a, &bins));
  EXPECT_EQ(3, nColors);
  ASSERT_NE(nullptr, bins);
  EXPECT_DOUBLE_EQ(20.0, bins[1]);

  HFAEntry* reopened_bf = hfa->papoBand[0]->poNode->GetNamedChild(
      "Descriptor_Table.#Bin_Function840#");
  ASSERT_NE(nullptr, reopened_bf);
  EXPECT_EQ(nullptr, HFAReadBFUniqueBins(reopened_bf, 100));
  GByte* reopened_data = reopened_bf->GetData();
  ASSERT_NE(nullptr, reopened_data);
  WriteHfaLe(reopened_data, kBasedataOffset + 20, GInt16{EPT_u1});
  EXPECT_EQ(nullptr, HFAReadBFUniqueBins(reopened_bf, 3));
  WriteHfaLe(reopened_data, kBasedataOffset + 20, GInt16{EPT_f64});
  // Keep same 8-char length as "BFUnique" so variable-length 0:pcstring
  // SetInstValue does not overwrite adjacent MIFDictionary nCount bytes.
  reopened_bf->SetStringField("binFunction.type.string", "OtherTyp");
  EXPECT_EQ(nullptr, HFAReadBFUniqueBins(reopened_bf, 3));
  reopened_bf->SetStringField("binFunction.type.string", "BFUnique");

  HFAEntry* opacity =
      hfa->papoBand[0]->poNode->GetNamedChild("Descriptor_Table.Opacity");
  ASSERT_NE(nullptr, opacity);
  ASSERT_EQ(CE_None, opacity->RemoveAndDestroy());
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  EXPECT_EQ(CE_None, hfa->papoBand[0]->GetPCT(&nColors, &r, &g, &b, &a, &bins));
  EXPECT_EQ(3, nColors);
  ASSERT_NE(nullptr, bins);
  EXPECT_DOUBLE_EQ(1.0, a[0]);

  // 4. Corrupt Red column pointer and non-positive numRows fail GetPCT cleanly.

  HFAEntry* red_col =
      hfa->papoBand[0]->poNode->GetNamedChild("Descriptor_Table.Red");
  ASSERT_NE(nullptr, red_col);
  red_col->SetIntField("columnDataPtr", 999999);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              hfa->papoBand[0]->GetPCT(&nColors, &r, &g, &b, &a, &bins));
    CPLErrorReset();
  }
  red_col = hfa->papoBand[0]->poNode->GetNamedChild("Descriptor_Table.Red");
  ASSERT_NE(nullptr, red_col);
  red_col->SetIntField("numRows", 0);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              hfa->papoBand[0]->GetPCT(&nColors, &r, &g, &b, &a, &bins));
    CPLErrorReset();
  }
  red_col = hfa->papoBand[0]->poNode->GetNamedChild("Descriptor_Table.Red");
  ASSERT_NE(nullptr, red_col);
  red_col->SetIntField("numRows", -1);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "rb"));
  ASSERT_NE(nullptr, hfa);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              hfa->papoBand[0]->GetPCT(&nColors, &r, &g, &b, &a, &bins));
    CPLErrorReset();
  }
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
}

TEST(HfaBandTest, InlineOverviewsAndGrayscaleCompressionOptions) {
  constexpr char kPath[] = "/vsimem/hfaband_test/ovr.img";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  absl::Cleanup reset_tls_opts = [] {
    CPLSetThreadLocalConfigOption("GDAL_HFA_OVR_BLOCKSIZE", nullptr);
    CPLSetThreadLocalConfigOption("HFA_COMPRESS_OVR", nullptr);
  };

  // 1. Create 1-bit band with AVERAGE_BIT2GRAYSCALE and NEAREST overviews.
  HfaPtr hfa(HFACreate(kPath, 64, 64, 1, EPT_u1, nullptr));
  ASSERT_NE(nullptr, hfa);
  EXPECT_EQ(CE_None, hfa->papoBand[0]->SetNoDataValue(0.0));

  CPLSetThreadLocalConfigOption("GDAL_HFA_OVR_BLOCKSIZE", "17");
  CPLSetThreadLocalConfigOption("HFA_COMPRESS_OVR", "YES");
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(0, hfa->papoBand[0]->CreateOverview(2, "AVERAGE_BIT2GRAYSCALE"));
    CPLErrorReset();
  }
  CPLSetThreadLocalConfigOption("GDAL_HFA_OVR_BLOCKSIZE", "32");
  CPLSetThreadLocalConfigOption("HFA_COMPRESS_OVR", nullptr);
  EXPECT_EQ(1, hfa->papoBand[0]->CreateOverview(4, "NEAREST"));
  CPLSetThreadLocalConfigOption("GDAL_HFA_OVR_BLOCKSIZE", nullptr);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  // 2. Reopen and verify idempotent LoadOverviews.
  hfa.reset(HFAOpen(kPath, "rb"));
  ASSERT_NE(nullptr, hfa);
  EXPECT_EQ(CE_None, hfa->papoBand[0]->LoadOverviews());
  EXPECT_EQ(CE_None, hfa->papoBand[0]->LoadOverviews());
  EXPECT_EQ(2, hfa->papoBand[0]->nOverviews);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
}

TEST(HfaBandTest, ExternalRrdOverviewsAndCorruptNameListFallbacks) {
  constexpr char kPath[] = "/vsimem/hfaband_test/ovr.img";
  constexpr char kRrdPath[] = "/vsimem/hfaband_test/ovr.rrd";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  absl::Cleanup reset_tls_opts = [] {
    CPLSetThreadLocalConfigOption("HFA_USE_RRD", nullptr);
  };

  // 1. Create external .rrd overview with HFA_USE_RRD=YES and reopen.
  HfaPtr hfa(HFACreate(kPath, 64, 64, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);
  CPLSetThreadLocalConfigOption("HFA_USE_RRD", "YES");
  EXPECT_EQ(0, hfa->papoBand[0]->CreateOverview(2, "NEAREST"));
  CPLSetThreadLocalConfigOption("HFA_USE_RRD", nullptr);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  EXPECT_EQ(CE_None, hfa->papoBand[0]->LoadOverviews());
  EXPECT_EQ(1, hfa->papoBand[0]->nOverviews);

  // 2. Malformed RRDNamesList entries are skipped while valid entries load.
  HFAEntry* rrd_list = hfa->papoBand[0]->poNode->GetNamedChild("RRDNamesList");
  ASSERT_NE(nullptr, rrd_list);
  rrd_list->SetStringField("nameList[0].string",
                           "wrong_name.rrd(:Layer_1:_ss_2_)");
  rrd_list->SetStringField("nameList[1].string", "no_paren_entry");
  rrd_list->SetStringField("nameList[2].string",
                           "ovr.rrd(:Layer_1:MissingNode)");
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "rb"));
  ASSERT_NE(nullptr, hfa);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None, hfa->papoBand[0]->LoadOverviews());
    CPLErrorReset();
  }
  EXPECT_EQ(1, hfa->papoBand[0]->nOverviews);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  // 3. Unlinking the sidecar .rrd results in 0 overviews on LoadOverviews.
  VSIUnlink(kRrdPath);
  hfa.reset(HFAOpen(kPath, "rb"));
  ASSERT_NE(nullptr, hfa);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None, hfa->papoBand[0]->LoadOverviews());
    CPLErrorReset();
  }
  EXPECT_EQ(0, hfa->papoBand[0]->nOverviews);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  // 4. Zero-width overview subsample layer zeroes band dimensions on load.

  hfa.reset(HFAOpen(kPath, "r+b"));
  ASSERT_NE(nullptr, hfa);
  HFAEntry* zero_ov = HFAEntry::New(
      hfa.get(), "zero_ov", "Eimg_Layer_SubSample", hfa->papoBand[0]->poNode);
  ASSERT_NO_FATAL_FAILURE(SetLayerDims(zero_ov, 0, 32, 32, 32));
  rrd_list = hfa->papoBand[0]->poNode->GetNamedChild("RRDNamesList");
  ASSERT_NE(nullptr, rrd_list);
  rrd_list->SetStringField("nameList[0].string", "ovr.img(:Layer_1:zero_ov)");
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  hfa.reset(HFAOpen(kPath, "rb"));
  ASSERT_NE(nullptr, hfa);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None, hfa->papoBand[0]->LoadOverviews());
    CPLErrorReset();
  }
  EXPECT_EQ(0, hfa->papoBand[0]->nWidth);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
}

TEST(HfaBandTest, AuxProbingAndSpillOverviewCreation) {
  constexpr char kSpillOvrPath[] = "/vsimem/hfaband_test/spill_ovr.img";
  constexpr char kAuxPath[] = "/vsimem/hfaband_test/aux_probe.aux";
  constexpr char kAuxRrdPath[] = "/vsimem/hfaband_test/aux_probe.rrd";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };
  absl::Cleanup reset_tls_opts = [] {
    CPLSetThreadLocalConfigOption("USE_SPILL", nullptr);
  };

  // 1. Create overview with USE_SPILL=YES enabled.
  HfaPtr hfa(HFACreate(kSpillOvrPath, 64, 64, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, hfa);
  CPLSetThreadLocalConfigOption("USE_SPILL", "YES");
  EXPECT_EQ(0, hfa->papoBand[0]->CreateOverview(2, "NEAREST"));
  CPLSetThreadLocalConfigOption("USE_SPILL", nullptr);
  ASSERT_EQ(CE_None, HFAClose(hfa.release()));

  // 2. .aux file without RRDNamesList discovers and sorts sidecar .rrd layers.
  HfaPtr aux_hfa(HFACreate(kAuxPath, 64, 64, 1, EPT_u8, nullptr));
  ASSERT_NE(nullptr, aux_hfa);
  ASSERT_EQ(CE_None, HFAClose(aux_hfa.release()));

  HfaPtr rrd_hfa(HFACreateLL(kAuxRrdPath));
  ASSERT_NE(nullptr, rrd_hfa);
  HFAEntry* rrd_layer =
      HFAEntry::New(rrd_hfa.get(), "Layer_1", "Eimg_Layer", rrd_hfa->poRoot);
  ASSERT_NE(nullptr, rrd_layer);
  HFAEntry* sub16 =
      HFAEntry::New(rrd_hfa.get(), "sub16", "Eimg_Layer_SubSample", rrd_layer);
  ASSERT_NO_FATAL_FAILURE(SetLayerDims(sub16, 16, 16, 16, 16));
  HFAEntry* sub32 =
      HFAEntry::New(rrd_hfa.get(), "sub32", "Eimg_Layer_SubSample", rrd_layer);
  ASSERT_NO_FATAL_FAILURE(SetLayerDims(sub32, 32, 32, 32, 32));
  ASSERT_EQ(CE_None, HFAClose(rrd_hfa.release()));

  aux_hfa.reset(HFAOpen(kAuxPath, "rb"));
  ASSERT_NE(nullptr, aux_hfa);
  EXPECT_EQ(CE_None, aux_hfa->papoBand[0]->LoadOverviews());
  EXPECT_EQ(2, aux_hfa->papoBand[0]->nOverviews);
  EXPECT_EQ(32, aux_hfa->papoBand[0]->papoOverviews[0]->nWidth);
  EXPECT_EQ(16, aux_hfa->papoBand[0]->papoOverviews[1]->nWidth);
  ASSERT_EQ(CE_None, HFAClose(aux_hfa.release()));

  // 3. Zero-width subsample layer in probed .rrd zeroes band width on load.

  rrd_hfa.reset(HFAOpen(kAuxRrdPath, "r+b"));
  ASSERT_NE(nullptr, rrd_hfa);
  rrd_layer = rrd_hfa->poRoot->GetNamedChild("Layer_1");
  ASSERT_NE(nullptr, rrd_layer);
  HFAEntry* sub0 =
      HFAEntry::New(rrd_hfa.get(), "sub0", "Eimg_Layer_SubSample", rrd_layer);
  ASSERT_NO_FATAL_FAILURE(SetLayerDims(sub0, 0, 32, 32, 32));
  ASSERT_EQ(CE_None, HFAClose(rrd_hfa.release()));

  aux_hfa.reset(HFAOpen(kAuxPath, "rb"));
  ASSERT_NE(nullptr, aux_hfa);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None, aux_hfa->papoBand[0]->LoadOverviews());
    CPLErrorReset();
  }
  EXPECT_EQ(0, aux_hfa->papoBand[0]->nWidth);
  ASSERT_EQ(CE_None, HFAClose(aux_hfa.release()));
}

TEST(HfaBandTest, LoadBlockInfoMissingFields) {
  struct MissingDmsFieldCase {
    const char* type_def;
    const char* type_name;
  };
  constexpr MissingDmsFieldCase kCases[] = {
      {.type_def = "{1:Lsize,}TestBI1,{0:poTestBI1,blockinfo,}TestDMS1,",
       .type_name = "TestDMS1"},
      {.type_def = "{1:Loffset,}TestBI2,{0:poTestBI2,blockinfo,}TestDMS2,",
       .type_name = "TestDMS2"},
      {.type_def =
           "{1:Loffset,1:Lsize,}TestBI3,{0:poTestBI3,blockinfo,}TestDMS3,",
       .type_name = "TestDMS3"},
      {.type_def = "{1:Loffset,1:Lsize,1:e2:false,true,logvalid,}TestBI4,"
                   "{0:poTestBI4,blockinfo,}TestDMS4,",
       .type_name = "TestDMS4"},
  };
  static_assert(std::size(kCases) == 4);

  // 1. Missing offset/size/logvalid fields on RasterDMS fail LoadBlockInfo.

  constexpr char kPath[] = "/vsimem/hfaband_test/missing_dms.img";
  absl::Cleanup cleanup_dir = [] { VSIRmdirRecursive(kTestDir); };

  HfaPtr hfa(HFACreateLL(kPath));
  ASSERT_NE(nullptr, hfa);

  std::vector<GByte> buf(1024, 0);

  for (const MissingDmsFieldCase& tc : kCases) {
    ASSERT_NO_FATAL_FAILURE(RegisterCustomTypes(hfa.get(), tc.type_def));

    HFAEntry* layer =
        HFAEntry::New(hfa.get(), tc.type_name, "Eimg_Layer", hfa->poRoot);
    ASSERT_NO_FATAL_FAILURE(SetLayerDims(layer, 32, 32, 32, 32));

    HFAEntry* dms = HFAEntry::New(hfa.get(), "RasterDMS", tc.type_name, layer);
    ASSERT_NE(nullptr, dms);
    // TestDMS holds a 0:poTestBI* pointer (8B header: count=1, offset=8) plus
    // TestBI* struct fields (up to 16B).
    GByte* dms_data = dms->MakeData(64);
    ASSERT_NE(nullptr, dms_data);
    std::memset(dms_data, 0, 64);
    WriteHfaLe(dms_data, 0, GInt32{1});
    WriteHfaLe(dms_data, 4, GInt32{8});

    {
      WithQuietHandler quiet;
      CPLErrorReset();
      HFABand band(hfa.get(), layer);
      EXPECT_EQ(CE_Failure, band.GetRasterBlock(0, 0, buf.data(), 1024))
          << tc.type_name;
      CPLErrorReset();
    }
  }

  ASSERT_EQ(CE_None, HFAClose(hfa.release()));
}

// TODO: b/335317901 - Cover remaining multi-block null/compressed read-only
// error paths in hfaband.cpp.

}  // namespace
}  // namespace autotest2
