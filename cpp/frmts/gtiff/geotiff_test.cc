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
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/frmts/gtiff/gtiff.h"
#include "third_party/gdal/frmts/gtiff/tifvsi.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdalsubdatasetinfo.h"
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

int CallWarningExt(const char* module, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int rc = GTiffWarningHandlerExt(nullptr, nullptr, module, fmt, ap);
  va_end(ap);
  return rc;
}

int CallErrorExt(const char* module, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int rc = GTiffErrorHandlerExt(nullptr, nullptr, module, fmt, ap);
  va_end(ap);
  return rc;
}

TEST(GeoTiffTest, OneTimeInitAndDriverRegistrationLifecycle) {
  LibgeotiffOneTimeInit();
  GTiffOneTimeInit();
  GTiffOneTimeInit();  // second call hits bOneTimeInitDone early return

  GDALAllRegister();
  GDALDriver* drv = static_cast<GDALDriver*>(GDALGetDriverByName("GTiff"));
  ASSERT_NE(drv, nullptr);
  // Early return when GTiff is already registered.
  GDALRegister_GTiff();
  EXPECT_EQ(GDALGetDriverByName("GTiff"), drv);

  // NOTE: GDALDeregister_GTiff() is deliberately not exercised here. Deleting
  // the driver unregisters the JXL libtiff codecs, and GTiffOneTimeInit()'s
  // static guard means they would never be re-registered for the rest of the
  // process, breaking any later JXL test in this binary.
  EXPECT_TRUE(TIFFIsCODECConfigured(COMPRESSION_JXL));

  // Check compression values for both COG and non-COG.
  bool lzw = false, deflate = false, lzma = false, zstd = false;
  bool jpeg = false, webp = false, lerc = false;
  CPLString xml_gtiff = GTiffGetCompressValues(lzw, deflate, lzma, zstd, jpeg,
                                               webp, lerc, /*bForCOG=*/false);
  EXPECT_NE(xml_gtiff.find("PACKBITS"), std::string::npos);
  EXPECT_NE(xml_gtiff.find("LERC_ZSTD"), std::string::npos);
  CPLString xml_cog = GTiffGetCompressValues(lzw, deflate, lzma, zstd, jpeg,
                                             webp, lerc, /*bForCOG=*/true);
  EXPECT_EQ(xml_cog.find("PACKBITS"), std::string::npos);
}

