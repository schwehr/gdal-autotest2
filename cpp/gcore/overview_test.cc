// Tests gcore/overview.cpp.
//
// Copyright 2026 Google Inc. All Rights Reserved.
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
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_cpp_functions.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_float.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

struct ScopedConfigOption {
  const char* key;
  ScopedConfigOption(const char* k, const char* v) : key(k) {
    CPLSetThreadLocalConfigOption(key, v);
  }
  ~ScopedConfigOption() { CPLSetThreadLocalConfigOption(key, nullptr); }
};

struct QuietErrorHandler {
  QuietErrorHandler() { CPLPushErrorHandler(CPLQuietErrorHandler); }
  ~QuietErrorHandler() { CPLPopErrorHandler(); }
};

class OverviewTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { GDALAllRegister(); }

  static std::unique_ptr<GDALDataset> CreateMemDS(int width, int height,
                                                  int bands, GDALDataType dt) {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
    return std::unique_ptr<GDALDataset>(
        drv->Create("", width, height, bands, dt, nullptr));
  }
};

int CPL_STDCALL CancelAtStartProgress(double /*dfComplete*/,
                                      const char* /*pszMessage*/,
                                      void* /*pProgressArg*/) {
  return FALSE;
}

int CPL_STDCALL CancelAtEndProgress(double dfComplete,
                                    const char* /*pszMessage*/,
                                    void* /*pProgressArg*/) {
  return dfComplete >= 1.0 ? FALSE : TRUE;
}

int CPL_STDCALL CancelOnNthCallProgress(double /*dfComplete*/,
                                        const char* /*pszMessage*/,
                                        void* pProgressArg) {
  int* remaining = static_cast<int*>(pProgressArg);
  return (--(*remaining) <= 0) ? FALSE : TRUE;
}

TEST_F(OverviewTest, GetResampleFunctionAndWorkDataType) {
  int radius = -1;
  EXPECT_NE(nullptr, GDALGetResampleFunction("NEAREST", &radius));
  EXPECT_EQ(0, radius);
  EXPECT_NE(nullptr, GDALGetResampleFunction("NEAR", nullptr));
  EXPECT_NE(nullptr, GDALGetResampleFunction("AVERAGE", &radius));
  EXPECT_EQ(0, radius);
  EXPECT_NE(nullptr, GDALGetResampleFunction("AVERAGE_BIT2GRAYSCALE", &radius));
  EXPECT_NE(nullptr, GDALGetResampleFunction("RMS", &radius));
  EXPECT_EQ(0, radius);
  EXPECT_NE(nullptr, GDALGetResampleFunction("GAUSS", &radius));
  EXPECT_EQ(1, radius);
  EXPECT_NE(nullptr, GDALGetResampleFunction("GAUSS", nullptr));
  EXPECT_NE(nullptr, GDALGetResampleFunction("MODE", &radius));
  EXPECT_EQ(0, radius);
  EXPECT_NE(nullptr, GDALGetResampleFunction("CUBIC", &radius));
  EXPECT_GT(radius, 0);
  EXPECT_NE(nullptr, GDALGetResampleFunction("CUBIC", nullptr));
  EXPECT_NE(nullptr, GDALGetResampleFunction("CUBICSPLINE", &radius));
  EXPECT_GT(radius, 0);
  EXPECT_NE(nullptr, GDALGetResampleFunction("CUBICSPLINE", nullptr));
  EXPECT_NE(nullptr, GDALGetResampleFunction("LANCZOS", &radius));
  EXPECT_GT(radius, 0);
  EXPECT_NE(nullptr, GDALGetResampleFunction("LANCZOS", nullptr));
  EXPECT_NE(nullptr, GDALGetResampleFunction("BILINEAR", &radius));
  EXPECT_GT(radius, 0);
  EXPECT_NE(nullptr, GDALGetResampleFunction("BILINEAR", nullptr));

  {
    QuietErrorHandler quiet;
    EXPECT_EQ(nullptr, GDALGetResampleFunction("UNSUPPORTED_METHOD", &radius));
  }

  // Test GDALGetOvrWorkDataType across methods and types.
  EXPECT_EQ(GDT_Int16, GDALGetOvrWorkDataType("NEAREST", GDT_Int16));
  EXPECT_EQ(GDT_Int32, GDALGetOvrWorkDataType("MODE", GDT_Int32));
  EXPECT_EQ(GDT_Byte, GDALGetOvrWorkDataType("AVERAGE", GDT_Byte));
  EXPECT_EQ(GDT_Byte, GDALGetOvrWorkDataType("RMS", GDT_Byte));
  EXPECT_EQ(GDT_Byte, GDALGetOvrWorkDataType("CUBIC", GDT_Byte));
  EXPECT_EQ(GDT_Byte, GDALGetOvrWorkDataType("CUBICSPLINE", GDT_Byte));
  EXPECT_EQ(GDT_Byte, GDALGetOvrWorkDataType("LANCZOS", GDT_Byte));
  EXPECT_EQ(GDT_Byte, GDALGetOvrWorkDataType("BILINEAR", GDT_Byte));

  EXPECT_EQ(GDT_UInt16, GDALGetOvrWorkDataType("AVERAGE", GDT_UInt16));
  EXPECT_EQ(GDT_UInt16, GDALGetOvrWorkDataType("RMS", GDT_UInt16));
  EXPECT_EQ(GDT_UInt16, GDALGetOvrWorkDataType("CUBIC", GDT_UInt16));
  EXPECT_EQ(GDT_UInt16, GDALGetOvrWorkDataType("CUBICSPLINE", GDT_UInt16));
  EXPECT_EQ(GDT_UInt16, GDALGetOvrWorkDataType("LANCZOS", GDT_UInt16));
  EXPECT_EQ(GDT_UInt16, GDALGetOvrWorkDataType("BILINEAR", GDT_UInt16));

  EXPECT_EQ(GDT_Float64, GDALGetOvrWorkDataType("GAUSS", GDT_Byte));
  EXPECT_EQ(GDT_Float64, GDALGetOvrWorkDataType("GAUSS", GDT_UInt16));
  EXPECT_EQ(GDT_Float64, GDALGetOvrWorkDataType("GAUSS", GDT_Float32));

  EXPECT_EQ(GDT_Float32, GDALGetOvrWorkDataType("AVERAGE", GDT_Int8));
  EXPECT_EQ(GDT_Float32, GDALGetOvrWorkDataType("AVERAGE", GDT_Int16));
  EXPECT_EQ(GDT_Float32, GDALGetOvrWorkDataType("AVERAGE", GDT_Float32));
  EXPECT_EQ(GDT_Float64, GDALGetOvrWorkDataType("AVERAGE", GDT_Int32));
  EXPECT_EQ(GDT_Float64, GDALGetOvrWorkDataType("AVERAGE", GDT_UInt32));
  EXPECT_EQ(GDT_Float64, GDALGetOvrWorkDataType("AVERAGE", GDT_Int64));
  EXPECT_EQ(GDT_Float64, GDALGetOvrWorkDataType("AVERAGE", GDT_UInt64));
  EXPECT_EQ(GDT_Float64, GDALGetOvrWorkDataType("AVERAGE", GDT_Float64));
}

