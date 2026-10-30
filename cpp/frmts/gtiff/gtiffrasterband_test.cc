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

#include "third_party/gdal/frmts/gtiff/gtiffrasterband.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class GTiffRasterBandTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    driver_ = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(driver_, nullptr);
  }

  void TearDown() override { CPLErrorReset(); }

  GDALDriver* driver_ = nullptr;
};

// Returns true if every element of `buf` equals `value`.
bool AllEqual(const std::vector<uint8_t>& buf, uint8_t value) {
  return std::all_of(buf.begin(), buf.end(),
                     [value](uint8_t v) { return v == value; });
}

// Covers all data type mappings in GTiffRasterBand::GTiffRasterBand constructor
// (8, 16, 32, 64, 128-bit and unsupported 128-bit integer).
TEST_F(GTiffRasterBandTest, AllDataTypesInConstructor) {
  const GDALDataType kTypes[] = {
      GDT_Byte,    GDT_Int8,     GDT_UInt16,   GDT_Int16,
      GDT_Float16, GDT_UInt32,   GDT_Int32,    GDT_Float32,
      GDT_CInt16,  GDT_CFloat16, GDT_UInt64,   GDT_Int64,
      GDT_Float64, GDT_CInt32,   GDT_CFloat32, GDT_CFloat64,
  };

  for (int idx = 0; idx < static_cast<int>(std::size(kTypes)); ++idx) {
    const std::string filename =
        "/vsimem/gtiffrb_dtype_" + std::to_string(idx) + ".tif";
    absl::Cleanup cleanup = [&]() { VSIUnlink(filename.c_str()); };

    GDALDataset* ds =
        driver_->Create(filename.c_str(), 4, 4, 1, kTypes[idx], nullptr);
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterBand(1)->GetRasterDataType(), kTypes[idx]);
    EXPECT_EQ(ds->GetRasterBand(1)->GetSuggestedBlockAccessPattern(),
              GSBAP_RANDOM);
    GDALClose(ds);
  }

  // Unsupported 128-bit UINT -> constructs GTiffRasterBand with GDT_Unknown,
  // then OpenOffset rejects cleanly.
  constexpr char kBad128[] = "/vsimem/gtiffrb_bad128.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kBad128); };
  VSILFILE* fp = VSIFOpenL(kBad128, "wb+");
  ASSERT_NE(fp, nullptr);
  TIFF* tif = VSI_TIFFOpen(kBad128, "w+", fp);
  ASSERT_NE(tif, nullptr);
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 4);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 4);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 128);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 4);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
  std::vector<uint8_t> zeros(4 * 4 * 16, 0);
  TIFFWriteEncodedStrip(tif, 0, zeros.data(), zeros.size());
  TIFFWriteDirectory(tif);
  TIFFClose(tif);
  VSIFCloseL(fp);

  CPLErrorStateBackuper quiet_errors(CPLQuietErrorHandler);
  EXPECT_EQ(GDALOpen(kBad128, GA_ReadOnly), nullptr);
}