TEST(GeoTiffTest, PredictorOverviewBlockSizeAndCodecSetters) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  GDALAllRegister();
  GDALDriver* drv = static_cast<GDALDriver*>(GDALGetDriverByName("GTiff"));
  ASSERT_NE(drv, nullptr);

  EXPECT_TRUE(GTIFFSupportsPredictor(COMPRESSION_LZW));
  EXPECT_TRUE(GTIFFSupportsPredictor(COMPRESSION_ADOBE_DEFLATE));
  EXPECT_TRUE(GTIFFSupportsPredictor(COMPRESSION_ZSTD));
  EXPECT_FALSE(GTIFFSupportsPredictor(COMPRESSION_NONE));

  constexpr char kTiled[] = "/vsimem/geotiff_bs_tiled.tif";
  constexpr char kStripped[] = "/vsimem/geotiff_bs_strip.tif";
  absl::Cleanup c = [&]() {
    VSIUnlink(kTiled);
    VSIUnlink(kStripped);
  };

  char** topts = nullptr;
  topts = CSLSetNameValue(topts, "TILED", "YES");
  topts = CSLSetNameValue(topts, "BLOCKXSIZE", "256");
  topts = CSLSetNameValue(topts, "BLOCKYSIZE", "256");
  GDALDataset* ds_tiled = drv->Create(kTiled, 256, 256, 1, GDT_Byte, topts);
  CSLDestroy(topts);
  ASSERT_NE(ds_tiled, nullptr);

  int bx = 0, by = 0;
  // 1. From band block size (valid 256x256 power of 2).
  GTIFFGetOverviewBlockSize(
      GDALRasterBand::ToHandle(ds_tiled->GetRasterBand(1)), &bx, &by, nullptr,
      nullptr);
  EXPECT_EQ(bx, 256);
  EXPECT_EQ(by, 256);

  // 2. From option list (valid 512, and invalid 100 -> defaults to 128).
  char** bopts = CSLSetNameValue(nullptr, "BLOCKSIZE", "512");
  GTIFFGetOverviewBlockSize(
      GDALRasterBand::ToHandle(ds_tiled->GetRasterBand(1)), &bx, &by, bopts,
      "BLOCKSIZE");
  EXPECT_EQ(bx, 512);
  CSLDestroy(bopts);

  char** bad_opts = CSLSetNameValue(nullptr, "BLOCKSIZE", "100");
  GTIFFGetOverviewBlockSize(
      GDALRasterBand::ToHandle(ds_tiled->GetRasterBand(1)), &bx, &by, bad_opts,
      "BLOCKSIZE");
  EXPECT_EQ(bx, 128);
  CSLDestroy(bad_opts);

  // 3. From GDAL_TIFF_OVR_BLOCKSIZE config option.
  {
    CPLConfigOptionSetter cfg("GDAL_TIFF_OVR_BLOCKSIZE", "64", false);
    GTIFFGetOverviewBlockSize(
        GDALRasterBand::ToHandle(ds_tiled->GetRasterBand(1)), &bx, &by, nullptr,
        "BLOCKSIZE");
    EXPECT_EQ(bx, 64);
  }

  // 4. Band fallback on stripped non-square band -> defaults to 128x128.
  GDALDataset* ds_strip =
      drv->Create(kStripped, 100, 100, 1, GDT_Byte, nullptr);
  ASSERT_NE(ds_strip, nullptr);
  GTIFFGetOverviewBlockSize(
      GDALRasterBand::ToHandle(ds_strip->GetRasterBand(1)), &bx, &by, nullptr,
      nullptr);
  EXPECT_EQ(bx, 128);
  EXPECT_EQ(by, 128);
  GDALClose(ds_strip);

  // Build 1 internal overview on ds_tiled so all 11 GTIFFSet* functions iterate
  // over m_papoOverviewDS[i]!
  int ovr_level = 2;
  ASSERT_EQ(ds_tiled->BuildOverviews("NEAREST", 1, &ovr_level, 0, nullptr,
                                     nullptr, nullptr),
            CE_None);
  GDALDatasetH hDS = GDALDataset::ToHandle(ds_tiled);
  GTIFFSetJpegQuality(hDS, 85);
  GTIFFSetWebPLevel(hDS, 80);
  GTIFFSetWebPLossless(hDS, true);
  GTIFFSetJpegTablesMode(hDS, 1);
  GTIFFSetZLevel(hDS, 6);
  GTIFFSetZSTDLevel(hDS, 9);
  GTIFFSetMaxZError(hDS, 0.25);
  GTIFFSetJXLLossless(hDS, false);
  GTIFFSetJXLEffort(hDS, 4);
  GTIFFSetJXLDistance(hDS, 1.5f);
  GTIFFSetJXLAlphaDistance(hDS, 0.5f);

  GDALClose(ds_tiled);
}