TEST_F(OverviewTest, NearestAllDataTypesAndChunkOffsets) {
  const GDALDataType kTypes[] = {
      GDT_Byte,    GDT_Int8,    GDT_Int16,    GDT_UInt16,
      GDT_Float16, GDT_CInt16,  GDT_CFloat16, GDT_Int32,
      GDT_UInt32,  GDT_Float32, GDT_CInt32,   GDT_CFloat32,
      GDT_Int64,   GDT_UInt64,  GDT_Float64,  GDT_CFloat64};

  for (GDALDataType dt : kTypes) {
    auto src_ds = CreateMemDS(8, 8, 1, dt);
    auto ovr_ds = CreateMemDS(4, 4, 1, dt);
    ASSERT_NE(nullptr, src_ds);
    ASSERT_NE(nullptr, ovr_ds);
    GDALFillRaster(src_ds->GetRasterBand(1), 42.0, 7.0);
    GDALRasterBandH ovr_band = ovr_ds->GetRasterBand(1);
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &ovr_band,
                                      "NEAREST", nullptr, nullptr));
    double real_val = 0.0;
    EXPECT_EQ(CE_None, ovr_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 1, 1, &real_val, 1, 1, GDT_Float64, 0,
                           0, nullptr));
    EXPECT_NEAR(42.0, real_val, 1e-5);
  }

  // Exercise nSrcXOff < nChunkXOff and nSrcYOff < nChunkYOff in NearT directly.
  GDALResampleFunction fn = GDALGetResampleFunction("NEAREST", nullptr);
  ASSERT_NE(nullptr, fn);
  GDALOverviewResampleArgs args;
  args.eSrcDataType = GDT_Byte;
  args.eOvrDataType = GDT_Byte;
  args.eWrkDataType = GDT_Byte;
  args.nOvrXSize = 4;
  args.nOvrYSize = 4;
  args.dfXRatioDstToSrc = 2.0;
  args.dfYRatioDstToSrc = 2.0;
  args.nChunkXOff = 2;
  args.nChunkXSize = 4;
  args.nChunkYOff = 2;
  args.nChunkYSize = 4;
  args.nDstXOff = 0;
  args.nDstXOff2 = 2;
  args.nDstYOff = 0;
  args.nDstYOff2 = 2;
  args.pszResampling = "NEAREST";

  std::vector<uint8_t> chunk(16, 99);
  void* dst_buf = nullptr;
  GDALDataType dst_dt = GDT_Unknown;
  EXPECT_EQ(CE_None, fn(args, chunk.data(), &dst_buf, &dst_dt));
  EXPECT_EQ(GDT_Byte, dst_dt);
  ASSERT_NE(nullptr, dst_buf);
  EXPECT_EQ(99, static_cast<uint8_t*>(dst_buf)[0]);
  CPLFree(dst_buf);
}

TEST_F(OverviewTest, AverageAndRmsFast2x2SSE2AndRemainder) {
  // Use 70x4 -> 35x2 so that SSE2/AVX2 loops execute multiple iterations
  // (8, 16, or 32 pixels) and scalar remainder loop also executes (35 % 8 !=
  // 0).
  const int src_w = 70;
  const int src_h = 4;
  const int dst_w = 35;
  const int dst_h = 2;

  // 1. GDT_Byte: AVERAGE and RMS
  {
    auto src_ds = CreateMemDS(src_w, src_h, 1, GDT_Byte);
    auto ovr_avg = CreateMemDS(dst_w, dst_h, 1, GDT_Byte);
    auto ovr_rms = CreateMemDS(dst_w, dst_h, 1, GDT_Byte);
    std::vector<GByte> buf(src_w * src_h);
    for (int i = 0; i < src_w * src_h; ++i) {
      buf[i] = static_cast<GByte>((i * 17 + 3) % 256);
    }
    ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, src_w, src_h, buf.data(), src_w,
                           src_h, GDT_Byte, 0, 0, nullptr));
    GDALRasterBandH h_avg = ovr_avg->GetRasterBand(1);
    GDALRasterBandH h_rms = ovr_rms->GetRasterBand(1);
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_avg,
                                      "AVERAGE", nullptr, nullptr));
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_rms,
                                      "RMS", nullptr, nullptr));
  }

  // 2. GDT_UInt16: AVERAGE and RMS
  // First half of scanline has values < 16384 (fits in 14 bits fast path),
  // second half has values >= 16384 (e.g. 40000..65000, full double SSE2 path)
  // plus remainder pixels that test ComputeIntegerRMS_4values<GUInt16, double>
  // both with and without rounding increment.
  {
    auto src_ds = CreateMemDS(src_w, src_h, 1, GDT_UInt16);
    auto ovr_avg = CreateMemDS(dst_w, dst_h, 1, GDT_UInt16);
    auto ovr_rms = CreateMemDS(dst_w, dst_h, 1, GDT_UInt16);
    std::vector<GUInt16> buf(src_w * src_h);
    for (int y = 0; y < src_h; ++y) {
      for (int x = 0; x < src_w; ++x) {
        if (x < 32) {
          buf[y * src_w + x] = static_cast<GUInt16>((x * 100 + y * 7) % 10000);
        } else {
          buf[y * src_w + x] =
              static_cast<GUInt16>(30000 + ((x * 503 + y * 19) % 35000));
        }
      }
    }
    ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, src_w, src_h, buf.data(), src_w,
                           src_h, GDT_UInt16, 0, 0, nullptr));
    GDALRasterBandH h_avg = ovr_avg->GetRasterBand(1);
    GDALRasterBandH h_rms = ovr_rms->GetRasterBand(1);
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_avg,
                                      "AVERAGE", nullptr, nullptr));
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_rms,
                                      "RMS", nullptr, nullptr));
  }

  // 3. GDT_Float32: AVERAGE and RMS (including 0.0f, +inf, -inf, normal values
  // in both SSE2 and scalar remainder positions).
  {
    auto src_ds = CreateMemDS(src_w, src_h, 1, GDT_Float32);
    auto ovr_avg = CreateMemDS(dst_w, dst_h, 1, GDT_Float32);
    auto ovr_rms = CreateMemDS(dst_w, dst_h, 1, GDT_Float32);
    std::vector<float> buf(src_w * src_h);
    for (int y = 0; y < src_h; ++y) {
      for (int x = 0; x < src_w; ++x) {
        int dst_x = x / 2;
        if (dst_x == 0 || dst_x == 32) {
          buf[y * src_w + x] = 0.0f;
        } else if (dst_x == 1 || dst_x == 33) {
          buf[y * src_w + x] = std::numeric_limits<float>::infinity();
        } else if (dst_x == 2) {
          buf[y * src_w + x] = 1e35f;
        } else {
          buf[y * src_w + x] = static_cast<float>(x * 1.5f + y);
        }
      }
    }
    ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, src_w, src_h, buf.data(), src_w,
                           src_h, GDT_Float32, 0, 0, nullptr));
    GDALRasterBandH h_avg = ovr_avg->GetRasterBand(1);
    GDALRasterBandH h_rms = ovr_rms->GetRasterBand(1);
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_avg,
                                      "AVERAGE", nullptr, nullptr));
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_rms,
                                      "RMS", nullptr, nullptr));
  }

  // 4. GDT_Float64: AVERAGE and RMS (including 0.0, +inf, large values).
  {
    auto src_ds = CreateMemDS(src_w, src_h, 1, GDT_Float64);
    auto ovr_avg = CreateMemDS(dst_w, dst_h, 1, GDT_Float64);
    auto ovr_rms = CreateMemDS(dst_w, dst_h, 1, GDT_Float64);
    std::vector<double> buf(src_w * src_h);
    for (int y = 0; y < src_h; ++y) {
      for (int x = 0; x < src_w; ++x) {
        int dst_x = x / 2;
        if (dst_x == 0 || dst_x == 32) {
          buf[y * src_w + x] = 0.0;
        } else if (dst_x == 1 || dst_x == 33) {
          buf[y * src_w + x] = std::numeric_limits<double>::infinity();
        } else if (dst_x == 2) {
          buf[y * src_w + x] = 1e200;
        } else {
          buf[y * src_w + x] = static_cast<double>(x * 2.5 + y);
        }
      }
    }
    ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, src_w, src_h, buf.data(), src_w,
                           src_h, GDT_Float64, 0, 0, nullptr));
    GDALRasterBandH h_avg = ovr_avg->GetRasterBand(1);
    GDALRasterBandH h_rms = ovr_rms->GetRasterBand(1);
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_avg,
                                      "AVERAGE", nullptr, nullptr));
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_rms,
                                      "RMS", nullptr, nullptr));
  }
}

