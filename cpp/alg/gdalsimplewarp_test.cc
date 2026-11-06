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
// Tests simple image warper (alg/gdalsimplewarp.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalsimplewarp.cpp

#include <algorithm>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

// Identity transformer mapping destination (x, y) directly to source (x, y).
int IdentityTransformer(void* /*pTransformArg*/, int /*bDstToSrc*/,
                        int nPointCount, double* /*x*/, double* /*y*/,
                        double* /*z*/, int* panSuccess) {
  if (panSuccess != nullptr) {
    for (int i = 0; i < nPointCount; ++i) {
      panSuccess[i] = TRUE;
    }
  }
  return TRUE;
}

// Transformer where only even indices succeed; odd indices fail.
int PartialFailureTransformer(void* /*pTransformArg*/, int /*bDstToSrc*/,
                              int nPointCount, double* /*x*/, double* /*y*/,
                              double* /*z*/, int* panSuccess) {
  if (panSuccess != nullptr) {
    for (int i = 0; i < nPointCount; ++i) {
      panSuccess[i] = (i % 2 == 0) ? TRUE : FALSE;
    }
  }
  return TRUE;
}

// Transformer where all points fail.
int TotalFailureTransformer(void* /*pTransformArg*/, int /*bDstToSrc*/,
                            int nPointCount, double* /*x*/, double* /*y*/,
                            double* /*z*/, int* panSuccess) {
  if (panSuccess != nullptr) {
    for (int i = 0; i < nPointCount; ++i) {
      panSuccess[i] = FALSE;
    }
  }
  return FALSE;
}

// Transformer that injects out-of-bounds and negative coordinates.
int BoundaryConditionTransformer(void* /*pTransformArg*/, int /*bDstToSrc*/,
                                 int nPointCount, double* x, double* y,
                                 double* /*z*/, int* panSuccess) {
  if (panSuccess != nullptr) {
    for (int i = 0; i < nPointCount; ++i) {
      panSuccess[i] = TRUE;
    }
  }
  for (int i = 0; i < nPointCount; ++i) {
    switch (i % 5) {
      case 0:
        x[i] = -0.5;  // Triggers x < 0.0 check.
        break;
      case 1:
        y[i] = -0.5;  // Triggers y < 0.0 check.
        break;
      case 2:
        x[i] = 1000.0;  // Triggers iSrcX >= nSrcXSize check.
        break;
      case 3:
        y[i] = 1000.0;  // Triggers iSrcY >= nSrcYSize check.
        break;
      default:
        // Valid coordinate: keep unchanged.
        break;
    }
  }
  return TRUE;
}

// Progress tracking data structure.
struct ProgressTracker {
  std::vector<double> progress_values;
  double fail_threshold = 2.0;  // > 1.0 means never fail
};

int RecordingProgress(double dfComplete, const char* /*pszMessage*/,
                      void* pProgressArg) {
  if (pProgressArg != nullptr) {
    auto* tracker = static_cast<ProgressTracker*>(pProgressArg);
    tracker->progress_values.push_back(dfComplete);
    if (dfComplete >= tracker->fail_threshold) {
      return FALSE;
    }
  }
  return TRUE;
}

GDALDatasetUniquePtr CreateMemDataset(int width, int height, int bands,
                                      GDALDataType type = GDT_Byte) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  if (mem_driver == nullptr) {
    return nullptr;
  }
  return GDALDatasetUniquePtr(
      mem_driver->Create("", width, height, bands, type, nullptr));
}

std::vector<uint8_t> ReadBandValues(GDALDataset* ds, int band_num, int width,
                                    int height) {
  std::vector<uint8_t> buf(width * height, 0);
  GDALRasterBand* band = ds->GetRasterBand(band_num);
  if (band != nullptr) {
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, width, height, buf.data(),
                                      width, height, GDT_Byte, 0, 0, nullptr));
  }
  return buf;
}

