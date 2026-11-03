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
// Tests GeoTIFF internal/external overview generation, TIFF IFD directory
// writing, BigTIFF threshold checks, and codec options (gt_overview.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/gt_overview.cpp

#include "third_party/gdal/frmts/gtiff/gt_overview.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_cpp_functions.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

class GTOverviewTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_JPEG();
    GDALRegister_MEM();
    GDALRegister_WEBP();
    GTiffOneTimeInit();
    CPLErrorReset();

    mem_drv_ = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_drv_);
  }

  void TearDown() override { CPLErrorReset(); }

  GDALDriver* mem_drv_ = nullptr;
};

TEST_F(GTOverviewTest, WriteDirectoryAndBuildOverviewMetadataDirect) {
  constexpr char kFile[] = "/vsimem/gtovr_writedir.tif";
  absl::Cleanup clean_file = [&]() { VSIUnlink(kFile); };

  {
    VSILFILE* fp = VSIFOpenL(kFile, "wb+");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [fp]() { EXPECT_EQ(0, VSIFCloseL(fp)); };
    TIFF* tif = VSI_TIFFOpen(kFile, "w+", fp);
    ASSERT_NE(nullptr, tif);
    absl::Cleanup close_tif = [tif]() { TIFFClose(tif); };

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 16);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 16);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 16);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    std::vector<uint8_t> zeros(256, 0);
    TIFFWriteEncodedStrip(tif, 0, zeros.data(), zeros.size());
    TIFFWriteDirectory(tif);

    // 1. Striped 1-sample palette + metadata + nodata + defer strile.
    std::vector<unsigned short> cmap(256, 100);
    const toff_t off1 = GTIFFWriteDirectory(
        tif, FILETYPE_REDUCEDIMAGE, 16, 16, 8, PLANARCONFIG_CONTIG, 1, 16, 16,
        FALSE, COMPRESSION_ADOBE_DEFLATE, PHOTOMETRIC_PALETTE,
        SAMPLEFORMAT_UINT, PREDICTOR_HORIZONTAL, cmap.data(), cmap.data(),
        cmap.data(), 0, nullptr,
        "<GDALMetadata><Item name=\"A\">B</Item></GDALMetadata>", nullptr,
        nullptr, "-9999", nullptr, /*bDeferStrileArrayWriting=*/true);
    EXPECT_GT(off1, 0U);

    // 2. Tiled 3-sample YCbCr JPEG & 4-sample separate LERC with extrasamples.
    const toff_t off2 = GTIFFWriteDirectory(
        tif, FILETYPE_REDUCEDIMAGE, 16, 16, 8, PLANARCONFIG_CONTIG, 3, 16, 16,
        TRUE, COMPRESSION_JPEG, PHOTOMETRIC_YCBCR, SAMPLEFORMAT_UINT, 1,
        nullptr, nullptr, nullptr, 0, nullptr, nullptr, "80", "1", nullptr,
        nullptr, false);
    EXPECT_GT(off2, 0U);

    unsigned short extra = EXTRASAMPLE_UNASSALPHA;
    uint32_t lerc_params[2] = {LERC_VERSION_2_4, LERC_ADD_COMPRESSION_DEFLATE};
    const toff_t off3 = GTIFFWriteDirectory(
        tif, FILETYPE_REDUCEDIMAGE, 16, 16, 8, PLANARCONFIG_SEPARATE, 4, 16, 16,
        TRUE, COMPRESSION_LERC, PHOTOMETRIC_RGB, SAMPLEFORMAT_UINT, 1, nullptr,
        nullptr, nullptr, 1, &extra, "", nullptr, nullptr, nullptr, lerc_params,
        false);
    EXPECT_GT(off3, 0U);
  }

  // 3. Read-only handle -> TIFFWriteCheck fails and returns 0.
  {
    VSILFILE* rfp = VSIFOpenL(kFile, "rb");
    ASSERT_NE(nullptr, rfp);
    absl::Cleanup close_rfp = [rfp]() { EXPECT_EQ(0, VSIFCloseL(rfp)); };
    TIFF* rtif = VSI_TIFFOpen(kFile, "r", rfp);
    ASSERT_NE(nullptr, rtif);
    absl::Cleanup close_rtif = [rtif]() { TIFFClose(rtif); };

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(0U, GTIFFWriteDirectory(rtif, FILETYPE_REDUCEDIMAGE, 16, 16, 8,
                                      PLANARCONFIG_CONTIG, 1, 16, 16, TRUE,
                                      COMPRESSION_NONE, PHOTOMETRIC_MINISBLACK,
                                      SAMPLEFORMAT_UINT, 1, nullptr, nullptr,
                                      nullptr, 0, nullptr, nullptr, nullptr,
                                      nullptr, nullptr, nullptr, false));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 4. GTIFFBuildOverviewMetadata all branches.
  {
    GDALDatasetUniquePtr mds(mem_drv_->Create("", 4, 4, 2, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mds);
    CPLString md;
    GTIFFBuildOverviewMetadata("NONE", mds.get(), false, md);
    EXPECT_TRUE(md.empty());

    GTIFFBuildOverviewMetadata("AVERAGE", mds.get(), true, md);
    EXPECT_THAT(static_cast<const std::string&>(md),
                HasSubstr("INTERNAL_MASK_FLAGS_1"));

    mds->SetMetadataItem("INTERNAL_MASK_FLAGS_1", "2");
    mds->SetMetadataItem("INTERNAL_MASK_FLAGS_2", "2");
    mds->SetMetadataItem("NODATA_VALUES", "0 0");
    GTIFFBuildOverviewMetadata("BILINEAR", mds.get(), false, md);
    EXPECT_THAT(static_cast<const std::string&>(md),
                HasSubstr("NODATA_VALUES"));
    EXPECT_THAT(static_cast<const std::string&>(md),
                HasSubstr("INTERNAL_MASK_FLAGS_2"));
  }
}

TEST_F(GTOverviewTest, AllDataTypesAndValidationErrors) {
  // 1. 0 bands / 0 overviews early return.
  constexpr int kOvr2 = 2;
  EXPECT_EQ(CE_None, GTIFFBuildOverviews("/vsimem/unused.ovr", 0, nullptr, 1,
                                         &kOvr2, "NEAREST", GDALDummyProgress,
                                         nullptr, nullptr));

  // 2. Loop all 16 valid GDALDataType values.
  constexpr auto kTypes = std::to_array<GDALDataType>({
      GDT_Byte,
      GDT_Int8,
      GDT_UInt16,
      GDT_Int16,
      GDT_UInt32,
      GDT_Int32,
      GDT_UInt64,
      GDT_Int64,
      GDT_Float16,
      GDT_Float32,
      GDT_Float64,
      GDT_CInt16,
      GDT_CInt32,
      GDT_CFloat16,
      GDT_CFloat32,
      GDT_CFloat64,
  });
  for (GDALDataType dt : kTypes) {
    constexpr char kOvr[] = "/vsimem/gtovr_dt.ovr";
    absl::Cleanup clean_ovr = [&]() { VSIUnlink(kOvr); };
    GDALDatasetUniquePtr mds(mem_drv_->Create("", 8, 8, 1, dt, nullptr));
    ASSERT_NE(nullptr, mds);
    GDALRasterBand* b = mds->GetRasterBand(1);
    const char* resamp =
        (dt == GDT_CInt16 || dt == GDT_Byte) ? "NEAREST" : "NONE";
    EXPECT_EQ(CE_None,
              GTIFFBuildOverviews(kOvr, 1, &b, 1, &kOvr2, resamp,
                                  GDALDummyProgress, nullptr, nullptr));
  }

  // 3. NBITS=1 with AVERAGE_BIT2GRAYSCALE promotes nBandBits to 8.
  {
    constexpr char kOvr[] = "/vsimem/gtovr_bit2gray.ovr";
    absl::Cleanup clean_ovr = [&]() { VSIUnlink(kOvr); };
    GDALDatasetUniquePtr mds(mem_drv_->Create("", 8, 8, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mds);
    mds->GetRasterBand(1)->SetMetadataItem("NBITS", "1", "IMAGE_STRUCTURE");
    GDALRasterBand* b = mds->GetRasterBand(1);
    EXPECT_EQ(CE_None, GTIFFBuildOverviews(
                           kOvr, 1, &b, 1, &kOvr2, "AVERAGE_BIT2GRAYSCALE",
                           GDALDummyProgress, nullptr, nullptr));
  }

  // 4. Validation errors: mixed band types, band-2 colortable, different band
  // sizes, JPEG >16-bit, JPEG on colortable, YCBCR on 1-band.
  {
    GDALDatasetUniquePtr ds_b(mem_drv_->Create("", 8, 8, 1, GDT_Byte, nullptr));
    GDALDatasetUniquePtr ds_u16(
        mem_drv_->Create("", 8, 8, 1, GDT_UInt16, nullptr));
    GDALDatasetUniquePtr ds_b2(
        mem_drv_->Create("", 8, 8, 1, GDT_Byte, nullptr));
    GDALDatasetUniquePtr ds_big(
        mem_drv_->Create("", 16, 16, 1, GDT_Byte, nullptr));
    GDALDatasetUniquePtr ds_f32(
        mem_drv_->Create("", 8, 8, 1, GDT_Float32, nullptr));
    ASSERT_NE(nullptr, ds_b);
    ASSERT_NE(nullptr, ds_u16);
    ASSERT_NE(nullptr, ds_b2);
    ASSERT_NE(nullptr, ds_big);
    ASSERT_NE(nullptr, ds_f32);

    GDALRasterBand* mixed[2] = {ds_b->GetRasterBand(1),
                                ds_u16->GetRasterBand(1)};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(
          CE_Failure,
          GTIFFBuildOverviews("/vsimem/err1.ovr", 2, mixed, 1, &kOvr2,
                              "NEAREST", GDALDummyProgress, nullptr, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }

    GDALColorTable ct;
    GDALColorEntry ce = {1, 2, 3, 255};
    ct.SetColorEntry(0, &ce);
    ds_b2->GetRasterBand(1)->SetColorTable(&ct);
    GDALRasterBand* ct_bands[2] = {ds_b->GetRasterBand(1),
                                   ds_b2->GetRasterBand(1)};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(
          CE_Failure,
          GTIFFBuildOverviews("/vsimem/err2.ovr", 2, ct_bands, 1, &kOvr2,
                              "NEAREST", GDALDummyProgress, nullptr, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }

    GDALRasterBand* diff_sz[2] = {ds_b->GetRasterBand(1),
                                  ds_big->GetRasterBand(1)};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(
          CE_Failure,
          GTIFFBuildOverviews("/vsimem/err3.ovr", 2, diff_sz, 1, &kOvr2,
                              "NEAREST", GDALDummyProgress, nullptr, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }

    CPLStringList jpeg_opts;
    jpeg_opts.SetNameValue("COMPRESS", "JPEG");
    GDALRasterBand* f32_b = ds_f32->GetRasterBand(1);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure,
                GTIFFBuildOverviews("/vsimem/err4.ovr", 1, &f32_b, 1, &kOvr2,
                                    "NEAREST", GDALDummyProgress, nullptr,
                                    jpeg_opts.List()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
    GDALRasterBand* ct_b = ds_b2->GetRasterBand(1);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure,
                GTIFFBuildOverviews("/vsimem/err5.ovr", 1, &ct_b, 1, &kOvr2,
                                    "NEAREST", GDALDummyProgress, nullptr,
                                    jpeg_opts.List()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }

    CPLStringList ycbcr_opts;
    ycbcr_opts.SetNameValue("COMPRESS", "JPEG");
    ycbcr_opts.SetNameValue("PHOTOMETRIC", "YCBCR");
    GDALRasterBand* b1 = ds_b->GetRasterBand(1);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(
          CE_Failure,
          GTIFFBuildOverviews("/vsimem/err6.ovr", 1, &b1, 1, &kOvr2, "NEAREST",
                              GDALDummyProgress, nullptr, ycbcr_opts.List()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
  }
}

TEST_F(GTOverviewTest, CodecsBigTiffColorInterpAndRegenerationPaths) {
  // 1. BigTIFF checks (`IF_NEEDED`, `BIGTIFF=NO` >4GB failure, tile array >2GB
  // failure, existing file reopen `r+` & corrupt `.ovr` failure).
  {
    GDALDatasetUniquePtr mds(
        mem_drv_->Create("", 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mds);
    GDALRasterBand* b = mds->GetRasterBand(1);
    constexpr std::pair<int, int> kHugeOvr = {70000, 70000};

    // BIGTIFF=NO -> fails before opening file.
    CPLStringList no_bt;
    no_bt.SetNameValue("BIGTIFF", "NO");
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure,
                GTIFFBuildOverviewsEx("/vsimem/bt_no.ovr", 1, &b, 1, nullptr,
                                      &kHugeOvr, "NONE", no_bt.List(),
                                      GDALDummyProgress, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }

    // BIGTIFF=IF_NEEDED with NONE resampling -> creates BigTIFF header only.
    {
      constexpr char kBt[] = "/vsimem/bt_needed.ovr";
      absl::Cleanup clean_bt = [&]() { VSIUnlink(kBt); };
      CPLStringList bt_opts;
      bt_opts.SetNameValue("BIGTIFF", "IF_NEEDED");
      EXPECT_EQ(CE_None, GTIFFBuildOverviewsEx(
                             kBt, 1, &b, 1, nullptr, &kHugeOvr, "NONE",
                             bt_opts.List(), GDALDummyProgress, nullptr));

      // Reopen existing BigTIFF .ovr (`r+` path).
      constexpr std::pair<int, int> kSmallOvr = {8, 8};
      EXPECT_EQ(CE_None, GTIFFBuildOverviewsEx(kBt, 1, &b, 1, nullptr,
                                               &kSmallOvr, "NONE", nullptr,
                                               GDALDummyProgress, nullptr));

      GDALDatasetUniquePtr ovr_ds(
          GDALDataset::Open(kBt, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ovr_ds);
      EXPECT_EQ(kHugeOvr.first, ovr_ds->GetRasterXSize());
      EXPECT_EQ(kHugeOvr.second, ovr_ds->GetRasterYSize());
      EXPECT_EQ(1, ovr_ds->GetRasterCount());
    }

    // Tile count > 2GB overflow: 1500000x1500000 with BLOCKSIZE=64.
    {
      constexpr char kTileOvf[] = "/vsimem/bt_tile_ovf.ovr";
      absl::Cleanup clean_ovf = [&]() { VSIUnlink(kTileOvf); };
      constexpr std::pair<int, int> kOvfOvr = {1500000, 1500000};
      CPLStringList topts;
      topts.SetNameValue("COMPRESS", "LZW");
      topts.SetNameValue("BLOCKSIZE", "64");
      topts.SetNameValue("BIGTIFF", "NO");
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure, GTIFFBuildOverviewsEx(
                                kTileOvf, 1, &b, 1, nullptr, &kOvfOvr, "NONE",
                                topts.List(), GDALDummyProgress, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }

    // Corrupt existing file -> VSI_TIFFOpen("r+") fails.
    {
      constexpr char kBad[] = "/vsimem/gtovr_corrupt_existing.ovr";
      VsiMemTempWrapper bad_file(kBad, "NOT_TIFF");
      constexpr int kOvr2 = 2;
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure,
                GTIFFBuildOverviews(kBad, 1, &b, 1, &kOvr2, "NEAREST",
                                    GDALDummyProgress, nullptr, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
  }

  // 2. Non-standard ColorInterp with empty osMetadata ("NONE") and non-empty
  // osMetadata ("NEAREST" / "AVERAGE"), plus Palette ColorTable (256 entries).
  {
    constexpr char kOvr1[] = "/vsimem/gtovr_ci_near.ovr";
    constexpr char kOvr2[] = "/vsimem/gtovr_ci_avg.ovr";
    constexpr char kOvrPal[] = "/vsimem/gtovr_pal.ovr";
    absl::Cleanup clean_ovrs = [&]() {
      VSIUnlink(kOvr1);
      VSIUnlink(kOvr2);
      VSIUnlink(kOvrPal);
    };

    GDALDatasetUniquePtr mds(
        mem_drv_->Create("", 16, 16, 2, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mds);
    mds->SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
    mds->GetRasterBand(1)->SetMetadataItem("COMPRESSION", "JPEG2000",
                                           "IMAGE_STRUCTURE");
    mds->GetRasterBand(1)->SetColorInterpretation(GCI_CyanBand);
    mds->GetRasterBand(2)->SetColorInterpretation(GCI_MagentaBand);
    mds->GetRasterBand(1)->SetNoDataValue(0.0);
    mds->GetRasterBand(2)->SetNoDataValue(0.0);
    GDALRasterBand* bands[2] = {mds->GetRasterBand(1), mds->GetRasterBand(2)};
    constexpr int kOvrs[2] = {16, 4};

    CPLStringList bad_interleave;
    bad_interleave.SetNameValue("INTERLEAVE", "BOGUS");
    bad_interleave.SetNameValue("COMPRESS", "LZW");
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_None, GTIFFBuildOverviews(kOvr1, 2, bands, 2, kOvrs, "NONE",
                                             GDALDummyProgress, nullptr,
                                             bad_interleave.List()));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_THAT(
          CPLGetLastErrorMsg(),
          ::testing::HasSubstr("INTERLEAVE_OVERVIEW=BOGUS unsupported"));
      CPLErrorReset();
    }
    VSIUnlink(kOvr1);
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_None, GTIFFBuildOverviews(kOvr1, 2, bands, 2, kOvrs,
                                             "NEAREST", GDALDummyProgress,
                                             nullptr, bad_interleave.List()));
      EXPECT_EQ(CE_None, CPLGetLastErrorType());
    }

    // BAND interleave uncompressed.
    CPLStringList band_opts;
    band_opts.SetNameValue("INTERLEAVE", "BAND");
    constexpr int kOvrsNormal[2] = {2, 4};
    EXPECT_EQ(CE_None, GTIFFBuildOverviews(kOvr2, 2, bands, 2, kOvrsNormal,
                                           "AVERAGE", GDALDummyProgress,
                                           nullptr, band_opts.List()));

    // Palette 1-band overview.
    GDALDatasetUniquePtr pds(
        mem_drv_->Create("", 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, pds);
    GDALColorTable ct;
    GDALColorEntry ce = {100, 150, 200, 255};
    ct.SetColorEntry(0, &ce);
    pds->GetRasterBand(1)->SetColorTable(&ct);
    GDALRasterBand* pb = pds->GetRasterBand(1);
    constexpr int kPalOvr = 2;
    EXPECT_EQ(CE_None,
              GTIFFBuildOverviews(kOvrPal, 1, &pb, 1, &kPalOvr, "NEAREST",
                                  GDALDummyProgress, nullptr, nullptr));
  }

  // 3. All codec option branches in GTIFFBuildOverviewsEx (JPEG, WEBP,
  // LERC_DEFLATE+ZLEVEL, LERC_ZSTD+ZSTD_LEVEL, LERC+MAX_Z_ERROR, ZSTD config,
  // JXL).
  {
    GDALDatasetUniquePtr rgb_ds(
        mem_drv_->Create("", 16, 16, 4, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, rgb_ds);
    rgb_ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    rgb_ds->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
    rgb_ds->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);
    rgb_ds->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
    GDALRasterBand* bands4[4] = {
        rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
        rgb_ds->GetRasterBand(3), rgb_ds->GetRasterBand(4)};
    constexpr std::pair<int, int> kOvrSz = {8, 8};

    auto run_codec = [&](const char* path, int nb, CSLConstList opts) {
      SCOPED_TRACE(path);
      absl::Cleanup clean_ovr = [path]() { VSIUnlink(path); };
      EXPECT_EQ(CE_None, GTIFFBuildOverviewsEx(path, nb, bands4, 1, nullptr,
                                               &kOvrSz, "AVERAGE", opts,
                                               GDALDummyProgress, nullptr));
      GDALDatasetUniquePtr ovr_ds(
          GDALDataset::Open(path, GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(nullptr, ovr_ds);
      EXPECT_EQ(kOvrSz.first, ovr_ds->GetRasterXSize());
      EXPECT_EQ(kOvrSz.second, ovr_ds->GetRasterYSize());
      EXPECT_EQ(nb, ovr_ds->GetRasterCount());
    };

    // JPEG (3 bands).
    CPLStringList jopts;
    jopts.SetNameValue("COMPRESS", "JPEG");
    jopts.SetNameValue("PHOTOMETRIC", "YCBCR");
    jopts.SetNameValue("INTERLEAVE", "PIXEL");
    jopts.SetNameValue("JPEG_QUALITY", "80");
    jopts.SetNameValue("JPEG_TABLESMODE", "1");
    ASSERT_NO_FATAL_FAILURE(
        run_codec("/vsimem/gtovr_jpeg.ovr", 3, jopts.List()));

    // WEBP (4 bands RGBA).
    CPLStringList wopts;
    wopts.SetNameValue("COMPRESS", "WEBP");
    wopts.SetNameValue("WEBP_LEVEL", "80");
    wopts.SetNameValue("WEBP_LOSSLESS", "NO");
    ASSERT_NO_FATAL_FAILURE(
        run_codec("/vsimem/gtovr_webp.ovr", 4, wopts.List()));

    // LERC_DEFLATE + ZLEVEL.
    CPLStringList ld_opts;
    ld_opts.SetNameValue("COMPRESS", "LERC_DEFLATE");
    ld_opts.SetNameValue("ZLEVEL", "6");
    ASSERT_NO_FATAL_FAILURE(
        run_codec("/vsimem/gtovr_lercd.ovr", 4, ld_opts.List()));

    // LERC_ZSTD + ZSTD_LEVEL.
    CPLStringList lz_opts;
    lz_opts.SetNameValue("COMPRESS", "LERC_ZSTD");
    lz_opts.SetNameValue("ZSTD_LEVEL", "5");
    ASSERT_NO_FATAL_FAILURE(
        run_codec("/vsimem/gtovr_lercz.ovr", 4, lz_opts.List()));

    // LERC + MAX_Z_ERROR.
    CPLStringList l_opts;
    l_opts.SetNameValue("COMPRESS", "LERC");
    l_opts.SetNameValue("MAX_Z_ERROR", "0.5");
    ASSERT_NO_FATAL_FAILURE(
        run_codec("/vsimem/gtovr_lerc.ovr", 4, l_opts.List()));

    // ZSTD + PREDICTOR via config option fallback.
    {
      CPLConfigOptionSetter c1("COMPRESS_OVERVIEW", "ZSTD",
                               /*bSetOnlyIfUndefined=*/false);
      CPLConfigOptionSetter c2("PREDICTOR_OVERVIEW", "2",
                               /*bSetOnlyIfUndefined=*/false);
      CPLConfigOptionSetter c3("ZSTD_LEVEL_OVERVIEW", "3",
                               /*bSetOnlyIfUndefined=*/false);
      ASSERT_NO_FATAL_FAILURE(run_codec("/vsimem/gtovr_zstd.ovr", 4, nullptr));
    }

    // JXL (4 bands RGBA).
    CPLStringList jxl_opts;
    jxl_opts.SetNameValue("COMPRESS", "JXL");
    jxl_opts.SetNameValue("JXL_LOSSLESS", "NO");
    jxl_opts.SetNameValue("JXL_EFFORT", "3");
    jxl_opts.SetNameValue("JXL_DISTANCE", "1.5");
    jxl_opts.SetNameValue("JXL_ALPHA_DISTANCE", "0.5");
    ASSERT_NO_FATAL_FAILURE(
        run_codec("/vsimem/gtovr_jxl.ovr", 4, jxl_opts.List()));
  }
}

}  // namespace
}  // namespace autotest2