TEST_F(OverviewTest, AverageAndRmsGeneralRatioAndNoData) {
  // Non-2x ratio (11x11 -> 3x3) exercises middle pixels (nSrcXOff + 1 <
  // nSrcXOff2) and fractional weights in GDALResampleChunk_AverageOrRMS_T, both
  // with and without NoData masks and with GDAL_OVR_PROPAGATE_NODATA.
  const GDALDataType kTypes[] = {GDT_Byte, GDT_UInt16, GDT_Float32,
                                 GDT_Float64};
  const char* kMethods[] = {"AVERAGE", "RMS"};

  for (GDALDataType dt : kTypes) {
    for (const char* method : kMethods) {
      // Without NoData
      {
        auto src_ds = CreateMemDS(11, 11, 1, dt);
        auto ovr_ds = CreateMemDS(3, 3, 1, dt);
        std::vector<double> buf(121);
        for (int i = 0; i < 121; ++i) {
          buf[i] = static_cast<double>((i * 7) % 100);
        }
        ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                               GF_Write, 0, 0, 11, 11, buf.data(), 11, 11,
                               GDT_Float64, 0, 0, nullptr));
        GDALRasterBandH h_ovr = ovr_ds->GetRasterBand(1);
        EXPECT_EQ(CE_None,
                  GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_ovr,
                                          method, nullptr, nullptr));
      }

      // With NoData:
      // - Top-left block all NoData (nCount == 0 -> tNoDataValue)
      // - Center block has valid values whose average/RMS equals tNoDataValue
      //   (triggers nVal == tNoDataValue -> tReplacementVal!)
      // - Bottom-right block has partial NoData (tested with and without
      //   GDAL_OVR_PROPAGATE_NODATA=YES).
      for (bool propagate : {false, true}) {
        ScopedConfigOption opt("GDAL_OVR_PROPAGATE_NODATA",
                               propagate ? "YES" : "NO");
        auto src_ds = CreateMemDS(10, 10, 1, dt);
        auto ovr_ds = CreateMemDS(3, 3, 1, dt);
        const double nodata_val = 50.0;
        src_ds->GetRasterBand(1)->SetNoDataValue(nodata_val);

        std::vector<double> buf(100, 50.0);  // Initialize all to NoData (50).
        // Make middle row/col have values 40 and 60 so average is 50 == NoData!
        // For RMS: 0 and 70 -> sqrt((0 + 4900)/2) = sqrt(2450) = 49.49 -> 50!
        for (int y = 3; y < 7; ++y) {
          for (int x = 0; x < 10; ++x) {
            if (EQUAL(method, "RMS")) {
              buf[y * 10 + x] = (x % 2 == 0) ? 0.0 : 71.0;
            } else {
              buf[y * 10 + x] = (x % 2 == 0) ? 40.0 : 60.0;
            }
          }
        }
        // Bottom-right: mix of NoData (50) and valid (20).
        for (int y = 7; y < 10; ++y) {
          for (int x = 0; x < 10; ++x) {
            buf[y * 10 + x] = (x % 2 == 0) ? 50.0 : 20.0;
          }
        }
        ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                               GF_Write, 0, 0, 10, 10, buf.data(), 10, 10,
                               GDT_Float64, 0, 0, nullptr));
        GDALRasterBandH h_ovr = ovr_ds->GetRasterBand(1);
        EXPECT_EQ(CE_None,
                  GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_ovr,
                                          method, nullptr, nullptr));
      }
    }
  }
}

TEST_F(OverviewTest, PaletteColorTableOverviews) {
  // Test GCI_PaletteIndex bands with AVERAGE, RMS, GAUSS, MODE, and CUBIC:
  // 1. RGB color table with explicit NoData value
  // 2. RGB color table without explicit NoData value, but with transparent
  // entry (c4 == 0)
  // 3. Identity color table
  // 4. Non-RGB color table (GPI_Gray) -> warning
  // 5. PaletteIndex band without color table -> warning
  // 6. CUBIC on PaletteIndex band -> warning
  const char* kMethods[] = {"AVERAGE", "RMS", "GAUSS", "MODE"};

  for (const char* method : kMethods) {
    for (bool explicit_nodata : {true, false}) {
      ScopedConfigOption opt("GDAL_OVR_PROPAGATE_NODATA",
                             explicit_nodata ? "NO" : "YES");
      auto src_ds = CreateMemDS(8, 8, 1, GDT_Byte);
      auto ovr_ds = CreateMemDS(4, 4, 1, GDT_Byte);

      GDALColorTable ct(GPI_RGB);
      const GDALColorEntry e0 = {0, 0, 0, 0};        // Transparent
      const GDALColorEntry e1 = {255, 0, 0, 255};    // Red
      const GDALColorEntry e2 = {0, 255, 0, 255};    // Green
      const GDALColorEntry e3 = {128, 128, 0, 255};  // Olive (midway R+G)
      ct.SetColorEntry(0, &e0);
      ct.SetColorEntry(1, &e1);
      ct.SetColorEntry(2, &e2);
      ct.SetColorEntry(3, &e3);

      src_ds->GetRasterBand(1)->SetColorInterpretation(GCI_PaletteIndex);
      src_ds->GetRasterBand(1)->SetColorTable(&ct);
      if (explicit_nodata) {
        src_ds->GetRasterBand(1)->SetNoDataValue(0.0);
      }

      std::vector<GByte> buf(64, 0);
      // Quadrant 0: all transparent (0)
      // Quadrant 1: mix of 1 (Red) and 2 (Green) -> closest to 3 (Olive)
      for (int y = 0; y < 4; ++y) {
        for (int x = 4; x < 8; ++x) {
          buf[y * 8 + x] = ((x + y) % 2 == 0) ? 1 : 2;
        }
      }
      // Quadrant 2: out-of-range palette indices (e.g. 200 >=
      // colorEntries.size()) mixed with valid entry 1
      for (int y = 4; y < 8; ++y) {
        for (int x = 0; x < 4; ++x) {
          buf[y * 8 + x] = (x % 2 == 0) ? 200 : 1;
        }
      }
      // Quadrant 3: mix of transparent (0) and valid (2)
      for (int y = 4; y < 8; ++y) {
        for (int x = 4; x < 8; ++x) {
          buf[y * 8 + x] = (x % 2 == 0) ? 0 : 2;
        }
      }

      ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 8, 8, buf.data(), 8, 8, GDT_Byte,
                             0, 0, nullptr));
      GDALRasterBandH h_ovr = ovr_ds->GetRasterBand(1);
      EXPECT_EQ(CE_None,
                GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_ovr,
                                        method, nullptr, nullptr));
    }
  }

  // Identity color table (256 entries where c1=c2=c3=i, c4=255)
  {
    auto src_ds = CreateMemDS(8, 8, 1, GDT_Byte);
    auto ovr_ds = CreateMemDS(4, 4, 1, GDT_Byte);
    GDALColorTable ct(GPI_RGB);
    for (int i = 0; i < 256; ++i) {
      const GDALColorEntry entry = {static_cast<short>(i),
                                    static_cast<short>(i),
                                    static_cast<short>(i), 255};
      ct.SetColorEntry(i, &entry);
    }
    src_ds->GetRasterBand(1)->SetColorInterpretation(GCI_PaletteIndex);
    src_ds->GetRasterBand(1)->SetColorTable(&ct);
    GDALFillRaster(src_ds->GetRasterBand(1), 100.0, 0.0);
    GDALRasterBandH h_ovr = ovr_ds->GetRasterBand(1);
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_ovr,
                                      "AVERAGE", nullptr, nullptr));
  }

  // Warnings: non-RGB palette, null palette, and CUBIC on palette band.
  {
    QuietErrorHandler quiet;
    auto src_ds = CreateMemDS(8, 8, 1, GDT_Byte);
    auto ovr_ds = CreateMemDS(4, 4, 1, GDT_Byte);
    src_ds->GetRasterBand(1)->SetColorInterpretation(GCI_PaletteIndex);
    GDALRasterBandH h_ovr = ovr_ds->GetRasterBand(1);

    // Null color table with AVERAGE
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_ovr,
                                      "AVERAGE", nullptr, nullptr));

    // CUBIC on GCI_PaletteIndex
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_ovr,
                                      "CUBIC", nullptr, nullptr));

    // Non-RGB color table (GPI_Gray)
    GDALColorTable gray_ct(GPI_Gray);
    const GDALColorEntry entry = {128, 0, 0, 255};
    gray_ct.SetColorEntry(0, &entry);
    src_ds->GetRasterBand(1)->SetColorTable(&gray_ct);
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_ovr,
                                      "AVERAGE", nullptr, nullptr));
  }
}

