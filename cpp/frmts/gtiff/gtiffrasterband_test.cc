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
// Tests GeoTIFF raster band construction, data type mappings, color
// interpretations, and IRasterIO fast paths (gtiffrasterband.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/gtiffrasterband.cpp

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
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

using ::testing::HasSubstr;

class GTiffRasterBandTest : public ::testing::Test {
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

// Returns true if every element of `buf` equals `value`.
bool AllEqual(const std::vector<uint8_t>& buf, uint8_t value) {
  return std::ranges::all_of(buf, [value](uint8_t v) { return v == value; });
}

// Covers all data type mappings in GTiffRasterBand::GTiffRasterBand constructor
// (8, 16, 32, 64, 128-bit and unsupported 128-bit integer).
TEST_F(GTiffRasterBandTest, AllDataTypesInConstructor) {
  constexpr GDALDataType kTypes[] = {
      GDT_Byte,    GDT_Int8,     GDT_UInt16,   GDT_Int16,
      GDT_Float16, GDT_UInt32,   GDT_Int32,    GDT_Float32,
      GDT_CInt16,  GDT_CFloat16, GDT_UInt64,   GDT_Int64,
      GDT_Float64, GDT_CInt32,   GDT_CFloat32, GDT_CFloat64,
  };

  for (size_t idx = 0; idx < std::size(kTypes); ++idx) {
    const std::string filename =
        std::format("/vsimem/gtiffrb_dtype_{}.tif", idx);
    absl::Cleanup cleanup = [&]() { VSIUnlink(filename.c_str()); };

    GDALDatasetUniquePtr ds(
        driver_->Create(filename.c_str(), 4, 4, 1, kTypes[idx], nullptr));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(kTypes[idx], ds->GetRasterBand(1)->GetRasterDataType());
    EXPECT_EQ(GSBAP_RANDOM,
              ds->GetRasterBand(1)->GetSuggestedBlockAccessPattern());
  }

  // Unsupported 128-bit UINT -> constructs GTiffRasterBand with GDT_Unknown,
  // then OpenOffset rejects cleanly.
  constexpr char kBad128[] = "/vsimem/gtiffrb_bad128.tif";
  absl::Cleanup cleanup = [&]() { VSIUnlink(kBad128); };
  {
    VSILFILE* fp = VSIFOpenL(kBad128, "wb+");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [&]() {
      if (fp != nullptr) VSIFCloseL(fp);
    };
    TIFF* tif = VSI_TIFFOpen(kBad128, "w+", fp);
    ASSERT_NE(nullptr, tif);
    absl::Cleanup close_tif = [&]() {
      if (tif != nullptr) TIFFClose(tif);
    };
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 4);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 4);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 128);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 4);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    std::vector<uint8_t> zeros(4 * 4 * 16, 0);
    ASSERT_GT(TIFFWriteEncodedStrip(tif, 0, zeros.data(), zeros.size()), 0);
    TIFFWriteDirectory(tif);
    TIFFClose(tif);
    tif = nullptr;
    EXPECT_EQ(0, VSIFCloseL(fp));
    fp = nullptr;
  }

  WithQuietHandler quiet;
  GDALDatasetUniquePtr bad_ds(
      GDALDataset::Open(kBad128, GDAL_OF_RASTER | GDAL_OF_READONLY));
  EXPECT_EQ(nullptr, bad_ds);
  CPLErrorReset();
}

