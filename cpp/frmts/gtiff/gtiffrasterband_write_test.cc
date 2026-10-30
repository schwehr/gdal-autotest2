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
#include <limits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class GTiffRasterBandWriteTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    GTiffOneTimeInit();
    driver_ = static_cast<GDALDriver*>(GDALGetDriverByName("GTiff"));
    ASSERT_NE(driver_, nullptr);
  }

  GDALDriver* driver_ = nullptr;
};

TEST_F(GTiffRasterBandWriteTest, SetDefaultRATAndPamInteraction) {
  constexpr char kFile[] = "/vsimem/rbwrite_rat.tif";
  constexpr char kAux[] = "/vsimem/rbwrite_rat.tif.aux.xml";
  absl::Cleanup c = [&]() {
    VSIUnlink(kFile);
    VSIUnlink(kAux);
  };
  CPLConfigOptionSetter pam("GDAL_PAM_ENABLED", "YES", false);

  GDALDataset* ds = driver_->Create(kFile, 4, 4, 1, GDT_Byte, nullptr);
  ASSERT_NE(ds, nullptr);
  GDALDefaultRasterAttributeTable rat;
  rat.CreateColumn("Value", GFT_Integer, GFU_MinMax);
  rat.SetRowCount(2);
  rat.SetValue(0, 0, 10);
  rat.SetValue(1, 0, 20);

  // 1. Write RAT to TIFF metadata (GTIFF_WRITE_RAT_TO_PAM=NO).
  EXPECT_EQ(ds->GetRasterBand(1)->SetDefaultRAT(&rat), CE_None);
  EXPECT_NE(ds->GetRasterBand(1)->GetDefaultRAT(), nullptr);
  EXPECT_EQ(ds->GetRasterBand(1)->SetDefaultRAT(nullptr), CE_None);

  // 2. Write RAT to PAM (GTIFF_WRITE_RAT_TO_PAM=YES), then overwrite with NO so
  // GDALPamRasterBand::GetDefaultRAT() != nullptr triggers clearing PAM RAT.
  {
    CPLConfigOptionSetter rat_pam("GTIFF_WRITE_RAT_TO_PAM", "YES", false);
    EXPECT_EQ(ds->GetRasterBand(1)->SetDefaultRAT(&rat), CE_None);
  }
  EXPECT_EQ(ds->GetRasterBand(1)->SetDefaultRAT(nullptr), CE_None);
  GDALClose(ds);
}