void WriteBandValues(GDALDataset* ds, int band_num, int width, int height,
                     const std::vector<uint8_t>& buf) {
  GDALRasterBand* band = ds->GetRasterBand(band_num);
  if (band != nullptr) {
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, width, height,
                                      const_cast<uint8_t*>(buf.data()), width,
                                      height, GDT_Byte, 0, 0, nullptr));
  }
}

void WriteBandValuesFloat(GDALDataset* ds, int band_num, int width, int height,
                          const std::vector<float>& buf) {
  GDALRasterBand* band = ds->GetRasterBand(band_num);
  if (band != nullptr) {
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, width, height,
                                      const_cast<float*>(buf.data()), width,
                                      height, GDT_Float32, 0, 0, nullptr));
  }
}

class GdalSimpleWarpTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }
};

TEST_F(GdalSimpleWarpTest, NullPointersValidation) {
  GDALDatasetUniquePtr ds = CreateMemDataset(2, 2, 1);
  ASSERT_NE(nullptr, ds);

  // 1. Null source dataset.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE,
              GDALSimpleImageWarp(nullptr, GDALDataset::ToHandle(ds.get()), 0,
                                  nullptr, IdentityTransformer, nullptr,
                                  nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 2. Null destination dataset.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE,
              GDALSimpleImageWarp(GDALDataset::ToHandle(ds.get()), nullptr, 0,
                                  nullptr, IdentityTransformer, nullptr,
                                  nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(GdalSimpleWarpTest, ZeroBandsSourceDatasetFails) {
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);
  GDALDatasetUniquePtr src_ds(
      mem_driver->Create("", 4, 4, 0, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(4, 4, 1);
  ASSERT_NE(nullptr, dst_ds);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(FALSE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                       GDALDataset::ToHandle(dst_ds.get()), 0,
                                       nullptr, IdentityTransformer, nullptr,
                                       nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("No raster band in source dataset"));
  CPLErrorReset();
}

TEST_F(GdalSimpleWarpTest, InvalidBandListFailure) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(4, 4, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(4, 4, 1);
  ASSERT_NE(nullptr, dst_ds);

  int invalid_bands[] = {99};
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(FALSE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                       GDALDataset::ToHandle(dst_ds.get()), 1,
                                       invalid_bands, IdentityTransformer,
                                       nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_FileIO, CPLGetLastErrorNo());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("GDALRasterIO failure"));
  CPLErrorReset();
}

TEST_F(GdalSimpleWarpTest, OneBandIdentityWarp) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  std::vector<uint8_t> src_pixels(kWidth * kHeight);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    src_pixels[i] = static_cast<uint8_t>(i + 1);
  }
  WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_pixels);

  int band_list[] = {1};
  EXPECT_EQ(TRUE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                      GDALDataset::ToHandle(dst_ds.get()), 1,
                                      band_list, IdentityTransformer, nullptr,
                                      nullptr, nullptr, nullptr));

  const std::vector<uint8_t> dst_pixels =
      ReadBandValues(dst_ds.get(), 1, kWidth, kHeight);
  EXPECT_EQ(src_pixels, dst_pixels);
}

TEST_F(GdalSimpleWarpTest, MultiBandAutoBandDiscoveryWarp) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  constexpr int kBands = 3;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, kBands);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, kBands);
  ASSERT_NE(nullptr, dst_ds);

  const std::vector<uint8_t> band1(kWidth * kHeight, 10);
  const std::vector<uint8_t> band2(kWidth * kHeight, 20);
  const std::vector<uint8_t> band3(kWidth * kHeight, 30);
  WriteBandValues(src_ds.get(), 1, kWidth, kHeight, band1);
  WriteBandValues(src_ds.get(), 2, kWidth, kHeight, band2);
  WriteBandValues(src_ds.get(), 3, kWidth, kHeight, band3);

  // nBandCount = 0 triggers automatic all-bands warping.
  EXPECT_EQ(TRUE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                      GDALDataset::ToHandle(dst_ds.get()), 0,
                                      nullptr, IdentityTransformer, nullptr,
                                      nullptr, nullptr, nullptr));

  EXPECT_EQ(band1, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
  EXPECT_EQ(band2, ReadBandValues(dst_ds.get(), 2, kWidth, kHeight));
  EXPECT_EQ(band3, ReadBandValues(dst_ds.get(), 3, kWidth, kHeight));
}