TEST(GeoTiffTest, AlphaColorInterpPhotometricAndNoDataHelpers) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  GDALAllRegister();

  EXPECT_EQ(GTiffGetAlphaValue(nullptr, 42), 42);
  EXPECT_EQ(GTiffGetAlphaValue("YES", 0), DEFAULT_ALPHA_TYPE);
  EXPECT_EQ(GTiffGetAlphaValue("PREMULTIPLIED", 0), EXTRASAMPLE_ASSOCALPHA);
  EXPECT_EQ(GTiffGetAlphaValue("NON-PREMULTIPLIED", 0), EXTRASAMPLE_UNASSALPHA);
  EXPECT_EQ(GTiffGetAlphaValue("NO", 0), EXTRASAMPLE_UNSPECIFIED);
  EXPECT_EQ(GTiffGetAlphaValue("UNSPECIFIED", 0), EXTRASAMPLE_UNSPECIFIED);
  EXPECT_EQ(GTiffGetAlphaValue("UNKNOWN_VAL", 77), 77);

  EXPECT_STREQ(
      GTiffFormatGDALNoDataTagValue(std::numeric_limits<double>::quiet_NaN())
          .c_str(),
      "nan");
  EXPECT_STREQ(GTiffFormatGDALNoDataTagValue(-9999.0).c_str(), "-9999");

  // GTIFFIsStandardColorInterpretation across photometrics.
  GDALDriver* mem_drv = static_cast<GDALDriver*>(GDALGetDriverByName("MEM"));
  ASSERT_NE(mem_drv, nullptr);

  // MINISBLACK: standard (Gray + Alpha) vs non-standard (Red).
  {
    GDALDataset* mds = mem_drv->Create("", 2, 2, 2, GDT_Byte, nullptr);
    mds->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex);
    mds->GetRasterBand(2)->SetColorInterpretation(GCI_AlphaBand);
    EXPECT_TRUE(GTIFFIsStandardColorInterpretation(
        GDALDataset::ToHandle(mds), PHOTOMETRIC_MINISBLACK, nullptr));
    mds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    EXPECT_FALSE(GTIFFIsStandardColorInterpretation(
        GDALDataset::ToHandle(mds), PHOTOMETRIC_MINISBLACK, nullptr));
    // PALETTE.
    mds->GetRasterBand(1)->SetColorInterpretation(GCI_PaletteIndex);
    EXPECT_TRUE(GTIFFIsStandardColorInterpretation(
        GDALDataset::ToHandle(mds), PHOTOMETRIC_PALETTE, nullptr));
    // Unsupported photometric -> false.
    EXPECT_FALSE(GTIFFIsStandardColorInterpretation(
        GDALDataset::ToHandle(mds), PHOTOMETRIC_CIELAB, nullptr));
    GDALClose(mds);
  }

  // RGB & YCBCR.
  {
    GDALDataset* mds = mem_drv->Create("", 2, 2, 4, GDT_Byte, nullptr);
    mds->GetRasterBand(1)->SetColorInterpretation(GCI_RedBand);
    mds->GetRasterBand(2)->SetColorInterpretation(GCI_GreenBand);
    mds->GetRasterBand(3)->SetColorInterpretation(GCI_BlueBand);
    mds->GetRasterBand(4)->SetColorInterpretation(GCI_AlphaBand);
    EXPECT_TRUE(GTIFFIsStandardColorInterpretation(GDALDataset::ToHandle(mds),
                                                   PHOTOMETRIC_RGB, nullptr));

    mds->GetRasterBand(1)->SetColorInterpretation(GCI_GrayIndex);
    EXPECT_FALSE(GTIFFIsStandardColorInterpretation(GDALDataset::ToHandle(mds),
                                                    PHOTOMETRIC_RGB, nullptr));

    // With PHOTOMETRIC=RGB and ALPHA=YES in creation options -> skips
    // bands 1..4!
    char** copts = nullptr;
    copts = CSLSetNameValue(copts, "PHOTOMETRIC", "RGB");
    copts = CSLSetNameValue(copts, "ALPHA", "YES");
    EXPECT_TRUE(GTIFFIsStandardColorInterpretation(GDALDataset::ToHandle(mds),
                                                   PHOTOMETRIC_RGB, copts));
    CSLDestroy(copts);
    GDALClose(mds);

    GDALDataset* yds = mem_drv->Create("", 2, 2, 3, GDT_Byte, nullptr);
    EXPECT_TRUE(GTIFFIsStandardColorInterpretation(GDALDataset::ToHandle(yds),
                                                   PHOTOMETRIC_YCBCR, nullptr));
    GDALClose(yds);
  }

  // GTIFFUpdatePhotometric all branches.
  uint16_t photo = PHOTOMETRIC_MINISBLACK;
  uint16_t planar = PLANARCONFIG_CONTIG;
  EXPECT_TRUE(GTIFFUpdatePhotometric(nullptr, "PHOTOMETRIC", COMPRESSION_NONE,
                                     nullptr, 1, photo, planar));
  for (const char* name : {"MINISBLACK", "MINISWHITE", "RGB", "CMYK", "CIELAB",
                           "ICCLAB", "ITULAB", "BOGUS_VALUE"}) {
    EXPECT_TRUE(GTIFFUpdatePhotometric(name, "PHOTOMETRIC", COMPRESSION_NONE,
                                       nullptr, 3, photo, planar));
  }
  // YCBCR error & success cases.
  EXPECT_FALSE(GTIFFUpdatePhotometric("YCBCR", "PHOTOMETRIC", COMPRESSION_NONE,
                                      nullptr, 3, photo, planar));
  planar = PLANARCONFIG_SEPARATE;
  EXPECT_FALSE(GTIFFUpdatePhotometric("YCBCR", "PHOTOMETRIC", COMPRESSION_JPEG,
                                      "BAND", 3, photo, planar));
  planar = PLANARCONFIG_CONTIG;
  EXPECT_FALSE(GTIFFUpdatePhotometric("YCBCR", "PHOTOMETRIC", COMPRESSION_JPEG,
                                      "PIXEL", 4, photo, planar));
  EXPECT_TRUE(GTIFFUpdatePhotometric("YCBCR", "PHOTOMETRIC", COMPRESSION_JPEG,
                                     "PIXEL", 3, photo, planar));
  EXPECT_EQ(photo, PHOTOMETRIC_YCBCR);
}

