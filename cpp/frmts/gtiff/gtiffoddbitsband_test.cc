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
// Tests odd-bit-depth and 24-bit floating-point GeoTIFF raster band read/write
// and clip/error handling (gtiffoddbitsband.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/gtiffoddbitsband.cpp

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <format>  // NOLINT
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class GTiffOddBitsBandTest : public ::testing::Test {
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

TEST_F(GTiffOddBitsBandTest, Float24ContigAndSeparateAndUnknownSampleFormat) {
  // 24-bit IEEEFP CONTIG (2 bands) and SEPARATE (2 bands).
  for (uint16_t planar : {static_cast<uint16_t>(PLANARCONFIG_CONTIG),
                          static_cast<uint16_t>(PLANARCONFIG_SEPARATE)}) {
    SCOPED_TRACE(planar);
    const char* file = (planar == PLANARCONFIG_CONTIG)
                           ? "/vsimem/oddbits_f24_contig.tif"
                           : "/vsimem/oddbits_f24_sep.tif";
    absl::Cleanup cleanup_file = [file]() { VSIUnlink(file); };

    {
      VSILFILE* fp = VSIFOpenL(file, "wb+");
      ASSERT_NE(nullptr, fp);
      absl::Cleanup close_fp = [&]() {
        if (fp != nullptr) VSIFCloseL(fp);
      };
      TIFF* tif = VSI_TIFFOpen(file, "w+", fp);
      ASSERT_NE(nullptr, tif);
      absl::Cleanup close_tif = [&]() {
        if (tif != nullptr) TIFFClose(tif);
      };
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
      // sign=0, exp=63 (0x3F), mant=0 -> 0x3F0000 -> LE bytes {0x00, 0x00,
      // 0x3F}.
      static_assert(std::endian::native == std::endian::little);
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
      tif = nullptr;
      EXPECT_EQ(0, VSIFCloseL(fp));
      fp = nullptr;
    }

    GDALDatasetUniquePtr ds(
        GDALDataset::Open(file, GDAL_OF_RASTER | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GDT_Float32, ds->GetRasterBand(1)->GetRasterDataType());

    std::array<float, 4> vals1 = {};
    std::array<float, 4> vals2 = {};
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 0, vals1.data()));
    EXPECT_EQ(CE_None, ds->GetRasterBand(2)->ReadBlock(0, 0, vals2.data()));
    EXPECT_FLOAT_EQ(1.0f, vals1[0]);
    EXPECT_FLOAT_EQ(2.0f, vals2[0]);

    // Exercise IsBaseGTiffClass() returning false during DirectIO RasterIO
    // (GTiffRasterBand::IRasterIO checks !IsBaseGTiffClass() under
    // GTIFF_DIRECT_IO).
    {
      CPLConfigOptionSetter dio("GTIFF_DIRECT_IO", "YES",
                                /*bSetOnlyIfUndefined=*/false);
      std::array<float, 4> rio_buf = {};
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                             GF_Read, 0, 0, 2, 2, rio_buf.data(), 2, 2,
                             GDT_Float32, 0, 0, nullptr));
    }

    // Writing 24-bit float is unsupported.
    {
      WithQuietHandler quiet;
      EXPECT_EQ(CE_Failure,
                ds->GetRasterBand(1)->WriteBlock(0, 0, vals1.data()));
      CPLErrorReset();
    }
  }

  // Raw 12-bit SAMPLEFORMAT_VOID -> GDT_Unknown open failure.
  {
    constexpr char kVoid[] = "/vsimem/oddbits_void12.tif";
    absl::Cleanup cleanup_void = [&]() { VSIUnlink(kVoid); };
    {
      VSILFILE* fp = VSIFOpenL(kVoid, "wb+");
      ASSERT_NE(nullptr, fp);
      absl::Cleanup close_fp = [&]() {
        if (fp != nullptr) VSIFCloseL(fp);
      };
      TIFF* tif = VSI_TIFFOpen(kVoid, "w+", fp);
      ASSERT_NE(nullptr, tif);
      absl::Cleanup close_tif = [&]() {
        if (tif != nullptr) TIFFClose(tif);
      };
      TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 4);
      TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 4);
      TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 12);
      TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
      TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_VOID);
      TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 4);
      TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
      TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
      std::vector<uint8_t> zeros(32, 0);
      ASSERT_GT(TIFFWriteEncodedStrip(tif, 0, zeros.data(), zeros.size()), 0);
      TIFFWriteDirectory(tif);
      TIFFClose(tif);
      tif = nullptr;
      EXPECT_EQ(0, VSIFCloseL(fp));
      fp = nullptr;
    }

    WithQuietHandler quiet;
    GDALDatasetUniquePtr bad_ds(
        GDALDataset::Open(kVoid, GDAL_OF_RASTER | GDAL_OF_READONLY));
    EXPECT_EQ(nullptr, bad_ds);
    CPLErrorReset();
  }
}