TEST_F(GdalSimpleWarpTest, MultiBandSubsetWarp) {
  constexpr int kWidth = 2;
  constexpr int kHeight = 2;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 3);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 2);
  ASSERT_NE(nullptr, dst_ds);

  const std::vector<uint8_t> band1 = {1, 2, 3, 4};
  const std::vector<uint8_t> band2 = {10, 20, 30, 40};
  const std::vector<uint8_t> band3 = {100, 101, 102, 103};
  WriteBandValues(src_ds.get(), 1, kWidth, kHeight, band1);
  WriteBandValues(src_ds.get(), 2, kWidth, kHeight, band2);
  WriteBandValues(src_ds.get(), 3, kWidth, kHeight, band3);

  int band_list[] = {1, 3};
  EXPECT_EQ(TRUE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                      GDALDataset::ToHandle(dst_ds.get()), 2,
                                      band_list, IdentityTransformer, nullptr,
                                      nullptr, nullptr, nullptr));

  EXPECT_EQ(band1, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
  EXPECT_EQ(band3, ReadBandValues(dst_ds.get(), 2, kWidth, kHeight));
}

TEST_F(GdalSimpleWarpTest, RemapSingleValueOption) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 2);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 2);
  ASSERT_NE(nullptr, dst_ds);

  const std::vector<uint8_t> src_band1 = {5, 10, 20, 5, 10, 20, 5, 10, 20};
  const std::vector<uint8_t> src_band2 = {5, 5, 5, 10, 10, 10, 30, 30, 30};
  WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_band1);
  WriteBandValues(src_ds.get(), 2, kWidth, kHeight, src_band2);

  CPLStringList warp_options;
  warp_options.AddString("REMAP=5 55");
  warp_options.AddString("REMAP=10 110");

  EXPECT_EQ(TRUE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                      GDALDataset::ToHandle(dst_ds.get()), 0,
                                      nullptr, IdentityTransformer, nullptr,
                                      nullptr, nullptr, warp_options.List()));

  const std::vector<uint8_t> expected_band1 = {55, 110, 20,  55, 110,
                                               20, 55,  110, 20};
  const std::vector<uint8_t> expected_band2 = {55,  55, 55, 110, 110,
                                               110, 30, 30, 30};
  EXPECT_EQ(expected_band1, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
  EXPECT_EQ(expected_band2, ReadBandValues(dst_ds.get(), 2, kWidth, kHeight));
}

TEST_F(GdalSimpleWarpTest, RemapSingleValueIllFormedWarning) {
  constexpr int kWidth = 2;
  constexpr int kHeight = 2;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  // 1. One token in REMAP.
  {
    CPLStringList warp_options;
    warp_options.AddString("REMAP=42");

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(TRUE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                        GDALDataset::ToHandle(dst_ds.get()), 0,
                                        nullptr, IdentityTransformer, nullptr,
                                        nullptr, nullptr, warp_options.List()));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Ill formed REMAP"));
    CPLErrorReset();
  }

  // 2. Three tokens in REMAP.
  {
    CPLStringList warp_options;
    warp_options.AddString("REMAP=1 2 3");

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(TRUE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                        GDALDataset::ToHandle(dst_ds.get()), 0,
                                        nullptr, IdentityTransformer, nullptr,
                                        nullptr, nullptr, warp_options.List()));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Ill formed REMAP"));
    CPLErrorReset();
  }
}

