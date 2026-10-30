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

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class GTJpegCopyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALAllRegister();
    GTiffOneTimeInit();
    gtiff_drv_ = static_cast<GDALDriver*>(GDALGetDriverByName("GTiff"));
    jpeg_drv_ = static_cast<GDALDriver*>(GDALGetDriverByName("JPEG"));
    mem_drv_ = static_cast<GDALDriver*>(GDALGetDriverByName("MEM"));
    vrt_drv_ = static_cast<GDALDriver*>(GDALGetDriverByName("VRT"));
    ASSERT_NE(gtiff_drv_, nullptr);
    ASSERT_NE(jpeg_drv_, nullptr);
    ASSERT_NE(mem_drv_, nullptr);
    ASSERT_NE(vrt_drv_, nullptr);
  }

  void CreateInMemoryJpeg(const char* jpg_path, int w, int h, int bands,
                          char** jpeg_opts = nullptr,
                          bool cmyk_interp = false) {
    GDALDataset* mds = mem_drv_->Create("", w, h, bands, GDT_Byte, nullptr);
    ASSERT_NE(mds, nullptr);
    for (int b = 1; b <= bands; ++b) {
      std::vector<uint8_t> buf(w * h);
      for (int i = 0; i < w * h; ++i) {
        buf[i] = static_cast<uint8_t>((i * 7 + b * 43) & 0xFF);
      }
      ASSERT_EQ(
          mds->GetRasterBand(b)->RasterIO(GF_Write, 0, 0, w, h, buf.data(), w,
                                          h, GDT_Byte, 0, 0, nullptr),
          CE_None);
    }
    if (cmyk_interp && bands == 4) {
      mds->GetRasterBand(1)->SetColorInterpretation(GCI_CyanBand);
      mds->GetRasterBand(2)->SetColorInterpretation(GCI_MagentaBand);
      mds->GetRasterBand(3)->SetColorInterpretation(GCI_YellowBand);
      mds->GetRasterBand(4)->SetColorInterpretation(GCI_BlackBand);
    }
    GDALDataset* jds = jpeg_drv_->CreateCopy(jpg_path, mds, FALSE, jpeg_opts,
                                             nullptr, nullptr);
    ASSERT_NE(jds, nullptr);
    GDALClose(jds);
    GDALClose(mds);
  }

  GDALDriver* gtiff_drv_ = nullptr;
  GDALDriver* jpeg_drv_ = nullptr;
  GDALDriver* mem_drv_ = nullptr;
  GDALDriver* vrt_drv_ = nullptr;
};

