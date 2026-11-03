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
// Tests GeoTIFF driver registration, compression/predictor/alpha helpers, RPC
// and JPEG table serialization, error handlers, and subdataset parsing
// (geotiff.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/gtiff/geotiff.cpp
//   https://gdal.org/drivers/raster/gtiff.html

#include <cstdarg>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

extern int GTiffWarningHandlerExt(TIFF* tif, void* user_data,
                                  const char* module, const char* fmt,
                                  va_list ap);
extern int GTiffErrorHandlerExt(TIFF* tif, void* user_data, const char* module,
                                const char* fmt, va_list ap);

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

int CallWarningExt(const char* module, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int rc = GTiffWarningHandlerExt(nullptr, nullptr, module, fmt, ap);
  va_end(ap);
  return rc;
}

int CallErrorExt(const char* module, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int rc = GTiffErrorHandlerExt(nullptr, nullptr, module, fmt, ap);
  va_end(ap);
  return rc;
}

TEST(GeoTiffTest, IdentifyDoesNotExistAndCorruptInput) {
  GDALRegister_GTiff();
  CPLErrorReset();

  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  ASSERT_NE(nullptr, drv->pfnIdentify);

  auto info_missing = std::make_unique<GDALOpenInfo>("/does_not_exist",
                                                     GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, drv->pfnIdentify(info_missing.get()));

  constexpr char kCorruptPath[] = "/vsimem/geotiff_test_corrupt.tif";
  VsiMemTempWrapper corrupt_file(kCorruptPath, "NOT_A_VALID_TIFF_HEADER");
  auto info_corrupt =
      std::make_unique<GDALOpenInfo>(kCorruptPath, GDAL_OF_READONLY, nullptr);
  {
    WithQuietHandler quiet;
    EXPECT_EQ(FALSE, drv->pfnIdentify(info_corrupt.get()));
    CPLErrorReset();
  }
}

TEST(GeoTiffTest, OneTimeInitAndDriverRegistrationLifecycle) {
  CPLErrorReset();
  LibgeotiffOneTimeInit();
  GTiffOneTimeInit();
  GTiffOneTimeInit();  // Second call hits bOneTimeInitDone early return.

  GDALRegister_GTiff();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  // Early return when GTiff is already registered.
  GDALRegister_GTiff();
  EXPECT_EQ(drv, GetGDALDriverManager()->GetDriverByName("GTiff"));

  // GDALDeregister_GTiff() is deliberately not exercised here because
  // unregistering libtiff JXL codecs would prevent re-registration after
  // GTiffOneTimeInit().
  EXPECT_TRUE(TIFFIsCODECConfigured(COMPRESSION_JXL));

  // Check compression values for both non-COG and COG modes.
  bool lzw = false;
  bool deflate = false;
  bool lzma = false;
  bool zstd = false;
  bool jpeg = false;
  bool webp = false;
  bool lerc = false;
  const CPLString xml_gtiff = GTiffGetCompressValues(
      lzw, deflate, lzma, zstd, jpeg, webp, lerc, /*bForCOG=*/false);
  EXPECT_THAT(static_cast<const std::string&>(xml_gtiff),
              HasSubstr("PACKBITS"));
  EXPECT_THAT(static_cast<const std::string&>(xml_gtiff),
              HasSubstr("LERC_ZSTD"));

  const CPLString xml_cog = GTiffGetCompressValues(
      lzw, deflate, lzma, zstd, jpeg, webp, lerc, /*bForCOG=*/true);
  EXPECT_THAT(static_cast<const std::string&>(xml_cog),
              Not(HasSubstr("PACKBITS")));
}