// Covers all photometric & ExtraSamples color interpretation paths in
// GTiffRasterBand constructor.
TEST_F(GTiffRasterBandTest, PhotometricAndExtraSamplesInterpretations) {
  // 1. Palette with 1 band (PaletteIndex).
  {
    constexpr char kFile[] = "/vsimem/gtiffrb_pal.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kFile); };
    char** opts = CSLSetNameValue(nullptr, "PHOTOMETRIC", "PALETTE");
    GDALDataset* ds = driver_->Create(kFile, 4, 4, 1, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterBand(1)->GetColorInterpretation(), GCI_PaletteIndex);
    GDALClose(ds);
  }

  // 2. RGB + premultiplied alpha + unspecified 5th band.
  {
    constexpr char kFile[] = "/vsimem/gtiffrb_rgba_premult.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kFile); };
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "PHOTOMETRIC", "RGB");
    opts = CSLSetNameValue(opts, "ALPHA", "PREMULTIPLIED");
    GDALDataset* ds = driver_->Create(kFile, 4, 4, 5, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);
    GDALClose(ds);

    GDALDataset* rds = static_cast<GDALDataset*>(GDALOpen(kFile, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    EXPECT_EQ(rds->GetRasterBand(1)->GetColorInterpretation(), GCI_RedBand);
    EXPECT_EQ(rds->GetRasterBand(2)->GetColorInterpretation(), GCI_GreenBand);
    EXPECT_EQ(rds->GetRasterBand(3)->GetColorInterpretation(), GCI_BlueBand);
    EXPECT_EQ(rds->GetRasterBand(4)->GetColorInterpretation(), GCI_AlphaBand);
    EXPECT_EQ(rds->GetRasterBand(5)->GetColorInterpretation(), GCI_Undefined);
    GDALClose(rds);
  }

  // 3. YCbCr + JPEG both with CONVERT_YCBCR_TO_RGB=YES and =NO.
  {
    constexpr char kFile[] = "/vsimem/gtiffrb_ycbcr.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kFile); };
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "COMPRESS", "JPEG");
    opts = CSLSetNameValue(opts, "PHOTOMETRIC", "YCBCR");
    GDALDataset* ds = driver_->Create(kFile, 16, 16, 3, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);
    std::vector<uint8_t> buf(16 * 16 * 3, 128);
    ASSERT_EQ(ds->RasterIO(GF_Write, 0, 0, 16, 16, buf.data(), 16, 16, GDT_Byte,
                           3, nullptr, 0, 0, 0, nullptr),
              CE_None);
    GDALClose(ds);

    GDALDataset* rds_rgb =
        static_cast<GDALDataset*>(GDALOpen(kFile, GA_ReadOnly));
    ASSERT_NE(rds_rgb, nullptr);
    EXPECT_EQ(rds_rgb->GetRasterBand(1)->GetColorInterpretation(), GCI_RedBand);
    GDALClose(rds_rgb);

    CPLConfigOptionSetter no_conv("CONVERT_YCBCR_TO_RGB", "NO", false);
    GDALDataset* rds_ycbcr =
        static_cast<GDALDataset*>(GDALOpen(kFile, GA_ReadOnly));
    ASSERT_NE(rds_ycbcr, nullptr);
    EXPECT_EQ(rds_ycbcr->GetRasterBand(1)->GetColorInterpretation(),
              GCI_YCbCr_YBand);
    EXPECT_EQ(rds_ycbcr->GetRasterBand(2)->GetColorInterpretation(),
              GCI_YCbCr_CbBand);
    EXPECT_EQ(rds_ycbcr->GetRasterBand(3)->GetColorInterpretation(),
              GCI_YCbCr_CrBand);
    GDALClose(rds_ycbcr);
  }

  // 4. 4-band raw YCbCr (1,1 subsampling, PLANARCONFIG_SEPARATE so libtiff
  // allows SamplesPerPixel == 4) -> hits YCbCr band 4 ExtraSamples.
  {
    constexpr char kFile[] = "/vsimem/gtiffrb_ycbcr4.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kFile); };
    VSILFILE* fp = VSIFOpenL(kFile, "wb+");
    ASSERT_NE(fp, nullptr);
    TIFF* tif = VSI_TIFFOpen(kFile, "w+", fp);
    ASSERT_NE(tif, nullptr);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 4);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 4);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 4);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 4);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR);
    TIFFSetField(tif, TIFFTAG_YCBCRSUBSAMPLING, 1, 1);
    uint16_t extra = EXTRASAMPLE_UNASSALPHA;
    TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &extra);
    std::vector<uint8_t> zeros(4 * 4, 0);
    for (int s = 0; s < 4; ++s) {
      TIFFWriteEncodedStrip(tif, s, zeros.data(), zeros.size());
    }
    TIFFWriteDirectory(tif);
    TIFFClose(tif);
    VSIFCloseL(fp);

    GDALDataset* rds = static_cast<GDALDataset*>(
        GDALOpen("GTIFF_RAW:/vsimem/gtiffrb_ycbcr4.tif", GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    EXPECT_EQ(rds->GetRasterBand(4)->GetColorInterpretation(), GCI_AlphaBand);
    GDALClose(rds);
  }

  // 5. CMYK (PHOTOMETRIC_SEPARATED) with 5 bands via Create + GTIFF_RAW:.
  {
    constexpr char kFile[] = "/vsimem/gtiffrb_cmyk5.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kFile); };
    char** opts = CSLSetNameValue(nullptr, "PHOTOMETRIC", "CMYK");
    GDALDataset* ds = driver_->Create(kFile, 4, 4, 5, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterBand(1)->GetColorInterpretation(), GCI_CyanBand);
    EXPECT_EQ(ds->GetRasterBand(2)->GetColorInterpretation(), GCI_MagentaBand);
    EXPECT_EQ(ds->GetRasterBand(3)->GetColorInterpretation(), GCI_YellowBand);
    EXPECT_EQ(ds->GetRasterBand(4)->GetColorInterpretation(), GCI_BlackBand);
    EXPECT_EQ(ds->GetRasterBand(5)->GetColorInterpretation(), GCI_Undefined);
    GDALClose(ds);
  }

  // 6. Wrong number of ExtraSamples warning + missing ExtraSamples tag.
  {
    constexpr char kWrongExtra[] = "/vsimem/gtiffrb_wrong_extra.tif";
    constexpr char kNoExtra[] = "/vsimem/gtiffrb_no_extra.tif";
    absl::Cleanup c = [&]() {
      VSIUnlink(kWrongExtra);
      VSIUnlink(kNoExtra);
    };

    // 3 samples, MINISBLACK (expects 1 base sample), only 1 ExtraSample.
    {
      // Write a valid 4-sample PHOTOMETRIC_RGB file with 1 ExtraSample, then
      // patch the PHOTOMETRIC IFD entry from RGB (2) to MINISBLACK (1). On
      // read, MINISBLACK expects 1 base sample (3 extras) but the tag holds
      // 1 extra, which triggers the "Wrong number of ExtraSamples" warning.
      VSILFILE* fp = VSIFOpenL(kWrongExtra, "wb+");
      ASSERT_NE(fp, nullptr);
      TIFF* tif = VSI_TIFFOpen(kWrongExtra, "w+", fp);
      ASSERT_NE(tif, nullptr);
      TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 4);
      TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 4);
      TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
      TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 4);
      TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 4);
      TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
      TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
      uint16_t extra = EXTRASAMPLE_UNASSALPHA;
      TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &extra);
      std::vector<uint8_t> zeros(4 * 4 * 4, 0);
      TIFFWriteEncodedStrip(tif, 0, zeros.data(), zeros.size());
      TIFFWriteDirectory(tif);
      TIFFClose(tif);
      VSIFCloseL(fp);

      vsi_l_offset sz = 0;
      GByte* raw = VSIGetMemFileBuffer(kWrongExtra, &sz, FALSE);
      ASSERT_NE(raw, nullptr);
      uint32_t ifd_off = 0;
      std::memcpy(&ifd_off, raw + 4, 4);
      uint16_t nentries = 0;
      std::memcpy(&nentries, raw + ifd_off, 2);
      for (uint16_t i = 0; i < nentries; ++i) {
        size_t pos = ifd_off + 2 + i * 12;
        uint16_t tag = 0;
        std::memcpy(&tag, raw + pos, 2);
        if (tag == TIFFTAG_PHOTOMETRIC) {
          uint16_t mb = PHOTOMETRIC_MINISBLACK;
          std::memcpy(raw + pos + 8, &mb, 2);
        }
      }

      CPLErrorStateBackuper quiet_errors(CPLQuietErrorHandler);
      GDALDataset* rds =
          static_cast<GDALDataset*>(GDALOpen(kWrongExtra, GA_ReadOnly));
      ASSERT_NE(rds, nullptr);
      // Note: libtiff auto-pads ExtraSamples on read if SamplesPerPixel > base,
      // OR keeps count=1 depending on tif_dirread. Verify all 4 bands open:
      EXPECT_EQ(rds->GetRasterBand(1)->GetColorInterpretation(), GCI_GrayIndex);
      EXPECT_EQ(rds->GetRasterCount(), 4);
      GDALClose(rds);
    }

    // 2 samples, MINISBLACK, no TIFFTAG_EXTRASAMPLES at all.
    {
      VSILFILE* fp = VSIFOpenL(kNoExtra, "wb+");
      ASSERT_NE(fp, nullptr);
      TIFF* tif = VSI_TIFFOpen(kNoExtra, "w+", fp);
      ASSERT_NE(tif, nullptr);
      TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 4);
      TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 4);
      TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
      TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 2);
      TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 4);
      TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
      TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
      std::vector<uint8_t> zeros(4 * 4 * 2, 0);
      TIFFWriteEncodedStrip(tif, 0, zeros.data(), zeros.size());
      TIFFWriteDirectory(tif);
      TIFFClose(tif);
      VSIFCloseL(fp);

      GDALDataset* rds =
          static_cast<GDALDataset*>(GDALOpen(kNoExtra, GA_ReadOnly));
      ASSERT_NE(rds, nullptr);
      EXPECT_EQ(rds->GetRasterBand(2)->GetColorInterpretation(), GCI_Undefined);
      GDALClose(rds);
    }
  }
}