TEST_F(GTJpegCopyTest, CanCopyFromJPEGRejectionsAndUnderlyingVRT) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  constexpr char kJpg[] = "/vsimem/jcopy_src3.jpg";
  constexpr char kOut[] = "/vsimem/jcopy_out.tif";
  absl::Cleanup c = [&]() {
    VSIUnlink(kJpg);
    VSIUnlink(kOut);
  };
  CreateInMemoryJpeg(kJpg, 20, 20, 3);

  // 1. Driverless MEMDataset::Create (poSrcDS->GetDriver() == nullptr).
  {
    MEMDataset* raw_mem = MEMDataset::Create("", 16, 16, 1, GDT_Byte, nullptr);
    ASSERT_NE(raw_mem, nullptr);
    char** opts = CSLSetNameValue(nullptr, "COMPRESS", "JPEG");
    GDALDataset* out =
        gtiff_drv_->CreateCopy(kOut, raw_mem, FALSE, opts, nullptr, nullptr);
    CSLDestroy(opts);
    ASSERT_NE(out, nullptr);
    GDALClose(out);
    VSIUnlink(kOut);
    delete raw_mem;
  }

  // 2. Empty VRT (GetSingleSimpleSource() == nullptr) and non-JPEG MEM dataset.
  {
    GDALDataset* empty_vrt = vrt_drv_->Create("", 16, 16, 1, GDT_Byte, nullptr);
    ASSERT_NE(empty_vrt, nullptr);
    char** opts = CSLSetNameValue(nullptr, "COMPRESS", "JPEG");
    GDALDataset* out =
        gtiff_drv_->CreateCopy(kOut, empty_vrt, FALSE, opts, nullptr, nullptr);
    CSLDestroy(opts);
    ASSERT_NE(out, nullptr);
    GDALClose(out);
    VSIUnlink(kOut);
    GDALClose(empty_vrt);
  }

  // 3. VRT wrapping single JPEG -> succeeds via GetUnderlyingDataset!
  {
    GDALDataset* jds = static_cast<GDALDataset*>(GDALOpen(kJpg, GA_ReadOnly));
    ASSERT_NE(jds, nullptr);
    GDALDataset* vds =
        vrt_drv_->CreateCopy("", jds, FALSE, nullptr, nullptr, nullptr);
    ASSERT_NE(vds, nullptr);

    // Without COMPRESS=JPEG (e.g. LZW or nullptr) -> CanCopyFromJPEG returns
    // FALSE.
    GDALDataset* out_none =
        gtiff_drv_->CreateCopy(kOut, vds, FALSE, nullptr, nullptr, nullptr);
    ASSERT_NE(out_none, nullptr);
    GDALClose(out_none);
    VSIUnlink(kOut);

    // With COMPRESS=JPEG and BLOCKYSIZE=20 (equals nYSize, even though 20 % 16
    // != 0!).
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "COMPRESS", "JPEG");
    opts = CSLSetNameValue(opts, "BLOCKYSIZE", "20");
    GDALDataset* out_jpg =
        gtiff_drv_->CreateCopy(kOut, vds, FALSE, opts, nullptr, nullptr);
    CSLDestroy(opts);
    ASSERT_NE(out_jpg, nullptr);
    GDALClose(out_jpg);
    VSIUnlink(kOut);

    GDALClose(vds);

    // Rejections on YCbCr 20x20 JPEG:
    // CanCopyFromJPEG() must reject the lossless path but the generic
    // CreateCopy must still succeed and produce a full-size 3-band image.
    auto check_fallback = [&](char** copts) {
      GDALDataset* o =
          gtiff_drv_->CreateCopy(kOut, jds, FALSE, copts, nullptr, nullptr);
      ASSERT_NE(o, nullptr);
      EXPECT_EQ(o->GetRasterXSize(), 20);
      EXPECT_EQ(o->GetRasterYSize(), 20);
      EXPECT_EQ(o->GetRasterCount(), 3);
      EXPECT_STREQ(o->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"),
                   "JPEG");
      GDALClose(o);
      VSIUnlink(kOut);
    };

    // Incompatible PHOTOMETRIC=RGB on YCbCr source.
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

    // Non-MCU BLOCKYSIZE=10: rejected by CanCopyFromJPEG(), and the generic
    // JPEG-in-TIFF path refuses a strip height that is not a multiple of 8,
    // so the whole CreateCopy fails.
    CPLStringList o4;
    o4.SetNameValue("COMPRESS", "JPEG");
    o4.SetNameValue("BLOCKYSIZE", "10");
    EXPECT_EQ(gtiff_drv_->CreateCopy(kOut, jds, FALSE, o4.List(), nullptr,
                                     nullptr),
              nullptr);
    VSIUnlink(kOut);

    // COMPRESSION_REVERSIBILITY=LOSSLESS on source IMAGE_STRUCTURE metadata.
    jds->SetMetadataItem("COMPRESSION_REVERSIBILITY", "LOSSLESS",
                         "IMAGE_STRUCTURE");
    CPLStringList o5;
    o5.SetNameValue("COMPRESS", "JPEG");
    ASSERT_NO_FATAL_FAILURE(check_fallback(o5.List()));

    GDALClose(jds);
  }
}