TEST(GeoTiffTest, RPCTagJPEGTablesErrorHandlersCompressionAndSubdatasetInfo) {
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  GDALAllRegister();
  GTiffOneTimeInit();

  // 1. RPC tag write & read + JPEGTables helper on in-memory TIFF.
  {
    constexpr char kFile[] = "/vsimem/geotiff_rpc_jpeg.tif";
    absl::Cleanup c = [&]() { VSIUnlink(kFile); };
    VSILFILE* fp = VSIFOpenL(kFile, "wb+");
    ASSERT_NE(fp, nullptr);
    TIFF* tif = VSI_TIFFOpen(kFile, "w+", fp);
    ASSERT_NE(tif, nullptr);

    // Invalid RPC metadata -> early return; read before tag set -> nullptr.
    GTiffDatasetWriteRPCTag(tif, nullptr);
    EXPECT_EQ(GTiffDatasetReadRPCTag(tif), nullptr);

    // Valid RPC metadata -> writes 92 doubles -> reads back.
    CPLString coeffs = "1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0";
    char** rpc = nullptr;
    rpc = CSLSetNameValue(rpc, "ERR_BIAS", "1.5");
    rpc = CSLSetNameValue(rpc, "ERR_RAND", "0.5");
    rpc = CSLSetNameValue(rpc, "LINE_OFF", "100");
    rpc = CSLSetNameValue(rpc, "SAMP_OFF", "200");
    rpc = CSLSetNameValue(rpc, "LAT_OFF", "37.5");
    rpc = CSLSetNameValue(rpc, "LONG_OFF", "-122.0");
    rpc = CSLSetNameValue(rpc, "HEIGHT_OFF", "50");
    rpc = CSLSetNameValue(rpc, "LINE_SCALE", "100");
    rpc = CSLSetNameValue(rpc, "SAMP_SCALE", "200");
    rpc = CSLSetNameValue(rpc, "LAT_SCALE", "1");
    rpc = CSLSetNameValue(rpc, "LONG_SCALE", "1");
    rpc = CSLSetNameValue(rpc, "HEIGHT_SCALE", "100");
    rpc = CSLSetNameValue(rpc, "LINE_NUM_COEFF", coeffs.c_str());
    rpc = CSLSetNameValue(rpc, "LINE_DEN_COEFF", coeffs.c_str());
    rpc = CSLSetNameValue(rpc, "SAMP_NUM_COEFF", coeffs.c_str());
    rpc = CSLSetNameValue(rpc, "SAMP_DEN_COEFF", coeffs.c_str());
    GTiffDatasetWriteRPCTag(tif, rpc);
    CSLDestroy(rpc);

    char** read_rpc = GTiffDatasetReadRPCTag(tif);
    ASSERT_NE(read_rpc, nullptr);
    // Values are re-serialized with "%.15g", so integral inputs round-trip
    // verbatim.
    EXPECT_STREQ(CSLFetchNameValue(read_rpc, "LINE_OFF"), "100");
    EXPECT_STREQ(CSLFetchNameValue(read_rpc, "SAMP_OFF"), "200");
    EXPECT_STREQ(CSLFetchNameValue(read_rpc, "LAT_OFF"), "37.5");
    EXPECT_STREQ(CSLFetchNameValue(read_rpc, "ERR_BIAS"), "1.5");
    EXPECT_STREQ(CSLFetchNameValue(read_rpc, "LINE_NUM_COEFF"),
                 coeffs.c_str());
    CSLDestroy(read_rpc);

    // GTiffWriteJPEGTables:
    // Case A: before setting SAMPLESPERPIXEL / BITSPERSAMPLE (defaults to 1,1).
    GTiffWriteJPEGTables(tif, "MINISBLACK", nullptr, nullptr);
    // Case B: 3-band 8-bit YCBCR with quality "85" and tablesmode "1".
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    GTiffWriteJPEGTables(tif, "YCBCR", "85", "1");
    // Case C: 5-band (>4 bands branch) and 1-band YCBCR (CreateLL fails).
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 5);
    GTiffWriteJPEGTables(tif, "MINISBLACK", "75", "0");
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    GTiffWriteJPEGTables(tif, "YCBCR", "75", "1");

    TIFFClose(tif);
    VSIFCloseL(fp);
  }

  // 2. GTiffWarningHandlerExt & GTiffErrorHandlerExt all branches.
  {
    GTIFFGetThreadLocalLibtiffError() = 0;
    EXPECT_EQ(CallWarningExt("mod", "Unknown field with tag %d", 999), 1);
    EXPECT_EQ(CallWarningExt("mod", "ASCII value does not end in null byte"),
              1);
    EXPECT_EQ(CallWarningExt("mod", "Normal warning %d", 1), 1);

    GTIFFGetThreadLocalLibtiffError() = 1;
    EXPECT_EQ(CallWarningExt("mod", "Counted warning"), 1);
    GTIFFGetThreadLocalLibtiffError() = 10;
    EXPECT_EQ(CallWarningExt("mod", "Suppressed >10 warning"), 1);

    GTIFFGetThreadLocalLibtiffError() = 0;
    GTIFFSetThreadLocalInExternalOvr(false);
    EXPECT_EQ(CallErrorExt("mod", "Maximum TIFF file size exceeded"), 1);
    GTIFFSetThreadLocalInExternalOvr(true);
    EXPECT_EQ(CallErrorExt("mod", "Maximum TIFF file size exceeded"), 1);
    GTIFFSetThreadLocalInExternalOvr(false);
    EXPECT_EQ(CallErrorExt("mod", "Generic error %s", "msg"), 1);

    GTIFFGetThreadLocalLibtiffError() = 1;
    EXPECT_EQ(CallErrorExt("mod", "Counted error"), 1);
    GTIFFGetThreadLocalLibtiffError() = 10;
    EXPECT_EQ(CallErrorExt("mod", "Suppressed >10 error"), 1);
    GTIFFGetThreadLocalLibtiffError() = 0;
  }

  // 3. GTIFFGetCompressionMethodName & GTIFFGetCompressionMethod.
  EXPECT_STREQ(GTIFFGetCompressionMethodName(COMPRESSION_NONE), "NONE");
  EXPECT_STREQ(GTIFFGetCompressionMethodName(COMPRESSION_ADOBE_DEFLATE),
               "DEFLATE");
  EXPECT_EQ(GTIFFGetCompressionMethodName(-99999), nullptr);

  EXPECT_EQ(GTIFFGetCompressionMethod("NONE", "COMPRESS"), COMPRESSION_NONE);
  EXPECT_EQ(GTIFFGetCompressionMethod("DEFLATE", "COMPRESS"),
            COMPRESSION_ADOBE_DEFLATE);
  EXPECT_EQ(GTIFFGetCompressionMethod("BOGUS_CODEC", "COMPRESS"),
            COMPRESSION_NONE);

  // Temporarily override a codec with nullptr init to hit
  // !TIFFIsCODECConfigured at L1042-1049, then immediately unregister the
  // override!
  TIFFCodec* dummy =
      TIFFRegisterCODEC(COMPRESSION_CCITTRLE, "CCITTRLE", nullptr);
  EXPECT_EQ(GTIFFGetCompressionMethod("CCITTRLE", "COMPRESS"), -1);
  TIFFUnRegisterCODEC(dummy);

  // 4. GDALGetSubdatasetInfo -> GTiffDriverGetSubdatasetInfo / parseFileName.
  {
    GDALSubdatasetInfoH info_posix =
        GDALGetSubdatasetInfo("GTIFF_DIR:1:/vsimem/foo.tif");
    ASSERT_NE(info_posix, nullptr);
    {
      char* path = GDALSubdatasetInfoGetPathComponent(info_posix);
      char* sub = GDALSubdatasetInfoGetSubdatasetComponent(info_posix);
      EXPECT_STREQ(path, "/vsimem/foo.tif");
      EXPECT_STREQ(sub, "1");
      CPLFree(path);
      CPLFree(sub);
    }
    GDALDestroySubdatasetInfo(info_posix);

    // Windows drive letter: the "C" token is re-joined with the rest of the
    // path.
    GDALSubdatasetInfoH info_win =
        GDALGetSubdatasetInfo("GTIFF_DIR:2:C:\\path\\foo.tif");
    ASSERT_NE(info_win, nullptr);
    {
      char* path = GDALSubdatasetInfoGetPathComponent(info_win);
      char* sub = GDALSubdatasetInfoGetSubdatasetComponent(info_win);
      EXPECT_STREQ(path, "C:\\path\\foo.tif");
      EXPECT_STREQ(sub, "2");
      CPLFree(path);
      CPLFree(sub);
    }
    GDALDestroySubdatasetInfo(info_win);

    // Invalid subdataset strings -> nullptr.
    EXPECT_EQ(GDALGetSubdatasetInfo("GTIFF_DIR:1"), nullptr);
    EXPECT_EQ(GDALGetSubdatasetInfo("GTIFF_DIR:1:C"), nullptr);
    EXPECT_EQ(GDALGetSubdatasetInfo("GTIFF_DIR:1:notdrive:foo.tif"), nullptr);
    EXPECT_EQ(GDALGetSubdatasetInfo("GTIFF_DIR:1:2:3:4"), nullptr);
  }
}

}  // namespace
}  // namespace autotest2