TEST_F(GTiffOddBitsBandTest, SeparateAndSingleBandReadWriteAllBitDepths) {
  // 1. 1-bit SEPARATE (2 bands, width=11 so both 8-pixel fast loop and
  // remainder bits 8..10 are tested).
  {
    constexpr char kFile[] = "/vsimem/oddbits_1bit_sep.tif";
    absl::Cleanup cleanup_file = [&]() { VSIUnlink(kFile); };
    std::vector<uint8_t> b1(22, 0);
    std::vector<uint8_t> b2(22, 0);
    for (int i = 0; i < 22; ++i) {
      b1[i] = (i % 2 == 0) ? 1 : 0;
      b2[i] = (i % 3 == 0) ? 1 : 0;
    }
    {
      CPLStringList opts;
      opts.AddNameValue("NBITS", "1");
      opts.AddNameValue("INTERLEAVE", "BAND");
      GDALDatasetUniquePtr ds(
          driver_->Create(kFile, 11, 2, 2, GDT_Byte, opts.List()));
      ASSERT_NE(nullptr, ds);

      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, b1.data()));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->WriteBlock(0, 0, b2.data()));
    }

    GDALDatasetUniquePtr rds(
        GDALDataset::Open(kFile, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds);
    std::vector<uint8_t> out1(22, 99);
    std::vector<uint8_t> out2(22, 99);
    EXPECT_EQ(CE_None, rds->GetRasterBand(1)->ReadBlock(0, 0, out1.data()));
    EXPECT_EQ(CE_None, rds->GetRasterBand(2)->ReadBlock(0, 0, out2.data()));
    EXPECT_EQ(b1, out1);
    EXPECT_EQ(b2, out2);
  }

  // PromoteTo8Bits via internal mask IFD (`m_bPromoteTo8Bits == true`).
  {
    constexpr char kMaskFile[] = "/vsimem/oddbits_mask_promote.tif";
    absl::Cleanup cleanup_mask = [&]() { VSIUnlink(kMaskFile); };
    CPLConfigOptionSetter internal_mask("GDAL_TIFF_INTERNAL_MASK", "YES",
                                        false);
    {
      GDALDatasetUniquePtr ds(
          driver_->Create(kMaskFile, 11, 2, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
      ASSERT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));
      GDALRasterBand* mask = ds->GetRasterBand(1)->GetMaskBand();
      ASSERT_NE(nullptr, mask);
      std::vector<uint8_t> mdata(22, 0);
      mdata[0] = 255;
      mdata[9] = 255;
      EXPECT_EQ(CE_None, mask->WriteBlock(0, 0, mdata.data()));
    }

    GDALDatasetUniquePtr rds(
        GDALDataset::Open(kMaskFile, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds);
    GDALRasterBand* rmask = rds->GetRasterBand(1)->GetMaskBand();
    ASSERT_NE(nullptr, rmask);
    std::vector<uint8_t> rout(22, 0);
    EXPECT_EQ(CE_None, rmask->ReadBlock(0, 0, rout.data()));
    EXPECT_EQ(255, rout[0]);
    EXPECT_EQ(0, rout[1]);
    EXPECT_EQ(255, rout[9]);
  }

  // 2. 12-bit single-band (width=5 so non-byte-aligned row & odd/even nibbles,
  // plus >= 2 clipped values > 4095 to hit m_bClipWarn false & true).
  {
    constexpr char kFile[] = "/vsimem/oddbits_12bit_1b.tif";
    absl::Cleanup cleanup_file = [&]() { VSIUnlink(kFile); };
    {
      CPLStringList opts;
      opts.AddNameValue("NBITS", "12");
      GDALDatasetUniquePtr ds(
          driver_->Create(kFile, 5, 2, 1, GDT_UInt16, opts.List()));
      ASSERT_NE(nullptr, ds);

      std::array<uint16_t, 10> in = {0,    1234, 4095, 5000, 6000,
                                     2730, 1365, 42,   4096, 7};
      WithQuietHandler quiet;
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, in.data()));
      CPLErrorReset();
    }

    GDALDatasetUniquePtr rds(
        GDALDataset::Open(kFile, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds);
    std::array<uint16_t, 10> out = {};
    EXPECT_EQ(CE_None, rds->GetRasterBand(1)->ReadBlock(0, 0, out.data()));
    EXPECT_EQ(0, out[0]);
    EXPECT_EQ(1234, out[1]);
    EXPECT_EQ(4095, out[2]);
    EXPECT_EQ(4095, out[3]);
    EXPECT_EQ(4095, out[4]);
    EXPECT_EQ(2730, out[5]);
    EXPECT_EQ(7, out[9]);
  }

  // 3. Separate/1-band generic bit depths: NBITS=3 (Byte), NBITS=10 (UInt16),
  // NBITS=20 (UInt32), and NBITS=24 (UInt32 special case).
  struct CaseSpec {
    int nbits;
    GDALDataType dt;
    uint32_t max_val;
  };
  constexpr CaseSpec kCases[] = {
      {.nbits = 3, .dt = GDT_Byte, .max_val = 7},
      {.nbits = 10, .dt = GDT_UInt16, .max_val = 1023},
      {.nbits = 20, .dt = GDT_UInt32, .max_val = (1U << 20) - 1},
      {.nbits = 24, .dt = GDT_UInt32, .max_val = (1U << 24) - 1},
  };
  for (const auto& spec : kCases) {
    SCOPED_TRACE(spec.nbits);
    const std::string fname =
        std::format("/vsimem/oddbits_sep_{}.tif", spec.nbits);
    absl::Cleanup cleanup_case = [&]() { VSIUnlink(fname.c_str()); };
    {
      CPLStringList opts;
      opts.AddNameValue("NBITS", std::to_string(spec.nbits).c_str());
      GDALDatasetUniquePtr ds(
          driver_->Create(fname.c_str(), 5, 2, 1, spec.dt, opts.List()));
      ASSERT_NE(nullptr, ds);

      WithQuietHandler quiet;
      if (spec.dt == GDT_Byte) {
        std::array<uint8_t, 10> in = {0, 1, 5, 7, 20, 30, 2, 3, 4, 6};
        EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, in.data()));
      } else if (spec.dt == GDT_UInt16) {
        std::array<uint16_t, 10> in = {0,    1,  511, 1023, 2000,
                                       3000, 42, 77,  100,  999};
        EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, in.data()));
      } else {
        std::array<uint32_t, 10> in = {0,
                                       1,
                                       0x12345,
                                       spec.max_val,
                                       spec.max_val + 100,
                                       spec.max_val + 200,
                                       42,
                                       256,
                                       65535,
                                       99999};
        EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, in.data()));
      }
      CPLErrorReset();
    }

    GDALDatasetUniquePtr rds(
        GDALDataset::Open(fname.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds);
    std::vector<uint32_t> out(10, 0);
    EXPECT_EQ(CE_None,
              rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 5, 2, out.data(),
                                              5, 2, GDT_UInt32, 0, 0, nullptr));
    EXPECT_EQ(0U, out[0]);
    EXPECT_EQ(1U, out[1]);
    EXPECT_EQ(spec.max_val, out[3]);
    EXPECT_EQ(spec.max_val, out[4]);
  }
}

