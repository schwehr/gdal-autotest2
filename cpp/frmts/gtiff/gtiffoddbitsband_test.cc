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
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_float.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class GTiffOddBitsBandTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    GTiffOneTimeInit();
    driver_ = static_cast<GDALDriver*>(GDALGetDriverByName("GTiff"));
    ASSERT_NE(driver_, nullptr);
  }

  GDALDriver* driver_ = nullptr;
};

TEST_F(GTiffOddBitsBandTest, Float24ContigAndSeparateAndUnknownSampleFormat) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // 24-bit IEEEFP CONTIG (2 bands) and SEPARATE (2 bands).
  for (uint16_t planar : {static_cast<uint16_t>(PLANARCONFIG_CONTIG),
                          static_cast<uint16_t>(PLANARCONFIG_SEPARATE)}) {
    SCOPED_TRACE(planar);
    const char* kFile = (planar == PLANARCONFIG_CONTIG)
                            ? "/vsimem/oddbits_f24_contig.tif"
                            : "/vsimem/oddbits_f24_sep.tif";
    absl::Cleanup c = [kFile]() { VSIUnlink(kFile); };

    VSILFILE* fp = VSIFOpenL(kFile, "wb+");
    ASSERT_NE(fp, nullptr);
    TIFF* tif = VSI_TIFFOpen(kFile, "w+", fp);
    ASSERT_NE(tif, nullptr);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 2);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 2);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 24);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 2);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 2);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, planar);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);

    // 24-bit float little-endian bytes:
    // 1.0f in 24-bit float (1 sign, 7 exp bias 63, 16 mantissa):
    // sign=0, exp=63 (0x3F), mant=0 -> 0x3F0000 -> LE bytes {0x00, 0x00, 0x3F}.
    if (planar == PLANARCONFIG_CONTIG) {
      std::vector<uint8_t> buf(2 * 2 * 2 * 3, 0);
      for (int i = 0; i < 4; ++i) {
        buf[i * 6 + 0] = 0x00;
        buf[i * 6 + 1] = 0x00;
        buf[i * 6 + 2] = 0x3F;  // band 1 = 1.0f
        buf[i * 6 + 3] = 0x00;
        buf[i * 6 + 4] = 0x00;
        buf[i * 6 + 5] = 0x40;  // band 2 = 2.0f
      }
      ASSERT_GT(TIFFWriteEncodedStrip(tif, 0, buf.data(), buf.size()), 0);
    } else {
      std::vector<uint8_t> b1(2 * 2 * 3, 0);
      std::vector<uint8_t> b2(2 * 2 * 3, 0);
      for (int i = 0; i < 4; ++i) {
        b1[i * 3 + 2] = 0x3F;
        b2[i * 3 + 2] = 0x40;
      }
      ASSERT_GT(TIFFWriteEncodedStrip(tif, 0, b1.data(), b1.size()), 0);
      ASSERT_GT(TIFFWriteEncodedStrip(tif, 1, b2.data(), b2.size()), 0);
    }
    TIFFWriteDirectory(tif);
    TIFFClose(tif);
    VSIFCloseL(fp);

    GDALDataset* ds = static_cast<GDALDataset*>(GDALOpen(kFile, GA_Update));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterBand(1)->GetRasterDataType(), GDT_Float32);

    float vals1[4] = {};
    float vals2[4] = {};
    EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(0, 0, vals1), CE_None);
    EXPECT_EQ(ds->GetRasterBand(2)->ReadBlock(0, 0, vals2), CE_None);
    EXPECT_FLOAT_EQ(vals1[0], 1.0f);
    EXPECT_FLOAT_EQ(vals2[0], 2.0f);

    // Exercise IsBaseGTiffClass() returning false during dataset RasterIO.
    float rio_buf[4] = {};
    EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, rio_buf, 2, 2,
                                             GDT_Float32, 0, 0, nullptr),
              CE_None);

    // Writing 24-bit float is unsupported (hits L66-73).
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, vals1), CE_Failure);
    GDALClose(ds);
  }

  // Raw 12-bit SAMPLEFORMAT_VOID -> GDT_Unknown open failure.
  {
    constexpr char kVoid[] = "/vsimem/oddbits_void12.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kVoid); };
    VSILFILE* fp = VSIFOpenL(kVoid, "wb+");
    ASSERT_NE(fp, nullptr);
    TIFF* tif = VSI_TIFFOpen(kVoid, "w+", fp);
    ASSERT_NE(tif, nullptr);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 4);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 4);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 12);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_VOID);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 4);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    std::vector<uint8_t> zeros(32, 0);
    TIFFWriteEncodedStrip(tif, 0, zeros.data(), zeros.size());
    TIFFWriteDirectory(tif);
    TIFFClose(tif);
    VSIFCloseL(fp);

    EXPECT_EQ(GDALOpen(kVoid, GA_ReadOnly), nullptr);
  }
}