TEST_F(GdalSimpleWarpTest, RemapMultiOption) {
  constexpr int kWidth = 2;
  constexpr int kHeight = 2;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 2);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 2);
  ASSERT_NE(nullptr, dst_ds);

  // Pixel 0: (10, 20) -> matches remap from (10, 20) to (100, 200).
  // Pixel 1: (10, 25) -> band 2 differs, no remap.
  // Pixel 2: (15, 20) -> band 1 differs, no remap.
  // Pixel 3: (30, 40) -> matches second remap from (30, 40) to (130, 140).
  const std::vector<uint8_t> src_band1 = {10, 10, 15, 30};
  const std::vector<uint8_t> src_band2 = {20, 25, 20, 40};
  WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_band1);
  WriteBandValues(src_ds.get(), 2, kWidth, kHeight, src_band2);

  CPLStringList warp_options;
  warp_options.AddString("REMAP_MULTI=10 20 100 200");
  warp_options.AddString("REMAP_MULTI=30 40 130 140");

  EXPECT_EQ(TRUE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                      GDALDataset::ToHandle(dst_ds.get()), 0,
                                      nullptr, IdentityTransformer, nullptr,
                                      nullptr, nullptr, warp_options.List()));

  const std::vector<uint8_t> expected_band1 = {100, 10, 15, 130};
  const std::vector<uint8_t> expected_band2 = {200, 25, 20, 140};
  EXPECT_EQ(expected_band1, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
  EXPECT_EQ(expected_band2, ReadBandValues(dst_ds.get(), 2, kWidth, kHeight));
}

TEST_F(GdalSimpleWarpTest, RemapMultiSingleBand) {
  constexpr int kWidth = 2;
  constexpr int kHeight = 2;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  const std::vector<uint8_t> src_band = {10, 20, 10, 30};
  WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_band);

  CPLStringList warp_options;
  warp_options.AddString("REMAP_MULTI=10 99");

  EXPECT_EQ(TRUE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                      GDALDataset::ToHandle(dst_ds.get()), 0,
                                      nullptr, IdentityTransformer, nullptr,
                                      nullptr, nullptr, warp_options.List()));

  const std::vector<uint8_t> expected = {99, 20, 99, 30};
  EXPECT_EQ(expected, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
}

TEST_F(GdalSimpleWarpTest, RemapMultiIllFormedWarning) {
  constexpr int kWidth = 2;
  constexpr int kHeight = 2;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 2);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 2);
  ASSERT_NE(nullptr, dst_ds);

  // 1. Odd number of tokens.
  {
    CPLStringList warp_options;
    warp_options.AddString("REMAP_MULTI=10 20 30");

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(TRUE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                        GDALDataset::ToHandle(dst_ds.get()), 0,
                                        nullptr, IdentityTransformer, nullptr,
                                        nullptr, nullptr, warp_options.List()));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Ill formed REMAP_MULTI"));
    CPLErrorReset();
  }

  // 2. Empty REMAP_MULTI string (0 tokens).
  {
    CPLStringList warp_options;
    warp_options.AddString("REMAP_MULTI=");

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(TRUE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                        GDALDataset::ToHandle(dst_ds.get()), 0,
                                        nullptr, IdentityTransformer, nullptr,
                                        nullptr, nullptr, warp_options.List()));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Ill formed REMAP_MULTI"));
    CPLErrorReset();
  }

  // 3. More tokens than 2 * nBandCount (e.g. 6 tokens > 2 * 2).
  {
    CPLStringList warp_options;
    warp_options.AddString("REMAP_MULTI=1 2 3 4 5 6");

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(TRUE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                        GDALDataset::ToHandle(dst_ds.get()), 0,
                                        nullptr, IdentityTransformer, nullptr,
                                        nullptr, nullptr, warp_options.List()));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Ill formed REMAP_MULTI"));
    CPLErrorReset();
  }
}