TEST_F(GTiffOddBitsBandTest, ContigMultiBandReadWriteAllBitDepths) {
  // 1. 2-band CONTIG NBITS=12 (width=3, height=2; tests peer block null, clean,
  // and dirty cached, plus >=2 clipped values).
  {
    constexpr char kFile[] = "/vsimem/oddbits_12bit_contig.tif";
    absl::Cleanup cleanup_file = [&]() { VSIUnlink(kFile); };
    {
      CPLStringList opts;
      opts.AddNameValue("NBITS", "12");
      opts.AddNameValue("INTERLEAVE", "PIXEL");
      GDALDatasetUniquePtr ds(
          driver_->Create(kFile, 3, 2, 2, GDT_UInt16, opts.List()));
      ASSERT_NE(nullptr, ds);

      // Case A: Write band 1 directly while band 2 block is NOT in cache.
      std::array<uint16_t, 6> b1 = {100, 200, 4095, 5000, 6000, 321};
      {
        WithQuietHandler quiet;
        EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, b1.data()));
        CPLErrorReset();
      }
      ds->FlushCache(false);

      // Case B: Populate band 2 into block cache cleanly via RasterIO(GF_Read),
      // then WriteBlock on band 1 -> hits !poBlock->GetDirty() branch.
      std::array<uint16_t, 6> tmp_read = {};
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->RasterIO(
                             GF_Read, 0, 0, 3, 2, tmp_read.data(), 3, 2,
                             GDT_UInt16, 0, 0, nullptr));
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, b1.data()));

      // Case C: Dirty BOTH bands in block cache via partial RasterIO(GF_Write)
      // and then FlushCache -> flushes peer dirty block in IWriteBlock.
      uint16_t p1 = 777;
      uint16_t p2 = 888;
      EXPECT_EQ(CE_None,
                ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 1, 1, &p1, 1, 1,
                                               GDT_UInt16, 0, 0, nullptr));
      EXPECT_EQ(CE_None,
                ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 1, 1, &p2, 1, 1,
                                               GDT_UInt16, 0, 0, nullptr));
    }

    GDALDatasetUniquePtr rds(
        GDALDataset::Open(kFile, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds);
    std::array<uint16_t, 6> out1 = {};
    std::array<uint16_t, 6> out2 = {};
    EXPECT_EQ(CE_None, rds->GetRasterBand(1)->ReadBlock(0, 0, out1.data()));
    EXPECT_EQ(CE_None, rds->GetRasterBand(2)->ReadBlock(0, 0, out2.data()));
    EXPECT_EQ(777, out1[0]);
    EXPECT_EQ(4095, out1[3]);
    EXPECT_EQ(888, out2[0]);
  }

  // 2. 2-band CONTIG NBITS=1 (exercises bit setting & unsetting).
  {
    constexpr char kFile[] = "/vsimem/oddbits_1bit_contig.tif";
    absl::Cleanup cleanup_file = [&]() { VSIUnlink(kFile); };
    {
      CPLStringList opts;
      opts.AddNameValue("NBITS", "1");
      opts.AddNameValue("INTERLEAVE", "PIXEL");
      GDALDatasetUniquePtr ds(
          driver_->Create(kFile, 5, 2, 2, GDT_Byte, opts.List()));
      ASSERT_NE(nullptr, ds);

      std::array<uint8_t, 10> ones = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
      std::array<uint8_t, 10> mix = {1, 0, 1, 0, 5, 0, 1, 0, 1, 0};
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, ones.data()));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->WriteBlock(0, 0, ones.data()));
      // Overwrite band 1 with 0s and 1s (plus >1 clipped values) to exercise
      // unsetting bits in an existing block.
      std::array<uint8_t, 10> mix2 = {0, 1, 0, 9, 10, 1, 0, 1, 0, 1};
      {
        WithQuietHandler quiet;
        EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, mix2.data()));
        EXPECT_EQ(CE_None, ds->GetRasterBand(2)->WriteBlock(0, 0, mix.data()));
        CPLErrorReset();
      }
    }

    GDALDatasetUniquePtr rds(
        GDALDataset::Open(kFile, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds);
    std::array<uint8_t, 10> out1 = {};
    std::array<uint8_t, 10> out2 = {};
    EXPECT_EQ(CE_None, rds->GetRasterBand(1)->ReadBlock(0, 0, out1.data()));
    EXPECT_EQ(CE_None, rds->GetRasterBand(2)->ReadBlock(0, 0, out2.data()));
    EXPECT_EQ(0, out1[0]);
    EXPECT_EQ(1, out1[1]);
    EXPECT_EQ(1, out1[3]);
    EXPECT_EQ(1, out2[0]);
    EXPECT_EQ(0, out2[1]);
  }

  // 3. 2-band CONTIG NBITS=3 (Byte), NBITS=10 (UInt16), NBITS=20 (UInt32),
  // NBITS=24 (UInt32).
  struct CaseSpec {
    int nbits;
    GDALDataType dt;
    uint32_t max_val;
  };
  constexpr CaseSpec kCases[] = {
      {.nbits = 3, .dt = GDT_Byte, .max_val = 7},
      {.nbits = 10, .dt = GDT_UInt16, .max_val = 1023},
      {.nbits = 20, .dt = GDT_UInt32, .max_val = (1U << 20) - 1},
      {.nbits = 24, .dt = GDT_UInt32, .max_val = (1U << 24) - 1},
  };
  for (const auto& spec : kCases) {
    SCOPED_TRACE(spec.nbits);
    const std::string fname =
        std::format("/vsimem/oddbits_contig_{}.tif", spec.nbits);
    absl::Cleanup cleanup_case = [&]() { VSIUnlink(fname.c_str()); };
    {
      CPLStringList opts;
      opts.AddNameValue("NBITS", std::to_string(spec.nbits).c_str());
      opts.AddNameValue("INTERLEAVE", "PIXEL");
      GDALDatasetUniquePtr ds(
          driver_->Create(fname.c_str(), 3, 2, 2, spec.dt, opts.List()));
      ASSERT_NE(nullptr, ds);

      WithQuietHandler quiet;
      if (spec.dt == GDT_Byte) {
        std::array<uint8_t, 6> b1 = {0, 7, 15, 20, 3, 5};
        std::array<uint8_t, 6> b2 = {7, 0, 2, 4, 6, 1};
        EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, b1.data()));
        EXPECT_EQ(CE_None, ds->GetRasterBand(2)->WriteBlock(0, 0, b2.data()));
      } else if (spec.dt == GDT_UInt16) {
        std::array<uint16_t, 6> b1 = {0, 1023, 2000, 3000, 500, 42};
        std::array<uint16_t, 6> b2 = {1023, 0, 123, 456, 789, 11};
        // Dirty band 1 via cache so peer dirty flush in CONTIG U16 is covered.
        EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                               GF_Write, 0, 0, 3, 2, b1.data(), 3, 2,
                               GDT_UInt16, 0, 0, nullptr));
        EXPECT_EQ(CE_None, ds->GetRasterBand(2)->WriteBlock(0, 0, b2.data()));
      } else {
        std::array<uint32_t, 6> b1 = {
            0, spec.max_val, spec.max_val + 10, spec.max_val + 20, 0x1234, 99};
        std::array<uint32_t, 6> b2 = {spec.max_val, 0, 77, 88, 0x5678, 11};
        EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, b1.data()));
        EXPECT_EQ(CE_None, ds->GetRasterBand(2)->WriteBlock(0, 0, b2.data()));
      }
      CPLErrorReset();
    }

    GDALDatasetUniquePtr rds(
        GDALDataset::Open(fname.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds);
    std::array<uint32_t, 6> out1 = {};
    std::array<uint32_t, 6> out2 = {};
    EXPECT_EQ(CE_None,
              rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 3, 2, out1.data(),
                                              3, 2, GDT_UInt32, 0, 0, nullptr));
    EXPECT_EQ(CE_None,
              rds->GetRasterBand(2)->RasterIO(GF_Read, 0, 0, 3, 2, out2.data(),
                                              3, 2, GDT_UInt32, 0, 0, nullptr));
    EXPECT_EQ(0U, out1[0]);
    EXPECT_EQ(spec.max_val, out1[1]);
    EXPECT_EQ(spec.max_val, out1[2]);
    EXPECT_EQ(spec.max_val, out2[0]);
    EXPECT_EQ(0U, out2[1]);
  }
}

