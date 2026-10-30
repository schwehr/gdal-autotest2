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

#include "third_party/gdal/frmts/gtiff/gt_overview.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_cpp_functions.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class GTOverviewTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    GTiffOneTimeInit();
    mem_drv_ = static_cast<GDALDriver*>(GDALGetDriverByName("MEM"));
    ASSERT_NE(mem_drv_, nullptr);
  }

  GDALDriver* mem_drv_ = nullptr;
};

TEST_F(GTOverviewTest, WriteDirectoryAndBuildOverviewMetadataDirect) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  constexpr char kFile[] = "/vsimem/gtovr_writedir.tif";
  absl::Cleanup c = [&]() { VSIUnlink(kFile); };

  VSILFILE* fp = VSIFOpenL(kFile, "wb+");
  ASSERT_NE(fp, nullptr);
  TIFF* tif = VSI_TIFFOpen(kFile, "w+", fp);
  ASSERT_NE(tif, nullptr);
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
  toff_t off1 = GTIFFWriteDirectory(
      tif, FILETYPE_REDUCEDIMAGE, 16, 16, 8, PLANARCONFIG_CONTIG, 1, 16, 16,
      FALSE, COMPRESSION_ADOBE_DEFLATE, PHOTOMETRIC_PALETTE, SAMPLEFORMAT_UINT,
      PREDICTOR_HORIZONTAL, cmap.data(), cmap.data(), cmap.data(), 0, nullptr,
      "<GDALMetadata><Item name=\"A\">B</Item></GDALMetadata>", nullptr,
      nullptr, "-9999", nullptr, /*bDeferStrileArrayWriting=*/true);
  EXPECT_GT(off1, 0U);

  // 2. Tiled 3-sample YCbCr JPEG + extrasamples (separate 4-sample LERC).
  toff_t off2 = GTIFFWriteDirectory(
      tif, FILETYPE_REDUCEDIMAGE, 16, 16, 8, PLANARCONFIG_CONTIG, 3, 16, 16,
      TRUE, COMPRESSION_JPEG, PHOTOMETRIC_YCBCR, SAMPLEFORMAT_UINT, 1, nullptr,
      nullptr, nullptr, 0, nullptr, nullptr, "80", "1", nullptr, nullptr,
      false);
  EXPECT_GT(off2, 0U);

  unsigned short extra = EXTRASAMPLE_UNASSALPHA;
  uint32_t lerc_params[2] = {LERC_VERSION_2_4, LERC_ADD_COMPRESSION_DEFLATE};
  toff_t off3 = GTIFFWriteDirectory(
      tif, FILETYPE_REDUCEDIMAGE, 16, 16, 8, PLANARCONFIG_SEPARATE, 4, 16, 16,
      TRUE, COMPRESSION_LERC, PHOTOMETRIC_RGB, SAMPLEFORMAT_UINT, 1, nullptr,
      nullptr, nullptr, 1, &extra, "", nullptr, nullptr, nullptr, lerc_params,
      false);
  EXPECT_GT(off3, 0U);

  TIFFClose(tif);
  VSIFCloseL(fp);

  // 3. Read-only handle -> TIFFWriteCheck fails and returns 0!
  VSILFILE* rfp = VSIFOpenL(kFile, "rb");
  ASSERT_NE(rfp, nullptr);
  TIFF* rtif = VSI_TIFFOpen(kFile, "r", rfp);
  ASSERT_NE(rtif, nullptr);
  EXPECT_EQ(GTIFFWriteDirectory(
                rtif, FILETYPE_REDUCEDIMAGE, 16, 16, 8, PLANARCONFIG_CONTIG, 1,
                16, 16, TRUE, COMPRESSION_NONE, PHOTOMETRIC_MINISBLACK,
                SAMPLEFORMAT_UINT, 1, nullptr, nullptr, nullptr, 0, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr, false),
            0U);
  TIFFClose(rtif);
  VSIFCloseL(rfp);

  // 4. GTIFFBuildOverviewMetadata all branches.
  GDALDataset* mds = mem_drv_->Create("", 4, 4, 2, GDT_Byte, nullptr);
  ASSERT_NE(mds, nullptr);
  CPLString md;
  // Empty result ("NONE" normalizes to "" and no mask/nodata items).
  GTIFFBuildOverviewMetadata("NONE", mds, false, md);
  EXPECT_TRUE(md.empty());

  // Mask band metadata.
  GTIFFBuildOverviewMetadata("AVERAGE", mds, true, md);
  EXPECT_NE(md.find("INTERNAL_MASK_FLAGS_1"), std::string::npos);

  // INTERNAL_MASK_FLAGS_* + NODATA_VALUES on base DS.
  mds->SetMetadataItem("INTERNAL_MASK_FLAGS_1", "2");
  mds->SetMetadataItem("INTERNAL_MASK_FLAGS_2", "2");
  mds->SetMetadataItem("NODATA_VALUES", "0 0");
  GTIFFBuildOverviewMetadata("BILINEAR", mds, false, md);
  EXPECT_NE(md.find("NODATA_VALUES"), std::string::npos);
  EXPECT_NE(md.find("INTERNAL_MASK_FLAGS_2"), std::string::npos);
  GDALClose(mds);
}