TEST_F(GTJpegCopyTest, PhotometricsGrayRGBCMYKAndTiledPartialMCU) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  constexpr char kGray[] = "/vsimem/jcopy_gray.jpg";
  constexpr char kRgb[] = "/vsimem/jcopy_rgb.jpg";
  constexpr char kCmyk[] = "/vsimem/jcopy_cmyk.jpg";
  constexpr char kYcbcr36[] = "/vsimem/jcopy_ycbcr36.jpg";
  constexpr char kOut[] = "/vsimem/jcopy_photo_out.tif";
  absl::Cleanup c = [&]() {
    VSIUnlink(kGray);
    VSIUnlink(kRgb);
    VSIUnlink(kCmyk);
    VSIUnlink(kYcbcr36);
    VSIUnlink(kOut);
  };

  // 1. 1-band Grayscale JPEG (sCInfo.num_components == 1, non-YCbCr JPEGTABLES
  // suppression of table 1, JPEGMEM config set).
  CreateInMemoryJpeg(kGray, 24, 24, 1);
  {
    CPLConfigOptionSetter jmem("JPEGMEM", "10000000", false);
    GDALDataset* jds = static_cast<GDALDataset*>(GDALOpen(kGray, GA_ReadOnly));
    ASSERT_NE(jds, nullptr);
    char** opts = CSLSetNameValue(nullptr, "COMPRESS", "JPEG");
    opts = CSLSetNameValue(opts, "PHOTOMETRIC", "MINISBLACK");
    opts = CSLSetNameValue(opts, "BLOCKYSIZE", "8");
    GDALDataset* out =
        gtiff_drv_->CreateCopy(kOut, jds, FALSE, opts, nullptr, nullptr);
    CSLDestroy(opts);
    ASSERT_NE(out, nullptr);
    GDALClose(out);
    VSIUnlink(kOut);
    GDALClose(jds);
  }

  // 2. 3-band RGB JPEG (WRITE_RGB=YES -> nMCUSize == 8, PHOTOMETRIC=RGB).
  {
    char** jopts = CSLSetNameValue(nullptr, "WRITE_RGB", "YES");
    CreateInMemoryJpeg(kRgb, 16, 16, 3, jopts);
    CSLDestroy(jopts);

    GDALDataset* jds = static_cast<GDALDataset*>(GDALOpen(kRgb, GA_ReadOnly));
    ASSERT_NE(jds, nullptr);
    char** opts = CSLSetNameValue(nullptr, "COMPRESS", "JPEG");
    opts = CSLSetNameValue(opts, "PHOTOMETRIC", "RGB");
    GDALDataset* out =
        gtiff_drv_->CreateCopy(kOut, jds, FALSE, opts, nullptr, nullptr);
    CSLDestroy(opts);
    ASSERT_NE(out, nullptr);
    GDALClose(out);
    VSIUnlink(kOut);
    GDALClose(jds);
  }

  // 3. 4-band CMYK JPEG (GDAL_JPEG_TO_RGB=NO -> 4 Cyan/Magenta/Yellow/Black
  // bands, pszPhotometric == nullptr auto-sets PHOTOMETRIC=CMYK!).
  {
    CreateInMemoryJpeg(kCmyk, 16, 16, 4, nullptr, /*cmyk_interp=*/true);
    CPLConfigOptionSetter no_rgb("GDAL_JPEG_TO_RGB", "NO", false);
    GDALDataset* jds = static_cast<GDALDataset*>(GDALOpen(kCmyk, GA_ReadOnly));
    ASSERT_NE(jds, nullptr);
    ASSERT_EQ(jds->GetRasterCount(), 4);
    char** opts = CSLSetNameValue(nullptr, "COMPRESS", "JPEG");
    GDALDataset* out =
        gtiff_drv_->CreateCopy(kOut, jds, FALSE, opts, nullptr, nullptr);
    CSLDestroy(opts);
    ASSERT_NE(out, nullptr);
    GDALClose(out);
    VSIUnlink(kOut);
    GDALClose(jds);
  }

  // 4. 36x36 3-band YCbCr JPEG copied to TILED 32x32 TIFF!
  // 36 pixels = 5 DCT blocks in Y (2x2 sampling ->
  // comp_info[0].v_samp_factor=2, height_in_blocks=5, width_in_blocks=5), while
  // 2x2 tiles of 32x32 have 4 DCT blocks each -> tile (1,0), (0,1), and (1,1)
  // hit ALL partial X & Y DCT block zero-padding loops in
  // GTIFF_CopyBlockFromJPEG (L686-750)!
  CreateInMemoryJpeg(kYcbcr36, 36, 36, 3);
  {
    GDALDataset* jds =
        static_cast<GDALDataset*>(GDALOpen(kYcbcr36, GA_ReadOnly));
    ASSERT_NE(jds, nullptr);
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "COMPRESS", "JPEG");
    opts = CSLSetNameValue(opts, "PHOTOMETRIC", "YCBCR");
    opts = CSLSetNameValue(opts, "INTERLEAVE", "PIXEL");
    opts = CSLSetNameValue(opts, "TILED", "YES");
    opts = CSLSetNameValue(opts, "BLOCKXSIZE", "32");
    opts = CSLSetNameValue(opts, "BLOCKYSIZE", "32");
    GDALDataset* out =
        gtiff_drv_->CreateCopy(kOut, jds, FALSE, opts, nullptr, nullptr);
    CSLDestroy(opts);
    ASSERT_NE(out, nullptr);
    // The DCT-coefficient copy preserves the source's coefficients, but the
    // partial edge tiles are zero-padded, which perturbs chroma upsampling
    // next to tile boundaries. Decoded pixels must therefore be very close
    // to the source everywhere and identical away from the tile edges.
    for (int b = 1; b <= 3; ++b) {
      SCOPED_TRACE(b);
      std::vector<uint8_t> got(36 * 36), want(36 * 36);
      ASSERT_EQ(out->GetRasterBand(b)->RasterIO(GF_Read, 0, 0, 36, 36,
                                                got.data(), 36, 36, GDT_Byte,
                                                0, 0, nullptr),
                CE_None);
      ASSERT_EQ(jds->GetRasterBand(b)->RasterIO(GF_Read, 0, 0, 36, 36,
                                                want.data(), 36, 36, GDT_Byte,
                                                0, 0, nullptr),
                CE_None);
      int max_diff = 0;
      long sum_diff = 0;
      for (size_t i = 0; i < got.size(); ++i) {
        const int d = std::abs(static_cast<int>(got[i]) - want[i]);
        max_diff = std::max(max_diff, d);
        sum_diff += d;
      }
      // Padding-induced chroma differences are confined to the tile edges
      // (observed max ~55 at the 32px boundary, mean ~1.3 over the image).
      EXPECT_LE(max_diff, 96);
      EXPECT_LT(static_cast<double>(sum_diff) / got.size(), 4.0);
      // Interior of the first (full) tile is unaffected by padding.
      for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
          EXPECT_EQ(got[y * 36 + x], want[y * 36 + x])
              << "x=" << x << " y=" << y;
        }
      }
    }
    GDALClose(out);
    VSIUnlink(kOut);

    // Stripped multi-strip (BLOCKYSIZE=16 on 36x36) + progress cancellation!
    auto cancel_cb = [](double dfComplete, const char*, void*) -> int {
      return dfComplete < 0.4 ? 1 : 0;
    };
    char** sopts = nullptr;
    sopts = CSLSetNameValue(sopts, "COMPRESS", "JPEG");
    sopts = CSLSetNameValue(sopts, "BLOCKYSIZE", "16");
    EXPECT_EQ(
        gtiff_drv_->CreateCopy(kOut, jds, FALSE, sopts, cancel_cb, nullptr),
        nullptr);
    CSLDestroy(sopts);
    VSIUnlink(kOut);

    // Raw write failure via ||maxlength on both tiled and stripped outputs!
    constexpr char kCapReal[] = "/vsimem/jcopy_cap.tif";
    constexpr char kCapLim[] = "/vsimem/jcopy_cap.tif||maxlength=700";
    char** cap_sopts = CSLSetNameValue(nullptr, "COMPRESS", "JPEG");
    cap_sopts = CSLSetNameValue(cap_sopts, "BLOCKYSIZE", "16");
    EXPECT_EQ(gtiff_drv_->CreateCopy(kCapLim, jds, FALSE, cap_sopts, nullptr,
                                     nullptr),
              nullptr);
    CSLDestroy(cap_sopts);
    VSIUnlink(kCapReal);

    char** cap_topts = CSLSetNameValue(nullptr, "COMPRESS", "JPEG");
    cap_topts = CSLSetNameValue(cap_topts, "TILED", "YES");
    cap_topts = CSLSetNameValue(cap_topts, "BLOCKXSIZE", "16");
    cap_topts = CSLSetNameValue(cap_topts, "BLOCKYSIZE", "16");
    EXPECT_EQ(gtiff_drv_->CreateCopy(kCapLim, jds, FALSE, cap_topts, nullptr,
                                     nullptr),
              nullptr);
    CSLDestroy(cap_topts);
    VSIUnlink(kCapReal);

    GDALClose(jds);
  }
}