TEST_F(GTiffOddBitsBandTest, SeparateAndSingleBandReadWriteAllBitDepths) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // 1. 1-bit SEPARATE (2 bands, width=11 so both 8-pixel fast loop and
  // remainder bits 8..10 are tested, plus GDAL_TIFF_INTERNAL_MASK / promote).
  {
    constexpr char kFile[] = "/vsimem/oddbits_1bit_sep.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kFile); };
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "NBITS", "1");
    opts = CSLSetNameValue(opts, "INTERLEAVE", "BAND");
    GDALDataset* ds = driver_->Create(kFile, 11, 2, 2, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);

    std::vector<uint8_t> b1(22, 0);
    std::vector<uint8_t> b2(22, 0);
    for (int i = 0; i < 22; ++i) {
      b1[i] = (i % 2 == 0) ? 1 : 0;
      b2[i] = (i % 3 == 0) ? 1 : 0;
    }
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, b1.data()), CE_None);
    EXPECT_EQ(ds->GetRasterBand(2)->WriteBlock(0, 0, b2.data()), CE_None);
    GDALClose(ds);

    GDALDataset* rds = static_cast<GDALDataset*>(GDALOpen(kFile, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    std::vector<uint8_t> out1(22, 99);
    std::vector<uint8_t> out2(22, 99);
    EXPECT_EQ(rds->GetRasterBand(1)->ReadBlock(0, 0, out1.data()), CE_None);
    EXPECT_EQ(rds->GetRasterBand(2)->ReadBlock(0, 0, out2.data()), CE_None);
    EXPECT_EQ(out1, b1);
    EXPECT_EQ(out2, b2);
    GDALClose(rds);
  }

  // PromoteTo8Bits via internal mask IFD (`m_bPromoteTo8Bits == true`).
  {
    constexpr char kMaskFile[] = "/vsimem/oddbits_mask_promote.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kMaskFile); };
    CPLConfigOptionSetter internal_mask("GDAL_TIFF_INTERNAL_MASK", "YES",
                                        false);
    GDALDataset* ds = driver_->Create(kMaskFile, 11, 2, 1, GDT_Byte, nullptr);
    ASSERT_NE(ds, nullptr);
    ASSERT_EQ(ds->CreateMaskBand(GMF_PER_DATASET), CE_None);
    GDALRasterBand* mask = ds->GetRasterBand(1)->GetMaskBand();
    ASSERT_NE(mask, nullptr);
    std::vector<uint8_t> mdata(22, 0);
    mdata[0] = 255;
    mdata[9] = 255;
    EXPECT_EQ(mask->WriteBlock(0, 0, mdata.data()), CE_None);
    GDALClose(ds);

    GDALDataset* rds =
        static_cast<GDALDataset*>(GDALOpen(kMaskFile, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    GDALRasterBand* rmask = rds->GetRasterBand(1)->GetMaskBand();
    ASSERT_NE(rmask, nullptr);
    std::vector<uint8_t> rout(22, 0);
    EXPECT_EQ(rmask->ReadBlock(0, 0, rout.data()), CE_None);
    EXPECT_EQ(rout[0], 255);
    EXPECT_EQ(rout[1], 0);
    EXPECT_EQ(rout[9], 255);
    GDALClose(rds);
  }

  // 2. 12-bit single-band (width=5 so non-byte-aligned row & odd/even nibbles,
  // plus >= 2 clipped values > 4095 to hit m_bClipWarn false & true).
  {
    constexpr char kFile[] = "/vsimem/oddbits_12bit_1b.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kFile); };
    char** opts = CSLSetNameValue(nullptr, "NBITS", "12");
    GDALDataset* ds = driver_->Create(kFile, 5, 2, 1, GDT_UInt16, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);

    uint16_t in[10] = {0, 1234, 4095, 5000, 6000, 2730, 1365, 42, 4096, 7};
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, in), CE_None);
    GDALClose(ds);

    GDALDataset* rds = static_cast<GDALDataset*>(GDALOpen(kFile, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    uint16_t out[10] = {};
    EXPECT_EQ(rds->GetRasterBand(1)->ReadBlock(0, 0, out), CE_None);
    EXPECT_EQ(out[0], 0);
    EXPECT_EQ(out[1], 1234);
    EXPECT_EQ(out[2], 4095);
    EXPECT_EQ(out[3], 4095);
    EXPECT_EQ(out[4], 4095);
    EXPECT_EQ(out[5], 2730);
    EXPECT_EQ(out[9], 7);
    GDALClose(rds);
  }

  // 3. Separate/1-band generic bit depths: NBITS=3 (Byte), NBITS=10 (UInt16),
  // NBITS=20 (UInt32), and NBITS=24 (UInt32 special case).
  struct CaseSpec {
    int nbits;
    GDALDataType dt;
    uint32_t max_val;
  };
  const CaseSpec cases[] = {
      {3, GDT_Byte, 7},
      {10, GDT_UInt16, 1023},
      {20, GDT_UInt32, (1U << 20) - 1},
      {24, GDT_UInt32, (1U << 24) - 1},
  };
  for (const auto& spec : cases) {
    SCOPED_TRACE(spec.nbits);
    CPLString fname;
    fname.Printf("/vsimem/oddbits_sep_%d.tif", spec.nbits);
    absl::Cleanup c = [fname]() { VSIUnlink(fname); };
    char** opts =
        CSLSetNameValue(nullptr, "NBITS", CPLSPrintf("%d", spec.nbits));
    GDALDataset* ds = driver_->Create(fname, 5, 2, 1, spec.dt, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);

    if (spec.dt == GDT_Byte) {
      uint8_t in[10] = {0, 1, 5, 7, 20, 30, 2, 3, 4, 6};
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, in), CE_None);
    } else if (spec.dt == GDT_UInt16) {
      uint16_t in[10] = {0, 1, 511, 1023, 2000, 3000, 42, 77, 100, 999};
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, in), CE_None);
    } else {
      uint32_t in[10] = {0,
                         1,
                         0x12345,
                         spec.max_val,
                         spec.max_val + 100,
                         spec.max_val + 200,
                         42,
                         256,
                         65535,
                         99999};
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, in), CE_None);
    }
    GDALClose(ds);

    GDALDataset* rds = static_cast<GDALDataset*>(GDALOpen(fname, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    std::vector<uint32_t> out(10, 0);
    EXPECT_EQ(rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 5, 2, out.data(),
                                              5, 2, GDT_UInt32, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(out[0], 0U);
    EXPECT_EQ(out[1], 1U);
    EXPECT_EQ(out[3], spec.max_val);
    EXPECT_EQ(out[4], spec.max_val);
    GDALClose(rds);
  }
}

TEST_F(GTiffOddBitsBandTest, ContigMultiBandReadWriteAllBitDepths) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // 1. 2-band CONTIG NBITS=12 (width=3, height=2; tests peer block null, clean,
  // and dirty cached, plus >=2 clipped values).
  {
    constexpr char kFile[] = "/vsimem/oddbits_12bit_contig.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kFile); };
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "NBITS", "12");
    opts = CSLSetNameValue(opts, "INTERLEAVE", "PIXEL");
    GDALDataset* ds = driver_->Create(kFile, 3, 2, 2, GDT_UInt16, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);

    // Case A: Write band 1 directly while band 2 block is NOT in cache (null).
    uint16_t b1[6] = {100, 200, 4095, 5000, 6000, 321};
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, b1), CE_None);
    ds->FlushCache(false);

    // Case B: Populate band 2 into block cache cleanly via RasterIO(GF_Read),
    // then WriteBlock on band 1 -> hits !poBlock->GetDirty() branch!
    uint16_t tmp_read[6] = {};
    EXPECT_EQ(ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 3, 2, tmp_read, 3,
                                             2, GDT_UInt16, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, b1), CE_None);

    // Case C: Dirty BOTH bands in block cache via partial RasterIO(GF_Write)
    // and then FlushCache -> flushes peer dirty block in IWriteBlock!
    uint16_t p1 = 777;
    uint16_t p2 = 888;
    EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &p1, 1, 1,
                                             GDT_UInt16, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 1, 1, &p2, 1, 1,
                                             GDT_UInt16, 0, 0, nullptr),
              CE_None);
    GDALClose(ds);

    GDALDataset* rds = static_cast<GDALDataset*>(GDALOpen(kFile, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    uint16_t out1[6] = {};
    uint16_t out2[6] = {};
    EXPECT_EQ(rds->GetRasterBand(1)->ReadBlock(0, 0, out1), CE_None);
    EXPECT_EQ(rds->GetRasterBand(2)->ReadBlock(0, 0, out2), CE_None);
    EXPECT_EQ(out1[0], 777);
    EXPECT_EQ(out1[3], 4095);
    EXPECT_EQ(out2[0], 888);
    GDALClose(rds);
  }

  // 2. 2-band CONTIG NBITS=1 (hits L754-772 in IReadBlock and L477-495 in
  // IWriteBlock, including unsetting previously set bits!).
  {
    constexpr char kFile[] = "/vsimem/oddbits_1bit_contig.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kFile); };
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "NBITS", "1");
    opts = CSLSetNameValue(opts, "INTERLEAVE", "PIXEL");
    GDALDataset* ds = driver_->Create(kFile, 5, 2, 2, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);

    uint8_t ones[10] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    uint8_t mix[10] = {1, 0, 1, 0, 5, 0, 1, 0, 1, 0};
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, ones), CE_None);
    EXPECT_EQ(ds->GetRasterBand(2)->WriteBlock(0, 0, ones), CE_None);
    // Overwrite band 1 with 0s and 1s (plus >1 clipped values) to exercise
    // unsetting bits in an existing block.
    uint8_t mix2[10] = {0, 1, 0, 9, 10, 1, 0, 1, 0, 1};
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, mix2), CE_None);
    EXPECT_EQ(ds->GetRasterBand(2)->WriteBlock(0, 0, mix), CE_None);
    GDALClose(ds);

    GDALDataset* rds = static_cast<GDALDataset*>(GDALOpen(kFile, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    uint8_t out1[10] = {};
    uint8_t out2[10] = {};
    EXPECT_EQ(rds->GetRasterBand(1)->ReadBlock(0, 0, out1), CE_None);
    EXPECT_EQ(rds->GetRasterBand(2)->ReadBlock(0, 0, out2), CE_None);
    EXPECT_EQ(out1[0], 0);
    EXPECT_EQ(out1[1], 1);
    EXPECT_EQ(out1[3], 1);
    EXPECT_EQ(out2[0], 1);
    EXPECT_EQ(out2[1], 0);
    GDALClose(rds);
  }

  // 3. 2-band CONTIG NBITS=3 (Byte), NBITS=10 (UInt16), NBITS=20 (UInt32),
  // NBITS=24 (UInt32).
  struct CaseSpec {
    int nbits;
    GDALDataType dt;
    uint32_t max_val;
  };
  const CaseSpec cases[] = {
      {3, GDT_Byte, 7},
      {10, GDT_UInt16, 1023},
      {20, GDT_UInt32, (1U << 20) - 1},
      {24, GDT_UInt32, (1U << 24) - 1},
  };
  for (const auto& spec : cases) {
    SCOPED_TRACE(spec.nbits);
    CPLString fname;
    fname.Printf("/vsimem/oddbits_contig_%d.tif", spec.nbits);
    absl::Cleanup c = [fname]() { VSIUnlink(fname); };
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "NBITS", CPLSPrintf("%d", spec.nbits));
    opts = CSLSetNameValue(opts, "INTERLEAVE", "PIXEL");
    GDALDataset* ds = driver_->Create(fname, 3, 2, 2, spec.dt, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);

    if (spec.dt == GDT_Byte) {
      uint8_t b1[6] = {0, 7, 15, 20, 3, 5};
      uint8_t b2[6] = {7, 0, 2, 4, 6, 1};
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, b1), CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->WriteBlock(0, 0, b2), CE_None);
    } else if (spec.dt == GDT_UInt16) {
      uint16_t b1[6] = {0, 1023, 2000, 3000, 500, 42};
      uint16_t b2[6] = {1023, 0, 123, 456, 789, 11};
      // Dirty band 1 via cache so peer dirty flush in CONTIG U16 is covered.
      EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 3, 2, b1, 3, 2,
                                               GDT_UInt16, 0, 0, nullptr),
                CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->WriteBlock(0, 0, b2), CE_None);
    } else {
      uint32_t b1[6] = {
          0, spec.max_val, spec.max_val + 10, spec.max_val + 20, 0x1234, 99};
      uint32_t b2[6] = {spec.max_val, 0, 77, 88, 0x5678, 11};
      EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, b1), CE_None);
      EXPECT_EQ(ds->GetRasterBand(2)->WriteBlock(0, 0, b2), CE_None);
    }
    GDALClose(ds);

    GDALDataset* rds = static_cast<GDALDataset*>(GDALOpen(fname, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    uint32_t out1[6] = {};
    uint32_t out2[6] = {};
    EXPECT_EQ(rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 3, 2, out1, 3, 2,
                                              GDT_UInt32, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(rds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 3, 2, out2, 3, 2,
                                              GDT_UInt32, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(out1[0], 0U);
    EXPECT_EQ(out1[1], spec.max_val);
    EXPECT_EQ(out1[2], spec.max_val);
    EXPECT_EQ(out2[0], spec.max_val);
    EXPECT_EQ(out2[1], 0U);
    GDALClose(rds);
  }
}

TEST_F(GTiffOddBitsBandTest, SparseAndErrorPathsInReadAndWriteBlock) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // 1. Sparse block read -> NullBlock(pImage) & CE_None.
  {
    constexpr char kSparse[] = "/vsimem/oddbits_sparse.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kSparse); };
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "NBITS", "4");
    opts = CSLSetNameValue(opts, "TILED", "YES");
    opts = CSLSetNameValue(opts, "BLOCKXSIZE", "16");
    opts = CSLSetNameValue(opts, "BLOCKYSIZE", "16");
    opts = CSLSetNameValue(opts, "SPARSE_OK", "YES");
    GDALDataset* ds = driver_->Create(kSparse, 32, 16, 1, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);
    ASSERT_EQ(ds->GetRasterBand(1)->SetNoDataValue(5), CE_None);
    std::vector<uint8_t> tile(256, 99);
    EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(0, 0, tile.data()), CE_None);
    EXPECT_EQ(tile[0], 5);
    GDALClose(ds);
  }

  // 2. Write error via ||maxlength -> sets m_bWriteError and fails subsequent
  // IWriteBlock at L59-64, plus LoadBlockBuf failure at L85-86!
  {
    constexpr char kReal[] = "/vsimem/oddbits_max_err.tif";
    constexpr char kLimited[] = "/vsimem/oddbits_max_err.tif||maxlength=350";
    absl::Cleanup c = [&]() { VSIUnlink(kReal); };
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "NBITS", "12");
    opts = CSLSetNameValue(opts, "BLOCKYSIZE", "16");
    GDALDataset* ds = driver_->Create(kLimited, 32, 64, 1, GDT_UInt16, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);
    std::vector<uint16_t> strip(32 * 16, 1234);
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, strip.data()), CE_None);
    // Writing block 1 flushes block 0 via LoadBlockBuf -> exceeds maxlength,
    // setting m_bWriteError and returning CE_Failure.
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 1, strip.data()),
              CE_Failure);
    // Subsequent WriteBlock hits the m_bWriteError guard.
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 2, strip.data()),
              CE_Failure);
    GDALClose(ds);
  }

  // 3. Corrupted compressed strip -> LoadBlockBuf failure in IReadBlock (L550)
  // and in CONTIG IWriteBlock (L85).
  {
    constexpr char kCorrupt[] = "/vsimem/oddbits_corrupt_lzw.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kCorrupt); };
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "NBITS", "4");
    opts = CSLSetNameValue(opts, "COMPRESS", "LZW");
    opts = CSLSetNameValue(opts, "INTERLEAVE", "PIXEL");
    GDALDataset* ds = driver_->Create(kCorrupt, 16, 16, 2, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);
    std::vector<uint8_t> data(256, 3);
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, data.data()), CE_None);
    EXPECT_EQ(ds->GetRasterBand(2)->WriteBlock(0, 0, data.data()), CE_None);
    GDALClose(ds);

    // Corrupt strip payload bytes at end of file.
    vsi_l_offset sz = 0;
    GByte* raw = VSIGetMemFileBuffer(kCorrupt, &sz, FALSE);
    ASSERT_NE(raw, nullptr);
    ASSERT_GT(sz, 16U);
    memset(raw + sz - 12, 0xFF, 12);

    GDALDataset* rds = static_cast<GDALDataset*>(GDALOpen(kCorrupt, GA_Update));
    ASSERT_NE(rds, nullptr);
    std::vector<uint8_t> out(256, 0);
    EXPECT_EQ(rds->GetRasterBand(1)->ReadBlock(0, 0, out.data()), CE_Failure);
    EXPECT_EQ(rds->GetRasterBand(1)->WriteBlock(0, 0, out.data()), CE_Failure);
    GDALClose(rds);
  }
}

}  // namespace
}  // namespace autotest2