TEST_F(GTOverviewTest, AllDataTypesAndValidationErrors) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // 0 bands / 0 overviews early return.
  int ovr2 = 2;
  EXPECT_EQ(GTIFFBuildOverviews("/vsimem/unused.ovr", 0, nullptr, 1, &ovr2,
                                "NEAREST", GDALDummyProgress, nullptr, nullptr),
            CE_None);

  // Loop all 16 valid GDALDataType values (with "NONE" resampling for speed on
  // exotic types, plus real regeneration on representative types).
  const GDALDataType kTypes[] = {
      GDT_Byte,    GDT_Int8,     GDT_UInt16,   GDT_Int16,
      GDT_UInt32,  GDT_Int32,    GDT_UInt64,   GDT_Int64,
      GDT_Float16, GDT_Float32,  GDT_Float64,  GDT_CInt16,
      GDT_CInt32,  GDT_CFloat16, GDT_CFloat32, GDT_CFloat64,
  };
  for (GDALDataType dt : kTypes) {
    constexpr char kOvr[] = "/vsimem/gtovr_dt.ovr";
    absl::Cleanup c = [&]() { VSIUnlink(kOvr); };
    GDALDataset* mds = mem_drv_->Create("", 8, 8, 1, dt, nullptr);
    ASSERT_NE(mds, nullptr);
    GDALRasterBand* b = mds->GetRasterBand(1);
    const char* resamp =
        (dt == GDT_CInt16 || dt == GDT_Byte) ? "NEAREST" : "NONE";
    EXPECT_EQ(GTIFFBuildOverviews(kOvr, 1, &b, 1, &ovr2, resamp,
                                  GDALDummyProgress, nullptr, nullptr),
              CE_None);
    GDALClose(mds);
  }

  // NBITS=1 with AVERAGE_BIT2GRAYSCALE promotes nBandBits to 8.
  {
    constexpr char kOvr[] = "/vsimem/gtovr_bit2gray.ovr";
    absl::Cleanup c = [&]() { VSIUnlink(kOvr); };
    GDALDataset* mds = mem_drv_->Create("", 8, 8, 1, GDT_Byte, nullptr);
    mds->GetRasterBand(1)->SetMetadataItem("NBITS", "1", "IMAGE_STRUCTURE");
    GDALRasterBand* b = mds->GetRasterBand(1);
    EXPECT_EQ(
        GTIFFBuildOverviews(kOvr, 1, &b, 1, &ovr2, "AVERAGE_BIT2GRAYSCALE",
                            GDALDummyProgress, nullptr, nullptr),
        CE_None);
    GDALClose(mds);
  }

  // Validation errors: mixed band types, band-2 colortable, different band
  // sizes, JPEG >16-bit, JPEG on colortable, YCBCR on 1-band.
  {
    GDALDataset* ds_b = mem_drv_->Create("", 8, 8, 1, GDT_Byte, nullptr);
    GDALDataset* ds_u16 = mem_drv_->Create("", 8, 8, 1, GDT_UInt16, nullptr);
    GDALDataset* ds_b2 = mem_drv_->Create("", 8, 8, 1, GDT_Byte, nullptr);
    GDALDataset* ds_big = mem_drv_->Create("", 16, 16, 1, GDT_Byte, nullptr);
    GDALDataset* ds_f32 = mem_drv_->Create("", 8, 8, 1, GDT_Float32, nullptr);

    GDALRasterBand* mixed[2] = {ds_b->GetRasterBand(1),
                                ds_u16->GetRasterBand(1)};
    EXPECT_EQ(
        GTIFFBuildOverviews("/vsimem/err1.ovr", 2, mixed, 1, &ovr2, "NEAREST",
                            GDALDummyProgress, nullptr, nullptr),
        CE_Failure);

    GDALColorTable ct;
    GDALColorEntry ce = {1, 2, 3, 255};
    ct.SetColorEntry(0, &ce);
    ds_b2->GetRasterBand(1)->SetColorTable(&ct);
    GDALRasterBand* ct_bands[2] = {ds_b->GetRasterBand(1),
                                   ds_b2->GetRasterBand(1)};
    EXPECT_EQ(
        GTIFFBuildOverviews("/vsimem/err2.ovr", 2, ct_bands, 1, &ovr2,
                            "NEAREST", GDALDummyProgress, nullptr, nullptr),
        CE_Failure);

    GDALRasterBand* diff_sz[2] = {ds_b->GetRasterBand(1),
                                  ds_big->GetRasterBand(1)};
    EXPECT_EQ(
        GTIFFBuildOverviews("/vsimem/err3.ovr", 2, diff_sz, 1, &ovr2, "NEAREST",
                            GDALDummyProgress, nullptr, nullptr),
        CE_Failure);

    char** jpeg_opts = CSLSetNameValue(nullptr, "COMPRESS", "JPEG");
    GDALRasterBand* f32_b = ds_f32->GetRasterBand(1);
    EXPECT_EQ(
        GTIFFBuildOverviews("/vsimem/err4.ovr", 1, &f32_b, 1, &ovr2, "NEAREST",
                            GDALDummyProgress, nullptr, jpeg_opts),
        CE_Failure);
    GDALRasterBand* ct_b = ds_b2->GetRasterBand(1);
    EXPECT_EQ(
        GTIFFBuildOverviews("/vsimem/err5.ovr", 1, &ct_b, 1, &ovr2, "NEAREST",
                            GDALDummyProgress, nullptr, jpeg_opts),
        CE_Failure);
    CSLDestroy(jpeg_opts);

    char** ycbcr_opts = nullptr;
    ycbcr_opts = CSLSetNameValue(ycbcr_opts, "COMPRESS", "JPEG");
    ycbcr_opts = CSLSetNameValue(ycbcr_opts, "PHOTOMETRIC", "YCBCR");
    GDALRasterBand* b1 = ds_b->GetRasterBand(1);
    EXPECT_EQ(
        GTIFFBuildOverviews("/vsimem/err6.ovr", 1, &b1, 1, &ovr2, "NEAREST",
                            GDALDummyProgress, nullptr, ycbcr_opts),
        CE_Failure);
    CSLDestroy(ycbcr_opts);

    GDALClose(ds_b);
    GDALClose(ds_u16);
    GDALClose(ds_b2);
    GDALClose(ds_big);
    GDALClose(ds_f32);
  }
}