// Covers IRasterIO read paths: Overviews, VirtualMemIO, DirectIO,
// MultiThreadedRead, HasOptimizedReadMultiRange (with/without threads and COG
// interleaved mask), and aggressive band caching disable.
TEST_F(GTiffRasterBandTest, IRasterIOReadPaths) {
  constexpr char kUncomp[] = "/vsimem/gtiffrb_uncomp_tiled.tif";
  constexpr char kSrcMask[] = "/vsimem/gtiffrb_src_mask.tif";
  constexpr char kCogMask[] = "/vsimem/gtiffrb_cog_mask.tif";
  constexpr char kRgb3[] = "/vsimem/gtiffrb_rgb3.tif";
  absl::Cleanup cleanup = [&]() {
    VSIUnlink(kUncomp);
    VSIUnlink(kSrcMask);
    VSIUnlink(kCogMask);
    VSIUnlink(kRgb3);
  };

  // Create 64x64 tiled uncompressed dataset + build 2x overview.
  {
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "TILED", "YES");
    opts = CSLSetNameValue(opts, "BLOCKXSIZE", "16");
    opts = CSLSetNameValue(opts, "BLOCKYSIZE", "16");
    GDALDataset* ds = driver_->Create(kUncomp, 64, 64, 1, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);
    std::vector<uint8_t> data(64 * 64, 77);
    ASSERT_EQ(
        ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 64, 64, data.data(), 64,
                                       64, GDT_Byte, 0, 0, nullptr),
        CE_None);
    int ovr = 2;
    ASSERT_EQ(
        ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr, nullptr, nullptr),
        CE_None);
    GDALClose(ds);
  }

  // 1. Overview downsampled read (NearestNeighbour hits bTried==TRUE return).
  {
    GDALDataset* rds =
        static_cast<GDALDataset*>(GDALOpen(kUncomp, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    std::vector<uint8_t> out(32 * 32, 0);
    EXPECT_EQ(rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64, out.data(),
                                              32, 32, GDT_Byte, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(out[0], 77);

    // Bilinear upsampling skips overview, hits GDALPamRasterBand::IRasterIO
    // non-nearest-neighbour branch.
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.eResampleAlg = GRIORA_Bilinear;
    std::vector<uint8_t> up(32 * 32, 0);
    EXPECT_EQ(rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 16, 16, up.data(),
                                              32, 32, GDT_Byte, 0, 0, &extra),
              CE_None);
    EXPECT_TRUE(AllEqual(up, 77));
    GDALClose(rds);
  }

  // 2. VirtualMemIO success & fallback.
  {
    CPLConfigOptionSetter vmem("GTIFF_VIRTUAL_MEM_IO", "YES", false);
    GDALDataset* rds =
        static_cast<GDALDataset*>(GDALOpen(kUncomp, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    std::vector<uint8_t> out(64 * 64, 0);
    EXPECT_EQ(rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64, out.data(),
                                              64, 64, GDT_Byte, 0, 0, nullptr),
              CE_None);
    EXPECT_TRUE(AllEqual(out, 77));

    // Fallback (Bilinear returns -1 from VirtualMemIO).
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.eResampleAlg = GRIORA_Bilinear;
    std::vector<uint8_t> half(32 * 64, 0);
    EXPECT_EQ(
        rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64, half.data(), 32,
                                        64, GDT_Byte, 0, 0, &extra),
        CE_None);
    EXPECT_TRUE(AllEqual(half, 77));
    GDALClose(rds);
  }

  // 3. DirectIO success & fallback.
  {
    CPLConfigOptionSetter dio("GTIFF_DIRECT_IO", "YES", false);
    GDALDataset* rds =
        static_cast<GDALDataset*>(GDALOpen(kUncomp, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    std::vector<uint8_t> out(64 * 64, 0);
    EXPECT_EQ(rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64, out.data(),
                                              64, 64, GDT_Byte, 0, 0, nullptr),
              CE_None);
    EXPECT_TRUE(AllEqual(out, 77));

    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.eResampleAlg = GRIORA_Bilinear;
    std::vector<uint8_t> half(32 * 64, 0);
    EXPECT_EQ(
        rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64, half.data(), 32,
                                        64, GDT_Byte, 0, 0, &extra),
        CE_None);
    EXPECT_TRUE(AllEqual(half, 77));
    GDALClose(rds);
  }

  // 4. MultiThreadedRead + HasOptimizedReadMultiRange (with & without threads).
  {
    CPLConfigOptionSetter multi_range("GTIFF_HAS_OPTIMIZED_READ_MULTI_RANGE",
                                      "YES", false);
    // Single-threaded -> CacheMultiRange + BufferedDataFreer cleanup.
    {
      GDALDataset* rds =
          static_cast<GDALDataset*>(GDALOpen(kUncomp, GA_ReadOnly));
      ASSERT_NE(rds, nullptr);
      std::vector<uint8_t> out(64 * 64, 0);
      EXPECT_EQ(
          rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64, out.data(), 64,
                                          64, GDT_Byte, 0, 0, nullptr),
          CE_None);
      EXPECT_TRUE(AllEqual(out, 77));
      GDALClose(rds);
    }

    // Multi-threaded -> HasPRead() true + MultiThreadedRead; also 1-block read.
    {
      const char* open_opts[] = {"NUM_THREADS=2", nullptr};
      GDALDataset* rds = static_cast<GDALDataset*>(
          GDALOpenEx(kUncomp, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                     open_opts, nullptr));
      ASSERT_NE(rds, nullptr);
      std::vector<uint8_t> out(64 * 64, 0);
      EXPECT_EQ(
          rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64, out.data(), 64,
                                          64, GDT_Byte, 0, 0, nullptr),
          CE_None);
      EXPECT_TRUE(AllEqual(out, 77));
      std::vector<uint8_t> one_tile(16 * 16, 0);
      EXPECT_EQ(rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 16, 16,
                                                one_tile.data(), 16, 16,
                                                GDT_Byte, 0, 0, nullptr),
                CE_None);
      EXPECT_TRUE(AllEqual(one_tile, 77));
      GDALClose(rds);
    }
  }

  // 5. COG-style interleaved mask with HasOptimizedReadMultiRange.
  {
    {
      CPLConfigOptionSetter imask("GDAL_TIFF_INTERNAL_MASK", "YES", false);
      GDALDataset* src =
          driver_->Create(kSrcMask, 64, 64, 1, GDT_Byte, nullptr);
      ASSERT_NE(src, nullptr);
      ASSERT_EQ(src->CreateMaskBand(GMF_PER_DATASET), CE_None);
      std::vector<uint8_t> ones(64 * 64, 255);
      ASSERT_EQ(src->GetRasterBand(1)->GetMaskBand()->RasterIO(
                    GF_Write, 0, 0, 64, 64, ones.data(), 64, 64, GDT_Byte, 0, 0,
                    nullptr),
                CE_None);

      char** copts = nullptr;
      copts = CSLSetNameValue(copts, "TILED", "YES");
      copts = CSLSetNameValue(copts, "BLOCKXSIZE", "16");
      copts = CSLSetNameValue(copts, "BLOCKYSIZE", "16");
      copts = CSLSetNameValue(copts, "COMPRESS", "LZW");
      copts = CSLSetNameValue(copts, "COPY_SRC_OVERVIEWS", "YES");
      GDALDataset* cog =
          driver_->CreateCopy(kCogMask, src, FALSE, copts, nullptr, nullptr);
      CSLDestroy(copts);
      ASSERT_NE(cog, nullptr);
      GDALClose(cog);
      GDALClose(src);
    }

    CPLConfigOptionSetter multi_range("GTIFF_HAS_OPTIMIZED_READ_MULTI_RANGE",
                                      "YES", false);
    GDALDataset* rds =
        static_cast<GDALDataset*>(GDALOpen(kCogMask, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    GDALRasterBand* mask_band = rds->GetRasterBand(1)->GetMaskBand();
    ASSERT_NE(mask_band, nullptr);
    std::vector<uint8_t> mask_buf(64 * 64, 0);
    EXPECT_EQ(mask_band->RasterIO(GF_Read, 0, 0, 64, 64, mask_buf.data(), 64,
                                  64, GDT_Byte, 0, 0, nullptr),
              CE_None);
    EXPECT_TRUE(AllEqual(mask_buf, 255));
    GDALClose(rds);
  }

  // 6. Aggressive band caching disable when required mem > GDALGetCacheMax64().
  {
    const GIntBig old_cache = GDALGetCacheMax64();
    absl::Cleanup restore_cache = [&]() { GDALSetCacheMax64(old_cache); };
    GDALSetCacheMax64(1024);

    GDALDataset* ds = driver_->Create(kRgb3, 64, 64, 3, GDT_Byte, nullptr);
    ASSERT_NE(ds, nullptr);
    GDALClose(ds);

    GDALDataset* rds = static_cast<GDALDataset*>(GDALOpen(kRgb3, GA_ReadOnly));
    ASSERT_NE(rds, nullptr);
    std::vector<uint8_t> out(64 * 64, 0);
    // Call twice: 1st sets m_bHasWarnedDisableAggressiveBandCaching, 2nd skips
    // the warning branch.
    EXPECT_EQ(rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64, out.data(),
                                              64, 64, GDT_Byte, 0, 0, nullptr),
              CE_None);
    EXPECT_EQ(rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64, out.data(),
                                              64, 64, GDT_Byte, 0, 0, nullptr),
              CE_None);
    GDALClose(rds);
  }
}

