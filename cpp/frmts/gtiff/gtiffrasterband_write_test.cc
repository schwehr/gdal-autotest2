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
// Tests GeoTIFF raster band RAT writing, block writing, metadata, color
// interpretation/table mutation, and NoData handling
// (gtiffrasterband_write.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/gtiffrasterband_write.cpp

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

class GTiffRasterBandWriteTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GTiffOneTimeInit();
    CPLErrorReset();
    driver_ = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, driver_);
  }

  void TearDown() override { CPLErrorReset(); }

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

  GDALDatasetUniquePtr ds(driver_->Create(kFile, 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  GDALDefaultRasterAttributeTable rat;
  rat.CreateColumn("Value", GFT_Integer, GFU_MinMax);
  rat.SetRowCount(2);
  rat.SetValue(0, 0, 10);
  rat.SetValue(1, 0, 20);

  // 1. Write RAT to TIFF metadata (GTIFF_WRITE_RAT_TO_PAM=NO).
  EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetDefaultRAT(&rat));
  EXPECT_NE(nullptr, ds->GetRasterBand(1)->GetDefaultRAT());
  EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetDefaultRAT(nullptr));

  // 2. Write RAT to PAM (GTIFF_WRITE_RAT_TO_PAM=YES), then overwrite with NO so
  // GDALPamRasterBand::GetDefaultRAT() != nullptr triggers clearing PAM RAT.
  {
    CPLConfigOptionSetter rat_pam("GTIFF_WRITE_RAT_TO_PAM", "YES", false);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetDefaultRAT(&rat));
  }
  EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetDefaultRAT(nullptr));
}