TEST_F(OverviewTest, GaussAllMatrixSizesAndNoData) {
  // Factor 2 -> 3x3 Gauss matrix
  // Factor 4 -> 5x5 Gauss matrix
  // Factor 8 -> 7x7 Gauss matrix
  for (int factor : {2, 4, 8}) {
    auto src_ds = CreateMemDS(16, 16, 1, GDT_Float32);
    auto ovr_ds = CreateMemDS(16 / factor, 16 / factor, 1, GDT_Float32);
    src_ds->GetRasterBand(1)->SetNoDataValue(-999.0);

    std::vector<float> buf(256, -999.0f);
    // Leave top-left all NoData, fill second half with valid values
    for (int i = 128; i < 256; ++i) {
      buf[i] = static_cast<float>(i);
    }
    ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 16, 16, buf.data(), 16, 16,
                           GDT_Float32, 0, 0, nullptr));
    GDALRasterBandH h_ovr = ovr_ds->GetRasterBand(1);
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_ovr,
                                      "GAUSS", nullptr, nullptr));
  }

  // Direct call to Gauss with nChunkXOff > 0 and nChunkYOff > 0 to hit
  // nSrcYOff < nChunkYOff and nSrcXOff < nChunkXOff branches.
  GDALResampleFunction fn = GDALGetResampleFunction("GAUSS", nullptr);
  ASSERT_NE(nullptr, fn);
  GDALOverviewResampleArgs args;
  args.eSrcDataType = GDT_Float64;
  args.eOvrDataType = GDT_Float64;
  args.eWrkDataType = GDT_Float64;
  args.nOvrXSize = 4;
  args.nOvrYSize = 4;
  args.dfXRatioDstToSrc = 2.0;
  args.dfYRatioDstToSrc = 2.0;
  args.nChunkXOff = 2;
  args.nChunkXSize = 4;
  args.nChunkYOff = 2;
  args.nChunkYSize = 4;
  args.nDstXOff = 0;
  args.nDstXOff2 = 2;
  args.nDstYOff = 0;
  args.nDstYOff2 = 2;
  args.pszResampling = "GAUSS";

  std::vector<double> chunk(16, 25.0);
  void* dst_buf = nullptr;
  GDALDataType dst_dt = GDT_Unknown;
  EXPECT_EQ(CE_None, fn(args, chunk.data(), &dst_buf, &dst_dt));
  CPLFree(dst_buf);
}

TEST_F(OverviewTest, ModeAllDataTypesAndNaNs) {
  const GDALDataType kTypes[] = {
      GDT_Byte,    GDT_Int8,    GDT_Int16,    GDT_UInt16,
      GDT_Float16, GDT_CInt16,  GDT_CFloat16, GDT_Int32,
      GDT_UInt32,  GDT_Float32, GDT_CInt32,   GDT_CFloat32,
      GDT_Int64,   GDT_UInt64,  GDT_Float64,  GDT_CFloat64};

  for (GDALDataType dt : kTypes) {
    auto src_ds = CreateMemDS(12, 12, 1, dt);
    auto ovr_ds = CreateMemDS(3, 3, 1, dt);
    if (!GDALDataTypeIsComplex(dt)) {
      src_ds->GetRasterBand(1)->SetNoDataValue(0.0);
    }

    // Populate with varied values so mode algorithm sees new entries,
    // repeated entries that become the new mode, and all-NoData blocks.
    std::vector<double> buf(12 * 12 * 2, 0.0);
    for (int y = 0; y < 12; ++y) {
      for (int x = 0; x < 12; ++x) {
        int idx = y * 12 + x;
        if (y < 4 && x < 4) {
          // Leave as 0 (NoData for non-complex)
          buf[idx * 2] = 0.0;
          buf[idx * 2 + 1] = 0.0;
        } else if (y >= 8 && (dt == GDT_Float16 || dt == GDT_Float32 ||
                              dt == GDT_Float64 || dt == GDT_CFloat16 ||
                              dt == GDT_CFloat32 || dt == GDT_CFloat64)) {
          // NaN values to test IsSame<T> NaN handling!
          buf[idx * 2] = std::numeric_limits<double>::quiet_NaN();
          buf[idx * 2 + 1] = std::numeric_limits<double>::quiet_NaN();
        } else {
          // Within each 4x4 block: 1 pixel of 10, 2 pixels of 20, 5 pixels of
          // 30
          int local = (y % 4) * 4 + (x % 4);
          double val = (local == 0) ? 10.0 : (local < 3 ? 20.0 : 30.0);
          buf[idx * 2] = val;
          buf[idx * 2 + 1] = val + 1.0;
        }
      }
    }

    ASSERT_EQ(CE_None,
              src_ds->GetRasterBand(1)->RasterIO(
                  GF_Write, 0, 0, 12, 12, buf.data(), 12, 12,
                  GDALDataTypeIsComplex(dt) ? GDT_CFloat64 : GDT_Float64, 0, 0,
                  nullptr));
    GDALRasterBandH h_ovr = ovr_ds->GetRasterBand(1);
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_ovr,
                                      "MODE", nullptr, nullptr));
  }

  // Test MODE on GDT_Byte with a color table having > 256 entries (forces
  // bRegularProcessing = true for GByte) and test overflow error path.
  GDALResampleFunction fn = GDALGetResampleFunction("MODE", nullptr);
  ASSERT_NE(nullptr, fn);
  {
    GDALColorTable big_ct(GPI_RGB);
    const GDALColorEntry entry = {10, 20, 30, 255};
    big_ct.SetColorEntry(300, &entry);

    GDALOverviewResampleArgs args;
    args.eSrcDataType = GDT_Byte;
    args.eOvrDataType = GDT_Byte;
    args.eWrkDataType = GDT_Byte;
    args.nOvrXSize = 2;
    args.nOvrYSize = 2;
    args.dfXRatioDstToSrc = 2.0;
    args.dfYRatioDstToSrc = 2.0;
    args.nChunkXOff = 0;
    args.nChunkXSize = 4;
    args.nChunkYOff = 0;
    args.nChunkYSize = 4;
    args.nDstXOff = 0;
    args.nDstXOff2 = 2;
    args.nDstYOff = 0;
    args.nDstYOff2 = 2;
    args.pszResampling = "MODE";
    args.poColorTable = &big_ct;

    std::vector<GByte> chunk = {1, 2, 2, 3, 2, 1, 3, 2, 4, 4, 4, 1, 4, 2, 1, 4};
    void* dst_buf = nullptr;
    GDALDataType dst_dt = GDT_Unknown;
    EXPECT_EQ(CE_None, fn(args, chunk.data(), &dst_buf, &dst_dt));
    ASSERT_NE(nullptr, dst_buf);
    EXPECT_EQ(2, static_cast<GByte*>(dst_buf)[0]);
    CPLFree(dst_buf);

    // Trigger "Too big downsampling factor" / invalid window check in ModeT.
    QuietErrorHandler quiet;
    args.nChunkXSize = 0;
    dst_buf = nullptr;
    EXPECT_EQ(CE_Failure, fn(args, chunk.data(), &dst_buf, &dst_dt));
    CPLFree(dst_buf);
  }
}