TEST_F(GTiffRasterBandWriteTest, IWriteBlockPaths) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // 1. GTIFF_DONT_WRITE_BLOCKS=YES early return.
  {
    constexpr char kDontWrite[] = "/vsimem/rbwrite_dont.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kDontWrite); };
    CPLConfigOptionSetter dont("GTIFF_DONT_WRITE_BLOCKS", "YES", false);
    GDALDataset* ds = driver_->Create(kDontWrite, 4, 4, 1, GDT_Byte, nullptr);
    ASSERT_NE(ds, nullptr);
    uint8_t buf[16] = {1};
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, buf), CE_None);
    GDALClose(ds);
  }

  // 2. Separate / 1-band WriteBlock + ||maxlength write error -> m_bWriteError.
  {
    constexpr char kReal[] = "/vsimem/rbwrite_sep_err.tif";
    constexpr char kLim[] = "/vsimem/rbwrite_sep_err.tif||maxlength=900";
    absl::Cleanup c = [&]() { VSIUnlink(kReal); };
    char** opts = CSLSetNameValue(nullptr, "BLOCKYSIZE", "16");
    GDALDataset* ds = driver_->Create(kLim, 32, 64, 1, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);
    std::vector<uint8_t> buf(32 * 16, 42);
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, buf.data()), CE_None);
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 1, buf.data()), CE_Failure);
    // Now m_bWriteError is true -> hits L64-69!
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 2, buf.data()), CE_Failure);
    GDALClose(ds);
  }

  // 3. CONTIG <= 10 bands (nBands = 3):
  // - peer blocks null -> bAllBlocksDirty = false
  // - peer block clean in cache -> DropLock & bAllBlocksDirty = false
  // - ALL peer blocks dirty in cache -> bAllBlocksDirty = true (synchronous)!
  {
    constexpr char kContig3[] = "/vsimem/rbwrite_contig3.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kContig3); };
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "INTERLEAVE", "PIXEL");
    opts = CSLSetNameValue(opts, "TILED", "YES");
    opts = CSLSetNameValue(opts, "BLOCKXSIZE", "16");
    opts = CSLSetNameValue(opts, "BLOCKYSIZE", "16");
    GDALDataset* ds = driver_->Create(kContig3, 32, 16, 3, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);

    std::vector<uint8_t> b1(256, 11);
    std::vector<uint8_t> b2(256, 22);
    std::vector<uint8_t> b3(256, 33);

    // Tile (0,0): peer blocks null -> bAllBlocksDirty = false.
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, b1.data()), CE_None);
    ds->FlushCache(false);

    // Populate band 2 into cache cleanly via RasterIO(GF_Read), dirty band 3
    // via partial RasterIO(GF_Write), then WriteBlock on band 1 -> hits both
    // !GetDirty() DropLock AND dirty peer flush when bAllBlocksDirty == false!
    std::vector<uint8_t> tmp(256, 0);
    EXPECT_EQ(ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 16, 16, tmp.data(),
                                             16, 16, GDT_Byte, 0, 0, nullptr),
              CE_None);
    uint8_t p3 = 99;
    EXPECT_EQ(ds->GetRasterBand(3)->RasterIO(GF_Write, 0, 0, 1, 1, &p3, 1, 1,
                                             GDT_Byte, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, b1.data()), CE_None);
    ds->FlushCache(false);

    // Tile (1,0): dirty bands 1 and 2 in cache via partial RasterIO(GF_Write),
    // then WriteBlock on band 3 -> ALL peer blocks dirty -> bAllBlocksDirty!
    uint8_t p1 = 55;
    uint8_t p2 = 66;
    EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 16, 0, 1, 1, &p1, 1, 1,
                                             GDT_Byte, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds->GetRasterBand(2)->RasterIO(GF_Write, 16, 0, 1, 1, &p2, 1, 1,
                                             GDT_Byte, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds->GetRasterBand(3)->WriteBlock(1, 0, b3.data()), CE_None);
    GDALClose(ds);
  }

  // 4. CONTIG <= 10 bands LoadBlockBuf error when peer block is locked dirty
  // (hits DropLock cleanup loop at L136-144!).
  {
    constexpr char kCorrupt[] = "/vsimem/rbwrite_corrupt_peer.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kCorrupt); };
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "COMPRESS", "LZW");
    opts = CSLSetNameValue(opts, "INTERLEAVE", "PIXEL");
    GDALDataset* ds = driver_->Create(kCorrupt, 16, 16, 3, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);
    std::vector<uint8_t> data(256, 7);
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, data.data()), CE_None);
    GDALClose(ds);

    vsi_l_offset sz = 0;
    GByte* raw = VSIGetMemFileBuffer(kCorrupt, &sz, FALSE);
    ASSERT_NE(raw, nullptr);
    ASSERT_GT(sz, 16U);
    memset(raw + sz - 10, 0xFF, 10);

    GDALDataset* uds = static_cast<GDALDataset*>(GDALOpen(kCorrupt, GA_Update));
    ASSERT_NE(uds, nullptr);
    // Dirty band 2's block in the cache without reading it: a full-band write
    // with eBufType = GDT_UInt16 (!= GDT_Byte) cannot go to IWriteBlock
    // directly, so GDALRasterBand::IRasterIO goes through the block cache with
    // bJustInitialize = TRUE.
    std::vector<uint16_t> u16_full(256, 9);
    EXPECT_EQ(
        uds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 16, 16, u16_full.data(),
                                        16, 16, GDT_UInt16, 0, 0, nullptr),
        CE_None);
    // Band 3 is NOT dirty, so bAllBlocksDirty == false -> LoadBlockBuf reads
    // corrupted LZW strip -> fails -> drops lock on apoBlocks[1]!
    EXPECT_EQ(uds->GetRasterBand(1)->WriteBlock(0, 0, data.data()), CE_Failure);
    GDALClose(uds);
  }

  // 5. CONTIG > 10 bands (nBands = 11 -> hits > MAX_BANDS_FOR_DIRTY_CHECK path
  // with null, clean, and dirty peer blocks at L168-180!).
  {
    constexpr char kContig11[] = "/vsimem/rbwrite_contig11.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kContig11); };
    char** opts = CSLSetNameValue(nullptr, "INTERLEAVE", "PIXEL");
    GDALDataset* ds = driver_->Create(kContig11, 8, 8, 11, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);
    uint8_t buf[64] = {1};
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, buf), CE_None);
    ds->FlushCache(false);

    // Band 2: clean in cache; Band 3: dirty in cache; Band 4..11: null.
    uint8_t rbuf[64] = {};
    EXPECT_EQ(ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 8, 8, rbuf, 8, 8,
                                             GDT_Byte, 0, 0, nullptr),
              CE_None);
    uint16_t wbuf[64] = {5};
    EXPECT_EQ(ds->GetRasterBand(3)->RasterIO(GF_Write, 0, 0, 8, 8, wbuf, 8, 8,
                                             GDT_UInt16, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(ds->GetRasterBand(1)->WriteBlock(0, 0, buf), CE_None);
    GDALClose(ds);
  }
}