TEST_F(GTJpegCopyTest, UnlinkedAndCorruptedJpegSetjmpErrorPaths) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  constexpr char kUnlinked[] = "/vsimem/jcopy_unlinked.jpg";
  constexpr char kZeroed[] = "/vsimem/jcopy_zeroed.jpg";
  constexpr char kBadQuant[] = "/vsimem/jcopy_badquant.jpg";
  constexpr char kOddSamp[] = "/vsimem/jcopy_oddsamp.jpg";
  constexpr char kBadScan[] = "/vsimem/jcopy_badscan.jpg";
  constexpr char kOut[] = "/vsimem/jcopy_err_out.tif";
  absl::Cleanup c = [&]() {
    VSIUnlink(kUnlinked);
    VSIUnlink(kZeroed);
    VSIUnlink(kBadQuant);
    VSIUnlink(kOddSamp);
    VSIUnlink(kBadScan);
    VSIUnlink(kOut);
  };

  char** opts = CSLSetNameValue(nullptr, "COMPRESS", "JPEG");
  absl::Cleanup co = [&]() { CSLDestroy(opts); };

  // 1. Unlink JPEG after GDALOpen -> VSIFOpenL fails in WriteAdditionalTags &
  // CopyFromJPEG!
  CreateInMemoryJpeg(kUnlinked, 16, 16, 3);
  {
    GDALDataset* jds =
        static_cast<GDALDataset*>(GDALOpen(kUnlinked, GA_ReadOnly));
    ASSERT_NE(jds, nullptr);
    VSIUnlink(kUnlinked);
    // VSIFOpenL() on the unlinked path fails inside the JPEG-copy helpers, so
    // the lossless path is abandoned; the already-open JPEG handle still has
    // its /vsimem buffer, so the generic re-encoding copy succeeds.
    GDALDataset* out =
        gtiff_drv_->CreateCopy(kOut, jds, FALSE, opts, nullptr, nullptr);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->GetRasterXSize(), 16);
    GDALClose(out);
    VSIUnlink(kOut);
    GDALClose(jds);
  }

  // 2. Zero-out JPEG header after GDALOpen -> jpeg_read_header triggers
  // GTIFF_ErrorExitJPEG -> setjmp with bCallDestroyDecompress == true!
  CreateInMemoryJpeg(kZeroed, 16, 16, 3);
  {
    GDALDataset* jds =
        static_cast<GDALDataset*>(GDALOpen(kZeroed, GA_ReadOnly));
    ASSERT_NE(jds, nullptr);
    vsi_l_offset sz = 0;
    GByte* buf = VSIGetMemFileBuffer(kZeroed, &sz, FALSE);
    ASSERT_NE(buf, nullptr);
    std::memset(buf, 0, sz);
    EXPECT_EQ(gtiff_drv_->CreateCopy(kOut, jds, FALSE, opts, nullptr, nullptr),
              nullptr);
    VSIUnlink(kOut);
    GDALClose(jds);
  }

  // Helper to locate SOF0 marker (FF C0) in JPEG buffer.
  auto find_sof0 = [](GByte* buf, vsi_l_offset sz) -> size_t {
    for (size_t i = 0; i + 15 < sz; ++i) {
      if (buf[i] == 0xFF && buf[i + 1] == 0xC0) return i;
    }
    return 0;
  };

  // 3. Patch SOF0 component 0 quant_tbl_no (buf[sof0 + 12] = 3, where table 3
  // was not defined) -> jpeg_read_header succeeds, then
  // jpeg_copy_critical_parameters / jpeg_write_tables triggers setjmp with BOTH
  // bCallDestroyCompress == true AND bCallDestroyDecompress == true!
  CreateInMemoryJpeg(kBadQuant, 16, 16, 3);
  {
    GDALDataset* jds =
        static_cast<GDALDataset*>(GDALOpen(kBadQuant, GA_ReadOnly));
    ASSERT_NE(jds, nullptr);
    vsi_l_offset sz = 0;
    GByte* buf = VSIGetMemFileBuffer(kBadQuant, &sz, FALSE);
    size_t sof0 = find_sof0(buf, sz);
    ASSERT_GT(sof0, 0U);
    buf[sof0 + 12] = 3;  // nonexistent quantization table 3
    EXPECT_EQ(gtiff_drv_->CreateCopy(kOut, jds, FALSE, opts, nullptr, nullptr),
              nullptr);
    VSIUnlink(kOut);
    GDALClose(jds);
  }

  // 4. Patch SOF0 component 0 sampling factor to 0x31 (3x1 unusual sampling) ->
  // hits "Unusual sampling factors" branch at L522-526!
  CreateInMemoryJpeg(kOddSamp, 24, 24, 3);
  {
    GDALDataset* jds =
        static_cast<GDALDataset*>(GDALOpen(kOddSamp, GA_ReadOnly));
    ASSERT_NE(jds, nullptr);
    vsi_l_offset sz = 0;
    GByte* buf = VSIGetMemFileBuffer(kOddSamp, &sz, FALSE);
    size_t sof0 = find_sof0(buf, sz);
    ASSERT_GT(sof0, 0U);
    buf[sof0 + 11] = 0x31;
    // Unusual sampling factors are rejected by CanCopyFromJPEG() and the
    // generic (re-encoding) copy path is used instead.
    GDALDataset* out =
        gtiff_drv_->CreateCopy(kOut, jds, FALSE, opts, nullptr, nullptr);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->GetRasterXSize(), 24);
    EXPECT_EQ(out->GetRasterCount(), 3);
    GDALClose(out);
    VSIUnlink(kOut);
    GDALClose(jds);
  }

  // 5. Truncate / corrupt SOS scan data after header so WriteAdditionalTags
  // (which only calls jpeg_read_header) SUCCEEDS, while GTIFF_CopyFromJPEG
  // (which calls jpeg_read_coefficients) fails or hits setjmp!
  CreateInMemoryJpeg(kBadScan, 16, 16, 3);
  {
    GDALDataset* jds =
        static_cast<GDALDataset*>(GDALOpen(kBadScan, GA_ReadOnly));
    ASSERT_NE(jds, nullptr);
    vsi_l_offset sz = 0;
    GByte* buf = VSIGetMemFileBuffer(kBadScan, &sz, FALSE);
    // Find SOS (FF DA) and replace it with an invalid marker FF C1 (SOF1 inside
    // scan)
    for (size_t i = 2; i + 4 < sz; ++i) {
      if (buf[i] == 0xFF && buf[i + 1] == 0xDA) {
        size_t hdr_len = (static_cast<size_t>(buf[i + 2]) << 8) | buf[i + 3];
        if (i + 2 + hdr_len + 2 < sz) {
          buf[i + 2 + hdr_len] = 0xFF;
          buf[i + 2 + hdr_len + 1] = 0xC2;  // unexpected SOF2 marker in scan!
        }
        break;
      }
    }
    // jpeg_read_header() succeeds but jpeg_read_coefficients() fails, so the
    // lossless path is abandoned and the generic copy path produces a dataset.
    GDALDataset* out =
        gtiff_drv_->CreateCopy(kOut, jds, FALSE, opts, nullptr, nullptr);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->GetRasterXSize(), 16);
    GDALClose(out);
    VSIUnlink(kOut);
    GDALClose(jds);
  }
}

}  // namespace
}  // namespace autotest2