TEST_F(OverviewTest, ConvolutionAllMethodsDataTypesAndMasks) {
  const char* kConvMethods[] = {"BILINEAR", "CUBIC", "CUBICSPLINE", "LANCZOS"};
  // Test source/dest data types that exercise all eWrkDataType and dstDataType
  // branches in GDALResampleChunk_ConvolutionT:
  // GDT_Byte, GDT_Int8, GDT_UInt16, GDT_Int16, GDT_UInt32, GDT_Int32,
  // GDT_UInt64, GDT_Int64, GDT_Float32, GDT_Float64.
  const GDALDataType kTypes[] = {GDT_Byte,    GDT_Int8,   GDT_UInt16, GDT_Int16,
                                 GDT_UInt32,  GDT_Int32,  GDT_UInt64, GDT_Int64,
                                 GDT_Float32, GDT_Float64};

  // 1. Test all types with CUBIC and BILINEAR (both 2x ratio so
  // nSrcPixelCount=4 for BILINEAR, and 3x ratio so bSrcPixelCountLess8 is
  // exercised, and width
  // >= 17 so 16-col / 8-col / 2-col and remainder vertical loops all run!).
  for (GDALDataType dt : kTypes) {
    for (const char* method : {"BILINEAR", "CUBIC"}) {
      auto src_ds = CreateMemDS(38, 14, 1, dt);
      auto ovr_ds = CreateMemDS(19, 7, 1, dt);
      src_ds->GetRasterBand(1)->SetNoDataValue(50.0);

      // Fill without mask (first test without NoData, then with NoData).
      src_ds->GetRasterBand(1)->DeleteNoDataValue();
      std::vector<double> buf(38 * 14);
      for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<double>(10 + (i % 80));
      }
      ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 38, 14, buf.data(), 38, 14,
                             GDT_Float64, 0, 0, nullptr));
      GDALRasterBandH h_ovr = ovr_ds->GetRasterBand(1);
      EXPECT_EQ(CE_None,
                GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_ovr,
                                        method, nullptr, nullptr));
    }
  }

  // 2. Test all 4 convolution methods with 3x downsampling (so BILINEAR has
  // nSrcPixelCount = 6 -> bSrcPixelCountLess8 branch!) and with NoData masks
  // on GDT_Byte, GDT_UInt16, GDT_Float32, GDT_Float64.
  const GDALDataType kMainTypes[] = {GDT_Byte, GDT_UInt16, GDT_Float32,
                                     GDT_Float64};
  for (const char* method : kConvMethods) {
    for (GDALDataType dt : kMainTypes) {
      // Without NoData, 3x downsampling
      {
        auto src_ds = CreateMemDS(21, 14, 1, dt);
        auto ovr_ds = CreateMemDS(7, 5, 1, dt);
        std::vector<double> buf(21 * 14);
        for (size_t i = 0; i < buf.size(); ++i) {
          buf[i] = static_cast<double>(20 + (i % 100));
        }
        ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                               GF_Write, 0, 0, 21, 14, buf.data(), 21, 14,
                               GDT_Float64, 0, 0, nullptr));
        GDALRasterBandH h_ovr = ovr_ds->GetRasterBand(1);
        EXPECT_EQ(CE_None,
                  GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_ovr,
                                          method, nullptr, nullptr));
      }

      // With NoData mask: include all-NoData region, scattered valid pixels
      // (so nMaxConsecutiveValid < nSrcPixelCount / 2 for CUBIC/LANCZOS),
      // and region where output equals NoDataValue (triggers
      // replaceValIfNodata).
      {
        auto src_ds = CreateMemDS(24, 16, 1, dt);
        auto ovr_ds = CreateMemDS(8, 5, 1, dt);
        src_ds->GetRasterBand(1)->SetNoDataValue(50.0);

        std::vector<double> buf(24 * 16, 50.0);
        // Scattered valid pixels in rows 4..7 (alternating valid and nodata)
        for (int y = 4; y < 8; ++y) {
          for (int x = 0; x < 24; ++x) {
            buf[y * 24 + x] = (x % 3 == 0) ? 80.0 : 50.0;
          }
        }
        // Dense valid pixels in rows 8..15 with values around 50 so some
        // outputs clamp/round to 50 (the NoData value) and get replaced by
        // replacement val.
        for (int y = 8; y < 16; ++y) {
          for (int x = 0; x < 24; ++x) {
            buf[y * 24 + x] = (x < 12) ? 50.0 - 1.0 + 2.0 * (x % 2) : 100.0;
          }
        }
        ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                               GF_Write, 0, 0, 24, 16, buf.data(), 24, 16,
                               GDT_Float64, 0, 0, nullptr));
        GDALRasterBandH h_ovr = ovr_ds->GetRasterBand(1);
        EXPECT_EQ(CE_None,
                  GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_ovr,
                                          method, nullptr, nullptr));
      }
    }
  }

  // 3. Test Float32 and Float64 with +/-Inf and NaN under CUBIC (exercises
  // ScaleValue NaN/+Inf/-Inf handling in horizontal and vertical filters).
  for (GDALDataType dt : {GDT_Float32, GDT_Float64}) {
    auto src_ds = CreateMemDS(24, 12, 1, dt);
    auto ovr_ds = CreateMemDS(12, 6, 1, dt);
    std::vector<double> buf(24 * 12, 1.0);
    for (int y = 0; y < 12; ++y) {
      for (int x = 0; x < 8; ++x) {
        buf[y * 24 + x] = std::numeric_limits<double>::infinity();
      }
      for (int x = 8; x < 16; ++x) {
        buf[y * 24 + x] = (x % 2 == 0)
                              ? std::numeric_limits<double>::infinity()
                              : -std::numeric_limits<double>::infinity();
      }
      for (int x = 16; x < 24; ++x) {
        buf[y * 24 + x] = std::numeric_limits<double>::quiet_NaN();
      }
    }
    ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 24, 12, buf.data(), 24, 12,
                           GDT_Float64, 0, 0, nullptr));
    GDALRasterBandH h_ovr = ovr_ds->GetRasterBand(1);
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_ovr,
                                      "CUBIC", nullptr, nullptr));
  }

  // 4. Test NBITS clamping in Convolution (both < type bits and == type bits)
  // and direct call with bHasNoData=true & pabyChunkNodataMask=nullptr.
  {
    auto src_ds = CreateMemDS(36, 12, 1, GDT_Byte);
    auto ovr_ds = CreateMemDS(18, 6, 1, GDT_Byte);
    ovr_ds->GetRasterBand(1)->SetMetadataItem("NBITS", "7", "IMAGE_STRUCTURE");
    GDALFillRaster(src_ds->GetRasterBand(1), 200.0, 0.0);
    GDALRasterBandH h_ovr = ovr_ds->GetRasterBand(1);
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_ovr,
                                      "CUBIC", nullptr, nullptr));
    GByte val = 0;
    EXPECT_EQ(CE_None,
              ovr_ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 1, 1, &val, 1,
                                                 1, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(127, val);

    // NBITS == 8 on GDT_Byte (no clamping)
    ovr_ds->GetRasterBand(1)->SetMetadataItem("NBITS", "8", "IMAGE_STRUCTURE");
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_ovr,
                                      "CUBIC", nullptr, nullptr));
  }

  // Direct call to GDALResampleChunk_Convolution with:
  // - eWrkDataType = GDT_Float64, eOvrDataType = GDT_UInt32, nOvrNBITS = 10
  // - eWrkDataType = GDT_Byte, eOvrDataType = GDT_Byte, bHasNoData = true,
  //   pabyChunkNodataMask = nullptr, width = 20 (exercises vertical 16/8-col
  //   bHasNoData loop and clamping!).
  GDALResampleFunction fn = GDALGetResampleFunction("CUBIC", nullptr);
  ASSERT_NE(nullptr, fn);
  {
    GDALOverviewResampleArgs args;
    args.eSrcDataType = GDT_UInt32;
    args.eOvrDataType = GDT_UInt32;
    args.eWrkDataType = GDT_Float64;
    args.nOvrXSize = 4;
    args.nOvrYSize = 4;
    args.nOvrNBITS = 10;
    args.dfXRatioDstToSrc = 2.0;
    args.dfYRatioDstToSrc = 2.0;
    args.nChunkXOff = 0;
    args.nChunkXSize = 8;
    args.nChunkYOff = 0;
    args.nChunkYSize = 8;
    args.nDstXOff = 0;
    args.nDstXOff2 = 4;
    args.nDstYOff = 0;
    args.nDstYOff2 = 4;
    args.pszResampling = "CUBIC";
    args.bHasNoData = true;
    args.dfNoDataValue = 2000.0;

    std::vector<double> chunk(64, 2000.0);
    void* dst_buf = nullptr;
    GDALDataType dst_dt = GDT_Unknown;
    EXPECT_EQ(CE_None, fn(args, chunk.data(), &dst_buf, &dst_dt));
    ASSERT_NE(nullptr, dst_buf);
    EXPECT_EQ(1023U, static_cast<uint32_t*>(dst_buf)[0]);
    CPLFree(dst_buf);
  }
  {
    GDALOverviewResampleArgs args;
    args.eSrcDataType = GDT_Byte;
    args.eOvrDataType = GDT_Byte;
    args.eWrkDataType = GDT_Byte;
    args.nOvrXSize = 20;
    args.nOvrYSize = 4;
    args.dfXRatioDstToSrc = 2.0;
    args.dfYRatioDstToSrc = 2.0;
    args.nChunkXOff = 0;
    args.nChunkXSize = 40;
    args.nChunkYOff = 0;
    args.nChunkYSize = 8;
    args.nDstXOff = 0;
    args.nDstXOff2 = 20;
    args.nDstYOff = 0;
    args.nDstYOff2 = 4;
    args.pszResampling = "CUBIC";
    args.bHasNoData = true;
    args.dfNoDataValue = 100.0;

    std::vector<GByte> chunk(40 * 8, 100);
    void* dst_buf = nullptr;
    GDALDataType dst_dt = GDT_Unknown;
    EXPECT_EQ(CE_None, fn(args, chunk.data(), &dst_buf, &dst_dt));
    ASSERT_NE(nullptr, dst_buf);
    CPLFree(dst_buf);
  }
}