TEST_F(GTiffRasterBandWriteTest, IWriteBlockPaths) {
  // 1. GTIFF_DONT_WRITE_BLOCKS=YES early return.
  {
    constexpr char kDontWrite[] = "/vsimem/rbwrite_dont.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kDontWrite); };
    CPLConfigOptionSetter dont("GTIFF_DONT_WRITE_BLOCKS", "YES", false);
    GDALDatasetUniquePtr ds(
        driver_->Create(kDontWrite, 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    std::array<uint8_t, 16> buf = {1};
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, buf.data()));
  }

  // 2. CONTIG / multi-band WriteBlock + ||maxlength write error ->
  // m_bWriteError.
  {
    constexpr char kReal[] = "/vsimem/rbwrite_sep_err.tif";
    constexpr char kLim[] = "/vsimem/rbwrite_sep_err.tif||maxlength=900";
    absl::Cleanup c = [&]() {
      VSIUnlink(kLim);
      VSIUnlink(kReal);
    };
    WithQuietHandler quiet;
    CPLStringList opts;
    opts.AddNameValue("INTERLEAVE", "PIXEL");
    opts.AddNameValue("BLOCKYSIZE", "16");
    GDALDatasetUniquePtr ds(
        driver_->Create(kLim, 32, 64, 2, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);
    std::vector<uint8_t> buf(32 * 16, 42);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, buf.data()));
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ds->GetRasterBand(1)->WriteBlock(0, 1, buf.data()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    // Now m_bWriteError is true -> fails immediately.
    EXPECT_EQ(CE_Failure, ds->GetRasterBand(1)->WriteBlock(0, 2, buf.data()));
    CPLErrorReset();
  }

  // 3. CONTIG <= 10 bands (nBands = 3):
  // - peer blocks null -> bAllBlocksDirty = false
  // - peer block clean in cache -> DropLock & bAllBlocksDirty = false
  // - ALL peer blocks dirty in cache -> bAllBlocksDirty = true (synchronous).
  {
    constexpr char kContig3[] = "/vsimem/rbwrite_contig3.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kContig3); };
    CPLStringList opts;
    opts.AddNameValue("INTERLEAVE", "PIXEL");
    opts.AddNameValue("TILED", "YES");
    opts.AddNameValue("BLOCKXSIZE", "16");
    opts.AddNameValue("BLOCKYSIZE", "16");
    GDALDatasetUniquePtr ds(
        driver_->Create(kContig3, 32, 16, 3, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);

    std::vector<uint8_t> b1(256, 11);
    std::vector<uint8_t> b3(256, 33);

    // Tile (0,0): peer blocks null -> bAllBlocksDirty = false.
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, b1.data()));
    ds->FlushCache(false);

    // Populate band 2 into cache cleanly via RasterIO(GF_Read), dirty band 3
    // via partial RasterIO(GF_Write), then WriteBlock on band 1 -> hits both
    // !GetDirty() DropLock AND dirty peer flush when bAllBlocksDirty == false.
    std::vector<uint8_t> tmp(256, 0);
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 16, 16, tmp.data(),
                                             16, 16, GDT_Byte, 0, 0, nullptr));
    uint8_t p3 = 99;
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(3)->RasterIO(GF_Write, 0, 0, 1, 1, &p3, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, b1.data()));
    ds->FlushCache(false);

    // Tile (1,0): dirty bands 1 and 2 in cache via partial RasterIO(GF_Write),
    // then WriteBlock on band 3 -> ALL peer blocks dirty -> bAllBlocksDirty.
    uint8_t p1 = 55;
    uint8_t p2 = 66;
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 16, 0, 1, 1, &p1, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(2)->RasterIO(GF_Write, 16, 0, 1, 1, &p2, 1, 1,
                                             GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->GetRasterBand(3)->WriteBlock(1, 0, b3.data()));
  }

  // 4. CONTIG <= 10 bands LoadBlockBuf error when peer block is locked dirty
  // (exercises DropLock cleanup loop).
  {
    constexpr char kCorrupt[] = "/vsimem/rbwrite_corrupt_peer.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kCorrupt); };
    std::vector<uint8_t> data(256, 7);
    {
      CPLStringList opts;
      opts.AddNameValue("COMPRESS", "LZW");
      opts.AddNameValue("INTERLEAVE", "PIXEL");
      GDALDatasetUniquePtr ds(
          driver_->Create(kCorrupt, 16, 16, 3, GDT_Byte, opts.List()));
      ASSERT_NE(nullptr, ds);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, data.data()));
    }

    vsi_l_offset sz = 0;
    GByte* raw = VSIGetMemFileBuffer(kCorrupt, &sz, FALSE);
    ASSERT_NE(nullptr, raw);
    ASSERT_GT(sz, 16U);
    std::memset(raw + sz - 10, 0xFF, 10);

    WithQuietHandler quiet;
    GDALDatasetUniquePtr uds(
        GDALDataset::Open(kCorrupt, GDAL_OF_RASTER | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, uds);
    // Dirty band 2's block in the cache without reading it: a full-band write
    // with eBufType = GDT_UInt16 (!= GDT_Byte) goes through the block cache
    // with bJustInitialize = TRUE.
    std::vector<uint16_t> u16_full(256, 9);
    EXPECT_EQ(CE_None, uds->GetRasterBand(2)->RasterIO(
                           GF_Write, 0, 0, 16, 16, u16_full.data(), 16, 16,
                           GDT_UInt16, 0, 0, nullptr));
    // Band 3 is NOT dirty, so bAllBlocksDirty == false -> LoadBlockBuf reads
    // corrupted LZW strip -> fails -> drops lock on apoBlocks[1].
    EXPECT_EQ(CE_Failure, uds->GetRasterBand(1)->WriteBlock(0, 0, data.data()));
    CPLErrorReset();
  }

  // 5. CONTIG > 10 bands (nBands = 11 -> hits > MAX_BANDS_FOR_DIRTY_CHECK path
  // with null, clean, and dirty peer blocks).
  {
    constexpr char kContig11[] = "/vsimem/rbwrite_contig11.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kContig11); };
    CPLStringList opts;
    opts.AddNameValue("INTERLEAVE", "PIXEL");
    GDALDatasetUniquePtr ds(
        driver_->Create(kContig11, 8, 8, 11, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);
    std::array<uint8_t, 64> buf = {1};
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, buf.data()));
    ds->FlushCache(false);

    // Band 2: clean in cache; Band 3: dirty in cache; Band 4..11: null.
    std::array<uint8_t, 64> rbuf = {};
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 8, 8, rbuf.data(),
                                             8, 8, GDT_Byte, 0, 0, nullptr));
    std::array<uint16_t, 64> wbuf = {5};
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(3)->RasterIO(GF_Write, 0, 0, 8, 8, wbuf.data(),
                                             8, 8, GDT_UInt16, 0, 0, nullptr));
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, buf.data()));
  }
}