TEST_F(GTiffOddBitsBandTest, SparseAndErrorPathsInReadAndWriteBlock) {
  // 1. Sparse block read -> NullBlock(pImage) & CE_None.
  {
    constexpr char kSparse[] = "/vsimem/oddbits_sparse.tif";
    absl::Cleanup cleanup_sparse = [&]() { VSIUnlink(kSparse); };
    CPLStringList opts;
    opts.AddNameValue("NBITS", "4");
    opts.AddNameValue("TILED", "YES");
    opts.AddNameValue("BLOCKXSIZE", "16");
    opts.AddNameValue("BLOCKYSIZE", "16");
    opts.AddNameValue("SPARSE_OK", "YES");
    GDALDatasetUniquePtr ds(
        driver_->Create(kSparse, 32, 16, 1, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->SetNoDataValue(5));
    std::vector<uint8_t> tile(256, 99);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 0, tile.data()));
    EXPECT_EQ(5, tile[0]);
  }

  // 2. Write error via ||maxlength -> sets m_bWriteError and fails subsequent
  // IWriteBlock and LoadBlockBuf.
  {
    constexpr char kReal[] = "/vsimem/oddbits_max_err.tif";
    constexpr char kLimited[] = "/vsimem/oddbits_max_err.tif||maxlength=350";
    absl::Cleanup cleanup_real = [&]() {
      VSIUnlink(kLimited);
      VSIUnlink(kReal);
    };
    WithQuietHandler quiet;
    CPLStringList opts;
    opts.AddNameValue("NBITS", "12");
    opts.AddNameValue("BLOCKYSIZE", "16");
    GDALDatasetUniquePtr ds(
        driver_->Create(kLimited, 32, 64, 1, GDT_UInt16, opts.List()));
    ASSERT_NE(nullptr, ds);
    std::vector<uint16_t> strip(32 * 16, 1234);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, strip.data()));

    // Writing block 1 flushes block 0 via LoadBlockBuf -> exceeds maxlength,
    // setting m_bWriteError and returning CE_Failure.
    EXPECT_EQ(CE_Failure, ds->GetRasterBand(1)->WriteBlock(0, 1, strip.data()));
    // Subsequent WriteBlock hits the m_bWriteError guard.
    EXPECT_EQ(CE_Failure, ds->GetRasterBand(1)->WriteBlock(0, 2, strip.data()));
    CPLErrorReset();
  }

  // 3. Corrupted compressed strip -> LoadBlockBuf failure in IReadBlock and
  // CONTIG IWriteBlock.
  {
    constexpr char kCorrupt[] = "/vsimem/oddbits_corrupt_lzw.tif";
    absl::Cleanup cleanup_corrupt = [&]() { VSIUnlink(kCorrupt); };
    {
      CPLStringList opts;
      opts.AddNameValue("NBITS", "4");
      opts.AddNameValue("COMPRESS", "LZW");
      opts.AddNameValue("INTERLEAVE", "PIXEL");
      GDALDatasetUniquePtr ds(
          driver_->Create(kCorrupt, 16, 16, 2, GDT_Byte, opts.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<uint8_t> data(256, 3);
      EXPECT_EQ(CE_None, ds->GetRasterBand(1)->WriteBlock(0, 0, data.data()));
      EXPECT_EQ(CE_None, ds->GetRasterBand(2)->WriteBlock(0, 0, data.data()));
    }

    // Corrupt strip payload bytes at end of file.
    vsi_l_offset sz = 0;
    GByte* raw = VSIGetMemFileBuffer(kCorrupt, &sz, FALSE);
    ASSERT_NE(nullptr, raw);
    ASSERT_GT(sz, 16U);
    std::memset(raw + sz - 12, 0xFF, 12);

    GDALDatasetUniquePtr rds(
        GDALDataset::Open(kCorrupt, GDAL_OF_RASTER | GDAL_OF_UPDATE));
    ASSERT_NE(nullptr, rds);
    std::vector<uint8_t> out(256, 0);
    WithQuietHandler quiet;
    EXPECT_EQ(CE_Failure, rds->GetRasterBand(1)->ReadBlock(0, 0, out.data()));
    EXPECT_EQ(CE_Failure, rds->GetRasterBand(1)->WriteBlock(0, 0, out.data()));
    CPLErrorReset();
  }
}

// TODO: b/335317901 - Cover remaining defensive I/O error branches and
// big-endian byte-swapping paths in gtiffoddbitsband.cpp (~91.3% coverage
// today).
}  // namespace
}  // namespace autotest2