TEST_F(GdalSimpleWarpTest, InitOptionWithUnmappedPixels) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 2;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  const std::vector<uint8_t> src_pixels = {1, 2, 3, 4, 5, 6, 7, 8};
  WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_pixels);

  // INIT=128 initializes destination scanline buffer to 128.
  // PartialFailureTransformer causes odd columns to fail transform.
  // Note: INIT_DEST is a GDALWarpOperation option ignored by
  // GDALSimpleImageWarp; passing it here verifies that unrecognized warp
  // options are harmlessly ignored.
  CPLStringList warp_options;
  warp_options.SetNameValue("INIT", "128");
  warp_options.SetNameValue("INIT_DEST", "NO_DATA");

  EXPECT_EQ(TRUE,
            GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                GDALDataset::ToHandle(dst_ds.get()), 0, nullptr,
                                PartialFailureTransformer, nullptr, nullptr,
                                nullptr, warp_options.List()));

  const std::vector<uint8_t> expected = {1, 128, 3, 128, 5, 128, 7, 128};
  EXPECT_EQ(expected, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
}

TEST_F(GdalSimpleWarpTest, InitMultiBandAndEmptyTokens) {
  constexpr int kWidth = 2;
  constexpr int kHeight = 2;

  // 1. Multi-band distinct INIT values: INIT=50,150.
  {
    GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 2);
    ASSERT_NE(nullptr, src_ds);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 2);
    ASSERT_NE(nullptr, dst_ds);

    const std::vector<uint8_t> src_b1 = {10, 20, 30, 40};
    const std::vector<uint8_t> src_b2 = {11, 21, 31, 41};
    WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_b1);
    WriteBandValues(src_ds.get(), 2, kWidth, kHeight, src_b2);

    CPLStringList warp_options;
    warp_options.SetNameValue("INIT", "50,150");

    EXPECT_EQ(TRUE,
              GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                  GDALDataset::ToHandle(dst_ds.get()), 0,
                                  nullptr, PartialFailureTransformer, nullptr,
                                  nullptr, nullptr, warp_options.List()));

    const std::vector<uint8_t> expected_b1 = {10, 50, 30, 50};
    const std::vector<uint8_t> expected_b2 = {11, 150, 31, 150};
    EXPECT_EQ(expected_b1, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
    EXPECT_EQ(expected_b2, ReadBandValues(dst_ds.get(), 2, kWidth, kHeight));
  }

  // 2. Empty INIT string sets initial value to 0.
  {
    GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
    ASSERT_NE(nullptr, src_ds);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
    ASSERT_NE(nullptr, dst_ds);

    const std::vector<uint8_t> src = {7, 8, 9, 10};
    WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src);

    CPLStringList warp_options;
    warp_options.SetNameValue("INIT", "");

    EXPECT_EQ(TRUE,
              GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                  GDALDataset::ToHandle(dst_ds.get()), 0,
                                  nullptr, PartialFailureTransformer, nullptr,
                                  nullptr, nullptr, warp_options.List()));

    const std::vector<uint8_t> expected = {7, 0, 9, 0};
    EXPECT_EQ(expected, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
  }
}

TEST_F(GdalSimpleWarpTest, InitMinusOnePreservesPreExistingDestination) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 2;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  const std::vector<uint8_t> src_pixels = {1, 2, 3, 4, 5, 6, 7, 8};
  WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_pixels);

  // Pre-populate destination dataset with sentinel 77.
  const std::vector<uint8_t> dst_initial(kWidth * kHeight, 77);
  WriteBandValues(dst_ds.get(), 1, kWidth, kHeight, dst_initial);

  // INIT=-1 reads initial values from the destination dataset.
  CPLStringList warp_options;
  warp_options.SetNameValue("INIT", "-1");

  EXPECT_EQ(TRUE,
            GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                GDALDataset::ToHandle(dst_ds.get()), 0, nullptr,
                                PartialFailureTransformer, nullptr, nullptr,
                                nullptr, warp_options.List()));

  // Even columns overwritten by source pixels; odd columns preserve 77.
  const std::vector<uint8_t> expected = {1, 77, 3, 77, 5, 77, 7, 77};
  EXPECT_EQ(expected, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
}