TEST_F(OverviewTest, ComplexDataResamplingC32R) {
  // Tests GDALResampleChunkC32R with AVERAGE, AVERAGE_MAGPHASE, RMS,
  // AVERAGE_MP, and unsupported method error handling.
  auto src_ds = CreateMemDS(8, 8, 1, GDT_CFloat32);
  auto ovr_ds1 = CreateMemDS(4, 4, 1, GDT_CFloat32);
  auto ovr_ds2 = CreateMemDS(2, 2, 1, GDT_CFloat32);

  std::vector<float> buf(8 * 8 * 2);
  for (int i = 0; i < 64; ++i) {
    buf[i * 2] = static_cast<float>((i % 8) + 1);
    buf[i * 2 + 1] = static_cast<float>((i / 8) + 1);
  }
  ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, 8, 8, buf.data(), 8, 8, GDT_CFloat32,
                         0, 0, nullptr));

  GDALRasterBandH ovr_bands[2] = {ovr_ds1->GetRasterBand(1),
                                  ovr_ds2->GetRasterBand(1)};

  for (const char* method :
       {"AVERAGE", "AVERAGE_MAGPHASE", "RMS", "AVERAGE_MP"}) {
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 2, ovr_bands,
                                      method, nullptr, nullptr));
  }

  // Unsupported method on complex band -> CE_Failure
  {
    QuietErrorHandler quiet;
    EXPECT_EQ(CE_Failure,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, ovr_bands,
                                      "BILINEAR", nullptr, nullptr));
  }
}

TEST_F(OverviewTest, AverageBit2GrayscaleAllWorkTypesAndCascading) {
  // Test AVERAGE_BIT2GRAYSCALE and AVERAGE_BIT2GRAYSCALE_MINISWHITE across:
  // - GDT_Byte (eWrkDataType == GDT_Byte)
  // - GDT_UInt16 (eWrkDataType == GDT_UInt16)
  // - GDT_Int16 (eWrkDataType == GDT_Float32)
  // - GDT_Int32 (eWrkDataType == GDT_Float64)
  const GDALDataType kTypes[] = {GDT_Byte, GDT_UInt16, GDT_Int16, GDT_Int32};
  const char* kMethods[] = {"AVERAGE_BIT2GRAYSCALE",
                            "AVERAGE_BIT2GRAYSCALE_MINISWHITE"};

  for (GDALDataType dt : kTypes) {
    for (const char* method : kMethods) {
      auto src_ds = CreateMemDS(8, 8, 1, dt);
      auto ovr_small = CreateMemDS(2, 2, 1, GDT_Byte);
      auto ovr_large = CreateMemDS(4, 4, 1, GDT_Byte);

      std::vector<int> buf(64);
      for (int i = 0; i < 64; ++i) {
        buf[i] = i % 2;
      }
      ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                             GF_Write, 0, 0, 8, 8, buf.data(), 8, 8, GDT_Int32,
                             0, 0, nullptr));

      // Pass overview bands in smallest-to-largest order to exercise the bubble
      // sort swap in GDALRegenerateCascadingOverviews AND the cascading
      // transition from AVERAGE_BIT2GRAYSCALE -> AVERAGE!
      GDALRasterBandH ovr_bands[2] = {ovr_small->GetRasterBand(1),
                                      ovr_large->GetRasterBand(1)};
      EXPECT_EQ(CE_None,
                GDALRegenerateOverviews(src_ds->GetRasterBand(1), 2, ovr_bands,
                                        method, nullptr, nullptr));

      GByte v = 0;
      EXPECT_EQ(CE_None,
                ovr_large->GetRasterBand(1)->RasterIO(
                    GF_Read, 0, 0, 1, 1, &v, 1, 1, GDT_Byte, 0, 0, nullptr));
      EXPECT_NEAR(128, v, 1);
    }
  }
}