TEST_F(GTOverviewTest, CodecsBigTiffColorInterpAndRegenerationPaths) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // 1. BigTIFF checks (`IF_NEEDED`, `BIGTIFF=NO` >4GB failure, tile array >2GB
  // failure, existing file reopen `r+` & corrupt `.ovr` failure).
  {
    GDALDataset* mds = mem_drv_->Create("", 16, 16, 1, GDT_Byte, nullptr);
    GDALRasterBand* b = mds->GetRasterBand(1);
    std::pair<int, int> huge_ovr = {70000, 70000};  // 4.9 GB uncompressed

    // BIGTIFF=NO -> fails before opening file!
    char** no_bt = CSLSetNameValue(nullptr, "BIGTIFF", "NO");
    EXPECT_EQ(
        GTIFFBuildOverviewsEx("/vsimem/bt_no.ovr", 1, &b, 1, nullptr, &huge_ovr,
                              "NONE", no_bt, GDALDummyProgress, nullptr),
        CE_Failure);
    CSLDestroy(no_bt);

    // BIGTIFF=IF_NEEDED with NONE resampling -> creates BigTIFF header only!
    {
      constexpr char kBt[] = "/vsimem/bt_needed.ovr";
      absl::Cleanup c = [&]() { VSIUnlink(kBt); };
      char** bt_opts = CSLSetNameValue(nullptr, "BIGTIFF", "IF_NEEDED");
      EXPECT_EQ(GTIFFBuildOverviewsEx(kBt, 1, &b, 1, nullptr, &huge_ovr, "NONE",
                                      bt_opts, GDALDummyProgress, nullptr),
                CE_None);
      CSLDestroy(bt_opts);

      // Reopen existing BigTIFF .ovr (`r+` path at L727-738)!
      std::pair<int, int> small_ovr = {8, 8};
      EXPECT_EQ(
          GTIFFBuildOverviewsEx(kBt, 1, &b, 1, nullptr, &small_ovr, "NONE",
                                nullptr, GDALDummyProgress, nullptr),
          CE_None);

      // The .ovr must still open; its first IFD is the 70000x70000 1-band
      // overview written by the first call.
      GDALDatasetH ovr_ds = GDALOpen(kBt, GA_ReadOnly);
      ASSERT_NE(ovr_ds, nullptr);
      EXPECT_EQ(GDALGetRasterXSize(ovr_ds), huge_ovr.first);
      EXPECT_EQ(GDALGetRasterYSize(ovr_ds), huge_ovr.second);
      EXPECT_EQ(GDALGetRasterCount(ovr_ds), 1);
      GDALClose(ovr_ds);
    }

    // Tile count > 2GB overflow (L908-916): 1500000x1500000 with BLOCKSIZE=64
    // and COMPRESS=LZW (so uncompressed BigTIFF check doesn't trigger).
    {
      constexpr char kTileOvf[] = "/vsimem/bt_tile_ovf.ovr";
      absl::Cleanup c = [&]() { VSIUnlink(kTileOvf); };
      std::pair<int, int> ovf_ovr = {1500000, 1500000};
      char** topts = nullptr;
      topts = CSLSetNameValue(topts, "COMPRESS", "LZW");
      topts = CSLSetNameValue(topts, "BLOCKSIZE", "64");
      topts = CSLSetNameValue(topts, "BIGTIFF", "NO");
      EXPECT_EQ(
          GTIFFBuildOverviewsEx(kTileOvf, 1, &b, 1, nullptr, &ovf_ovr, "NONE",
                                topts, GDALDummyProgress, nullptr),
          CE_Failure);
      CSLDestroy(topts);
    }

    // Corrupt existing file -> VSI_TIFFOpen("r+") fails (L739-749).
    {
      constexpr char kBad[] = "/vsimem/gtovr_corrupt_existing.ovr";
      absl::Cleanup c = [&]() { VSIUnlink(kBad); };
      VSILFILE* fp = VSIFOpenL(kBad, "wb");
      const char junk[8] = {'N', 'O', 'T', '_', 'T', 'I', 'F', 'F'};
      VSIFWriteL(junk, 1, 8, fp);
      VSIFCloseL(fp);
      int ovr2 = 2;
      EXPECT_EQ(GTIFFBuildOverviews(kBad, 1, &b, 1, &ovr2, "NEAREST",
                                    GDALDummyProgress, nullptr, nullptr),
                CE_Failure);
    }
    GDALClose(mds);
  }

  // 2. Non-standard ColorInterp with empty osMetadata ("NEAREST") and non-empty
  // osMetadata ("AVERAGE"), plus Palette ColorTable (256 entries), plus
  // JPEG2000 + INTERLEAVE=PIXEL source metadata, plus 4-band RGB+Alpha+Extra.
  {
    constexpr char kOvr1[] = "/vsimem/gtovr_ci_near.ovr";
    constexpr char kOvr2[] = "/vsimem/gtovr_ci_avg.ovr";
    constexpr char kOvrPal[] = "/vsimem/gtovr_pal.ovr";
    absl::Cleanup c = [&]() {
      VSIUnlink(kOvr1);
      VSIUnlink(kOvr2);
      VSIUnlink(kOvrPal);
    };
    GDALDataset* mds = mem_drv_->Create("", 16, 16, 2, GDT_Byte, nullptr);
    mds->SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
    mds->GetRasterBand(1)->SetMetadataItem("COMPRESSION", "JPEG2000",
                                           "IMAGE_STRUCTURE");
    mds->GetRasterBand(1)->SetColorInterpretation(GCI_CyanBand);
    mds->GetRasterBand(2)->SetColorInterpretation(GCI_MagentaBand);
    mds->GetRasterBand(1)->SetNoDataValue(0.0);
    mds->GetRasterBand(2)->SetNoDataValue(0.0);
    GDALRasterBand* bands[2] = {mds->GetRasterBand(1), mds->GetRasterBand(2)};
    // Unsorted factors; factor 16 on a 16x16 source yields a degenerate 1x1
    // overview (16 / 16 == 1).
    int ovrs[2] = {16, 4};
    char** bad_interleave = CSLSetNameValue(nullptr, "INTERLEAVE", "BOGUS");
    bad_interleave = CSLSetNameValue(bad_interleave, "COMPRESS", "LZW");
    // First test NONE resampling with non-standard ColorInterp (hits empty
    // osMetadata -> "<GDALMetadata>" branch at L829-832!).
    EXPECT_EQ(GTIFFBuildOverviews(kOvr1, 2, bands, 2, ovrs, "NONE",
                                  GDALDummyProgress, nullptr, bad_interleave),
              CE_None);
    VSIUnlink(kOvr1);
    EXPECT_EQ(GTIFFBuildOverviews(kOvr1, 2, bands, 2, ovrs, "NEAREST",
                                  GDALDummyProgress, nullptr, bad_interleave),
              CE_None);
    CSLDestroy(bad_interleave);

    // Path 2 (GDALRegenerateOverviewsEx): BAND interleave uncompressed!
    char** band_opts = CSLSetNameValue(nullptr, "INTERLEAVE", "BAND");
    int ovrs_normal[2] = {2, 4};
    EXPECT_EQ(GTIFFBuildOverviews(kOvr2, 2, bands, 2, ovrs_normal, "AVERAGE",
                                  GDALDummyProgress, nullptr, band_opts),
              CE_None);
    CSLDestroy(band_opts);
    GDALClose(mds);

    // Palette 1-band overview.
    GDALDataset* pds = mem_drv_->Create("", 16, 16, 1, GDT_Byte, nullptr);
    GDALColorTable ct;
    GDALColorEntry ce = {100, 150, 200, 255};
    ct.SetColorEntry(0, &ce);
    pds->GetRasterBand(1)->SetColorTable(&ct);
    GDALRasterBand* pb = pds->GetRasterBand(1);
    int ovr2 = 2;
    EXPECT_EQ(GTIFFBuildOverviews(kOvrPal, 1, &pb, 1, &ovr2, "NEAREST",
                                  GDALDummyProgress, nullptr, nullptr),
              CE_None);
    GDALClose(pds);
  }

  // 3. All codec option branches in GTIFFBuildOverviewsEx (JPEG, WEBP,
  // LERC_DEFLATE+ZLEVEL, LERC_ZSTD+ZSTD_LEVEL, LERC+MAX_Z_ERROR, JXL).
  {
    GDALDataset* rgb_ds = mem_drv_->Create("", 16, 16, 4, GDT_Byte, nullptr);
    rgb_ds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    rgb_ds->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
    rgb_ds->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);
    rgb_ds->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
    GDALRasterBand* bands4[4] = {
        rgb_ds->GetRasterBand(1), rgb_ds->GetRasterBand(2),
        rgb_ds->GetRasterBand(3), rgb_ds->GetRasterBand(4)};
    std::pair<int, int> ovr_sz = {8, 8};

    auto run_codec = [&](const char* path, int nb, char** opts) {
      SCOPED_TRACE(path);
      absl::Cleanup c = [path]() { VSIUnlink(path); };
      EXPECT_EQ(
          GTIFFBuildOverviewsEx(path, nb, bands4, 1, nullptr, &ovr_sz,
                                "AVERAGE", opts, GDALDummyProgress, nullptr),
          CE_None);
      // The produced .ovr must be a readable GeoTIFF with the requested
      // overview geometry.
      GDALDatasetH ovr_ds = GDALOpen(path, GA_ReadOnly);
      ASSERT_NE(ovr_ds, nullptr);
      EXPECT_EQ(GDALGetRasterXSize(ovr_ds), ovr_sz.first);
      EXPECT_EQ(GDALGetRasterYSize(ovr_ds), ovr_sz.second);
      EXPECT_EQ(GDALGetRasterCount(ovr_ds), nb);
      GDALClose(ovr_ds);
    };

    // JPEG (3 bands).
    char** jopts = nullptr;
    jopts = CSLSetNameValue(jopts, "COMPRESS", "JPEG");
    jopts = CSLSetNameValue(jopts, "PHOTOMETRIC", "YCBCR");
    jopts = CSLSetNameValue(jopts, "INTERLEAVE", "PIXEL");
    jopts = CSLSetNameValue(jopts, "JPEG_QUALITY", "80");
    jopts = CSLSetNameValue(jopts, "JPEG_TABLESMODE", "1");
    run_codec("/vsimem/gtovr_jpeg.ovr", 3, jopts);
    CSLDestroy(jopts);

    // WEBP (4 bands RGBA).
    char** wopts = nullptr;
    wopts = CSLSetNameValue(wopts, "COMPRESS", "WEBP");
    wopts = CSLSetNameValue(wopts, "WEBP_LEVEL", "80");
    wopts = CSLSetNameValue(wopts, "WEBP_LOSSLESS", "NO");
    run_codec("/vsimem/gtovr_webp.ovr", 4, wopts);
    CSLDestroy(wopts);

    // LERC_DEFLATE + ZLEVEL + PREDICTOR (also test LERC_ZSTD + ZSTD_LEVEL, and
    // LERC + MAX_Z_ERROR).
    char** ld_opts = nullptr;
    ld_opts = CSLSetNameValue(ld_opts, "COMPRESS", "LERC_DEFLATE");
    ld_opts = CSLSetNameValue(ld_opts, "ZLEVEL", "6");
    run_codec("/vsimem/gtovr_lercd.ovr", 4, ld_opts);
    CSLDestroy(ld_opts);

    char** lz_opts = nullptr;
    lz_opts = CSLSetNameValue(lz_opts, "COMPRESS", "LERC_ZSTD");
    lz_opts = CSLSetNameValue(lz_opts, "ZSTD_LEVEL", "5");
    run_codec("/vsimem/gtovr_lercz.ovr", 4, lz_opts);
    CSLDestroy(lz_opts);

    char** l_opts = nullptr;
    l_opts = CSLSetNameValue(l_opts, "COMPRESS", "LERC");
    l_opts = CSLSetNameValue(l_opts, "MAX_Z_ERROR", "0.5");
    run_codec("/vsimem/gtovr_lerc.ovr", 4, l_opts);
    CSLDestroy(l_opts);

    // ZSTD + PREDICTOR via config option fallback (`GetOptionValue` config
    // path).
    {
      CPLConfigOptionSetter c1("COMPRESS_OVERVIEW", "ZSTD", false);
      CPLConfigOptionSetter c2("PREDICTOR_OVERVIEW", "2", false);
      CPLConfigOptionSetter c3("ZSTD_LEVEL_OVERVIEW", "3", false);
      run_codec("/vsimem/gtovr_zstd.ovr", 4, nullptr);
    }

    // JXL (4 bands RGBA).
    char** jxl_opts = nullptr;
    jxl_opts = CSLSetNameValue(jxl_opts, "COMPRESS", "JXL");
    jxl_opts = CSLSetNameValue(jxl_opts, "JXL_LOSSLESS", "NO");
    jxl_opts = CSLSetNameValue(jxl_opts, "JXL_EFFORT", "3");
    jxl_opts = CSLSetNameValue(jxl_opts, "JXL_DISTANCE", "1.5");
    jxl_opts = CSLSetNameValue(jxl_opts, "JXL_ALPHA_DISTANCE", "0.5");
    run_codec("/vsimem/gtovr_jxl.ovr", 4, jxl_opts);
    CSLDestroy(jxl_opts);

    GDALClose(rgb_ds);
  }
}

}  // namespace
}  // namespace autotest2