TEST_F(GTiffRasterBandWriteTest, MetadataAndScaleOffsetUnitDescription) {
  constexpr char kFile[] = "/vsimem/rbwrite_meta.tif";
  constexpr char kAux[] = "/vsimem/rbwrite_meta.tif.aux.xml";
  constexpr char kStream[] = "/vsimem/rbwrite_stream.tif";
  absl::Cleanup c = [&]() {
    VSIUnlink(kFile);
    VSIUnlink(kAux);
    VSIUnlink(kStream);
  };
  CPLConfigOptionSetter pam("GDAL_PAM_ENABLED", "YES", false);

  {
    GDALDatasetUniquePtr ds(driver_->Create(kFile, 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* b = ds->GetRasterBand(1);

    b->SetDescription(nullptr);
    b->SetDescription("elev");
    b->SetDescription("elev");  // no-op branch

    EXPECT_EQ(CE_None, b->SetOffset(10.0));
    EXPECT_EQ(CE_None, b->SetOffset(10.0));  // duplicate
    EXPECT_EQ(CE_None, b->SetScale(2.5));
    EXPECT_EQ(CE_None, b->SetScale(2.5));  // duplicate
    EXPECT_EQ(CE_None, b->SetUnitType(nullptr));
    EXPECT_EQ(CE_None, b->SetUnitType("m"));
    EXPECT_EQ(CE_None, b->SetUnitType("m"));  // duplicate

    // _temporary_ domain vs normal domain.
    CPLStringList tmp_md;
    tmp_md.AddNameValue("TKEY", "TVAL");
    EXPECT_EQ(CE_None, b->SetMetadata(tmp_md.List(), "_temporary_"));
    EXPECT_EQ(CE_None, b->SetMetadata(tmp_md.List(), nullptr));
    EXPECT_EQ(CE_None, b->SetMetadataItem("TI", "TV", "_temporary_"));
    EXPECT_EQ(CE_None, b->SetMetadataItem("K1", "V1", nullptr));
  }

  // Reopen READONLY -> writes metadata to PAM; then reopen UPDATE ->
  // cancels PAM metadata on SetMetadata / SetMetadataItem.
  {
    GDALDatasetUniquePtr rds(
        GDALDataset::Open(kFile, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds);
    CPLStringList pam_md;
    pam_md.AddNameValue("PAMK", "PAMV");
    EXPECT_EQ(CE_None,
              rds->GetRasterBand(1)->SetMetadata(pam_md.List(), "MYDOM"));
    EXPECT_EQ(CE_None,
              rds->GetRasterBand(1)->SetMetadataItem("PITEM", "PV", nullptr));
  }

  {
    GDALDatasetUniquePtr uds(
        GDALDataset::Open(kFile, GDAL_OF_RASTER | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, uds);
    CPLStringList new_md;
    new_md.AddNameValue("PAMK", "NEWV");
    EXPECT_EQ(CE_None,
              uds->GetRasterBand(1)->SetMetadata(new_md.List(), "MYDOM"));
    EXPECT_EQ(CE_None, uds->GetRasterBand(1)->SetMetadataItem("PITEM", "NEWPV",
                                                              nullptr));
  }

  // Streamable output crystallized failures for SetMetadata, SetMetadataItem,
  // SetNoDataValue, SetNoDataValueAsInt64, SetNoDataValueAsUInt64,
  // DeleteNoDataValue.
  {
    CPLStringList sopts;
    sopts.AddNameValue("STREAMABLE_OUTPUT", "YES");
    GDALDatasetUniquePtr sds(
        driver_->Create(kStream, 4, 4, 1, GDT_Byte, sopts.List()));
    ASSERT_NE(nullptr, sds);
    std::array<uint8_t, 16> sbuf = {};
    EXPECT_EQ(CE_None, sds->GetRasterBand(1)->WriteBlock(0, 0, sbuf.data()));

    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, sds->GetRasterBand(1)->SetMetadata(nullptr, nullptr));
    EXPECT_EQ(CE_Failure,
              sds->GetRasterBand(1)->SetMetadataItem("A", "B", nullptr));
    EXPECT_EQ(CE_Failure, sds->GetRasterBand(1)->SetNoDataValue(1.0));
    EXPECT_EQ(CE_Failure, sds->GetRasterBand(1)->SetNoDataValueAsInt64(1));
    EXPECT_EQ(CE_Failure, sds->GetRasterBand(1)->SetNoDataValueAsUInt64(1));
    EXPECT_EQ(CE_Failure, sds->GetRasterBand(1)->DeleteNoDataValue());
    CPLErrorReset();
  }
}

TEST_F(GTiffRasterBandWriteTest, SetColorInterpretationAndSetColorTable) {
  CPLConfigOptionSetter pam("GDAL_PAM_ENABLED", "YES", false);

  // 1. 5-band MINISBLACK promoted to RGB (updates ExtraSamples from 4 to 2),
  // then demoted back to MINISBLACK (expands ExtraSamples from 2 to 4), plus
  // duplicate Alpha warnings.
  {
    constexpr char kFile5[] = "/vsimem/rbwrite_ci5.tif";
    constexpr char kAux5[] = "/vsimem/rbwrite_ci5.tif.aux.xml";
    absl::Cleanup c = [&]() {
      VSIUnlink(kFile5);
      VSIUnlink(kAux5);
    };
    {
      GDALDatasetUniquePtr ds(
          driver_->Create(kFile5, 4, 4, 5, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      // Same value no-op.
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetColorInterpretation(
                             ds->GetRasterBand(1)->GetColorInterpretation()));
      // Promote to RGB (bands 1..3 = R, G, B).
      EXPECT_EQ(CE_None,
                ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand));
      EXPECT_EQ(CE_None,
                ds->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand));
      EXPECT_EQ(CE_None,
                ds->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand));

      // Mark band 4 and band 5 as Alpha -> triggers duplicate alpha warning
      // (without ALPHA creation option).
      EXPECT_EQ(CE_None,
                ds->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand));
      {
        WithQuietHandler quiet;
        EXPECT_EQ(CE_None,
                  ds->GetRasterBand(5)->SetColorInterpretation(GCI_AlphaBand));
        CPLErrorReset();
      }
      // Set band 5 back to GCI_Undefined -> sets EXTRASAMPLE_UNSPECIFIED.
      EXPECT_EQ(CE_None,
                ds->GetRasterBand(5)->SetColorInterpretation(GCI_Undefined));

      // Demote back to MINISBLACK by changing band 1 to GCI_GrayIndex.
      EXPECT_EQ(CE_None,
                ds->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex));
    }

    // Read-only SetColorInterpretation -> PAM.
    GDALDatasetUniquePtr rds(
        GDALDataset::Open(kFile5, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds);
    // Demotion to MINISBLACK persisted: band 1 is GrayIndex on reopen.
    EXPECT_EQ(GCI_GrayIndex, rds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(CE_None,
              rds->GetRasterBand(1)->SetColorInterpretation(GCI_CyanBand));
    EXPECT_EQ(GCI_CyanBand, rds->GetRasterBand(1)->GetColorInterpretation());
  }

  // 2. Duplicate alpha warning WITH ALPHA=YES creation option, plus
  // SetColorTable multi-sample error on 5-band dataset.
  {
    constexpr char kAlphaOpt[] = "/vsimem/rbwrite_alpha_opt.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kAlphaOpt); };
    CPLStringList opts;
    opts.AddNameValue("ALPHA", "YES");
    GDALDatasetUniquePtr ds(
        driver_->Create(kAlphaOpt, 4, 4, 5, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);
    // Band 4 is already GCI_AlphaBand; marking band 5 as GCI_AlphaBand triggers
    // duplicate alpha warning.
    WithQuietHandler quiet;
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(5)->SetColorInterpretation(GCI_AlphaBand));
    GDALColorTable ct;
    EXPECT_EQ(CE_Failure, ds->GetRasterBand(1)->SetColorTable(&ct));
    EXPECT_EQ(CE_Failure, ds->GetRasterBand(2)->SetColorTable(&ct));
    CPLErrorReset();
  }

  // 3. Reopened MINISWHITE 1-band dataset without creation options -> changing
  // ColorInterpretation switches to MINISBLACK.
  {
    constexpr char kMW[] = "/vsimem/rbwrite_miniswhite.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kMW); };
    {
      CPLStringList opts;
      opts.AddNameValue("PHOTOMETRIC", "MINISWHITE");
      GDALDatasetUniquePtr ds(
          driver_->Create(kMW, 4, 4, 1, GDT_Byte, opts.List()));
      ASSERT_NE(nullptr, ds);
    }

    GDALDatasetUniquePtr uds(
        GDALDataset::Open(kMW, GDAL_OF_RASTER | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, uds);
    EXPECT_EQ(CE_None,
              uds->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex));
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

    GDALColorTable ct;
    GDALColorEntry e0 = {10, 20, 30, 255};
    ct.SetColorEntry(0, &e0);

    {
      GDALDatasetUniquePtr ds_i16(
          driver_->Create(kInt16, 4, 4, 1, GDT_Int16, nullptr));
      ASSERT_NE(nullptr, ds_i16);
      WithQuietHandler quiet;
      EXPECT_EQ(CE_Failure, ds_i16->GetRasterBand(1)->SetColorTable(&ct));
      CPLErrorReset();
      EXPECT_EQ(nullptr, ds_i16->GetRasterBand(1)->GetColorTable());
    }

    {
      GDALDatasetUniquePtr ds_b(
          driver_->Create(kByte, 4, 4, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds_b);
    }

    // Read-only SetColorTable -> PAM.
    {
      GDALDatasetUniquePtr rds_b(
          GDALDataset::Open(kByte, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, rds_b);
      EXPECT_EQ(CE_None, rds_b->GetRasterBand(1)->SetColorTable(&ct));
      EXPECT_NE(nullptr, rds_b->GetRasterBand(1)->GetColorTable());
      EXPECT_EQ(CE_None, rds_b->GetRasterBand(1)->SetColorTable(nullptr));
      EXPECT_EQ(nullptr, rds_b->GetRasterBand(1)->GetColorTable());
      EXPECT_EQ(CE_None, rds_b->GetRasterBand(1)->SetColorTable(&ct));
      EXPECT_NE(nullptr, rds_b->GetRasterBand(1)->GetColorTable());
    }

    // Reopen UPDATE -> clears PAM color table, writes 256-entry colormap, then
    // clears with nullptr.
    {
      GDALDatasetUniquePtr uds_b(
          GDALDataset::Open(kByte, GDAL_OF_RASTER | GDAL_OF_UPDATE));
      ASSERT_NE(nullptr, uds_b);
      EXPECT_EQ(CE_None, uds_b->GetRasterBand(1)->SetColorTable(&ct));
      EXPECT_NE(nullptr, uds_b->GetRasterBand(1)->GetColorTable());
      EXPECT_EQ(GCI_PaletteIndex,
                uds_b->GetRasterBand(1)->GetColorInterpretation());
      EXPECT_EQ(CE_None, uds_b->GetRasterBand(1)->SetColorTable(nullptr));
      EXPECT_EQ(nullptr, uds_b->GetRasterBand(1)->GetColorTable());
    }

    // UInt16 reopened so m_nColorTableMultiplier == 0 -> defaults to 257.
    {
      GDALDatasetUniquePtr ds_u16(
          driver_->Create(kU16, 4, 4, 1, GDT_UInt16, nullptr));
      ASSERT_NE(nullptr, ds_u16);
    }

    {
      GDALDatasetUniquePtr uds_u16(
          GDALDataset::Open(kU16, GDAL_OF_RASTER | GDAL_OF_UPDATE));
      ASSERT_NE(nullptr, uds_u16);
      EXPECT_EQ(CE_None, uds_u16->GetRasterBand(1)->SetColorTable(&ct));
      EXPECT_NE(nullptr, uds_u16->GetRasterBand(1)->GetColorTable());
    }
  }
}

TEST_F(GTiffRasterBandWriteTest, NoDataVariantsAndNullBlock) {
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
    {
      CPLStringList opts;
      opts.AddNameValue("SPARSE_OK", "YES");
      GDALDatasetUniquePtr ds(
          driver_->Create(kF64, 4, 4, 2, GDT_Float64, opts.List()));
      ASSERT_NE(nullptr, ds);

      // NullBlock without nodata.
      std::array<double, 16> buf = {99.0};
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));
      EXPECT_DOUBLE_EQ(0.0, buf[0]);

      // SetNoDataValue finite + duplicate + mismatch on band 2 and band 1.
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(-999.0));
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(-999.0));
      {
        WithQuietHandler quiet;
        EXPECT_EQ(CE_None, ds->GetRasterBand(2)->SetNoDataValue(-888.0));
        EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(-777.0));
        CPLErrorReset();
      }

      // NullBlock with nodata.
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));
      EXPECT_DOUBLE_EQ(-777.0, buf[0]);

      // NaN + duplicate NaN.
      const double nan_val = std::numeric_limits<double>::quiet_NaN();
      {
        WithQuietHandler quiet;
        EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(nan_val));
        CPLErrorReset();
      }
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(nan_val));
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->DeleteNoDataValue());
    }

    // ReadOnly SetNoDataValue -> PAM, DeleteNoDataValue, SetNoDataValue -> PAM,
    // then Update SetNoDataValue cancels PAM.
    {
      GDALDatasetUniquePtr rds(
          GDALDataset::Open(kF64, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, rds);
      EXPECT_EQ(CE_None, rds->GetRasterBand(1)->SetNoDataValue(12.0));
      EXPECT_EQ(CE_None, rds->GetRasterBand(1)->DeleteNoDataValue());
      EXPECT_EQ(CE_None, rds->GetRasterBand(1)->SetNoDataValue(34.0));
    }

    {
      GDALDatasetUniquePtr uds(
          GDALDataset::Open(kF64, GDAL_OF_RASTER | GDAL_OF_UPDATE));
      ASSERT_NE(nullptr, uds);
      EXPECT_EQ(CE_None, uds->GetRasterBand(1)->SetNoDataValue(56.0));
    }
  }

  // 2. Int64 SetNoDataValue(double), SetNoDataValueAsInt64, PAM, NullBlock.
  {
    constexpr char kI64[] = "/vsimem/rbwrite_nd_i64.tif";
    constexpr char kI64Aux[] = "/vsimem/rbwrite_nd_i64.tif.aux.xml";
    absl::Cleanup c = [&]() {
      VSIUnlink(kI64);
      VSIUnlink(kI64Aux);
    };
    {
      CPLStringList opts;
      opts.AddNameValue("SPARSE_OK", "YES");
      GDALDatasetUniquePtr ds(
          driver_->Create(kI64, 4, 4, 2, GDT_Int64, opts.List()));
      ASSERT_NE(nullptr, ds);

      std::array<int64_t, 16> buf = {99};
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));
      EXPECT_EQ(0, buf[0]);

      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(-42.0));
      // Overwriting dataset nodata via SetNoDataValueAsInt64 when band 2
      // already has nodata at -42 emits a dataset-wide nodata warning.
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValueAsInt64(
                               -1000000000000LL));
        EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
        EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("has nodata at -42"));
        CPLErrorReset();
      }
      // Duplicate.
      EXPECT_EQ(CE_None,
                ds->GetRasterBand(1)->SetNoDataValueAsInt64(-1000000000000LL));
      // Mismatch on band 2 & band 1.
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_EQ(CE_None, ds->GetRasterBand(2)->SetNoDataValueAsInt64(
                               -2000000000000LL));
        EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValueAsInt64(
                               -3000000000000LL));
        CPLErrorReset();
      }

      // NullBlock with Int64 nodata set.
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));
      EXPECT_EQ(-3000000000000LL, buf[0]);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->DeleteNoDataValue());
    }

    {
      GDALDatasetUniquePtr rds(
          GDALDataset::Open(kI64, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, rds);
      EXPECT_EQ(CE_None, rds->GetRasterBand(1)->SetNoDataValueAsInt64(-500LL));
    }

    {
      GDALDatasetUniquePtr uds(
          GDALDataset::Open(kI64, GDAL_OF_RASTER | GDAL_OF_UPDATE));
      ASSERT_NE(nullptr, uds);
      EXPECT_EQ(CE_None, uds->GetRasterBand(1)->SetNoDataValueAsInt64(-600LL));
    }
  }

  // 3. UInt64 SetNoDataValue(double), SetNoDataValueAsUInt64, PAM, NullBlock.
  {
    constexpr char kU64[] = "/vsimem/rbwrite_nd_u64.tif";
    constexpr char kU64Aux[] = "/vsimem/rbwrite_nd_u64.tif.aux.xml";
    absl::Cleanup c = [&]() {
      VSIUnlink(kU64);
      VSIUnlink(kU64Aux);
    };
    {
      CPLStringList opts;
      opts.AddNameValue("SPARSE_OK", "YES");
      GDALDatasetUniquePtr ds(
          driver_->Create(kU64, 4, 4, 2, GDT_UInt64, opts.List()));
      ASSERT_NE(nullptr, ds);

      std::array<uint64_t, 16> buf = {99};
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));
      EXPECT_EQ(0ULL, buf[0]);

      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(42.0));
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValueAsUInt64(
                               1000000000000ULL));
        EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
        EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("has nodata at 42"));
        CPLErrorReset();
      }
      EXPECT_EQ(CE_None,
                ds->GetRasterBand(1)->SetNoDataValueAsUInt64(1000000000000ULL));
      {
        WithQuietHandler quiet;
        CPLErrorReset();
        EXPECT_EQ(CE_None, ds->GetRasterBand(2)->SetNoDataValueAsUInt64(
                               2000000000000ULL));
        EXPECT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValueAsUInt64(
                               3000000000000ULL));
        CPLErrorReset();
      }

      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 0, buf.data()));
      EXPECT_EQ(3000000000000ULL, buf[0]);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->DeleteNoDataValue());
    }

    {
      GDALDatasetUniquePtr rds(
          GDALDataset::Open(kU64, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, rds);
      EXPECT_EQ(CE_None, rds->GetRasterBand(1)->SetNoDataValueAsUInt64(500ULL));
    }

    {
      GDALDatasetUniquePtr uds(
          GDALDataset::Open(kU64, GDAL_OF_RASTER | GDAL_OF_UPDATE));
      ASSERT_NE(nullptr, uds);
      EXPECT_EQ(CE_None, uds->GetRasterBand(1)->SetNoDataValueAsUInt64(600ULL));
    }
  }
}

}  // namespace
}  // namespace autotest2