TEST(GeoTiffTest, PredictorOverviewBlockSizeAndCodecSetters) {
  CPLErrorReset();
  GDALRegister_GTiff();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);

  EXPECT_TRUE(GTIFFSupportsPredictor(COMPRESSION_LZW));
  EXPECT_TRUE(GTIFFSupportsPredictor(COMPRESSION_ADOBE_DEFLATE));
  EXPECT_TRUE(GTIFFSupportsPredictor(COMPRESSION_ZSTD));
  EXPECT_FALSE(GTIFFSupportsPredictor(COMPRESSION_NONE));

  constexpr char kTiled[] = "/vsimem/geotiff_bs_tiled.tif";
  constexpr char kStripped[] = "/vsimem/geotiff_bs_strip.tif";
  absl::Cleanup clean_files = [&]() {
    VSIUnlink(kTiled);
    VSIUnlink(kStripped);
  };

  CPLStringList topts;
  topts.SetNameValue("TILED", "YES");
  topts.SetNameValue("BLOCKXSIZE", "256");
  topts.SetNameValue("BLOCKYSIZE", "256");
  GDALDatasetUniquePtr ds_tiled(
      drv->Create(kTiled, 256, 256, 1, GDT_Byte, topts.List()));
  ASSERT_NE(nullptr, ds_tiled);

  int bx = 0;
  int by = 0;
  // 1. From band block size (valid 256x256 power of 2).
  GTIFFGetOverviewBlockSize(
      GDALRasterBand::ToHandle(ds_tiled->GetRasterBand(1)), &bx, &by, nullptr,
      nullptr);
  EXPECT_EQ(256, bx);
  EXPECT_EQ(256, by);

  // 2. From option list (valid 512, and invalid 100 -> defaults to 128).
  CPLStringList bopts;
  bopts.SetNameValue("BLOCKSIZE", "512");
  GTIFFGetOverviewBlockSize(
      GDALRasterBand::ToHandle(ds_tiled->GetRasterBand(1)), &bx, &by,
      bopts.List(), "BLOCKSIZE");
  EXPECT_EQ(512, bx);

  CPLStringList bad_opts;
  bad_opts.SetNameValue("BLOCKSIZE", "100");
  GTIFFGetOverviewBlockSize(
      GDALRasterBand::ToHandle(ds_tiled->GetRasterBand(1)), &bx, &by,
      bad_opts.List(), "BLOCKSIZE");
  EXPECT_EQ(128, bx);

  // 3. From GDAL_TIFF_OVR_BLOCKSIZE config option.
  {
    CPLConfigOptionSetter cfg("GDAL_TIFF_OVR_BLOCKSIZE", "64",
                              /*bSetOnlyIfUndefined=*/false);
    GTIFFGetOverviewBlockSize(
        GDALRasterBand::ToHandle(ds_tiled->GetRasterBand(1)), &bx, &by, nullptr,
        "BLOCKSIZE");
    EXPECT_EQ(64, bx);
  }

  // 4. Band fallback on stripped non-square band -> defaults to 128x128.
  {
    GDALDatasetUniquePtr ds_strip(
        drv->Create(kStripped, 100, 100, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds_strip);
    GTIFFGetOverviewBlockSize(
        GDALRasterBand::ToHandle(ds_strip->GetRasterBand(1)), &bx, &by, nullptr,
        nullptr);
    EXPECT_EQ(128, bx);
    EXPECT_EQ(128, by);
  }

  // Build 1 internal overview on ds_tiled so all 11 GTIFFSet* functions iterate
  // over m_papoOverviewDS[i] (updating private member fields verified
  // indirectly via overview write tests).
  constexpr int kOvrLevel = 2;
  ASSERT_EQ(CE_None, ds_tiled->BuildOverviews("NEAREST", 1, &kOvrLevel, 0,
                                              nullptr, nullptr, nullptr));
  GDALDatasetH h_ds = GDALDataset::ToHandle(ds_tiled.get());
  GTIFFSetJpegQuality(h_ds, 85);
  GTIFFSetWebPLevel(h_ds, 80);
  GTIFFSetWebPLossless(h_ds, true);
  GTIFFSetJpegTablesMode(h_ds, 1);
  GTIFFSetZLevel(h_ds, 6);
  GTIFFSetZSTDLevel(h_ds, 9);
  GTIFFSetMaxZError(h_ds, 0.25);
  GTIFFSetJXLLossless(h_ds, false);
  GTIFFSetJXLEffort(h_ds, 4);
  GTIFFSetJXLDistance(h_ds, 1.5f);
  GTIFFSetJXLAlphaDistance(h_ds, 0.5f);
}

