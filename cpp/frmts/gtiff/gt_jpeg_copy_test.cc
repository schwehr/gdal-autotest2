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
// Tests lossless JPEG-to-GeoTIFF block copying, VRT/MEM source handling,
// partial MCU padding, and corrupted JPEG setjmp recovery (gt_jpeg_copy.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/gt_jpeg_copy.cpp

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::AnyOf;
using ::testing::HasSubstr;

class GTJpegCopyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_GTiff();
    GDALRegister_JPEG();
    GDALRegister_MEM();
    GDALRegister_VRT();
    CPLErrorReset();

    gtiff_drv_ = GetGDALDriverManager()->GetDriverByName("GTiff");
    jpeg_drv_ = GetGDALDriverManager()->GetDriverByName("JPEG");
    mem_drv_ = GetGDALDriverManager()->GetDriverByName("MEM");
    vrt_drv_ = GetGDALDriverManager()->GetDriverByName("VRT");
    ASSERT_NE(nullptr, gtiff_drv_);
    ASSERT_NE(nullptr, jpeg_drv_);
    ASSERT_NE(nullptr, mem_drv_);
    ASSERT_NE(nullptr, vrt_drv_);
  }

  void TearDown() override { CPLErrorReset(); }

  void CreateInMemoryJpeg(const char* jpg_path, int w, int h, int bands,
                          CSLConstList jpeg_opts = nullptr,
                          bool cmyk_interp = false) {
    GDALDatasetUniquePtr mds(
        mem_drv_->Create("", w, h, bands, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mds);
    for (int b = 1; b <= bands; ++b) {
      std::vector<uint8_t> buf(w * h);
      for (int i = 0; i < w * h; ++i) {
        buf[i] = static_cast<uint8_t>((i * 7 + b * 43) & 0xFF);
      }
      ASSERT_EQ(CE_None, mds->GetRasterBand(b)->RasterIO(
                             GF_Write, 0, 0, w, h, buf.data(), w, h, GDT_Byte,
                             0, 0, nullptr));
    }
    if (cmyk_interp && bands == 4) {
      mds->GetRasterBand(1)->SetColorInterpretation(GCI_CyanBand);
      mds->GetRasterBand(2)->SetColorInterpretation(GCI_MagentaBand);
      mds->GetRasterBand(3)->SetColorInterpretation(GCI_YellowBand);
      mds->GetRasterBand(4)->SetColorInterpretation(GCI_BlackBand);
    }
    GDALDatasetUniquePtr jds(jpeg_drv_->CreateCopy(
        jpg_path, mds.get(), FALSE, jpeg_opts, nullptr, nullptr));
    ASSERT_NE(nullptr, jds);
  }

  GDALDriver* gtiff_drv_ = nullptr;
  GDALDriver* jpeg_drv_ = nullptr;
  GDALDriver* mem_drv_ = nullptr;
  GDALDriver* vrt_drv_ = nullptr;
};