TEST_F(OverviewTest, MaskBandsThreadingAndChunkConfigOptions) {
  auto src_ds = CreateMemDS(16, 16, 1, GDT_Byte);
  auto ovr_ds1 = CreateMemDS(8, 8, 1, GDT_Byte);
  auto ovr_ds2 = CreateMemDS(4, 4, 1, GDT_Byte);
  GDALFillRaster(src_ds->GetRasterBand(1), 100.0, 0.0);

  // Create per-dataset mask band (GMF_PER_DATASET -> bCanUseCascaded = false).
  ASSERT_EQ(CE_None, src_ds->CreateMaskBand(GMF_PER_DATASET));
  GDALRasterBand* mask_band = src_ds->GetRasterBand(1)->GetMaskBand();
  GDALFillRaster(mask_band, 255.0, 0.0);

  // 1. Regenerate overviews on the mask band itself (poSrcBand->IsMaskBand() ==
  // true).
  GDALRasterBandH ovr_bands[2] = {ovr_ds1->GetRasterBand(1),
                                  ovr_ds2->GetRasterBand(1)};
  EXPECT_EQ(CE_None, GDALRegenerateOverviews(mask_band, 2, ovr_bands, "AVERAGE",
                                             nullptr, nullptr));

  // 2. Regenerate overviews on the band with GMF_PER_DATASET mask, using
  // multi-threading (GDAL_NUM_THREADS=2) and small GDAL_OVR_CHUNKYSIZE=2 so
  // multiple chunks are submitted to the thread pool queue!
  {
    ScopedConfigOption opt_threads("GDAL_NUM_THREADS", "2");
    ScopedConfigOption opt_chunk("GDAL_OVR_CHUNKYSIZE", "2");
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 2, ovr_bands,
                                      "AVERAGE", nullptr, nullptr));
  }

  // 3. Test GDAL_OVR_CHUNK_MAX_SIZE small on untiled dataset with NEAREST and
  // 2 overviews -> triggers cascading NEAREST fallback at line 5013!
  {
    ScopedConfigOption opt_max("GDAL_OVR_CHUNK_MAX_SIZE", "10");
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 2, ovr_bands,
                                      "NEAREST", nullptr, nullptr));
  }

  // 4. Test GDAL_OVR_CHUNK_MAX_SIZE small on a tiled GTiff dataset with AVERAGE
  // -> triggers delegation to GDALRegenerateOverviewsMultiBand at line 5009!
  {
    ScopedConfigOption opt_max("GDAL_OVR_CHUNK_MAX_SIZE", "100");
    GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    ASSERT_NE(nullptr, gtiff_drv);
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    const char* vsimem_path = "/vsimem/overview_tiled_delegate.tif";
    auto tif_ds = std::unique_ptr<GDALDataset>(
        gtiff_drv->Create(vsimem_path, 32, 32, 1, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, tif_ds);
    GDALFillRaster(tif_ds->GetRasterBand(1), 50.0, 0.0);
    auto ovr_tif = CreateMemDS(16, 16, 1, GDT_Byte);
    GDALRasterBandH h_ovr = ovr_tif->GetRasterBand(1);
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(tif_ds->GetRasterBand(1), 1, &h_ovr,
                                      "AVERAGE", nullptr, nullptr));
    tif_ds.reset();
    VSIUnlink(vsimem_path);
  }

  // 5. Test "NONE" resampling and progress cancellation in
  // GDALRegenerateOverviewsEx.
  EXPECT_EQ(CE_None,
            GDALRegenerateOverviewsEx(src_ds->GetRasterBand(1), 2, ovr_bands,
                                      "NONE", nullptr, nullptr, nullptr));
  {
    QuietErrorHandler quiet;
    EXPECT_EQ(CE_Failure, GDALRegenerateOverviewsEx(
                              src_ds->GetRasterBand(1), 1, ovr_bands, "AVERAGE",
                              CancelAtStartProgress, nullptr, nullptr));
    // Also cancel inside GDALRegenerateCascadingOverviews (using a band with
    // GMF_ALL_VALID)
    auto clean_src = CreateMemDS(16, 16, 1, GDT_Byte);
    EXPECT_EQ(CE_Failure, GDALRegenerateOverviewsEx(clean_src->GetRasterBand(1),
                                                    2, ovr_bands, "AVERAGE",
                                                    CancelAtStartProgress,
                                                    nullptr, nullptr));
  }
}

TEST_F(OverviewTest, RegenerateOverviewsMultiBandValidationAndNormal) {
  auto ds1 = CreateMemDS(16, 16, 2, GDT_Byte);
  auto ds_diff_size = CreateMemDS(8, 16, 1, GDT_Byte);
  auto ds_diff_type = CreateMemDS(16, 16, 1, GDT_UInt16);
  auto ovr1 = CreateMemDS(8, 8, 2, GDT_Byte);
  auto ovr2 = CreateMemDS(4, 4, 2, GDT_Byte);

  GDALFillRaster(ds1->GetRasterBand(1), 40.0, 0.0);
  GDALFillRaster(ds1->GetRasterBand(2), 80.0, 0.0);
  ds1->GetRasterBand(1)->SetNoDataValue(0.0);

  // Vector overload with empty overview list
  std::vector<GDALRasterBand*> empty_src;
  std::vector<std::vector<GDALRasterBand*>> empty_ovr;
  EXPECT_EQ(CE_None,
            GDALRegenerateOverviewsMultiBand(empty_src, empty_ovr, "AVERAGE",
                                             nullptr, nullptr, nullptr));

  // Vector overload normal execution with 2 bands and 2 overview levels,
  // partial window options (XOFF, YOFF, XSIZE, YSIZE), and 2 threads!
  std::vector<GDALRasterBand*> src_bands = {ds1->GetRasterBand(1),
                                            ds1->GetRasterBand(2)};
  std::vector<std::vector<GDALRasterBand*>> ovr_bands = {
      {ovr1->GetRasterBand(1), ovr2->GetRasterBand(1)},
      {ovr1->GetRasterBand(2), ovr2->GetRasterBand(2)}};

  {
    ScopedConfigOption opt_threads("GDAL_NUM_THREADS", "2");
    CPLStringList opts;
    opts.SetNameValue("XOFF", "0");
    opts.SetNameValue("YOFF", "0");
    opts.SetNameValue("XSIZE", "16");
    opts.SetNameValue("YSIZE", "16");
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviewsMultiBand(src_bands, ovr_bands, "AVERAGE",
                                               nullptr, nullptr, opts.List()));
  }

  // "NONE" resampling or 0 bands/overviews
  EXPECT_EQ(CE_None,
            GDALRegenerateOverviewsMultiBand(src_bands, ovr_bands, "NONE",
                                             nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_None,
            GDALRegenerateOverviewsMultiBand(0, nullptr, 0, nullptr, "AVERAGE",
                                             nullptr, nullptr, nullptr));

  // Error conditions in GDALRegenerateOverviewsMultiBand
  {
    QuietErrorHandler quiet;
    // Unsupported resampling
    EXPECT_EQ(CE_Failure, GDALRegenerateOverviewsMultiBand(
                              src_bands, ovr_bands, "AVERAGE_MAGPHASE", nullptr,
                              nullptr, nullptr));

    // Mismatched source band dimensions
    std::vector<GDALRasterBand*> bad_src_size = {
        ds1->GetRasterBand(1), ds_diff_size->GetRasterBand(1)};
    EXPECT_EQ(CE_Failure, GDALRegenerateOverviewsMultiBand(
                              bad_src_size, ovr_bands, "AVERAGE", nullptr,
                              nullptr, nullptr));

    // Mismatched source band data types
    std::vector<GDALRasterBand*> bad_src_type = {
        ds1->GetRasterBand(1), ds_diff_type->GetRasterBand(1)};
    EXPECT_EQ(CE_Failure, GDALRegenerateOverviewsMultiBand(
                              bad_src_type, ovr_bands, "AVERAGE", nullptr,
                              nullptr, nullptr));

    // Mismatched overview band dimensions
    std::vector<std::vector<GDALRasterBand*>> bad_ovr_size = {
        {ovr1->GetRasterBand(1)}, {ovr2->GetRasterBand(2)}};
    EXPECT_EQ(CE_Failure, GDALRegenerateOverviewsMultiBand(
                              src_bands, bad_ovr_size, "AVERAGE", nullptr,
                              nullptr, nullptr));

    // Mismatched overview band data types
    std::vector<std::vector<GDALRasterBand*>> bad_ovr_type = {
        {ovr1->GetRasterBand(1)}, {ds_diff_type->GetRasterBand(1)}};
    EXPECT_EQ(CE_Failure, GDALRegenerateOverviewsMultiBand(
                              src_bands, bad_ovr_type, "AVERAGE", nullptr,
                              nullptr, nullptr));

    // Progress cancellation
    EXPECT_EQ(CE_Failure, GDALRegenerateOverviewsMultiBand(
                              src_bands, ovr_bands, "AVERAGE",
                              CancelAtStartProgress, nullptr, nullptr));
  }
}