TEST_F(GTiffRasterBandWriteTest, MetadataAndScaleOffsetUnitDescription) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  constexpr char kFile[] = "/vsimem/rbwrite_meta.tif";
  constexpr char kAux[] = "/vsimem/rbwrite_meta.tif.aux.xml";
  constexpr char kStream[] = "/vsimem/rbwrite_stream.tif";
  absl::Cleanup c = [&]() {
    VSIUnlink(kFile);
    VSIUnlink(kAux);
    VSIUnlink(kStream);
  };
  CPLConfigOptionSetter pam("GDAL_PAM_ENABLED", "YES", false);

  GDALDataset* ds = driver_->Create(kFile, 4, 4, 1, GDT_Byte, nullptr);
  ASSERT_NE(ds, nullptr);
  GDALRasterBand* b = ds->GetRasterBand(1);

  b->SetDescription(nullptr);
  b->SetDescription("elev");
  b->SetDescription("elev");  // no-op branch

  EXPECT_EQ(b->SetOffset(10.0), CE_None);
  EXPECT_EQ(b->SetOffset(10.0), CE_None);  // duplicate
  EXPECT_EQ(b->SetScale(2.5), CE_None);
  EXPECT_EQ(b->SetScale(2.5), CE_None);  // duplicate
  EXPECT_EQ(b->SetUnitType(nullptr), CE_None);
  EXPECT_EQ(b->SetUnitType("m"), CE_None);
  EXPECT_EQ(b->SetUnitType("m"), CE_None);  // duplicate

  // _temporary_ domain vs normal domain.
  char** tmp_md = CSLSetNameValue(nullptr, "TKEY", "TVAL");
  EXPECT_EQ(b->SetMetadata(tmp_md, "_temporary_"), CE_None);
  EXPECT_EQ(b->SetMetadata(tmp_md, nullptr), CE_None);
  CSLDestroy(tmp_md);
  EXPECT_EQ(b->SetMetadataItem("TI", "TV", "_temporary_"), CE_None);
  EXPECT_EQ(b->SetMetadataItem("K1", "V1", nullptr), CE_None);
  GDALClose(ds);

  // Reopen GA_ReadOnly -> writes metadata to PAM; then reopen GA_Update ->
  // cancels PAM metadata on SetMetadata / SetMetadataItem!
  GDALDataset* rds = static_cast<GDALDataset*>(GDALOpen(kFile, GA_ReadOnly));
  ASSERT_NE(rds, nullptr);
  char** pam_md = CSLSetNameValue(nullptr, "PAMK", "PAMV");
  EXPECT_EQ(rds->GetRasterBand(1)->SetMetadata(pam_md, "MYDOM"), CE_None);
  CSLDestroy(pam_md);
  EXPECT_EQ(rds->GetRasterBand(1)->SetMetadataItem("PITEM", "PV", nullptr),
            CE_None);
  GDALClose(rds);

  GDALDataset* uds = static_cast<GDALDataset*>(GDALOpen(kFile, GA_Update));
  ASSERT_NE(uds, nullptr);
  char** new_md = CSLSetNameValue(nullptr, "PAMK", "NEWV");
  EXPECT_EQ(uds->GetRasterBand(1)->SetMetadata(new_md, "MYDOM"), CE_None);
  CSLDestroy(new_md);
  EXPECT_EQ(uds->GetRasterBand(1)->SetMetadataItem("PITEM", "NEWPV", nullptr),
            CE_None);
  GDALClose(uds);

  // Streamable output crystallized failures for SetMetadata, SetMetadataItem,
  // SetNoDataValue, SetNoDataValueAsInt64, SetNoDataValueAsUInt64,
  // DeleteNoDataValue.
  char** sopts = CSLSetNameValue(nullptr, "STREAMABLE_OUTPUT", "YES");
  GDALDataset* sds = driver_->Create(kStream, 4, 4, 1, GDT_Byte, sopts);
  CSLDestroy(sopts);
  ASSERT_NE(sds, nullptr);
  uint8_t sbuf[16] = {};
  EXPECT_EQ(sds->GetRasterBand(1)->WriteBlock(0, 0, sbuf), CE_None);
  EXPECT_EQ(sds->GetRasterBand(1)->SetMetadata(nullptr, nullptr), CE_Failure);
  EXPECT_EQ(sds->GetRasterBand(1)->SetMetadataItem("A", "B", nullptr),
            CE_Failure);
  EXPECT_EQ(sds->GetRasterBand(1)->SetNoDataValue(1.0), CE_Failure);
  EXPECT_EQ(sds->GetRasterBand(1)->SetNoDataValueAsInt64(1), CE_Failure);
  EXPECT_EQ(sds->GetRasterBand(1)->SetNoDataValueAsUInt64(1), CE_Failure);
  EXPECT_EQ(sds->GetRasterBand(1)->DeleteNoDataValue(), CE_Failure);
  GDALClose(sds);
}