// Covers IRasterIO whole-block write optimization: GTIFF_DONT_WRITE_BLOCKS,
// single full block, PLANARCONFIG_SEPARATE ComputeBlockId, partial edge tiles
// with zero-padding, type conversion, and write failure.
TEST_F(GTiffRasterBandTest, IRasterIOWholeBlockWritePaths) {
  constexpr char kDontWrite[] = "/vsimem/gtiffrb_dont_write.tif";
  constexpr char kSingleBlk[] = "/vsimem/gtiffrb_single_blk.tif";
  constexpr char kPartialEdge[] = "/vsimem/gtiffrb_partial_edge.tif";
  constexpr char kCapped[] = "/vsimem/gtiffrb_capped.tif";
  absl::Cleanup cleanup = [&]() {
    VSIUnlink(kDontWrite);
    VSIUnlink(kSingleBlk);
    VSIUnlink(kPartialEdge);
    VSIUnlink(kCapped);
  };

  // 1. GTIFF_DONT_WRITE_BLOCKS=YES.
  {
    CPLConfigOptionSetter dont_write("GTIFF_DONT_WRITE_BLOCKS", "YES", false);
    GDALDataset* ds = driver_->Create(kDontWrite, 16, 16, 1, GDT_Byte, nullptr);
    ASSERT_NE(ds, nullptr);
    std::vector<uint8_t> buf(16 * 16, 9);
    EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 16, 16, buf.data(),
                                             16, 16, GDT_Byte, 0, 0, nullptr),
              CE_None);
    GDALClose(ds);
  }

  // 2. Single full block write + PLANARCONFIG_SEPARATE Band 2 ComputeBlockId +
  // GDT_UInt16 -> GDT_Byte buffer conversion.
  {
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "TILED", "YES");
    opts = CSLSetNameValue(opts, "BLOCKXSIZE", "16");
    opts = CSLSetNameValue(opts, "BLOCKYSIZE", "16");
    opts = CSLSetNameValue(opts, "INTERLEAVE", "BAND");
    GDALDataset* ds = driver_->Create(kSingleBlk, 32, 32, 2, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);

    std::vector<uint8_t> blk(16 * 16, 33);
    EXPECT_EQ(ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 16, 16, blk.data(),
                                             16, 16, GDT_Byte, 0, 0, nullptr),
              CE_None);

    // Same whole-block path with eBufType != eDataType (hits LoadBlockBuf +
    // GDALCopyWords64).
    std::vector<uint16_t> blk16(16 * 16, 44);
    EXPECT_EQ(
        ds->GetRasterBand(1)->RasterIO(GF_Write, 16, 0, 16, 16, blk16.data(),
                                       16, 16, GDT_UInt16, 0, 0, nullptr),
        CE_None);
    GDALClose(ds);
  }

  // 3. Multi-block 20x20 with 16x16 tiles -> hits full tile (0,0) AND partial
  // edge tiles (1,0), (0,1), (1,1) with memset zero-padding; plus unaligned
  // fallback write.
  {
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "TILED", "YES");
    opts = CSLSetNameValue(opts, "BLOCKXSIZE", "16");
    opts = CSLSetNameValue(opts, "BLOCKYSIZE", "16");
    GDALDataset* ds = driver_->Create(kPartialEdge, 20, 20, 1, GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);

    std::vector<uint8_t> img(20 * 20, 55);
    EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 20, 20, img.data(),
                                             20, 20, GDT_Byte, 0, 0, nullptr),
              CE_None);

    // Sub-block unaligned write -> falls through to
    // GDALPamRasterBand::IRasterIO.
    uint8_t sub[4] = {1, 2, 3, 4};
    EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 1, 1, 2, 2, sub, 2, 2,
                                             GDT_Byte, 0, 0, nullptr),
              CE_None);
    GDALClose(ds);
  }

  // 4. WriteEncodedTileOrStrip failure via /vsimem/ maxlength cap.
  {
    CPLErrorStateBackuper quiet_errors(CPLQuietErrorHandler);
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "TILED", "YES");
    opts = CSLSetNameValue(opts, "BLOCKXSIZE", "16");
    opts = CSLSetNameValue(opts, "BLOCKYSIZE", "16");
    GDALDataset* ds =
        driver_->Create("/vsimem/gtiffrb_capped.tif||maxlength=1024", 64, 64, 1,
                        GDT_Byte, opts);
    CSLDestroy(opts);
    ASSERT_NE(ds, nullptr);

    std::vector<uint8_t> big(64 * 64, 99);
    EXPECT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 64, 64, big.data(),
                                             64, 64, GDT_Byte, 0, 0, nullptr),
              CE_Failure);
    GDALClose(ds);
  }
}

}  // namespace
}  // namespace autotest2