TEST(GeoTiffTest, AlphaColorInterpPhotometricAndNoDataHelpers) {
  CPLErrorReset();
  GDALRegister_MEM();

  EXPECT_EQ(42, GTiffGetAlphaValue(nullptr, 42));
  EXPECT_EQ(DEFAULT_ALPHA_TYPE, GTiffGetAlphaValue("YES", 0));
  EXPECT_EQ(EXTRASAMPLE_ASSOCALPHA, GTiffGetAlphaValue("PREMULTIPLIED", 0));
  EXPECT_EQ(EXTRASAMPLE_UNASSALPHA, GTiffGetAlphaValue("NON-PREMULTIPLIED", 0));
  EXPECT_EQ(EXTRASAMPLE_UNSPECIFIED, GTiffGetAlphaValue("NO", 0));
  EXPECT_EQ(EXTRASAMPLE_UNSPECIFIED, GTiffGetAlphaValue("UNSPECIFIED", 0));
  EXPECT_EQ(77, GTiffGetAlphaValue("UNKNOWN_VAL", 77));

  EXPECT_STREQ("nan", GTiffFormatGDALNoDataTagValue(
                          std::numeric_limits<double>::quiet_NaN())
                          .c_str());
  EXPECT_STREQ("-9999", GTiffFormatGDALNoDataTagValue(-9999.0).c_str());

  // GTIFFIsStandardColorInterpretation across photometrics.
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  // MINISBLACK: standard (Gray + Alpha) vs non-standard (Red), and PALETTE.
  {
    GDALDatasetUniquePtr mds(mem_drv->Create("", 2, 2, 2, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mds);
    mds->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex);
    mds->GetRasterBand(2)->SetColorInterpretation(GCI_AlphaBand);
    EXPECT_TRUE(GTIFFIsStandardColorInterpretation(
        GDALDataset::ToHandle(mds.get()), PHOTOMETRIC_MINISBLACK, nullptr));
    mds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    EXPECT_FALSE(GTIFFIsStandardColorInterpretation(
        GDALDataset::ToHandle(mds.get()), PHOTOMETRIC_MINISBLACK, nullptr));
    mds->GetRasterBand(1)->SetColorInterpretation(GCI_PaletteIndex);
    EXPECT_TRUE(GTIFFIsStandardColorInterpretation(
        GDALDataset::ToHandle(mds.get()), PHOTOMETRIC_PALETTE, nullptr));
    EXPECT_FALSE(GTIFFIsStandardColorInterpretation(
        GDALDataset::ToHandle(mds.get()), PHOTOMETRIC_CIELAB, nullptr));
  }

  // RGB & YCBCR.
  {
    GDALDatasetUniquePtr mds(mem_drv->Create("", 2, 2, 4, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, mds);
    mds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    mds->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
    mds->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);
    mds->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
    EXPECT_TRUE(GTIFFIsStandardColorInterpretation(
        GDALDataset::ToHandle(mds.get()), PHOTOMETRIC_RGB, nullptr));

    mds->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex);
    EXPECT_FALSE(GTIFFIsStandardColorInterpretation(
        GDALDataset::ToHandle(mds.get()), PHOTOMETRIC_RGB, nullptr));

    // With PHOTOMETRIC=RGB and ALPHA=YES in creation options -> skips bands.
    CPLStringList copts;
    copts.SetNameValue("PHOTOMETRIC", "RGB");
    copts.SetNameValue("ALPHA", "YES");
    EXPECT_TRUE(GTIFFIsStandardColorInterpretation(
        GDALDataset::ToHandle(mds.get()), PHOTOMETRIC_RGB, copts.List()));

    GDALDatasetUniquePtr yds(mem_drv->Create("", 2, 2, 3, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, yds);
    EXPECT_TRUE(GTIFFIsStandardColorInterpretation(
        GDALDataset::ToHandle(yds.get()), PHOTOMETRIC_YCBCR, nullptr));
  }

  // GTIFFUpdatePhotometric all branches.
  uint16_t photo = PHOTOMETRIC_MINISBLACK;
  uint16_t planar = PLANARCONFIG_CONTIG;
  EXPECT_TRUE(GTIFFUpdatePhotometric(nullptr, "PHOTOMETRIC", COMPRESSION_NONE,
                                     nullptr, 1, photo, planar));
  constexpr const char* kPhotometricNames[] = {
      "MINISBLACK", "MINISWHITE", "RGB", "CMYK", "CIELAB", "ICCLAB", "ITULAB",
  };
  for (const char* name : kPhotometricNames) {
    EXPECT_TRUE(GTIFFUpdatePhotometric(name, "PHOTOMETRIC", COMPRESSION_NONE,
                                       nullptr, 3, photo, planar));
  }
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_TRUE(GTIFFUpdatePhotometric("BOGUS_VALUE", "PHOTOMETRIC",
                                       COMPRESSION_NONE, nullptr, 3, photo,
                                       planar));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // YCBCR error & success cases.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(GTIFFUpdatePhotometric(
        "YCBCR", "PHOTOMETRIC", COMPRESSION_NONE, nullptr, 3, photo, planar));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());

    CPLErrorReset();
    planar = PLANARCONFIG_SEPARATE;
    EXPECT_FALSE(GTIFFUpdatePhotometric(
        "YCBCR", "PHOTOMETRIC", COMPRESSION_JPEG, "BAND", 3, photo, planar));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());

    CPLErrorReset();
    planar = PLANARCONFIG_CONTIG;
    EXPECT_FALSE(GTIFFUpdatePhotometric(
        "YCBCR", "PHOTOMETRIC", COMPRESSION_JPEG, "PIXEL", 4, photo, planar));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    CPLErrorReset();
  }
  EXPECT_TRUE(GTIFFUpdatePhotometric("YCBCR", "PHOTOMETRIC", COMPRESSION_JPEG,
                                     "PIXEL", 3, photo, planar));
  EXPECT_EQ(PHOTOMETRIC_YCBCR, photo);
}