TEST_F(OverviewTest, RegenerateOverviewsMultiBandTempVrtAndTempFiles) {
  // Force nMemRequirement > nChunkMaxSizeForTempFile by setting
  // GDAL_OVR_CHUNK_MAX_SIZE_FOR_TEMP_FILE=100 and GDAL_OVR_CHUNK_MAX_SIZE=100.
  ScopedConfigOption opt_max("GDAL_OVR_CHUNK_MAX_SIZE", "100");
  ScopedConfigOption opt_tmp_max("GDAL_OVR_CHUNK_MAX_SIZE_FOR_TEMP_FILE",
                                 "100");

  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_drv);

  // Branch A: Tiled destination overview (nDstChunkXSize < nDstWidth ||
  // nDstChunkYSize < nDstHeight) with GMF_PER_DATASET mask on source!
  {
    auto src_ds = CreateMemDS(64, 64, 2, GDT_Byte);
    ASSERT_EQ(CE_None, src_ds->CreateMaskBand(GMF_PER_DATASET));
    GDALFillRaster(src_ds->GetRasterBand(1), 50.0, 0.0);
    GDALFillRaster(src_ds->GetRasterBand(2), 100.0, 0.0);
    GDALFillRaster(src_ds->GetRasterBand(1)->GetMaskBand(), 255.0, 0.0);

    const char* ovr_path = "/vsimem/multiband_tiled_ovr.tif";
    CPLStringList co;
    co.SetNameValue("TILED", "YES");
    co.SetNameValue("BLOCKXSIZE", "16");
    co.SetNameValue("BLOCKYSIZE", "16");
    auto ovr_ds = std::unique_ptr<GDALDataset>(
        gtiff_drv->Create(ovr_path, 32, 32, 2, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, ovr_ds);

    std::vector<GDALRasterBand*> src_bands = {src_ds->GetRasterBand(1),
                                              src_ds->GetRasterBand(2)};
    std::vector<std::vector<GDALRasterBand*>> ovr_bands = {
        {ovr_ds->GetRasterBand(1)}, {ovr_ds->GetRasterBand(2)}};

    EXPECT_EQ(CE_None,
              GDALRegenerateOverviewsMultiBand(src_bands, ovr_bands, "AVERAGE",
                                               nullptr, nullptr, nullptr));

    // Also test progress cancellation in this VRT chunking loop
    {
      QuietErrorHandler quiet;
      EXPECT_EQ(CE_Failure, GDALRegenerateOverviewsMultiBand(
                                src_bands, ovr_bands, "AVERAGE",
                                CancelAtStartProgress, nullptr, nullptr));
    }

    ovr_ds.reset();
    VSIUnlink(ovr_path);
  }

  // Branch B: Untiled destination overview (nDstChunkXSize == nDstWidth &&
  // nDstChunkYSize == nDstHeight), tested with both GDAL_OVR_TEMP_DRIVER=MEM
  // and GDAL_OVR_TEMP_DRIVER=GTIFF!
  for (const char* temp_drv : {"MEM", "GTIFF"}) {
    ScopedConfigOption opt_drv("GDAL_OVR_TEMP_DRIVER", temp_drv);
    auto src_ds = CreateMemDS(64, 64, 2, GDT_Byte);
    GDALFillRaster(src_ds->GetRasterBand(1), 60.0, 0.0);
    GDALFillRaster(src_ds->GetRasterBand(2), 120.0, 0.0);

    const char* ovr_path = "/vsimem/multiband_untiled_ovr.tif";
    CPLStringList co;
    co.SetNameValue("BLOCKYSIZE", "32");
    auto ovr_ds = std::unique_ptr<GDALDataset>(
        gtiff_drv->Create(ovr_path, 32, 32, 2, GDT_Byte, co.List()));
    ASSERT_NE(nullptr, ovr_ds);

    std::vector<GDALRasterBand*> src_bands = {src_ds->GetRasterBand(1),
                                              src_ds->GetRasterBand(2)};
    std::vector<std::vector<GDALRasterBand*>> ovr_bands = {
        {ovr_ds->GetRasterBand(1)}, {ovr_ds->GetRasterBand(2)}};

    EXPECT_EQ(CE_None,
              GDALRegenerateOverviewsMultiBand(src_bands, ovr_bands, "AVERAGE",
                                               nullptr, nullptr, nullptr));
    ovr_ds.reset();
    VSIUnlink(ovr_path);
  }
}

TEST_F(OverviewTest, ComputeBandStatsAndMagnitudeCorrection) {
  // Null pointer checks
  {
    QuietErrorHandler quiet;
    EXPECT_EQ(CE_Failure, GDALComputeBandStats(nullptr, 1, nullptr, nullptr,
                                               nullptr, nullptr));
    EXPECT_EQ(CE_Failure, GDALOverviewMagnitudeCorrection(nullptr, 0, nullptr,
                                                          nullptr, nullptr));
  }

  // Real band stats and magnitude correction via AVERAGE_MP
  {
    auto src_ds = CreateMemDS(16, 16, 1, GDT_Float32);
    auto ovr_ds = CreateMemDS(8, 8, 1, GDT_Float32);
    std::vector<float> buf(256);
    for (int i = 0; i < 256; ++i) {
      buf[i] = (i % 2 == 0) ? 10.0f : 30.0f;
    }
    ASSERT_EQ(CE_None, src_ds->GetRasterBand(1)->RasterIO(
                           GF_Write, 0, 0, 16, 16, buf.data(), 16, 16,
                           GDT_Float32, 0, 0, nullptr));

    double mean = 0.0;
    double stddev = 0.0;
    // Test nSampleStep = 0 (clamped to 1) and nSampleStep = 100 (>= height,
    // clamped to 1)
    EXPECT_EQ(CE_None, GDALComputeBandStats(src_ds->GetRasterBand(1), 0, &mean,
                                            &stddev, nullptr, nullptr));
    EXPECT_NEAR(20.0, mean, 1e-4);
    EXPECT_NEAR(10.0, stddev, 1e-4);

    EXPECT_EQ(CE_None,
              GDALComputeBandStats(src_ds->GetRasterBand(1), 100, nullptr,
                                   nullptr, nullptr, nullptr));

    // Progress cancellation in GDALComputeBandStats (at start and at 1.0 end)
    {
      QuietErrorHandler quiet;
      EXPECT_EQ(CE_Failure,
                GDALComputeBandStats(src_ds->GetRasterBand(1), 1, &mean,
                                     &stddev, CancelAtStartProgress, nullptr));
      EXPECT_EQ(CE_Failure,
                GDALComputeBandStats(src_ds->GetRasterBand(1), 1, &mean,
                                     &stddev, CancelAtEndProgress, nullptr));
    }

    // AVERAGE_MP on real band triggers GDALOverviewMagnitudeCorrection
    // (!bComplex)
    GDALRasterBandH h_ovr = ovr_ds->GetRasterBand(1);
    EXPECT_EQ(CE_None,
              GDALRegenerateOverviews(src_ds->GetRasterBand(1), 1, &h_ovr,
                                      "AVERAGE_MP", nullptr, nullptr));

    // Progress cancellation in GDALOverviewMagnitudeCorrection:
    // Base band (16 lines, step 2 -> 8 lines + 1 end = 9 calls),
    // Ovr band stats (8 lines, step 1 -> 8 lines + 1 end = 9 calls),
    // Ovr band correction loop (8 lines -> calls 19..26, end = call 27).
    {
      QuietErrorHandler quiet;
      int calls_cancel_base = 1;
      EXPECT_EQ(CE_Failure, GDALOverviewMagnitudeCorrection(
                                src_ds->GetRasterBand(1), 1, &h_ovr,
                                CancelOnNthCallProgress, &calls_cancel_base));
      int calls_cancel_ovr_stats = 10;
      EXPECT_EQ(CE_Failure,
                GDALOverviewMagnitudeCorrection(src_ds->GetRasterBand(1), 1,
                                                &h_ovr, CancelOnNthCallProgress,
                                                &calls_cancel_ovr_stats));
      int calls_cancel_in_loop = 20;
      EXPECT_EQ(CE_Failure,
                GDALOverviewMagnitudeCorrection(src_ds->GetRasterBand(1), 1,
                                                &h_ovr, CancelOnNthCallProgress,
                                                &calls_cancel_in_loop));
      int calls_cancel_at_end = 27;
      EXPECT_EQ(CE_Failure, GDALOverviewMagnitudeCorrection(
                                src_ds->GetRasterBand(1), 1, &h_ovr,
                                CancelOnNthCallProgress, &calls_cancel_at_end));
    }
  }
}

}  // namespace