TEST_F(GdalSimpleWarpTest, InitMinusOneDestinationReadFailure) {
  constexpr int kWidth = 2;
  constexpr int kHeight = 2;
  // Source has 2 bands, destination has only 1 band.
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 2);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  CPLStringList warp_options;
  warp_options.SetNameValue("INIT", "-1");

  int bands[] = {1, 2};
  WithQuietHandler quiet;
  CPLErrorReset();
  // When INIT=-1 reads destination band 2, GDALGetRasterBand returns nullptr,
  // causing GDALRasterIO to fail.
  EXPECT_EQ(FALSE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                       GDALDataset::ToHandle(dst_ds.get()), 2,
                                       bands, IdentityTransformer, nullptr,
                                       nullptr, nullptr, warp_options.List()));
  CPLErrorReset();
}

TEST_F(GdalSimpleWarpTest, DestinationWriteFailure) {
  constexpr int kWidth = 2;
  constexpr int kHeight = 2;
  // Source has 2 bands, destination has only 1 band.
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 2);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  int bands[] = {1, 2};
  WithQuietHandler quiet;
  CPLErrorReset();
  // Writing band 2 to destination fails because destination only has 1 band.
  EXPECT_EQ(FALSE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                       GDALDataset::ToHandle(dst_ds.get()), 2,
                                       bands, IdentityTransformer, nullptr,
                                       nullptr, nullptr, nullptr));
  CPLErrorReset();
}

TEST_F(GdalSimpleWarpTest, UnknownWarpOptionsIgnored) {
  constexpr int kWidth = 2;
  constexpr int kHeight = 2;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  const std::vector<uint8_t> src_pixels = {11, 22, 33, 44};
  WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_pixels);

  // UNIFIED_SRC_NODATA is handled by GDALWarpOperation but ignored by
  // GDALSimpleImageWarp; verifies unrecognized options do not alter warp
  // outcome.
  CPLStringList warp_options;
  warp_options.SetNameValue("UNIFIED_SRC_NODATA", "YES");

  EXPECT_EQ(TRUE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                      GDALDataset::ToHandle(dst_ds.get()), 0,
                                      nullptr, IdentityTransformer, nullptr,
                                      nullptr, nullptr, warp_options.List()));

  EXPECT_EQ(src_pixels, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
}

TEST_F(GdalSimpleWarpTest, SrcMethodNoGeotransform) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  const std::vector<uint8_t> src_pixels = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_pixels);

  CPLStringList transformer_opts;
  transformer_opts.SetNameValue("SRC_METHOD", "NO_GEOTRANSFORM");
  transformer_opts.SetNameValue("DST_METHOD", "NO_GEOTRANSFORM");

  void* pTransformArg = GDALCreateGenImgProjTransformer2(
      GDALDataset::ToHandle(src_ds.get()), GDALDataset::ToHandle(dst_ds.get()),
      transformer_opts.List());
  ASSERT_NE(nullptr, pTransformArg);
  absl::Cleanup destroy_transformer = [pTransformArg] {
    GDALDestroyGenImgProjTransformer(pTransformArg);
  };

  EXPECT_EQ(TRUE,
            GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                GDALDataset::ToHandle(dst_ds.get()), 0, nullptr,
                                GDALGenImgProjTransform, pTransformArg, nullptr,
                                nullptr, nullptr));

  EXPECT_EQ(src_pixels, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
}