TEST_F(GTiffRasterBandWriteTest, SetColorInterpretationAndSetColorTable) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  CPLConfigOptionSetter pam("GDAL_PAM_ENABLED", "YES", false);

  // 1. 5-band MINISBLACK promoted to RGB (updates ExtraSamples from 4 to 2),
  // then demoted back to MINISBLACK (expands ExtraSamples from 2 to 4), plus
  // duplicate Alpha warnings!
  {
    constexpr char kFile5[] = "/vsimem/rbwrite_ci5.tif";
    constexpr char kAux5[] = "/vsimem/rbwrite_ci5.tif.aux.xml";
    absl::Cleanup c = [&]() {
      VSIUnlink(kFile5);
      VSIUnlink(kAux5);
    };
    GDALDataset* ds = driver_->Create(kFile5, 4, 4, 5, GDT_Byte, nullptr);
    ASSERT_NE(ds, nullptr);
    // Same value no-op.
    EXPECT_EQ(ds->GetRasterBand(1)->SetColorInterpretation(
                  ds->GetRasterBand(1)->GetColorInterpretation()),
              CE_None);
    // Promote to RGB (bands 1..3 = R, G, B).
    EXPECT_EQ(ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand),
              CE_None);
    EXPECT_EQ(ds->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand),
              CE_None);
    EXPECT_EQ(ds->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand),
              CE_None);

    // Mark band 4 and band 5 as Alpha -> triggers duplicate alpha warning
    // (without ALPHA creation option -> L516-520).
    EXPECT_EQ(ds->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand),
              CE_None);
    EXPECT_EQ(ds->GetRasterBand(5)->SetColorInterpretation(GCI_AlphaBand),
              CE_None);
    // Set band 5 back to GCI_Undefined -> sets EXTRASAMPLE_UNSPECIFIED (L545).
    EXPECT_EQ(ds->GetRasterBand(5)->SetColorInterpretation(GCI_Undefined),
              CE_None);

    // Demote back to MINISBLACK by changing band 1 to GCI_GrayIndex (L442-481).
    EXPECT_EQ(ds->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex),
              CE_None);
    GDALClose(ds);

    // Read-only SetColorInterpretation -> PAM.
    GDALDataset* rds = static_cast<GDALDataset*>(GDALOpen(kFile5, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    // Demotion to MINISBLACK persisted: band 1 is GrayIndex on reopen.
    EXPECT_EQ(rds->GetRasterBand(1)->GetColorInterpretation(), GCI_GrayIndex);
    EXPECT_EQ(rds->GetRasterBand(1)->SetColorInterpretation(GCI_CyanBand),
              CE_None);
    EXPECT_EQ(rds->GetRasterBand(1)->GetColorInterpretation(), GCI_CyanBand);
    GDALClose(rds);
  }

  // 2. Duplicate alpha warning WITH ALPHA=YES creation option (L502-512), plus
  // SetColorTable multi-sample error on 5-band dataset (L591-597).
  {
    constexpr char kAlphaOpt[] = "/vsimem/rbwrite_alpha_opt.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kAlphaOpt); };
    char** opts = CSLSetNameValue(nullptr, "ALPHA", "YES");
    GDALDataset* ds = driver_->Create(kAlphaOpt, 4, 4, 5, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);
    // Band 4 is already GCI_AlphaBand (i == nBaseSamples + 1 = 4); marking
    // band 5 as GCI_AlphaBand hits L506-512!
    EXPECT_EQ(ds->GetRasterBand(5)->SetColorInterpretation(GCI_AlphaBand),
              CE_None);
    GDALColorTable ct;
    EXPECT_EQ(ds->GetRasterBand(1)->SetColorTable(&ct), CE_Failure);
    EXPECT_EQ(ds->GetRasterBand(2)->SetColorTable(&ct), CE_Failure);
    GDALClose(ds);
  }

  // 3. Reopened MINISWHITE 1-band dataset without creation options -> changing
  // ColorInterpretation hits L558-565 (switches to MINISBLACK).
  {
    constexpr char kMW[] = "/vsimem/rbwrite_miniswhite.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kMW); };
    char** opts = CSLSetNameValue(nullptr, "PHOTOMETRIC", "MINISWHITE");
    GDALDataset* ds = driver_->Create(kMW, 4, 4, 1, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);
    GDALClose(ds);

    GDALDataset* uds = static_cast<GDALDataset*>(GDALOpen(kMW, GA_Update));
    ASSERT_NE(uds, nullptr);
    EXPECT_EQ(uds->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex),
              CE_None);
    GDALClose(uds);
  }

  // 4. SetColorTable: Int16 error, Byte partial table, ReadOnly PAM + Update
  // PAM clear, clear via nullptr, and reopened UInt16 (multiplier == 0 -> 257).
  {
    constexpr char kInt16[] = "/vsimem/rbwrite_ct_i16.tif";
    constexpr char kByte[] = "/vsimem/rbwrite_ct_byte.tif";
    constexpr char kByteAux[] = "/vsimem/rbwrite_ct_byte.tif.aux.xml";
    constexpr char kU16[] = "/vsimem/rbwrite_ct_u16.tif";
    absl::Cleanup c = [&]() {
      VSIUnlink(kInt16);
      VSIUnlink(kByte);
      VSIUnlink(kByteAux);
      VSIUnlink(kU16);
    };

    GDALDataset* ds_i16 = driver_->Create(kInt16, 4, 4, 1, GDT_Int16, nullptr);
    ASSERT_NE(ds_i16, nullptr);
    GDALColorTable ct;
    GDALColorEntry e0 = {10, 20, 30, 255};
    ct.SetColorEntry(0, &e0);
    EXPECT_EQ(ds_i16->GetRasterBand(1)->SetColorTable(&ct), CE_Failure);
    EXPECT_EQ(ds_i16->GetRasterBand(1)->GetColorTable(), nullptr);
    GDALClose(ds_i16);

    GDALDataset* ds_b = driver_->Create(kByte, 4, 4, 1, GDT_Byte, nullptr);
    ASSERT_NE(ds_b, nullptr);
    GDALClose(ds_b);

    // Read-only SetColorTable -> PAM.
    GDALDataset* rds_b =
        static_cast<GDALDataset*>(GDALOpen(kByte, GA_ReadOnly));
    ASSERT_NE(rds_b, nullptr);
    EXPECT_EQ(rds_b->GetRasterBand(1)->SetColorTable(&ct), CE_None);
    EXPECT_NE(rds_b->GetRasterBand(1)->GetColorTable(), nullptr);
    EXPECT_EQ(rds_b->GetRasterBand(1)->SetColorTable(nullptr), CE_None);
    EXPECT_EQ(rds_b->GetRasterBand(1)->GetColorTable(), nullptr);
    EXPECT_EQ(rds_b->GetRasterBand(1)->SetColorTable(&ct), CE_None);
    EXPECT_NE(rds_b->GetRasterBand(1)->GetColorTable(), nullptr);
    GDALClose(rds_b);

    // Reopen GA_Update -> clears PAM color table (L610-614), writes 256-entry
    // colormap, then clears with nullptr (L622-628).
    GDALDataset* uds_b = static_cast<GDALDataset*>(GDALOpen(kByte, GA_Update));
    ASSERT_NE(uds_b, nullptr);
    EXPECT_EQ(uds_b->GetRasterBand(1)->SetColorTable(&ct), CE_None);
    EXPECT_NE(uds_b->GetRasterBand(1)->GetColorTable(), nullptr);
    EXPECT_EQ(uds_b->GetRasterBand(1)->GetColorInterpretation(),
              GCI_PaletteIndex);
    EXPECT_EQ(uds_b->GetRasterBand(1)->SetColorTable(nullptr), CE_None);
    EXPECT_EQ(uds_b->GetRasterBand(1)->GetColorTable(), nullptr);
    GDALClose(uds_b);

    // UInt16 reopened so m_nColorTableMultiplier == 0 -> defaults to 257.
    GDALDataset* ds_u16 = driver_->Create(kU16, 4, 4, 1, GDT_UInt16, nullptr);
    ASSERT_NE(ds_u16, nullptr);
    GDALClose(ds_u16);

    GDALDataset* uds_u16 = static_cast<GDALDataset*>(GDALOpen(kU16, GA_Update));
    ASSERT_NE(uds_u16, nullptr);
    EXPECT_EQ(uds_u16->GetRasterBand(1)->SetColorTable(&ct), CE_None);
    EXPECT_NE(uds_u16->GetRasterBand(1)->GetColorTable(), nullptr);
    GDALClose(uds_u16);
  }
}