// Covers all photometric & ExtraSamples color interpretation paths in
// GTiffRasterBand constructor.
TEST_F(GTiffRasterBandTest, PhotometricAndExtraSamplesInterpretations) {
  // 1. Palette with 1 band (PaletteIndex).
  {
    constexpr char kFile[] = "/vsimem/gtiffrb_pal.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kFile); };
    CPLStringList opts;
    opts.AddNameValue("PHOTOMETRIC", "PALETTE");
    GDALDatasetUniquePtr ds(
        driver_->Create(kFile, 4, 4, 1, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GCI_PaletteIndex, ds->GetRasterBand(1)->GetColorInterpretation());
  }

  // 2. RGB + premultiplied alpha + unspecified 5th band.
  {
    constexpr char kFile[] = "/vsimem/gtiffrb_rgba_premult.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kFile); };
    {
      CPLStringList opts;
      opts.AddNameValue("PHOTOMETRIC", "RGB");
      opts.AddNameValue("ALPHA", "PREMULTIPLIED");
      GDALDatasetUniquePtr ds(
          driver_->Create(kFile, 4, 4, 5, GDT_Byte, opts.List()));
      ASSERT_NE(nullptr, ds);
    }

    GDALDatasetUniquePtr rds(
        GDALDataset::Open(kFile, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds);
    EXPECT_EQ(GCI_RedBand, rds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_GreenBand, rds->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_BlueBand, rds->GetRasterBand(3)->GetColorInterpretation());
    EXPECT_EQ(GCI_AlphaBand, rds->GetRasterBand(4)->GetColorInterpretation());
    EXPECT_EQ(GCI_Undefined, rds->GetRasterBand(5)->GetColorInterpretation());
  }

  // 3. YCbCr + JPEG both with CONVERT_YCBCR_TO_RGB=YES and =NO.
  {
    constexpr char kFile[] = "/vsimem/gtiffrb_ycbcr.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kFile); };
    {
      CPLStringList opts;
      opts.AddNameValue("COMPRESS", "JPEG");
      opts.AddNameValue("PHOTOMETRIC", "YCBCR");
      GDALDatasetUniquePtr ds(
          driver_->Create(kFile, 16, 16, 3, GDT_Byte, opts.List()));
      ASSERT_NE(nullptr, ds);
      std::vector<uint8_t> buf(16 * 16 * 3, 128);
      ASSERT_EQ(CE_None,
                ds->RasterIO(GF_Write, 0, 0, 16, 16, buf.data(), 16, 16,
                             GDT_Byte, 3, nullptr, 0, 0, 0, nullptr));
    }

    {
      GDALDatasetUniquePtr rds_rgb(
          GDALDataset::Open(kFile, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, rds_rgb);
      EXPECT_EQ(GCI_RedBand,
                rds_rgb->GetRasterBand(1)->GetColorInterpretation());
    }

    CPLConfigOptionSetter no_conv("CONVERT_YCBCR_TO_RGB", "NO", false);
    GDALDatasetUniquePtr rds_ycbcr(
        GDALDataset::Open(kFile, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds_ycbcr);
    EXPECT_EQ(GCI_YCbCr_YBand,
              rds_ycbcr->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_YCbCr_CbBand,
              rds_ycbcr->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_YCbCr_CrBand,
              rds_ycbcr->GetRasterBand(3)->GetColorInterpretation());
  }

  // 4. 4-band raw YCbCr (1,1 subsampling, PLANARCONFIG_SEPARATE so libtiff
  // allows SamplesPerPixel == 4) -> hits YCbCr band 4 ExtraSamples.
  {
    constexpr char kFile[] = "/vsimem/gtiffrb_ycbcr4.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kFile); };
    {
      VSILFILE* fp = VSIFOpenL(kFile, "wb+");
      ASSERT_NE(nullptr, fp);
      absl::Cleanup close_fp = [&]() {
        if (fp != nullptr) VSIFCloseL(fp);
      };
      TIFF* tif = VSI_TIFFOpen(kFile, "w+", fp);
      ASSERT_NE(nullptr, tif);
      absl::Cleanup close_tif = [&]() {
        if (tif != nullptr) TIFFClose(tif);
      };
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
        ASSERT_GT(TIFFWriteEncodedStrip(tif, s, zeros.data(), zeros.size()), 0);
      }
      TIFFWriteDirectory(tif);
      TIFFClose(tif);
      tif = nullptr;
      EXPECT_EQ(0, VSIFCloseL(fp));
      fp = nullptr;
    }

    GDALDatasetUniquePtr rds(
        GDALDataset::Open("GTIFF_RAW:/vsimem/gtiffrb_ycbcr4.tif",
                          GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds);
    EXPECT_EQ(GCI_AlphaBand, rds->GetRasterBand(4)->GetColorInterpretation());
  }

  // 5. CMYK (PHOTOMETRIC_SEPARATED) with 5 bands via Create + GTIFF_RAW:.
  {
    constexpr char kFile[] = "/vsimem/gtiffrb_cmyk5.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kFile); };
    CPLStringList opts;
    opts.AddNameValue("PHOTOMETRIC", "CMYK");
    GDALDatasetUniquePtr ds(
        driver_->Create(kFile, 4, 4, 5, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(GCI_CyanBand, ds->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_MagentaBand, ds->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_YellowBand, ds->GetRasterBand(3)->GetColorInterpretation());
    EXPECT_EQ(GCI_BlackBand, ds->GetRasterBand(4)->GetColorInterpretation());
    EXPECT_EQ(GCI_Undefined, ds->GetRasterBand(5)->GetColorInterpretation());
  }

  // 6. Wrong number of ExtraSamples warning + missing ExtraSamples tag.
  {
    constexpr char kWrongExtra[] = "/vsimem/gtiffrb_wrong_extra.tif";
    constexpr char kNoExtra[] = "/vsimem/gtiffrb_no_extra.tif";
    absl::Cleanup c = [&]() {
      VSIUnlink(kWrongExtra);
      VSIUnlink(kNoExtra);
    };

    // 4 samples, MINISBLACK (expects 1 base sample), only 1 ExtraSample.
    {
      {
        VSILFILE* fp = VSIFOpenL(kWrongExtra, "wb+");
        ASSERT_NE(nullptr, fp);
        absl::Cleanup close_fp = [&]() {
          if (fp != nullptr) VSIFCloseL(fp);
        };
        TIFF* tif = VSI_TIFFOpen(kWrongExtra, "w+", fp);
        ASSERT_NE(nullptr, tif);
        absl::Cleanup close_tif = [&]() {
          if (tif != nullptr) TIFFClose(tif);
        };
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
        ASSERT_GT(TIFFWriteEncodedStrip(tif, 0, zeros.data(), zeros.size()), 0);
        TIFFWriteDirectory(tif);
        TIFFClose(tif);
        tif = nullptr;
        EXPECT_EQ(0, VSIFCloseL(fp));
        fp = nullptr;
      }

      static_assert(std::endian::native == std::endian::little);
      vsi_l_offset sz = 0;
      GByte* raw = VSIGetMemFileBuffer(kWrongExtra, &sz, FALSE);
      ASSERT_NE(nullptr, raw);
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

      std::vector<std::string> warnings;
      auto capture = [](CPLErr err_class, CPLErrorNum /*err_no*/,
                        const char* msg) {
        if (err_class == CE_Warning) {
          static_cast<std::vector<std::string>*>(CPLGetErrorHandlerUserData())
              ->emplace_back(msg != nullptr ? msg : "");
        }
      };
      CPLPushErrorHandlerEx(capture, &warnings);
      CPLErrorReset();
      GDALDatasetUniquePtr rds(
          GDALDataset::Open(kWrongExtra, GDAL_OF_RASTER | GDAL_OF_READONLY));
      CPLPopErrorHandler();
      CPLErrorReset();
      EXPECT_THAT(
          warnings,
          ::testing::Contains(::testing::AnyOf(
              HasSubstr("Wrong number of ExtraSamples : 1. 3 were expected"),
              HasSubstr("Sum of Photometric type-related color channels and "
                        "ExtraSamples"))));
      ASSERT_NE(nullptr, rds);
      EXPECT_EQ(GCI_GrayIndex, rds->GetRasterBand(1)->GetColorInterpretation());
      EXPECT_EQ(4, rds->GetRasterCount());
      CPLErrorReset();
    }

    // 2 samples, MINISBLACK, no TIFFTAG_EXTRASAMPLES at all.
    {
      {
        VSILFILE* fp = VSIFOpenL(kNoExtra, "wb+");
        ASSERT_NE(nullptr, fp);
        absl::Cleanup close_fp = [&]() {
          if (fp != nullptr) VSIFCloseL(fp);
        };
        TIFF* tif = VSI_TIFFOpen(kNoExtra, "w+", fp);
        ASSERT_NE(nullptr, tif);
        absl::Cleanup close_tif = [&]() {
          if (tif != nullptr) TIFFClose(tif);
        };
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 4);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 4);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 2);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 4);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        std::vector<uint8_t> zeros(4 * 4 * 2, 0);
        ASSERT_GT(TIFFWriteEncodedStrip(tif, 0, zeros.data(), zeros.size()), 0);
        TIFFWriteDirectory(tif);
        TIFFClose(tif);
        tif = nullptr;
        EXPECT_EQ(0, VSIFCloseL(fp));
        fp = nullptr;
      }

      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetUniquePtr rds(
          GDALDataset::Open(kNoExtra, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, rds);
      EXPECT_EQ(GCI_Undefined, rds->GetRasterBand(2)->GetColorInterpretation());
      CPLErrorReset();
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
    CPLStringList opts;
    opts.AddNameValue("TILED", "YES");
    opts.AddNameValue("BLOCKXSIZE", "16");
    opts.AddNameValue("BLOCKYSIZE", "16");
    GDALDatasetUniquePtr ds(
        driver_->Create(kUncomp, 64, 64, 1, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);
    std::vector<uint8_t> data(64 * 64, 77);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 64, 64,
                                                      data.data(), 64, 64,
                                                      GDT_Byte, 0, 0, nullptr));
    int ovr = 2;
    ASSERT_EQ(CE_None, ds->BuildOverviews("NEAREST", 1, &ovr, 0, nullptr,
                                          nullptr, nullptr));
  }

  // 1. Overview downsampled read (NearestNeighbour hits bTried==TRUE return).
  {
    GDALDatasetUniquePtr rds(
        GDALDataset::Open(kUncomp, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds);
    std::vector<uint8_t> out(32 * 32, 0);
    EXPECT_EQ(CE_None,
              rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64, out.data(),
                                              32, 32, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(77, out[0]);

    // Bilinear upsampling skips overview, hits GDALPamRasterBand::IRasterIO
    // non-nearest-neighbour branch.
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.eResampleAlg = GRIORA_Bilinear;
    std::vector<uint8_t> up(32 * 32, 0);
    EXPECT_EQ(CE_None,
              rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 16, 16, up.data(),
                                              32, 32, GDT_Byte, 0, 0, &extra));
    EXPECT_TRUE(AllEqual(up, 77));
  }

  // 2. VirtualMemIO success & fallback.
  {
    CPLConfigOptionSetter vmem("GTIFF_VIRTUAL_MEM_IO", "YES", false);
    GDALDatasetUniquePtr rds(
        GDALDataset::Open(kUncomp, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds);
    std::vector<uint8_t> out(64 * 64, 0);
    EXPECT_EQ(CE_None,
              rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64, out.data(),
                                              64, 64, GDT_Byte, 0, 0, nullptr));
    EXPECT_TRUE(AllEqual(out, 77));

    // Fallback (Bilinear returns -1 from VirtualMemIO).
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.eResampleAlg = GRIORA_Bilinear;
    std::vector<uint8_t> half(32 * 64, 0);
    EXPECT_EQ(CE_None, rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64,
                                                       half.data(), 32, 64,
                                                       GDT_Byte, 0, 0, &extra));
    EXPECT_TRUE(AllEqual(half, 77));
  }

  // 3. DirectIO success & fallback.
  {
    CPLConfigOptionSetter dio("GTIFF_DIRECT_IO", "YES", false);
    GDALDatasetUniquePtr rds(
        GDALDataset::Open(kUncomp, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds);
    std::vector<uint8_t> out(64 * 64, 0);
    EXPECT_EQ(CE_None,
              rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64, out.data(),
                                              64, 64, GDT_Byte, 0, 0, nullptr));
    EXPECT_TRUE(AllEqual(out, 77));

    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.eResampleAlg = GRIORA_Bilinear;
    std::vector<uint8_t> half(32 * 64, 0);
    EXPECT_EQ(CE_None, rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64,
                                                       half.data(), 32, 64,
                                                       GDT_Byte, 0, 0, &extra));
    EXPECT_TRUE(AllEqual(half, 77));
  }

  // 4. MultiThreadedRead + HasOptimizedReadMultiRange (with & without threads).
  {
    CPLConfigOptionSetter multi_range("GTIFF_HAS_OPTIMIZED_READ_MULTI_RANGE",
                                      "YES", false);
    // Single-threaded -> CacheMultiRange + BufferedDataFreer cleanup.
    {
      GDALDatasetUniquePtr rds(
          GDALDataset::Open(kUncomp, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, rds);
      std::vector<uint8_t> out(64 * 64, 0);
      EXPECT_EQ(CE_None, rds->GetRasterBand(1)->RasterIO(
                             GF_Read, 0, 0, 64, 64, out.data(), 64, 64,
                             GDT_Byte, 0, 0, nullptr));
      EXPECT_TRUE(AllEqual(out, 77));
    }

    // Multi-threaded -> HasPRead() true + MultiThreadedRead; also 1-block read.
    {
      CPLStringList open_opts;
      open_opts.SetNameValue("NUM_THREADS", "2");
      GDALDatasetUniquePtr rds(
          GDALDataset::Open(kUncomp, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr,
                            open_opts.List(), nullptr));
      ASSERT_NE(nullptr, rds);
      std::vector<uint8_t> out(64 * 64, 0);
      EXPECT_EQ(CE_None, rds->GetRasterBand(1)->RasterIO(
                             GF_Read, 0, 0, 64, 64, out.data(), 64, 64,
                             GDT_Byte, 0, 0, nullptr));
      EXPECT_TRUE(AllEqual(out, 77));
      std::vector<uint8_t> one_tile(16 * 16, 0);
      EXPECT_EQ(CE_None, rds->GetRasterBand(1)->RasterIO(
                             GF_Read, 0, 0, 16, 16, one_tile.data(), 16, 16,
                             GDT_Byte, 0, 0, nullptr));
      EXPECT_TRUE(AllEqual(one_tile, 77));
    }
  }

  // 5. COG-style interleaved mask with HasOptimizedReadMultiRange.
  {
    {
      CPLConfigOptionSetter imask("GDAL_TIFF_INTERNAL_MASK", "YES", false);
      GDALDatasetUniquePtr src(
          driver_->Create(kSrcMask, 64, 64, 1, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, src);
      ASSERT_EQ(CE_None, src->CreateMaskBand(GMF_PER_DATASET));
      std::vector<uint8_t> ones(64 * 64, 255);
      ASSERT_EQ(CE_None, src->GetRasterBand(1)->GetMaskBand()->RasterIO(
                             GF_Write, 0, 0, 64, 64, ones.data(), 64, 64,
                             GDT_Byte, 0, 0, nullptr));

      CPLStringList copts;
      copts.AddNameValue("TILED", "YES");
      copts.AddNameValue("BLOCKXSIZE", "16");
      copts.AddNameValue("BLOCKYSIZE", "16");
      copts.AddNameValue("COMPRESS", "LZW");
      copts.AddNameValue("COPY_SRC_OVERVIEWS", "YES");
      GDALDatasetUniquePtr cog(driver_->CreateCopy(
          kCogMask, src.get(), FALSE, copts.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, cog);
    }

    CPLConfigOptionSetter multi_range("GTIFF_HAS_OPTIMIZED_READ_MULTI_RANGE",
                                      "YES", false);
    GDALDatasetUniquePtr rds(
        GDALDataset::Open(kCogMask, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds);
    GDALRasterBand* mask_band = rds->GetRasterBand(1)->GetMaskBand();
    ASSERT_NE(nullptr, mask_band);
    std::vector<uint8_t> mask_buf(64 * 64, 0);
    EXPECT_EQ(CE_None,
              mask_band->RasterIO(GF_Read, 0, 0, 64, 64, mask_buf.data(), 64,
                                  64, GDT_Byte, 0, 0, nullptr));
    EXPECT_TRUE(AllEqual(mask_buf, 255));
  }

  // 6. Aggressive band caching disable when required mem > GDALGetCacheMax64().
  {
    const GIntBig old_cache = GDALGetCacheMax64();
    absl::Cleanup restore_cache = [&]() { GDALSetCacheMax64(old_cache); };
    GDALSetCacheMax64(1024);

    {
      GDALDatasetUniquePtr ds(
          driver_->Create(kRgb3, 64, 64, 3, GDT_Byte, nullptr));
      ASSERT_NE(nullptr, ds);
    }

    GDALDatasetUniquePtr rds(
        GDALDataset::Open(kRgb3, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, rds);
    std::vector<uint8_t> out(64 * 64, 0);
    // Call twice: 1st sets m_bHasWarnedDisableAggressiveBandCaching, 2nd skips
    // the debug-log branch.
    EXPECT_EQ(CE_None,
              rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64, out.data(),
                                              64, 64, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_None,
              rds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 64, 64, out.data(),
                                              64, 64, GDT_Byte, 0, 0, nullptr));
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
  constexpr char kCappedLim[] = "/vsimem/gtiffrb_capped.tif||maxlength=1024";
  absl::Cleanup cleanup = [&]() {
    VSIUnlink(kDontWrite);
    VSIUnlink(kSingleBlk);
    VSIUnlink(kPartialEdge);
    VSIUnlink(kCappedLim);
    VSIUnlink(kCapped);
  };

  // 1. GTIFF_DONT_WRITE_BLOCKS=YES.
  {
    CPLConfigOptionSetter dont_write("GTIFF_DONT_WRITE_BLOCKS", "YES", false);
    GDALDatasetUniquePtr ds(
        driver_->Create(kDontWrite, 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    std::vector<uint8_t> buf(16 * 16, 9);
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 16, 16, buf.data(),
                                             16, 16, GDT_Byte, 0, 0, nullptr));
  }

  // 2. Single full block write + PLANARCONFIG_SEPARATE Band 2 ComputeBlockId +
  // GDT_UInt16 -> GDT_Byte buffer conversion.
  {
    CPLStringList opts;
    opts.AddNameValue("TILED", "YES");
    opts.AddNameValue("BLOCKXSIZE", "16");
    opts.AddNameValue("BLOCKYSIZE", "16");
    opts.AddNameValue("INTERLEAVE", "BAND");
    GDALDatasetUniquePtr ds(
        driver_->Create(kSingleBlk, 32, 32, 2, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);

    std::vector<uint8_t> blk(16 * 16, 33);
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 16, 16, blk.data(),
                                             16, 16, GDT_Byte, 0, 0, nullptr));

    // Same whole-block path with eBufType != eDataType (hits LoadBlockBuf +
    // GDALCopyWords64).
    std::vector<uint16_t> blk16(16 * 16, 44);
    EXPECT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 16, 0, 16, 16, blk16.data(), 16, 16,
                           GDT_UInt16, 0, 0, nullptr));
  }

  // 3. Multi-block 20x20 with 16x16 tiles -> hits full tile (0,0) AND partial
  // edge tiles (1,0), (0,1), (1,1) with memset zero-padding; plus unaligned
  // fallback write.
  {
    CPLStringList opts;
    opts.AddNameValue("TILED", "YES");
    opts.AddNameValue("BLOCKXSIZE", "16");
    opts.AddNameValue("BLOCKYSIZE", "16");
    GDALDatasetUniquePtr ds(
        driver_->Create(kPartialEdge, 20, 20, 1, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);

    std::vector<uint8_t> img(20 * 20, 55);
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 20, 20, img.data(),
                                             20, 20, GDT_Byte, 0, 0, nullptr));

    // Sub-block unaligned write -> falls through to
    // GDALPamRasterBand::IRasterIO.
    std::array<uint8_t, 4> sub = {1, 2, 3, 4};
    EXPECT_EQ(CE_None,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 1, 1, 2, 2, sub.data(),
                                             2, 2, GDT_Byte, 0, 0, nullptr));
  }

  // 4. WriteEncodedTileOrStrip failure via /vsimem/ maxlength cap.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    CPLStringList opts;
    opts.AddNameValue("TILED", "YES");
    opts.AddNameValue("BLOCKXSIZE", "16");
    opts.AddNameValue("BLOCKYSIZE", "16");
    GDALDatasetUniquePtr ds(
        driver_->Create(kCappedLim, 64, 64, 1, GDT_Byte, opts.List()));
    ASSERT_NE(nullptr, ds);

    std::vector<uint8_t> big(64 * 64, 99);
    EXPECT_EQ(CE_Failure,
              ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 64, 64, big.data(),
                                             64, 64, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