TEST(GeoTiffTest, RPCTagJPEGTablesErrorHandlersCompressionAndSubdatasetInfo) {
  CPLErrorReset();
  GDALRegister_GTiff();
  GTiffOneTimeInit();

  // 1. RPC tag write & read + JPEGTables helper on in-memory TIFF.
  {
    constexpr char kFile[] = "/vsimem/geotiff_rpc_jpeg.tif";
    absl::Cleanup clean_file = [&]() { VSIUnlink(kFile); };
    VSILFILE* fp = VSIFOpenL(kFile, "wb+");
    ASSERT_NE(nullptr, fp);
    absl::Cleanup close_fp = [fp]() { EXPECT_EQ(0, VSIFCloseL(fp)); };
    TIFF* tif = VSI_TIFFOpen(kFile, "w+", fp);
    ASSERT_NE(nullptr, tif);
    absl::Cleanup close_tif = [tif]() { TIFFClose(tif); };

    // Invalid RPC metadata -> early return; read before tag set -> nullptr.
    GTiffDatasetWriteRPCTag(tif, nullptr);
    EXPECT_EQ(nullptr, GTiffDatasetReadRPCTag(tif));

    // Valid RPC metadata -> writes 92 doubles -> reads back.
    constexpr const char* kCoeffs = "1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0";
    CPLStringList rpc;
    rpc.SetNameValue("ERR_BIAS", "1.5");
    rpc.SetNameValue("ERR_RAND", "0.5");
    rpc.SetNameValue("LINE_OFF", "100");
    rpc.SetNameValue("SAMP_OFF", "200");
    rpc.SetNameValue("LAT_OFF", "37.5");
    rpc.SetNameValue("LONG_OFF", "-122.0");
    rpc.SetNameValue("HEIGHT_OFF", "50");
    rpc.SetNameValue("LINE_SCALE", "100");
    rpc.SetNameValue("SAMP_SCALE", "200");
    rpc.SetNameValue("LAT_SCALE", "1");
    rpc.SetNameValue("LONG_SCALE", "1");
    rpc.SetNameValue("HEIGHT_SCALE", "100");
    rpc.SetNameValue("LINE_NUM_COEFF", kCoeffs);
    rpc.SetNameValue("LINE_DEN_COEFF", kCoeffs);
    rpc.SetNameValue("SAMP_NUM_COEFF", kCoeffs);
    rpc.SetNameValue("SAMP_DEN_COEFF", kCoeffs);
    GTiffDatasetWriteRPCTag(tif, rpc.List());

    CPLStringList read_rpc(GTiffDatasetReadRPCTag(tif),
                           /*bTakeOwnership=*/TRUE);
    ASSERT_FALSE(read_rpc.empty());
    EXPECT_STREQ("100", read_rpc.FetchNameValue("LINE_OFF"));
    EXPECT_STREQ("200", read_rpc.FetchNameValue("SAMP_OFF"));
    EXPECT_STREQ("37.5", read_rpc.FetchNameValue("LAT_OFF"));
    EXPECT_STREQ("1.5", read_rpc.FetchNameValue("ERR_BIAS"));
    EXPECT_STREQ(kCoeffs, read_rpc.FetchNameValue("LINE_NUM_COEFF"));

    // GTiffWriteJPEGTables:
    auto check_jpeg_tables = [&]() {
      uint32_t tbl_sz = 0;
      void* tbl_ptr = nullptr;
      EXPECT_EQ(1, TIFFGetField(tif, TIFFTAG_JPEGTABLES, &tbl_sz, &tbl_ptr));
      EXPECT_GT(tbl_sz, 0U);
    };
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, 16);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, 16);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 16);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_JPEG);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    GTiffWriteJPEGTables(tif, "MINISBLACK", nullptr, nullptr);
    check_jpeg_tables();

    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
    GTiffWriteJPEGTables(tif, "YCBCR", "85", "1");
    check_jpeg_tables();

    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 5);
    GTiffWriteJPEGTables(tif, "MINISBLACK", "75", "0");
    check_jpeg_tables();

    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    {
      WithQuietHandler quiet;
      GTiffWriteJPEGTables(tif, "YCBCR", "75", "1");
      CPLErrorReset();
    }
    check_jpeg_tables();
  }

  // 2. GTiffWarningHandlerExt & GTiffErrorHandlerExt all branches.
  {
    WithQuietHandler quiet;
    GTIFFGetThreadLocalLibtiffError() = 0;
    EXPECT_EQ(1, CallWarningExt("mod", "Unknown field with tag %d", 999));
    EXPECT_EQ(1,
              CallWarningExt("mod", "ASCII value does not end in null byte"));
    EXPECT_EQ(1, CallWarningExt("mod", "Normal warning %d", 1));

    GTIFFGetThreadLocalLibtiffError() = 1;
    EXPECT_EQ(1, CallWarningExt("mod", "Counted warning"));
    GTIFFGetThreadLocalLibtiffError() = 10;
    EXPECT_EQ(1, CallWarningExt("mod", "Suppressed >10 warning"));

    GTIFFGetThreadLocalLibtiffError() = 0;
    GTIFFSetThreadLocalInExternalOvr(false);
    EXPECT_EQ(1, CallErrorExt("mod", "Maximum TIFF file size exceeded"));
    GTIFFSetThreadLocalInExternalOvr(true);
    EXPECT_EQ(1, CallErrorExt("mod", "Maximum TIFF file size exceeded"));
    GTIFFSetThreadLocalInExternalOvr(false);
    EXPECT_EQ(1, CallErrorExt("mod", "Generic error %s", "msg"));

    GTIFFGetThreadLocalLibtiffError() = 1;
    EXPECT_EQ(1, CallErrorExt("mod", "Counted error"));
    GTIFFGetThreadLocalLibtiffError() = 10;
    EXPECT_EQ(1, CallErrorExt("mod", "Suppressed >10 error"));
    GTIFFGetThreadLocalLibtiffError() = 0;
    CPLErrorReset();
  }

  // 3. GTIFFGetCompressionMethodName & GTIFFGetCompressionMethod.
  EXPECT_STREQ("NONE", GTIFFGetCompressionMethodName(COMPRESSION_NONE));
  EXPECT_STREQ("DEFLATE",
               GTIFFGetCompressionMethodName(COMPRESSION_ADOBE_DEFLATE));
  EXPECT_EQ(nullptr, GTIFFGetCompressionMethodName(-99999));

  EXPECT_EQ(COMPRESSION_NONE, GTIFFGetCompressionMethod("NONE", "COMPRESS"));
  EXPECT_EQ(COMPRESSION_ADOBE_DEFLATE,
            GTIFFGetCompressionMethod("DEFLATE", "COMPRESS"));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(COMPRESSION_NONE,
              GTIFFGetCompressionMethod("BOGUS_CODEC", "COMPRESS"));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Temporarily override a codec with nullptr init to hit
  // !TIFFIsCODECConfigured, then immediately unregister the override via RAII.
  {
    TIFFCodec* dummy =
        TIFFRegisterCODEC(COMPRESSION_CCITTRLE, "CCITTRLE", nullptr);
    ASSERT_NE(nullptr, dummy);
    absl::Cleanup unreg = [dummy]() { TIFFUnRegisterCODEC(dummy); };
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(-1, GTIFFGetCompressionMethod("CCITTRLE", "COMPRESS"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 4. GDALGetSubdatasetInfo -> GTiffDriverGetSubdatasetInfo / parseFileName.
  {
    auto verify_subdataset = [](const char* input, const char* expected_path,
                                const char* expected_sub) {
      GDALSubdatasetInfoH info = GDALGetSubdatasetInfo(input);
      ASSERT_NE(nullptr, info);
      absl::Cleanup destroy_info = [info]() {
        GDALDestroySubdatasetInfo(info);
      };
      std::unique_ptr<char, CplFreeDeleter> path(
          GDALSubdatasetInfoGetPathComponent(info));
      std::unique_ptr<char, CplFreeDeleter> sub(
          GDALSubdatasetInfoGetSubdatasetComponent(info));
      EXPECT_STREQ(expected_path, path.get());
      EXPECT_STREQ(expected_sub, sub.get());
    };

    ASSERT_NO_FATAL_FAILURE(verify_subdataset("GTIFF_DIR:1:/vsimem/foo.tif",
                                              "/vsimem/foo.tif", "1"));
    // Windows drive letter: the "C" token is re-joined with the path.
    ASSERT_NO_FATAL_FAILURE(verify_subdataset("GTIFF_DIR:2:C:\\path\\foo.tif",
                                              "C:\\path\\foo.tif", "2"));

    // Invalid subdataset strings -> nullptr.
    {
      WithQuietHandler quiet;
      EXPECT_EQ(nullptr, GDALGetSubdatasetInfo("GTIFF_DIR:1"));
      EXPECT_EQ(nullptr, GDALGetSubdatasetInfo("GTIFF_DIR:1:C"));
      EXPECT_EQ(nullptr, GDALGetSubdatasetInfo("GTIFF_DIR:1:notdrive:foo.tif"));
      EXPECT_EQ(nullptr, GDALGetSubdatasetInfo("GTIFF_DIR:1:2:3:4"));
      CPLErrorReset();
    }
  }
  CPLErrorReset();
}

}  // namespace
}  // namespace autotest2