TEST_F(GTiffRasterBandWriteTest, NoDataVariantsAndNullBlock) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  CPLConfigOptionSetter pam("GDAL_PAM_ENABLED", "YES", false);

  // 1. Float64 NaN & finite duplicate, 2-band mismatch warnings, ReadOnly PAM
  // & Update PAM cancellation, DeleteNoDataValue, and NullBlock (Byte/Float).
  {
    constexpr char kF64[] = "/vsimem/rbwrite_nd_f64.tif";
    constexpr char kF64Aux[] = "/vsimem/rbwrite_nd_f64.tif.aux.xml";
    absl::Cleanup c = [&]() {
      VSIUnlink(kF64);
      VSIUnlink(kF64Aux);
    };
    char** opts = CSLSetNameValue(nullptr, "SPARSE_OK", "YES");
    GDALDataset* ds = driver_->Create(kF64, 4, 4, 2, GDT_Float64, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);

    // NullBlock without nodata.
    double buf[16] = {99.0};
    EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(0, 0, buf), CE_None);
    EXPECT_DOUBLE_EQ(buf[0], 0.0);

    // SetNoDataValue finite + duplicate + mismatch on band 2 and band 1.
    EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValue(-999.0), CE_None);
    EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValue(-999.0), CE_None);
    EXPECT_EQ(ds->GetRasterBand(2)->SetNoDataValue(-888.0), CE_None);
    EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValue(-777.0), CE_None);

    // NullBlock with nodata.
    EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(0, 0, buf), CE_None);
    EXPECT_DOUBLE_EQ(buf[0], -777.0);

    // NaN + duplicate NaN.
    const double nan_val = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValue(nan_val), CE_None);
    EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValue(nan_val), CE_None);
    EXPECT_EQ(ds->GetRasterBand(1)->DeleteNoDataValue(), CE_None);
    GDALClose(ds);

    // ReadOnly SetNoDataValue -> PAM, DeleteNoDataValue, SetNoDataValue -> PAM,
    // then Update SetNoDataValue cancels PAM!
    GDALDataset* rds = static_cast<GDALDataset*>(GDALOpen(kF64, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    EXPECT_EQ(rds->GetRasterBand(1)->SetNoDataValue(12.0), CE_None);
    EXPECT_EQ(rds->GetRasterBand(1)->DeleteNoDataValue(), CE_None);
    EXPECT_EQ(rds->GetRasterBand(1)->SetNoDataValue(34.0), CE_None);
    GDALClose(rds);

    GDALDataset* uds = static_cast<GDALDataset*>(GDALOpen(kF64, GA_Update));
    ASSERT_NE(uds, nullptr);
    EXPECT_EQ(uds->GetRasterBand(1)->SetNoDataValue(56.0), CE_None);
    GDALClose(uds);
  }

  // 2. Int64 SetNoDataValue(double), SetNoDataValueAsInt64, PAM, NullBlock.
  {
    constexpr char kI64[] = "/vsimem/rbwrite_nd_i64.tif";
    constexpr char kI64Aux[] = "/vsimem/rbwrite_nd_i64.tif.aux.xml";
    absl::Cleanup c = [&]() {
      VSIUnlink(kI64);
      VSIUnlink(kI64Aux);
    };
    char** opts = CSLSetNameValue(nullptr, "SPARSE_OK", "YES");
    GDALDataset* ds = driver_->Create(kI64, 4, 4, 2, GDT_Int64, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);

    int64_t buf[16] = {99};
    EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(0, 0, buf), CE_None);
    EXPECT_EQ(buf[0], 0);

    // SetNoDataValue(double) on Int64 (exact).
    EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValue(-42.0), CE_None);
    EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValueAsInt64(-1000000000000LL),
              CE_None);
    // Duplicate.
    EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValueAsInt64(-1000000000000LL),
              CE_None);
    // Mismatch on band 2 & band 1.
    EXPECT_EQ(ds->GetRasterBand(2)->SetNoDataValueAsInt64(-2000000000000LL),
              CE_None);
    EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValueAsInt64(-3000000000000LL),
              CE_None);

    // NullBlock with Int64 nodata set.
    EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(0, 0, buf), CE_None);
    EXPECT_EQ(buf[0], -3000000000000LL);
    EXPECT_EQ(ds->GetRasterBand(1)->DeleteNoDataValue(), CE_None);
    GDALClose(ds);

    GDALDataset* rds = static_cast<GDALDataset*>(GDALOpen(kI64, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    EXPECT_EQ(rds->GetRasterBand(1)->SetNoDataValueAsInt64(-500LL), CE_None);
    GDALClose(rds);

    GDALDataset* uds = static_cast<GDALDataset*>(GDALOpen(kI64, GA_Update));
    ASSERT_NE(uds, nullptr);
    EXPECT_EQ(uds->GetRasterBand(1)->SetNoDataValueAsInt64(-600LL), CE_None);
    GDALClose(uds);
  }

  // 3. UInt64 SetNoDataValue(double), SetNoDataValueAsUInt64, PAM, NullBlock.
  {
    constexpr char kU64[] = "/vsimem/rbwrite_nd_u64.tif";
    constexpr char kU64Aux[] = "/vsimem/rbwrite_nd_u64.tif.aux.xml";
    absl::Cleanup c = [&]() {
      VSIUnlink(kU64);
      VSIUnlink(kU64Aux);
    };
    char** opts = CSLSetNameValue(nullptr, "SPARSE_OK", "YES");
    GDALDataset* ds = driver_->Create(kU64, 4, 4, 2, GDT_UInt64, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);

    uint64_t buf[16] = {99};
    EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(0, 0, buf), CE_None);
    EXPECT_EQ(buf[0], 0ULL);

    EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValue(42.0), CE_None);
    EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValueAsUInt64(1000000000000ULL),
              CE_None);
    EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValueAsUInt64(1000000000000ULL),
              CE_None);
    EXPECT_EQ(ds->GetRasterBand(2)->SetNoDataValueAsUInt64(2000000000000ULL),
              CE_None);
    EXPECT_EQ(ds->GetRasterBand(1)->SetNoDataValueAsUInt64(3000000000000ULL),
              CE_None);

    EXPECT_EQ(ds->GetRasterBand(1)->ReadBlock(0, 0, buf), CE_None);
    EXPECT_EQ(buf[0], 3000000000000ULL);
    EXPECT_EQ(ds->GetRasterBand(1)->DeleteNoDataValue(), CE_None);
    GDALClose(ds);

    GDALDataset* rds = static_cast<GDALDataset*>(GDALOpen(kU64, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    EXPECT_EQ(rds->GetRasterBand(1)->SetNoDataValueAsUInt64(500ULL), CE_None);
    GDALClose(rds);

    GDALDataset* uds = static_cast<GDALDataset*>(GDALOpen(kU64, GA_Update));
    ASSERT_NE(uds, nullptr);
    EXPECT_EQ(uds->GetRasterBand(1)->SetNoDataValueAsUInt64(600ULL), CE_None);
    GDALClose(uds);
  }
}

}  // namespace
}  // namespace autotest2