TEST_F(GdalSimpleWarpTest, ProgressCallbackNormalAndCancellation) {
  constexpr int kWidth = 2;
  constexpr int kHeight = 4;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  // 1. Normal completion: progress callback records calls and returns TRUE.
  {
    ProgressTracker tracker;
    tracker.fail_threshold = 2.0;

    EXPECT_EQ(TRUE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                        GDALDataset::ToHandle(dst_ds.get()), 0,
                                        nullptr, IdentityTransformer, nullptr,
                                        RecordingProgress, &tracker, nullptr));

    ASSERT_GE(tracker.progress_values.size(), 2u);
    EXPECT_DOUBLE_EQ(0.0, tracker.progress_values.front());
    EXPECT_DOUBLE_EQ(1.0, tracker.progress_values.back());
  }

  // 2. Cancellation on initial progress call (0.0).
  {
    ProgressTracker tracker;
    tracker.fail_threshold = 0.0;

    EXPECT_EQ(FALSE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                         GDALDataset::ToHandle(dst_ds.get()), 0,
                                         nullptr, IdentityTransformer, nullptr,
                                         RecordingProgress, &tracker, nullptr));
    EXPECT_EQ(1u, tracker.progress_values.size());
    EXPECT_DOUBLE_EQ(0.0, tracker.progress_values[0]);
  }

  // 3. Cancellation during scanline processing (midway).
  {
    ProgressTracker tracker;
    tracker.fail_threshold = 0.25;

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(FALSE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                         GDALDataset::ToHandle(dst_ds.get()), 0,
                                         nullptr, IdentityTransformer, nullptr,
                                         RecordingProgress, &tracker, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_UserInterrupt, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User terminated"));
    CPLErrorReset();
  }
}

TEST_F(GdalSimpleWarpTest, NonByteSourceDataset) {
  constexpr int kWidth = 2;
  constexpr int kHeight = 2;
  // Source is GDT_Float32.
  GDALDatasetUniquePtr src_ds =
      CreateMemDataset(kWidth, kHeight, 1, GDT_Float32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1, GDT_Byte);
  ASSERT_NE(nullptr, dst_ds);

  const std::vector<float> float_pixels = {15.0f, 35.0f, 65.0f, 125.0f};
  WriteBandValuesFloat(src_ds.get(), 1, kWidth, kHeight, float_pixels);

  EXPECT_EQ(TRUE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                      GDALDataset::ToHandle(dst_ds.get()), 0,
                                      nullptr, IdentityTransformer, nullptr,
                                      nullptr, nullptr, nullptr));

  const std::vector<uint8_t> expected = {15, 35, 65, 125};
  EXPECT_EQ(expected, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
}

TEST_F(GdalSimpleWarpTest, TransformerFailuresAndBoundaryConditions) {
  constexpr int kWidth = 5;
  constexpr int kHeight = 2;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  const std::vector<uint8_t> src_pixels = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_pixels);

  // 1. Total transform failure initializes all destination pixels to INIT
  // value.
  {
    CPLStringList warp_options;
    warp_options.SetNameValue("INIT", "200");

    EXPECT_EQ(TRUE,
              GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                  GDALDataset::ToHandle(dst_ds.get()), 0,
                                  nullptr, TotalFailureTransformer, nullptr,
                                  nullptr, nullptr, warp_options.List()));

    const std::vector<uint8_t> expected(kWidth * kHeight, 200);
    EXPECT_EQ(expected, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
  }

  // 2. Boundary conditions: x < 0.0, y < 0.0, x >= width, y >= height.
  {
    CPLStringList warp_options;
    warp_options.SetNameValue("INIT", "55");

    EXPECT_EQ(TRUE, GDALSimpleImageWarp(GDALDataset::ToHandle(src_ds.get()),
                                        GDALDataset::ToHandle(dst_ds.get()), 0,
                                        nullptr, BoundaryConditionTransformer,
                                        nullptr, nullptr, nullptr,
                                        warp_options.List()));

    const std::vector<uint8_t> dst =
        ReadBandValues(dst_ds.get(), 1, kWidth, kHeight);
    // Boundary conditions trigger fallback to INIT (55) for indices 0, 1, 2, 3;
    // index 4 is valid and receives source pixel value.
    EXPECT_EQ(55, dst[0]);
    EXPECT_EQ(55, dst[1]);
    EXPECT_EQ(55, dst[2]);
    EXPECT_EQ(55, dst[3]);
    EXPECT_EQ(src_pixels[4], dst[4]);
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered lines are OOM allocation failure
// branches.