TEST_F(GTJpegCopyTest, CanCopyFromJPEGRejectionsAndUnderlyingVRT) {
  constexpr char kJpg[] = "/vsimem/jcopy_src3.jpg";
  constexpr char kOut[] = "/vsimem/jcopy_out.tif";
  absl::Cleanup clean_files = [&]() {
    VSIUnlink(kJpg);
    VSIUnlink(kOut);
  };
  ASSERT_NO_FATAL_FAILURE(CreateInMemoryJpeg(kJpg, 20, 20, 3));

  // 1. Driverless MEMDataset::Create (poSrcDS->GetDriver() == nullptr).
  {
    GDALDatasetUniquePtr raw_mem(
        MEMDataset::Create("", 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, raw_mem);
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "JPEG");
    GDALDatasetUniquePtr out(gtiff_drv_->CreateCopy(
        kOut, raw_mem.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, out);
    out.reset();
    VSIUnlink(kOut);
  }

  // 2. Empty VRT (GetSingleSimpleSource() == nullptr).
  {
    GDALDatasetUniquePtr empty_vrt(
        vrt_drv_->Create("", 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, empty_vrt);
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "JPEG");
    GDALDatasetUniquePtr out(gtiff_drv_->CreateCopy(
        kOut, empty_vrt.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, out);
    out.reset();
    VSIUnlink(kOut);
  }

  // 3. VRT wrapping single JPEG -> succeeds via GetUnderlyingDataset.
  {
    GDALDatasetUniquePtr jds(
        GDALDataset::Open(kJpg, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, jds);
    GDALDatasetUniquePtr vds(
        vrt_drv_->CreateCopy("", jds.get(), FALSE, nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, vds);

    // Without COMPRESS=JPEG -> CanCopyFromJPEG returns FALSE.
    {
      GDALDatasetUniquePtr out_none(gtiff_drv_->CreateCopy(
          kOut, vds.get(), FALSE, nullptr, nullptr, nullptr));
      ASSERT_NE(nullptr, out_none);
    }
    VSIUnlink(kOut);

    // With COMPRESS=JPEG and BLOCKYSIZE=20 (equals nYSize).
    {
      CPLStringList opts;
      opts.SetNameValue("COMPRESS", "JPEG");
      opts.SetNameValue("BLOCKYSIZE", "20");
      GDALDatasetUniquePtr out_jpg(gtiff_drv_->CreateCopy(
          kOut, vds.get(), FALSE, opts.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, out_jpg);
    }
    VSIUnlink(kOut);
    vds.reset();

    // Rejections on YCbCr 20x20 JPEG fall back to generic CreateCopy.
    auto check_fallback = [&](CSLConstList copts,
                              CPLErr expected_err = CE_None) {
      CPLErrorReset();
      std::optional<WithQuietHandler> quiet;
      if (expected_err != CE_None) {
        quiet.emplace();
      }
      GDALDatasetUniquePtr o(gtiff_drv_->CreateCopy(kOut, jds.get(), FALSE,
                                                    copts, nullptr, nullptr));
      EXPECT_EQ(expected_err, CPLGetLastErrorType());
      ASSERT_NE(nullptr, o);
      EXPECT_EQ(20, o->GetRasterXSize());
      EXPECT_EQ(20, o->GetRasterYSize());
      EXPECT_EQ(3, o->GetRasterCount());
      EXPECT_STREQ("JPEG",
                   o->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"));
      o.reset();
      VSIUnlink(kOut);
    };

    // Incompatible PHOTOMETRIC=RGB on YCbCr source falls back silently to
    // generic RGB JPEG CreateCopy.
    CPLStringList o1;
    o1.SetNameValue("COMPRESS", "JPEG");
    o1.SetNameValue("PHOTOMETRIC", "RGB");
    ASSERT_NO_FATAL_FAILURE(check_fallback(o1.List()));

    // Incompatible INTERLEAVE=BAND.
    CPLStringList o2;
    o2.SetNameValue("COMPRESS", "JPEG");
    o2.SetNameValue("INTERLEAVE", "BAND");
    ASSERT_NO_FATAL_FAILURE(check_fallback(o2.List()));

    // Explicit JPEG_QUALITY.
    CPLStringList o3;
    o3.SetNameValue("COMPRESS", "JPEG");
    o3.SetNameValue("JPEG_QUALITY", "85");
    ASSERT_NO_FATAL_FAILURE(check_fallback(o3.List()));

    // Non-MCU BLOCKYSIZE=10: rejected by both lossless and generic paths.
    CPLStringList o4;
    o4.SetNameValue("COMPRESS", "JPEG");
    o4.SetNameValue("BLOCKYSIZE", "10");
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetUniquePtr fail_bs(gtiff_drv_->CreateCopy(
          kOut, jds.get(), FALSE, o4.List(), nullptr, nullptr));
      EXPECT_EQ(nullptr, fail_bs);
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
    VSIUnlink(kOut);

    // COMPRESSION_REVERSIBILITY=LOSSLESS on source IMAGE_STRUCTURE metadata.
    jds->SetMetadataItem("COMPRESSION_REVERSIBILITY", "LOSSLESS",
                         "IMAGE_STRUCTURE");
    CPLStringList o5;
    o5.SetNameValue("COMPRESS", "JPEG");
    ASSERT_NO_FATAL_FAILURE(check_fallback(o5.List()));
  }
}

TEST_F(GTJpegCopyTest, PhotometricsGrayRGBCMYKAndTiledPartialMCU) {
  constexpr char kGray[] = "/vsimem/jcopy_gray.jpg";
  constexpr char kRgb[] = "/vsimem/jcopy_rgb.jpg";
  constexpr char kCmyk[] = "/vsimem/jcopy_cmyk.jpg";
  constexpr char kYcbcr36[] = "/vsimem/jcopy_ycbcr36.jpg";
  constexpr char kOut[] = "/vsimem/jcopy_photo_out.tif";
  absl::Cleanup clean_files = [&]() {
    VSIUnlink(kGray);
    VSIUnlink(kRgb);
    VSIUnlink(kCmyk);
    VSIUnlink(kYcbcr36);
    VSIUnlink(kOut);
  };

  // 1. 1-band Grayscale JPEG with JPEGMEM config set.
  ASSERT_NO_FATAL_FAILURE(CreateInMemoryJpeg(kGray, 24, 24, 1));
  {
    CPLConfigOptionSetter jmem("JPEGMEM", "10000000",
                               /*bSetOnlyIfUndefined=*/false);
    GDALDatasetUniquePtr jds(
        GDALDataset::Open(kGray, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, jds);
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "JPEG");
    opts.SetNameValue("PHOTOMETRIC", "MINISBLACK");
    opts.SetNameValue("BLOCKYSIZE", "8");
    GDALDatasetUniquePtr out(gtiff_drv_->CreateCopy(
        kOut, jds.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, out);
    out.reset();
    VSIUnlink(kOut);
  }

  // 2. 3-band RGB JPEG (JPEG_WRITE_RGB=YES -> nMCUSize == 8, PHOTOMETRIC=RGB).
  {
    {
      CPLConfigOptionSetter write_rgb("JPEG_WRITE_RGB", "YES",
                                      /*bSetOnlyIfUndefined=*/false);
      ASSERT_NO_FATAL_FAILURE(CreateInMemoryJpeg(kRgb, 16, 16, 3));
    }

    GDALDatasetUniquePtr jds(
        GDALDataset::Open(kRgb, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, jds);
    ASSERT_EQ(nullptr,
              jds->GetMetadataItem("SOURCE_COLOR_SPACE", "IMAGE_STRUCTURE"));
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "JPEG");
    opts.SetNameValue("PHOTOMETRIC", "RGB");
    GDALDatasetUniquePtr out(gtiff_drv_->CreateCopy(
        kOut, jds.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, out);
    EXPECT_STREQ("JPEG",
                 out->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"));
    EXPECT_EQ(GCI_RedBand, out->GetRasterBand(1)->GetColorInterpretation());
    EXPECT_EQ(GCI_GreenBand, out->GetRasterBand(2)->GetColorInterpretation());
    EXPECT_EQ(GCI_BlueBand, out->GetRasterBand(3)->GetColorInterpretation());
    out.reset();
    VSIUnlink(kOut);
  }

  // 3. 4-band CMYK JPEG (GDAL_JPEG_TO_RGB=NO -> 4 Cyan/Magenta/Yellow/Black
  // bands, pszPhotometric == nullptr auto-sets PHOTOMETRIC=CMYK).
  {
    ASSERT_NO_FATAL_FAILURE(
        CreateInMemoryJpeg(kCmyk, 16, 16, 4, nullptr, /*cmyk_interp=*/true));
    CPLConfigOptionSetter no_rgb("GDAL_JPEG_TO_RGB", "NO",
                                 /*bSetOnlyIfUndefined=*/false);
    GDALDatasetUniquePtr jds(
        GDALDataset::Open(kCmyk, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, jds);
    ASSERT_EQ(4, jds->GetRasterCount());
    CPLStringList opts;
    opts.SetNameValue("COMPRESS", "JPEG");
    GDALDatasetUniquePtr out(gtiff_drv_->CreateCopy(
        kOut, jds.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, out);
    out.reset();
    VSIUnlink(kOut);
  }

  // 4. 36x36 3-band YCbCr JPEG copied to TILED 32x32 TIFF: hits partial X & Y
  // DCT block zero-padding loops in GTIFF_CopyBlockFromJPEG.
  ASSERT_NO_FATAL_FAILURE(CreateInMemoryJpeg(kYcbcr36, 36, 36, 3));
  {
    GDALDatasetUniquePtr jds(
        GDALDataset::Open(kYcbcr36, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, jds);
    {
      CPLStringList opts;
      opts.SetNameValue("COMPRESS", "JPEG");
      opts.SetNameValue("PHOTOMETRIC", "YCBCR");
      opts.SetNameValue("INTERLEAVE", "PIXEL");
      opts.SetNameValue("TILED", "YES");
      opts.SetNameValue("BLOCKXSIZE", "32");
      opts.SetNameValue("BLOCKYSIZE", "32");
      GDALDatasetUniquePtr out(gtiff_drv_->CreateCopy(
          kOut, jds.get(), FALSE, opts.List(), nullptr, nullptr));
      ASSERT_NE(nullptr, out);

      for (int b = 1; b <= 3; ++b) {
        SCOPED_TRACE(b);
        std::vector<uint8_t> got(36 * 36);
        std::vector<uint8_t> want(36 * 36);
        ASSERT_EQ(CE_None, out->GetRasterBand(b)->RasterIO(
                               GF_Read, 0, 0, 36, 36, got.data(), 36, 36,
                               GDT_Byte, 0, 0, nullptr));
        ASSERT_EQ(CE_None, jds->GetRasterBand(b)->RasterIO(
                               GF_Read, 0, 0, 36, 36, want.data(), 36, 36,
                               GDT_Byte, 0, 0, nullptr));
        int max_diff = 0;
        long sum_diff = 0;
        for (size_t i = 0; i < got.size(); ++i) {
          const int d = std::abs(static_cast<int>(got[i]) - want[i]);
          max_diff = std::max(max_diff, d);
          sum_diff += d;
        }
        EXPECT_LE(max_diff, 96);
        EXPECT_LT(static_cast<double>(sum_diff) / got.size(), 4.0);
        // Interior of the first (full) tile is unaffected by padding.
        for (int y = 0; y < 16; ++y) {
          for (int x = 0; x < 16; ++x) {
            EXPECT_EQ(want[y * 36 + x], got[y * 36 + x])
                << "x=" << x << " y=" << y;
          }
        }
      }
    }
    VSIUnlink(kOut);

    // Stripped multi-strip (BLOCKYSIZE=16 on 36x36) + progress cancellation.
    {
      auto cancel_cb = [](double dfComplete, const char*, void*) -> int {
        return dfComplete < 0.4 ? 1 : 0;
      };
      CPLStringList sopts;
      sopts.SetNameValue("COMPRESS", "JPEG");
      sopts.SetNameValue("BLOCKYSIZE", "16");
      GDALDatasetUniquePtr canceled(gtiff_drv_->CreateCopy(
          kOut, jds.get(), FALSE, sopts.List(), cancel_cb, nullptr));
      EXPECT_EQ(nullptr, canceled);
    }
    VSIUnlink(kOut);

    // Raw write failure via ||maxlength on both stripped and tiled outputs.
    // VSI mem file manager registers paths with the ||maxlength= suffix as
    // distinct entries in the mem file map, so unlink kCapLim as well as
    // kCapReal.
    constexpr char kCapReal[] = "/vsimem/jcopy_cap.tif";
    constexpr char kCapLim[] = "/vsimem/jcopy_cap.tif||maxlength=700";
    absl::Cleanup clean_cap = [&]() {
      VSIUnlink(kCapLim);
      VSIUnlink(kCapReal);
    };
    {
      CPLStringList cap_sopts;
      cap_sopts.SetNameValue("COMPRESS", "JPEG");
      cap_sopts.SetNameValue("BLOCKYSIZE", "16");
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetUniquePtr fail_s(gtiff_drv_->CreateCopy(
          kCapLim, jds.get(), FALSE, cap_sopts.List(), nullptr, nullptr));
      EXPECT_EQ(nullptr, fail_s);
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
    VSIUnlink(kCapLim);
    VSIUnlink(kCapReal);

    {
      CPLStringList cap_topts;
      cap_topts.SetNameValue("COMPRESS", "JPEG");
      cap_topts.SetNameValue("TILED", "YES");
      cap_topts.SetNameValue("BLOCKXSIZE", "16");
      cap_topts.SetNameValue("BLOCKYSIZE", "16");
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetUniquePtr fail_t(gtiff_drv_->CreateCopy(
          kCapLim, jds.get(), FALSE, cap_topts.List(), nullptr, nullptr));
      EXPECT_EQ(nullptr, fail_t);
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
  }
}

TEST_F(GTJpegCopyTest, UnlinkedAndCorruptedJpegSetjmpErrorPaths) {
  constexpr char kUnlinked[] = "/vsimem/jcopy_unlinked.jpg";
  constexpr char kZeroed[] = "/vsimem/jcopy_zeroed.jpg";
  constexpr char kBadQuant[] = "/vsimem/jcopy_badquant.jpg";
  constexpr char kOddSamp[] = "/vsimem/jcopy_oddsamp.jpg";
  constexpr char kBadScan[] = "/vsimem/jcopy_badscan.jpg";
  constexpr char kOut[] = "/vsimem/jcopy_err_out.tif";
  absl::Cleanup clean_files = [&]() {
    VSIUnlink(kUnlinked);
    VSIUnlink(kZeroed);
    VSIUnlink(kBadQuant);
    VSIUnlink(kOddSamp);
    VSIUnlink(kBadScan);
    VSIUnlink(kOut);
  };

  CPLStringList opts;
  opts.SetNameValue("COMPRESS", "JPEG");

  // 1. Unlink JPEG after open -> VSIFOpenL fails in JPEG copy helpers;
  // generic re-encoding copy succeeds using already-open JPEG handle.
  ASSERT_NO_FATAL_FAILURE(CreateInMemoryJpeg(kUnlinked, 16, 16, 3));
  {
    GDALDatasetUniquePtr jds(
        GDALDataset::Open(kUnlinked, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, jds);
    VSIUnlink(kUnlinked);
    GDALDatasetUniquePtr out(gtiff_drv_->CreateCopy(
        kOut, jds.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(16, out->GetRasterXSize());
  }
  VSIUnlink(kOut);

  // 2. Zero-out JPEG header after open -> jpeg_read_header triggers
  // GTIFF_ErrorExitJPEG -> setjmp with bCallDestroyDecompress == true.
  ASSERT_NO_FATAL_FAILURE(CreateInMemoryJpeg(kZeroed, 16, 16, 3));
  {
    GDALDatasetUniquePtr jds(
        GDALDataset::Open(kZeroed, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, jds);
    vsi_l_offset sz = 0;
    GByte* buf = VSIGetMemFileBuffer(kZeroed, &sz, FALSE);
    ASSERT_NE(nullptr, buf);
    std::memset(buf, 0, sz);
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr fail(gtiff_drv_->CreateCopy(
        kOut, jds.get(), FALSE, opts.List(), nullptr, nullptr));
    EXPECT_EQ(nullptr, fail);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                AnyOf(HasSubstr("Not a valid JPEG file"),
                      HasSubstr("Improper call to JPEG library")));
  }
  VSIUnlink(kOut);

  auto find_sof0 = [](GByte* buf, vsi_l_offset sz) -> size_t {
    for (size_t i = 0; i + 15 < sz; ++i) {
      if (buf[i] == 0xFF && buf[i + 1] == 0xC0) return i;
    }
    return 0;
  };

  // 3. Patch SOF0 component 0 quant_tbl_no to nonexistent table 3 -> triggers
  // setjmp with both bCallDestroyCompress and bCallDestroyDecompress true.
  ASSERT_NO_FATAL_FAILURE(CreateInMemoryJpeg(kBadQuant, 16, 16, 3));
  {
    GDALDatasetUniquePtr jds(
        GDALDataset::Open(kBadQuant, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, jds);
    vsi_l_offset sz = 0;
    GByte* buf = VSIGetMemFileBuffer(kBadQuant, &sz, FALSE);
    ASSERT_NE(nullptr, buf);
    const size_t sof0 = find_sof0(buf, sz);
    ASSERT_GT(sof0, 0U);
    buf[sof0 + 12] = 3;
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr fail(gtiff_drv_->CreateCopy(
        kOut, jds.get(), FALSE, opts.List(), nullptr, nullptr));
    EXPECT_EQ(nullptr, fail);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("libjpeg:"));
  }
  VSIUnlink(kOut);

  // 4. Patch SOF0 component 0 sampling factor to 0x31 (h=3, v=1) -> hits the
  // unusual sampling-factors branch in GTIFF_CopyFromJPEG_WriteAdditionalTags
  // (gt_jpeg_copy.cpp:524), skipping TIFFTAG_YCBCRSUBSAMPLING, while still
  // copying DCT blocks into the 24x24 GTiff output.
  ASSERT_NO_FATAL_FAILURE(CreateInMemoryJpeg(kOddSamp, 24, 24, 3));
  {
    GDALDatasetUniquePtr jds(
        GDALDataset::Open(kOddSamp, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, jds);
    vsi_l_offset sz = 0;
    GByte* buf = VSIGetMemFileBuffer(kOddSamp, &sz, FALSE);
    ASSERT_NE(nullptr, buf);
    const size_t sof0 = find_sof0(buf, sz);
    ASSERT_GT(sof0, 0U);
    buf[sof0 + 11] = 0x31;
    GDALDatasetUniquePtr out(gtiff_drv_->CreateCopy(
        kOut, jds.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(24, out->GetRasterXSize());
    EXPECT_EQ(3, out->GetRasterCount());
  }
  VSIUnlink(kOut);

  // 5. Corrupt SOS scan data after header -> jpeg_read_header succeeds while
  // jpeg_read_coefficients fails, falling back to generic copy.
  ASSERT_NO_FATAL_FAILURE(CreateInMemoryJpeg(kBadScan, 16, 16, 3));
  {
    GDALDatasetUniquePtr jds(
        GDALDataset::Open(kBadScan, GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(nullptr, jds);
    vsi_l_offset sz = 0;
    GByte* buf = VSIGetMemFileBuffer(kBadScan, &sz, FALSE);
    ASSERT_NE(nullptr, buf);
    for (size_t i = 2; i + 4 < sz; ++i) {
      if (buf[i] == 0xFF && buf[i + 1] == 0xDA) {
        const size_t hdr_len =
            (static_cast<size_t>(buf[i + 2]) << 8) | buf[i + 3];
        if (i + 2 + hdr_len + 2 < sz) {
          buf[i + 2 + hdr_len] = 0xFF;
          buf[i + 2 + hdr_len + 1] = 0xC2;
        }
        break;
      }
    }
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr out(gtiff_drv_->CreateCopy(
        kOut, jds.get(), FALSE, opts.List(), nullptr, nullptr));
    ASSERT_NE(nullptr, out);
    EXPECT_EQ(16, out->GetRasterXSize());
  }
}

// TODO: b/335317901 - Cover remaining defensive libjpeg I/O failure branches
// and progressive/arithmetic SOF rejection in gt_jpeg_copy.cpp (~92.5%
// aggregate coverage today).
}  // namespace
}  // namespace autotest2
