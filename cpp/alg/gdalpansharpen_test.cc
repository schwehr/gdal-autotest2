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
// Tests pansharpening operations and weighted Brovey algorithm
// (alg/gdalpansharpen.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalpansharpen.cpp
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalpansharpen.h

#include "third_party/gdal/alg/gdalpansharpen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_geotransform.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

GDALDatasetUniquePtr CreateMemDataset(int width, int height, int bands,
                                      GDALDataType dt,
                                      const double gt[6] = nullptr) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (mem_drv == nullptr) {
    return nullptr;
  }
  auto ds = GDALDatasetUniquePtr(
      mem_drv->Create("", width, height, bands, dt, nullptr));
  if (ds != nullptr && gt != nullptr) {
    ds->SetGeoTransform(GDALGeoTransform(gt));
  }
  return ds;
}

template <typename T>
void FillBand(GDALRasterBand* band, const std::vector<T>& data) {
  const int x_size = band->GetXSize();
  const int y_size = band->GetYSize();
  EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, x_size, y_size,
                                    const_cast<T*>(data.data()), x_size, y_size,
                                    band->GetRasterDataType(), 0, 0));
}

template <typename T>
void FillBandConstant(GDALRasterBand* band, T value) {
  const int x_size = band->GetXSize();
  const int y_size = band->GetYSize();
  std::vector<T> data(static_cast<size_t>(x_size) * y_size, value);
  FillBand(band, data);
}

struct PansharpenSetup {
  GDALDatasetUniquePtr pan_ds;
  GDALDatasetUniquePtr ms_ds;
  std::vector<GDALDatasetUniquePtr> extra_ms_ds;
  std::unique_ptr<GDALPansharpenOptions,
                  decltype(&GDALDestroyPansharpenOptions)>
      options{nullptr, GDALDestroyPansharpenOptions};
};

PansharpenSetup CreateStandardSetup(
    int pan_width = 8, int pan_height = 8, int ms_width = 4, int ms_height = 4,
    int ms_bands = 3, GDALDataType pan_dt = GDT_Byte,
    GDALDataType ms_dt = GDT_Byte,
    GDALRIOResampleAlg resample_alg = GRIORA_Cubic) {
  PansharpenSetup setup;
  const double pan_gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  const double ms_scale_x = static_cast<double>(pan_width) / ms_width;
  const double ms_scale_y = -static_cast<double>(pan_height) / ms_height;
  const double ms_gt[6] = {0.0, ms_scale_x, 0.0, 0.0, 0.0, ms_scale_y};

  setup.pan_ds = CreateMemDataset(pan_width, pan_height, 1, pan_dt, pan_gt);
  setup.ms_ds = CreateMemDataset(ms_width, ms_height, ms_bands, ms_dt, ms_gt);

  auto* opts = GDALCreatePansharpenOptions();
  opts->ePansharpenAlg = GDAL_PSH_WEIGHTED_BROVEY;
  opts->eResampleAlg = resample_alg;
  opts->hPanchroBand = setup.pan_ds->GetRasterBand(1);
  opts->nInputSpectralBands = ms_bands;
  opts->pahInputSpectralBands = static_cast<GDALRasterBandH*>(
      CPLMalloc(sizeof(GDALRasterBandH) * ms_bands));
  for (int i = 0; i < ms_bands; ++i) {
    opts->pahInputSpectralBands[i] = setup.ms_ds->GetRasterBand(i + 1);
  }
  opts->nWeightCount = ms_bands;
  opts->padfWeights =
      static_cast<double*>(CPLMalloc(sizeof(double) * ms_bands));
  for (int i = 0; i < ms_bands; ++i) {
    opts->padfWeights[i] = 1.0 / ms_bands;
  }
  opts->nOutPansharpenedBands = ms_bands;
  opts->panOutPansharpenedBands =
      static_cast<int*>(CPLMalloc(sizeof(int) * ms_bands));
  for (int i = 0; i < ms_bands; ++i) {
    opts->panOutPansharpenedBands[i] = i;
  }
  opts->bHasNoData = FALSE;
  opts->dfNoData = 0.0;
  opts->nThreads = 0;

  setup.options = std::unique_ptr<GDALPansharpenOptions,
                                  decltype(&GDALDestroyPansharpenOptions)>(
      opts, GDALDestroyPansharpenOptions);
  return setup;
}

class GdalPansharpenTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_VRT();
  }
};

// ---------------------------------------------------------------------------
// 1. Options Lifecycle & Cloning
// ---------------------------------------------------------------------------

TEST_F(GdalPansharpenTest, CreateAndDestroyOptions) {
  // Test creation and default values.
  GDALPansharpenOptions* opts = GDALCreatePansharpenOptions();
  ASSERT_NE(nullptr, opts);
  EXPECT_EQ(GDAL_PSH_WEIGHTED_BROVEY, opts->ePansharpenAlg);
  EXPECT_EQ(GRIORA_Cubic, opts->eResampleAlg);
  EXPECT_EQ(0, opts->nBitDepth);
  EXPECT_EQ(0, opts->nWeightCount);
  EXPECT_EQ(nullptr, opts->padfWeights);
  EXPECT_EQ(nullptr, opts->hPanchroBand);
  EXPECT_EQ(0, opts->nInputSpectralBands);
  EXPECT_EQ(nullptr, opts->pahInputSpectralBands);
  EXPECT_EQ(0, opts->nOutPansharpenedBands);
  EXPECT_EQ(nullptr, opts->panOutPansharpenedBands);
  EXPECT_EQ(FALSE, opts->bHasNoData);
  EXPECT_DOUBLE_EQ(0.0, opts->dfNoData);
  EXPECT_EQ(0, opts->nThreads);

  GDALDestroyPansharpenOptions(opts);

  // Calling destroy on nullptr must be a safe no-op.
  GDALDestroyPansharpenOptions(nullptr);
}

TEST_F(GdalPansharpenTest, CloneOptionsEmpty) {
  std::unique_ptr<GDALPansharpenOptions,
                  decltype(&GDALDestroyPansharpenOptions)>
      opts(GDALCreatePansharpenOptions(), GDALDestroyPansharpenOptions);
  ASSERT_NE(nullptr, opts);

  std::unique_ptr<GDALPansharpenOptions,
                  decltype(&GDALDestroyPansharpenOptions)>
      cloned(GDALClonePansharpenOptions(opts.get()),
             GDALDestroyPansharpenOptions);
  ASSERT_NE(nullptr, cloned);
  EXPECT_EQ(opts->ePansharpenAlg, cloned->ePansharpenAlg);
  EXPECT_EQ(opts->eResampleAlg, cloned->eResampleAlg);
  EXPECT_EQ(opts->nBitDepth, cloned->nBitDepth);
  EXPECT_EQ(opts->nWeightCount, cloned->nWeightCount);
  EXPECT_EQ(nullptr, cloned->padfWeights);
  EXPECT_EQ(nullptr, cloned->hPanchroBand);
  EXPECT_EQ(opts->nInputSpectralBands, cloned->nInputSpectralBands);
  EXPECT_EQ(nullptr, cloned->pahInputSpectralBands);
  EXPECT_EQ(opts->nOutPansharpenedBands, cloned->nOutPansharpenedBands);
  EXPECT_EQ(nullptr, cloned->panOutPansharpenedBands);
  EXPECT_EQ(opts->bHasNoData, cloned->bHasNoData);
  EXPECT_DOUBLE_EQ(opts->dfNoData, cloned->dfNoData);
  EXPECT_EQ(opts->nThreads, cloned->nThreads);
}

TEST_F(GdalPansharpenTest, CloneOptionsPopulated) {
  auto setup = CreateStandardSetup();
  setup.options->nBitDepth = 12;
  setup.options->bHasNoData = TRUE;
  setup.options->dfNoData = -9999.0;
  setup.options->nThreads = 4;

  std::unique_ptr<GDALPansharpenOptions,
                  decltype(&GDALDestroyPansharpenOptions)>
      cloned(GDALClonePansharpenOptions(setup.options.get()),
             GDALDestroyPansharpenOptions);
  ASSERT_NE(nullptr, cloned);

  EXPECT_EQ(setup.options->ePansharpenAlg, cloned->ePansharpenAlg);
  EXPECT_EQ(setup.options->eResampleAlg, cloned->eResampleAlg);
  EXPECT_EQ(setup.options->nBitDepth, cloned->nBitDepth);
  EXPECT_EQ(setup.options->nWeightCount, cloned->nWeightCount);
  EXPECT_EQ(setup.options->hPanchroBand, cloned->hPanchroBand);
  EXPECT_EQ(setup.options->nInputSpectralBands, cloned->nInputSpectralBands);
  EXPECT_EQ(setup.options->nOutPansharpenedBands,
            cloned->nOutPansharpenedBands);
  EXPECT_EQ(setup.options->bHasNoData, cloned->bHasNoData);
  EXPECT_DOUBLE_EQ(setup.options->dfNoData, cloned->dfNoData);
  EXPECT_EQ(setup.options->nThreads, cloned->nThreads);

  // Deep-copy checks: pointers must differ but contents must match.
  ASSERT_NE(setup.options->padfWeights, cloned->padfWeights);
  for (int i = 0; i < setup.options->nWeightCount; ++i) {
    EXPECT_DOUBLE_EQ(setup.options->padfWeights[i], cloned->padfWeights[i]);
  }

  ASSERT_NE(setup.options->pahInputSpectralBands,
            cloned->pahInputSpectralBands);
  for (int i = 0; i < setup.options->nInputSpectralBands; ++i) {
    EXPECT_EQ(setup.options->pahInputSpectralBands[i],
              cloned->pahInputSpectralBands[i]);
  }

  ASSERT_NE(setup.options->panOutPansharpenedBands,
            cloned->panOutPansharpenedBands);
  for (int i = 0; i < setup.options->nOutPansharpenedBands; ++i) {
    EXPECT_EQ(setup.options->panOutPansharpenedBands[i],
              cloned->panOutPansharpenedBands[i]);
  }

  // Modifying clone does not mutate original.
  cloned->padfWeights[0] = 0.999;
  EXPECT_NE(setup.options->padfWeights[0], cloned->padfWeights[0]);
}

// ---------------------------------------------------------------------------
// 2. Initialize - Parameter Validation & Early Errors
// ---------------------------------------------------------------------------

TEST_F(GdalPansharpenTest, InitializeNullPanBand) {
  auto setup = CreateStandardSetup();
  setup.options->hPanchroBand = nullptr;
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("hPanchroBand not set"));
}

TEST_F(GdalPansharpenTest, InitializeZeroOrNegativeSpectralBands) {
  // 1. Zero input spectral bands.
  {
    auto setup = CreateStandardSetup();
    setup.options->nInputSpectralBands = 0;
    GDALPansharpenOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("No input spectral bands defined"));
  }
  // 2. Negative input spectral bands.
  {
    auto setup = CreateStandardSetup();
    setup.options->nInputSpectralBands = -1;
    GDALPansharpenOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("No input spectral bands defined"));
  }
}

TEST_F(GdalPansharpenTest, InitializeNullWeights) {
  auto setup = CreateStandardSetup();
  CPLFree(setup.options->padfWeights);
  setup.options->padfWeights = nullptr;
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("No weights defined"));
}

TEST_F(GdalPansharpenTest, InitializeMismatchedWeightCount) {
  auto setup = CreateStandardSetup();
  setup.options->nWeightCount = setup.options->nInputSpectralBands + 1;
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("not the same number as input spectral bands"));
}

class DummyOrphanBand final : public GDALRasterBand {
 public:
  DummyOrphanBand(GDALDataset* parent_ds, int band_num) {
    poDS = parent_ds;
    nBand = band_num;
    nRasterXSize = 4;
    nRasterYSize = 4;
    eDataType = GDT_Byte;
    nBlockXSize = 4;
    nBlockYSize = 1;
  }

 protected:
  CPLErr IReadBlock(int, int, void*) override { return CE_None; }
};

TEST_F(GdalPansharpenTest, InitializePanBandNoDataset) {
  auto setup = CreateStandardSetup();
  DummyOrphanBand standalone_band(nullptr, 1);
  setup.options->hPanchroBand = &standalone_band;
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(
      CPLGetLastErrorMsg(),
      HasSubstr("Cannot retrieve dataset associated with hPanchroBand"));
}

TEST_F(GdalPansharpenTest, InitializePanBandNotChildOfDataset) {
  auto setup = CreateStandardSetup();
  DummyOrphanBand detached_band(setup.pan_ds.get(), 1);
  setup.options->hPanchroBand = &detached_band;
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(
      CPLGetLastErrorMsg(),
      HasSubstr("poPanchroDS->GetRasterBand(poPanchroBand->GetBand()) != "
                "poPanchroBand"));
}

TEST_F(GdalPansharpenTest, InitializePanBandNoGeoTransform) {
  auto setup = CreateStandardSetup();
  auto pan_no_gt = CreateMemDataset(8, 8, 1, GDT_Byte, nullptr);
  setup.options->hPanchroBand = pan_no_gt->GetRasterBand(1);
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Panchromatic band has no associated geotransform"));
}

TEST_F(GdalPansharpenTest, InitializeSpectralBand0NoDataset) {
  auto setup = CreateStandardSetup();
  DummyOrphanBand standalone_band(nullptr, 1);
  setup.options->pahInputSpectralBands[0] = &standalone_band;
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(
      CPLGetLastErrorMsg(),
      HasSubstr(
          "Cannot retrieve dataset associated with ahInputSpectralBands[0]"));
}

TEST_F(GdalPansharpenTest, InitializeSpectralBand0NotChildOfDataset) {
  auto setup = CreateStandardSetup();
  DummyOrphanBand detached_band(setup.ms_ds.get(), 1);
  setup.options->pahInputSpectralBands[0] = &detached_band;
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("poRefBandDS->GetRasterBand(poRefBand->GetBand()) != "
                        "poRefBand"));
}

TEST_F(GdalPansharpenTest, InitializeSpectralBand0NoGeoTransform) {
  auto setup = CreateStandardSetup();
  auto ms_no_gt = CreateMemDataset(4, 4, 3, GDT_Byte, nullptr);
  setup.options->pahInputSpectralBands[0] = ms_no_gt->GetRasterBand(1);
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(
      CPLGetLastErrorMsg(),
      HasSubstr("ahInputSpectralBands[0] band has no associated geotransform"));
}

TEST_F(GdalPansharpenTest, InitializeSpectralBand0NonInvertibleGeoTransform) {
  auto setup = CreateStandardSetup();
  const double bad_gt[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  auto ms_bad_gt = CreateMemDataset(4, 4, 3, GDT_Byte, bad_gt);
  setup.options->pahInputSpectralBands[0] = ms_bad_gt->GetRasterBand(1);
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(
      CPLGetLastErrorMsg(),
      HasSubstr("ahInputSpectralBands[0] geotransform is not invertible"));
}

TEST_F(GdalPansharpenTest, InitializeRotationalTermsInGeoTransform) {
  auto setup = CreateStandardSetup();
  const double rotated_gt[6] = {0.0, 1.0, 0.5, 0.0, 0.5, -1.0};
  auto pan_rot = CreateMemDataset(8, 8, 1, GDT_Byte, rotated_gt);
  setup.options->hPanchroBand = pan_rot->GetRasterBand(1);
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("has rotational terms"));
}

TEST_F(GdalPansharpenTest, InitializeSpectralBandDimensionMismatch) {
  auto setup = CreateStandardSetup();
  const double ms_gt[6] = {0.0, 2.0, 0.0, 0.0, 0.0, -2.0};
  auto ms_diff_size = CreateMemDataset(6, 6, 1, GDT_Byte, ms_gt);
  setup.options->pahInputSpectralBands[1] = ms_diff_size->GetRasterBand(1);
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Dimensions of input spectral band 2 different"));
}

TEST_F(GdalPansharpenTest, InitializeSpectralBand1NoDataset) {
  auto setup = CreateStandardSetup();
  DummyOrphanBand standalone_band(nullptr, 1);
  setup.options->pahInputSpectralBands[1] = &standalone_band;
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(
      CPLGetLastErrorMsg(),
      HasSubstr(
          "Cannot retrieve dataset associated with input spectral band 2"));
}

TEST_F(GdalPansharpenTest, InitializeSpectralBand1NotChildOfDataset) {
  auto setup = CreateStandardSetup();
  DummyOrphanBand detached_band(setup.ms_ds.get(), 1);
  setup.options->pahInputSpectralBands[1] = &detached_band;
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(
      CPLGetLastErrorMsg(),
      HasSubstr("poBandDS->GetRasterBand(poBand->GetBand()) != poBand"));
}

TEST_F(GdalPansharpenTest, InitializeSpectralBand1NoGeoTransform) {
  auto setup = CreateStandardSetup();
  auto ms_band1_no_gt = CreateMemDataset(4, 4, 1, GDT_Byte, nullptr);
  setup.options->pahInputSpectralBands[1] = ms_band1_no_gt->GetRasterBand(1);
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(
      CPLGetLastErrorMsg(),
      HasSubstr("input spectral band 2 band has no associated geotransform"));
}

TEST_F(GdalPansharpenTest, InitializeSpectralBand1DifferentGeoTransform) {
  auto setup = CreateStandardSetup();
  const double diff_gt[6] = {10.0, 2.0, 0.0, 10.0, 0.0, -2.0};
  auto ms_diff_gt = CreateMemDataset(4, 4, 1, GDT_Byte, diff_gt);
  setup.options->pahInputSpectralBands[1] = ms_diff_gt->GetRasterBand(1);
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("input spectral band 2 has a different geotransform"));
}

TEST_F(GdalPansharpenTest, InitializeNoOutputBandsWarning) {
  auto setup = CreateStandardSetup();
  setup.options->nOutPansharpenedBands = 0;
  GDALPansharpenOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_None, op.Initialize(setup.options.get()));
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("No output pansharpened band defined"));
}

TEST_F(GdalPansharpenTest, InitializeInvalidOutputBandIndices) {
  // 1. Negative output band index.
  {
    auto setup = CreateStandardSetup();
    setup.options->panOutPansharpenedBands[0] = -1;
    GDALPansharpenOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid value panOutPansharpenedBands[0] = -1"));
  }
  // 2. Output band index >= input bands count.
  {
    auto setup = CreateStandardSetup();
    setup.options->panOutPansharpenedBands[0] =
        setup.options->nInputSpectralBands;
    GDALPansharpenOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid value panOutPansharpenedBands[0] = 3"));
  }
}

TEST_F(GdalPansharpenTest, InitializeBitDepthValidation) {
  // 1. Negative bit depth.
  {
    auto setup = CreateStandardSetup();
    setup.options->nBitDepth = -1;
    GDALPansharpenOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid value nBitDepth = -1"));
  }
  // 2. Bit depth > 31.
  {
    auto setup = CreateStandardSetup();
    setup.options->nBitDepth = 32;
    GDALPansharpenOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid value nBitDepth = 32"));
  }
  // 3. Byte with bit depth > 8.
  {
    auto setup = CreateStandardSetup(8, 8, 4, 4, 3, GDT_Byte, GDT_Byte);
    setup.options->nBitDepth = 9;
    GDALPansharpenOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid value nBitDepth = 9 for type Byte"));
  }
  // 4. UInt16 with bit depth > 16.
  {
    auto setup = CreateStandardSetup(8, 8, 4, 4, 3, GDT_UInt16, GDT_UInt16);
    setup.options->nBitDepth = 17;
    GDALPansharpenOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(setup.options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Invalid value nBitDepth = 17 for type UInt16"));
  }
  // 5. Byte with bit depth == 8: resets nBitDepth to 0.
  {
    auto setup = CreateStandardSetup(8, 8, 4, 4, 3, GDT_Byte, GDT_Byte);
    setup.options->nBitDepth = 8;
    GDALPansharpenOperation op;
    EXPECT_EQ(CE_None, op.Initialize(setup.options.get()));
    ASSERT_NE(nullptr, op.GetOptions());
    EXPECT_EQ(0, op.GetOptions()->nBitDepth);
  }
  // 6. UInt16 with bit depth == 16: resets nBitDepth to 0.
  {
    auto setup = CreateStandardSetup(8, 8, 4, 4, 3, GDT_UInt16, GDT_UInt16);
    setup.options->nBitDepth = 16;
    GDALPansharpenOperation op;
    EXPECT_EQ(CE_None, op.Initialize(setup.options.get()));
    ASSERT_NE(nullptr, op.GetOptions());
    EXPECT_EQ(0, op.GetOptions()->nBitDepth);
  }
  // 7. Float32 with bit depth == 12: warns and resets to 0.
  {
    auto setup = CreateStandardSetup(8, 8, 4, 4, 3, GDT_Float32, GDT_Float32);
    setup.options->nBitDepth = 12;
    GDALPansharpenOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None, op.Initialize(setup.options.get()));
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Ignoring nBitDepth = 12 for type Float32"));
    ASSERT_NE(nullptr, op.GetOptions());
    EXPECT_EQ(0, op.GetOptions()->nBitDepth);
  }
}

// ---------------------------------------------------------------------------
// 3. Initialize - Configuration Branches
// ---------------------------------------------------------------------------

TEST_F(GdalPansharpenTest, ResamplingAlgorithms) {
  constexpr std::array<GDALRIOResampleAlg, 9> kAlgorithms = {
      GRIORA_NearestNeighbour,
      GRIORA_Bilinear,
      GRIORA_Cubic,
      GRIORA_CubicSpline,
      GRIORA_Lanczos,
      GRIORA_Average,
      GRIORA_RMS,
      GRIORA_Mode,
      GRIORA_Gauss,
  };

  for (const auto alg : kAlgorithms) {
    auto setup = CreateStandardSetup();
    setup.options->eResampleAlg = alg;
    GDALPansharpenOperation op;
    EXPECT_EQ(CE_None, op.Initialize(setup.options.get()));
  }

  // Unknown resampling algorithm exercises the "UNKNOWN" branch.
  // GDALGetResampleFunction emits CE_Failure while Initialize still returns
  // CE_None.
  {
    auto setup = CreateStandardSetup();
    setup.options->eResampleAlg = static_cast<GDALRIOResampleAlg>(15);
    GDALPansharpenOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None, op.Initialize(setup.options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unsupported resampling method"));
    CPLErrorReset();
  }
}

TEST_F(GdalPansharpenTest, ThreadPoolConfiguration) {
  // 1. Explicit thread count > 1.
  {
    auto setup = CreateStandardSetup();
    setup.options->nThreads = 2;
    GDALPansharpenOperation op;
    EXPECT_EQ(CE_None, op.Initialize(setup.options.get()));
  }
  // 2. All CPUs (-1).
  {
    auto setup = CreateStandardSetup();
    setup.options->nThreads = -1;
    GDALPansharpenOperation op;
    EXPECT_EQ(CE_None, op.Initialize(setup.options.get()));
  }
  // 3. nThreads = 0 with GDAL_NUM_THREADS = "ALL_CPUS".
  {
    CPLSetThreadLocalConfigOption("GDAL_NUM_THREADS", "ALL_CPUS");
    absl::Cleanup cleaner = [] {
      CPLSetThreadLocalConfigOption("GDAL_NUM_THREADS", nullptr);
    };
    auto setup = CreateStandardSetup();
    setup.options->nThreads = 0;
    GDALPansharpenOperation op;
    EXPECT_EQ(CE_None, op.Initialize(setup.options.get()));
  }
  // 4. nThreads = 0 with GDAL_NUM_THREADS = "2".
  {
    CPLSetThreadLocalConfigOption("GDAL_NUM_THREADS", "2");
    absl::Cleanup cleaner = [] {
      CPLSetThreadLocalConfigOption("GDAL_NUM_THREADS", nullptr);
    };
    auto setup = CreateStandardSetup();
    setup.options->nThreads = 0;
    GDALPansharpenOperation op;
    EXPECT_EQ(CE_None, op.Initialize(setup.options.get()));
  }
  // 5. nThreads = 0 with GDAL_NUM_THREADS = "1".
  {
    CPLSetThreadLocalConfigOption("GDAL_NUM_THREADS", "1");
    absl::Cleanup cleaner = [] {
      CPLSetThreadLocalConfigOption("GDAL_NUM_THREADS", nullptr);
    };
    auto setup = CreateStandardSetup();
    setup.options->nThreads = 0;
    GDALPansharpenOperation op;
    EXPECT_EQ(CE_None, op.Initialize(setup.options.get()));
  }
}

TEST_F(GdalPansharpenTest, VRTWrappingNoData) {
  // Case A: Spectral bands already have matching nodata value -> no VRT needed.
  {
    auto setup = CreateStandardSetup();
    setup.options->bHasNoData = TRUE;
    setup.options->dfNoData = -9999.0;
    for (int b = 1; b <= 3; ++b) {
      setup.ms_ds->GetRasterBand(b)->SetNoDataValue(-9999.0);
    }
    GDALPansharpenOperation op;
    EXPECT_EQ(CE_None, op.Initialize(setup.options.get()));
  }

  // Case B: Spectral bands have no nodata -> wrapped in VRT (same dataset).
  {
    auto setup = CreateStandardSetup();
    setup.options->bHasNoData = TRUE;
    setup.options->dfNoData = -9999.0;
    setup.ms_ds->GetRasterBand(1)->SetMetadataItem("NBITS", "10",
                                                   "IMAGE_STRUCTURE");
    GDALPansharpenOperation op;
    EXPECT_EQ(CE_None, op.Initialize(setup.options.get()));

    // Run ProcessRegion on wrapped bands to exercise the VRT reading path.
    std::vector<uint8_t> out_buf(8 * 8 * 3);
    EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 8, 8, out_buf.data(), GDT_Byte));
  }

  // Case C: Spectral bands from different datasets without nodata -> separate
  // VRTs.
  {
    auto setup = CreateStandardSetup(8, 8, 4, 4, 1);
    const double ms_gt[6] = {0.0, 2.0, 0.0, 0.0, 0.0, -2.0};
    auto ms2 = CreateMemDataset(4, 4, 1, GDT_Byte, ms_gt);
    setup.options->nInputSpectralBands = 2;
    CPLFree(setup.options->pahInputSpectralBands);
    setup.options->pahInputSpectralBands =
        static_cast<GDALRasterBandH*>(CPLMalloc(sizeof(GDALRasterBandH) * 2));
    setup.options->pahInputSpectralBands[0] = setup.ms_ds->GetRasterBand(1);
    setup.options->pahInputSpectralBands[1] = ms2->GetRasterBand(1);

    setup.options->nWeightCount = 2;
    CPLFree(setup.options->padfWeights);
    setup.options->padfWeights =
        static_cast<double*>(CPLMalloc(sizeof(double) * 2));
    setup.options->padfWeights[0] = 0.5;
    setup.options->padfWeights[1] = 0.5;

    setup.options->nOutPansharpenedBands = 2;
    CPLFree(setup.options->panOutPansharpenedBands);
    setup.options->panOutPansharpenedBands =
        static_cast<int*>(CPLMalloc(sizeof(int) * 2));
    setup.options->panOutPansharpenedBands[0] = 0;
    setup.options->panOutPansharpenedBands[1] = 1;

    setup.options->bHasNoData = TRUE;
    setup.options->dfNoData = -9999.0;

    GDALPansharpenOperation op;
    EXPECT_EQ(CE_None, op.Initialize(setup.options.get()));

    std::vector<uint8_t> out_buf(8 * 8 * 2);
    EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 8, 8, out_buf.data(), GDT_Byte));
  }
}

// ---------------------------------------------------------------------------
// 4. ProcessRegion - Direct / Non-Upsampling Execution
// ---------------------------------------------------------------------------

TEST_F(GdalPansharpenTest, ProcessRegionUninitialized) {
  GDALPansharpenOperation op;
  std::vector<uint8_t> buf(16);
  EXPECT_EQ(CE_Failure, op.ProcessRegion(0, 0, 4, 4, buf.data(), GDT_Byte));
}

TEST_F(GdalPansharpenTest, ProcessRegionPanRasterIOError) {
  auto setup = CreateStandardSetup();
  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));
  std::vector<uint8_t> buf(16);
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.ProcessRegion(-10, -10, 4, 4, buf.data(), GDT_Byte));
}

TEST_F(GdalPansharpenTest, ProcessRegionNearestNeighbour) {
  auto setup = CreateStandardSetup(4, 4, 2, 2, 3, GDT_Byte, GDT_Byte,
                                   GRIORA_NearestNeighbour);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint8_t>(40));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(4 * 4 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 4, 4, out_buf.data(), GDT_Byte));

  // pseudo_pan = (10+20+30)/3 = 20. factor = 40/20 = 2.0.
  // Band 1 = 20, Band 2 = 40, Band 3 = 60.
  for (int j = 0; j < 16; ++j) {
    EXPECT_EQ(20, out_buf[j]);
    EXPECT_EQ(40, out_buf[16 + j]);
    EXPECT_EQ(60, out_buf[32 + j]);
  }
}

TEST_F(GdalPansharpenTest, ProcessRegionSameResolution) {
  // Pan and MS have identical resolution (4x4).
  auto setup = CreateStandardSetup(4, 4, 4, 4, 3, GDT_Byte, GDT_Byte);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint8_t>(40));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(4 * 4 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 4, 4, out_buf.data(), GDT_Byte));

  for (int j = 0; j < 16; ++j) {
    EXPECT_EQ(20, out_buf[j]);
    EXPECT_EQ(40, out_buf[16 + j]);
    EXPECT_EQ(60, out_buf[32 + j]);
  }
}

TEST_F(GdalPansharpenTest, ProcessRegionSingleLine) {
  // nYSize = 1 triggers the direct path.
  auto setup = CreateStandardSetup(8, 8, 4, 4, 3);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint8_t>(40));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(8 * 1 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 8, 1, out_buf.data(), GDT_Byte));

  for (int j = 0; j < 8; ++j) {
    EXPECT_EQ(20, out_buf[j]);
    EXPECT_EQ(40, out_buf[8 + j]);
    EXPECT_EQ(60, out_buf[16 + j]);
  }
}

TEST_F(GdalPansharpenTest, ProcessRegionDirectSeparateDatasets) {
  // Direct path with spectral bands coming from separate datasets.
  auto setup = CreateStandardSetup(4, 4, 4, 4, 1);
  const double ms_gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  auto ms2 = CreateMemDataset(4, 4, 1, GDT_Byte, ms_gt);
  ASSERT_NE(nullptr, ms2);

  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint8_t>(40));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(ms2->GetRasterBand(1), static_cast<uint8_t>(30));

  setup.options->nInputSpectralBands = 2;
  CPLFree(setup.options->pahInputSpectralBands);
  setup.options->pahInputSpectralBands =
      static_cast<GDALRasterBandH*>(CPLMalloc(sizeof(GDALRasterBandH) * 2));
  setup.options->pahInputSpectralBands[0] = setup.ms_ds->GetRasterBand(1);
  setup.options->pahInputSpectralBands[1] = ms2->GetRasterBand(1);

  setup.options->nWeightCount = 2;
  CPLFree(setup.options->padfWeights);
  setup.options->padfWeights =
      static_cast<double*>(CPLMalloc(sizeof(double) * 2));
  setup.options->padfWeights[0] = 0.5;
  setup.options->padfWeights[1] = 0.5;

  setup.options->nOutPansharpenedBands = 2;
  CPLFree(setup.options->panOutPansharpenedBands);
  setup.options->panOutPansharpenedBands =
      static_cast<int*>(CPLMalloc(sizeof(int) * 2));
  setup.options->panOutPansharpenedBands[0] = 0;
  setup.options->panOutPansharpenedBands[1] = 1;

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(4 * 4 * 2);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 4, 4, out_buf.data(), GDT_Byte));

  // pseudo_pan = (10+30)/2 = 20. factor = 40/20 = 2.0.
  // Band 1 = 20, Band 2 = 60.
  for (int j = 0; j < 16; ++j) {
    EXPECT_EQ(20, out_buf[j]);
    EXPECT_EQ(60, out_buf[16 + j]);
  }
}

// ---------------------------------------------------------------------------
// 5. ProcessRegion - Upsampling Execution (Single-Threaded)
// ---------------------------------------------------------------------------

TEST_F(GdalPansharpenTest, ProcessRegionUpsamplingSameDataset) {
  auto setup =
      CreateStandardSetup(8, 8, 4, 4, 3, GDT_Byte, GDT_Byte, GRIORA_Cubic);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint8_t>(40));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(8 * 8 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 8, 8, out_buf.data(), GDT_Byte));

  for (int j = 0; j < 64; ++j) {
    EXPECT_EQ(20, out_buf[j]);
    EXPECT_EQ(40, out_buf[64 + j]);
    EXPECT_EQ(60, out_buf[128 + j]);
  }
}

TEST_F(GdalPansharpenTest, ProcessRegionUpsamplingSeparateDatasets) {
  auto setup = CreateStandardSetup(8, 8, 4, 4, 1);
  const double ms_gt[6] = {0.0, 2.0, 0.0, 0.0, 0.0, -2.0};
  auto ms2 = CreateMemDataset(4, 4, 1, GDT_Byte, ms_gt);
  ASSERT_NE(nullptr, ms2);

  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint8_t>(40));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(ms2->GetRasterBand(1), static_cast<uint8_t>(30));

  setup.options->nInputSpectralBands = 2;
  CPLFree(setup.options->pahInputSpectralBands);
  setup.options->pahInputSpectralBands =
      static_cast<GDALRasterBandH*>(CPLMalloc(sizeof(GDALRasterBandH) * 2));
  setup.options->pahInputSpectralBands[0] = setup.ms_ds->GetRasterBand(1);
  setup.options->pahInputSpectralBands[1] = ms2->GetRasterBand(1);

  setup.options->nWeightCount = 2;
  CPLFree(setup.options->padfWeights);
  setup.options->padfWeights =
      static_cast<double*>(CPLMalloc(sizeof(double) * 2));
  setup.options->padfWeights[0] = 0.5;
  setup.options->padfWeights[1] = 0.5;

  setup.options->nOutPansharpenedBands = 2;
  CPLFree(setup.options->panOutPansharpenedBands);
  setup.options->panOutPansharpenedBands =
      static_cast<int*>(CPLMalloc(sizeof(int) * 2));
  setup.options->panOutPansharpenedBands[0] = 0;
  setup.options->panOutPansharpenedBands[1] = 1;

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(8 * 8 * 2);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 8, 8, out_buf.data(), GDT_Byte));

  for (int j = 0; j < 64; ++j) {
    EXPECT_EQ(20, out_buf[j]);
    EXPECT_EQ(60, out_buf[64 + j]);
  }
}

TEST_F(GdalPansharpenTest, ProcessRegionSubWindows) {
  auto setup =
      CreateStandardSetup(12, 12, 6, 6, 3, GDT_Byte, GDT_Byte, GRIORA_Cubic);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint8_t>(40));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  // 1. Top-left corner (tests nXOffExtract < 0, nYOffExtract < 0).
  {
    std::vector<uint8_t> out_buf(4 * 4 * 3);
    EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 4, 4, out_buf.data(), GDT_Byte));
    EXPECT_EQ(20, out_buf[0]);
    EXPECT_EQ(40, out_buf[16]);
    EXPECT_EQ(60, out_buf[32]);
  }

  // 2. Interior window.
  {
    std::vector<uint8_t> out_buf(4 * 4 * 3);
    EXPECT_EQ(CE_None, op.ProcessRegion(4, 4, 4, 4, out_buf.data(), GDT_Byte));
    EXPECT_EQ(20, out_buf[0]);
    EXPECT_EQ(40, out_buf[16]);
    EXPECT_EQ(60, out_buf[32]);
  }

  // 3. Bottom-right corner (tests right/bottom boundary clamping).
  {
    std::vector<uint8_t> out_buf(4 * 4 * 3);
    EXPECT_EQ(CE_None, op.ProcessRegion(8, 8, 4, 4, out_buf.data(), GDT_Byte));
    EXPECT_EQ(20, out_buf[0]);
    EXPECT_EQ(40, out_buf[16]);
    EXPECT_EQ(60, out_buf[32]);
  }
}

TEST_F(GdalPansharpenTest, ProcessRegionTinyWindowClamping) {
  // Pan is 20x20 with scale 1.0, MS is 2x2 with scale 10.0.
  auto setup = CreateStandardSetup(20, 20, 2, 2, 3, GDT_Byte, GDT_Byte);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint8_t>(40));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  // 1x1 region tests nSpectralXSize == 0 -> 1 and nSpectralYSize == 0 -> 1.
  std::vector<uint8_t> out_buf(1 * 1 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 1, 1, out_buf.data(), GDT_Byte));
  EXPECT_EQ(20, out_buf[0]);
  EXPECT_EQ(40, out_buf[1]);
  EXPECT_EQ(60, out_buf[2]);
}

// ---------------------------------------------------------------------------
// 6. Multi-Threaded Execution
// ---------------------------------------------------------------------------

TEST_F(GdalPansharpenTest, MultiThreadedUpsamplingMatchesSingleThreaded) {
  constexpr int kWidth = 12;
  constexpr int kHeight = 12;
  auto setup = CreateStandardSetup(kWidth, kHeight, 6, 6, 3, GDT_Byte, GDT_Byte,
                                   GRIORA_Cubic);

  // Fill pan band with varying values.
  std::vector<uint8_t> pan_data(kWidth * kHeight);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    pan_data[i] = static_cast<uint8_t>(20 + (i % 60));
  }
  FillBand(setup.pan_ds->GetRasterBand(1), pan_data);

  // Fill MS bands.
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(15));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(30));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(45));

  // 1. Single-threaded run.
  setup.options->nThreads = 0;
  GDALPansharpenOperation op_st;
  ASSERT_EQ(CE_None, op_st.Initialize(setup.options.get()));
  std::vector<uint8_t> out_st(kWidth * kHeight * 3);
  EXPECT_EQ(CE_None, op_st.ProcessRegion(0, 0, kWidth, kHeight, out_st.data(),
                                         GDT_Byte));

  // 2. Multi-threaded run with 2 worker threads.
  setup.options->nThreads = 2;
  GDALPansharpenOperation op_mt;
  ASSERT_EQ(CE_None, op_mt.Initialize(setup.options.get()));
  std::vector<uint8_t> out_mt(kWidth * kHeight * 3);
  EXPECT_EQ(CE_None, op_mt.ProcessRegion(0, 0, kWidth, kHeight, out_mt.data(),
                                         GDT_Byte));

  // Multi-threaded and single-threaded results must match bit-for-bit.
  EXPECT_EQ(out_st, out_mt);
}

TEST_F(GdalPansharpenTest, MultiThreadedDirectMatchesSingleThreaded) {
  constexpr int kWidth = 12;
  constexpr int kHeight = 12;
  auto setup = CreateStandardSetup(kWidth, kHeight, 6, 6, 3, GDT_Byte, GDT_Byte,
                                   GRIORA_NearestNeighbour);

  std::vector<uint8_t> pan_data(kWidth * kHeight);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    pan_data[i] = static_cast<uint8_t>(20 + (i % 60));
  }
  FillBand(setup.pan_ds->GetRasterBand(1), pan_data);
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(15));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(30));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(45));

  setup.options->nThreads = 0;
  GDALPansharpenOperation op_st;
  ASSERT_EQ(CE_None, op_st.Initialize(setup.options.get()));
  std::vector<uint8_t> out_st(kWidth * kHeight * 3);
  EXPECT_EQ(CE_None, op_st.ProcessRegion(0, 0, kWidth, kHeight, out_st.data(),
                                         GDT_Byte));

  setup.options->nThreads = 2;
  GDALPansharpenOperation op_mt;
  ASSERT_EQ(CE_None, op_mt.Initialize(setup.options.get()));
  std::vector<uint8_t> out_mt(kWidth * kHeight * 3);
  EXPECT_EQ(CE_None, op_mt.ProcessRegion(0, 0, kWidth, kHeight, out_mt.data(),
                                         GDT_Byte));

  EXPECT_EQ(out_st, out_mt);
}

TEST_F(GdalPansharpenTest, MultiThreadedHeightLessThanTasks) {
  // Height = 2 with 4 threads tests nTasks = nYSize clamping.
  auto setup = CreateStandardSetup(8, 8, 4, 4, 3);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint8_t>(40));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));

  setup.options->nThreads = 4;
  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(8 * 2 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 8, 2, out_buf.data(), GDT_Byte));

  for (int j = 0; j < 16; ++j) {
    EXPECT_EQ(20, out_buf[j]);
    EXPECT_EQ(40, out_buf[16 + j]);
    EXPECT_EQ(60, out_buf[32 + j]);
  }
}

// ---------------------------------------------------------------------------
// 7. Weighted Brovey Algorithms & SIMD Branches
// ---------------------------------------------------------------------------

TEST_F(GdalPansharpenTest, WeightedBrovey3Input3Output) {
  // 7x1 pixels: exercises 4-pixel SIMD loop and 3-pixel remainder loop.
  constexpr int kPixels = 7;
  auto setup =
      CreateStandardSetup(kPixels, 1, kPixels, 1, 3, GDT_Byte, GDT_Byte);

  const std::vector<uint8_t> pan_data = {10, 20, 30, 40, 50, 60, 70};
  FillBand(setup.pan_ds->GetRasterBand(1), pan_data);
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));

  setup.options->padfWeights[0] = 0.2;
  setup.options->padfWeights[1] = 0.4;
  setup.options->padfWeights[2] = 0.4;

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(kPixels * 3);
  EXPECT_EQ(CE_None,
            op.ProcessRegion(0, 0, kPixels, 1, out_buf.data(), GDT_Byte));

  // pseudo_pan = 0.2*10 + 0.4*20 + 0.4*30 = 22.0.
  // factor[j] = pan[j] / 22.0.
  for (int j = 0; j < kPixels; ++j) {
    const double factor = pan_data[j] / 22.0;
    const uint8_t exp1 = static_cast<uint8_t>(10.0 * factor + 0.5);
    const uint8_t exp2 = static_cast<uint8_t>(20.0 * factor + 0.5);
    const uint8_t exp3 = static_cast<uint8_t>(30.0 * factor + 0.5);
    EXPECT_EQ(exp1, out_buf[j]);
    EXPECT_EQ(exp2, out_buf[kPixels + j]);
    EXPECT_EQ(exp3, out_buf[2 * kPixels + j]);
  }
}

TEST_F(GdalPansharpenTest, WeightedBrovey4Input4Output) {
  // 7x1 pixels: exercises WeightedBroveyPositiveWeightsInternal<T, 4, 4>.
  constexpr int kPixels = 7;
  auto setup =
      CreateStandardSetup(kPixels, 1, kPixels, 1, 4, GDT_Byte, GDT_Byte);

  const std::vector<uint8_t> pan_data = {10, 20, 30, 40, 50, 60, 70};
  FillBand(setup.pan_ds->GetRasterBand(1), pan_data);
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));
  FillBandConstant(setup.ms_ds->GetRasterBand(4), static_cast<uint8_t>(40));

  setup.options->padfWeights[0] = 0.25;
  setup.options->padfWeights[1] = 0.25;
  setup.options->padfWeights[2] = 0.25;
  setup.options->padfWeights[3] = 0.25;

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(kPixels * 4);
  EXPECT_EQ(CE_None,
            op.ProcessRegion(0, 0, kPixels, 1, out_buf.data(), GDT_Byte));

  // pseudo_pan = (10+20+30+40)*0.25 = 25.0.
  for (int j = 0; j < kPixels; ++j) {
    const double factor = pan_data[j] / 25.0;
    const uint8_t exp1 = static_cast<uint8_t>(10.0 * factor + 0.5);
    const uint8_t exp2 = static_cast<uint8_t>(20.0 * factor + 0.5);
    const uint8_t exp3 = static_cast<uint8_t>(30.0 * factor + 0.5);
    const uint8_t exp4 = static_cast<uint8_t>(40.0 * factor + 0.5);
    EXPECT_EQ(exp1, out_buf[j]);
    EXPECT_EQ(exp2, out_buf[kPixels + j]);
    EXPECT_EQ(exp3, out_buf[2 * kPixels + j]);
    EXPECT_EQ(exp4, out_buf[3 * kPixels + j]);
  }
}

TEST_F(GdalPansharpenTest, WeightedBrovey4Input3Output) {
  // 7x1 pixels: exercises WeightedBroveyPositiveWeightsInternal<T, 4, 3>.
  constexpr int kPixels = 7;
  auto setup =
      CreateStandardSetup(kPixels, 1, kPixels, 1, 4, GDT_Byte, GDT_Byte);

  const std::vector<uint8_t> pan_data = {10, 20, 30, 40, 50, 60, 70};
  FillBand(setup.pan_ds->GetRasterBand(1), pan_data);
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));
  FillBandConstant(setup.ms_ds->GetRasterBand(4), static_cast<uint8_t>(40));

  setup.options->padfWeights[0] = 0.25;
  setup.options->padfWeights[1] = 0.25;
  setup.options->padfWeights[2] = 0.25;
  setup.options->padfWeights[3] = 0.25;

  setup.options->nOutPansharpenedBands = 3;
  setup.options->panOutPansharpenedBands[0] = 0;
  setup.options->panOutPansharpenedBands[1] = 1;
  setup.options->panOutPansharpenedBands[2] = 2;

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(kPixels * 3);
  EXPECT_EQ(CE_None,
            op.ProcessRegion(0, 0, kPixels, 1, out_buf.data(), GDT_Byte));

  // pseudo_pan = 25.0. Output bands: 0, 1, 2.
  for (int j = 0; j < kPixels; ++j) {
    const double factor = pan_data[j] / 25.0;
    const uint8_t exp1 = static_cast<uint8_t>(10.0 * factor + 0.5);
    const uint8_t exp2 = static_cast<uint8_t>(20.0 * factor + 0.5);
    const uint8_t exp3 = static_cast<uint8_t>(30.0 * factor + 0.5);
    EXPECT_EQ(exp1, out_buf[j]);
    EXPECT_EQ(exp2, out_buf[kPixels + j]);
    EXPECT_EQ(exp3, out_buf[2 * kPixels + j]);
  }
}

TEST_F(GdalPansharpenTest, WeightedBroveyFallback2Bands) {
  // 5x1 pixels (odd count): exercises general step-2 loop and remainder loop.
  constexpr int kPixels = 5;
  auto setup =
      CreateStandardSetup(kPixels, 1, kPixels, 1, 2, GDT_Byte, GDT_Byte);

  const std::vector<uint8_t> pan_data = {10, 20, 30, 40, 50};
  FillBand(setup.pan_ds->GetRasterBand(1), pan_data);
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(30));

  setup.options->padfWeights[0] = 0.5;
  setup.options->padfWeights[1] = 0.5;

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(kPixels * 2);
  EXPECT_EQ(CE_None,
            op.ProcessRegion(0, 0, kPixels, 1, out_buf.data(), GDT_Byte));

  // pseudo_pan = (10+30)*0.5 = 20.0.
  for (int j = 0; j < kPixels; ++j) {
    const double factor = pan_data[j] / 20.0;
    const uint8_t exp1 = static_cast<uint8_t>(10.0 * factor + 0.5);
    const uint8_t exp2 = static_cast<uint8_t>(30.0 * factor + 0.5);
    EXPECT_EQ(exp1, out_buf[j]);
    EXPECT_EQ(exp2, out_buf[kPixels + j]);
  }
}

TEST_F(GdalPansharpenTest, WeightedBroveyFallbackReorderedBands) {
  // 3 input bands, 2 output bands reordered (2, 0).
  constexpr int kPixels = 5;
  auto setup =
      CreateStandardSetup(kPixels, 1, kPixels, 1, 3, GDT_Byte, GDT_Byte);

  const std::vector<uint8_t> pan_data = {20, 40, 60, 80, 100};
  FillBand(setup.pan_ds->GetRasterBand(1), pan_data);
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));

  setup.options->nOutPansharpenedBands = 2;
  setup.options->panOutPansharpenedBands[0] = 2;
  setup.options->panOutPansharpenedBands[1] = 0;

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(kPixels * 2);
  EXPECT_EQ(CE_None,
            op.ProcessRegion(0, 0, kPixels, 1, out_buf.data(), GDT_Byte));

  // pseudo_pan = 20.0. Output band 0 maps to input 2 (val 30), band 1 to input
  // 0 (val 10).
  for (int j = 0; j < kPixels; ++j) {
    const double factor = pan_data[j] / 20.0;
    const uint8_t exp_band2 = static_cast<uint8_t>(30.0 * factor + 0.5);
    const uint8_t exp_band0 = static_cast<uint8_t>(10.0 * factor + 0.5);
    EXPECT_EQ(exp_band2, out_buf[j]);
    EXPECT_EQ(exp_band0, out_buf[kPixels + j]);
  }
}

TEST_F(GdalPansharpenTest, WeightedBroveyZeroPanAndZeroPseudoPanchro) {
  auto setup = CreateStandardSetup(2, 1, 2, 1, 3, GDT_Byte, GDT_Byte);

  // Pixel 0: pan = 0, spectral = 20, 20, 20.
  // Pixel 1: pan = 40, spectral = 0, 0, 0 (pseudo_pan = 0).
  FillBand(setup.pan_ds->GetRasterBand(1), std::vector<uint8_t>{0, 40});
  FillBand(setup.ms_ds->GetRasterBand(1), std::vector<uint8_t>{20, 0});
  FillBand(setup.ms_ds->GetRasterBand(2), std::vector<uint8_t>{20, 0});
  FillBand(setup.ms_ds->GetRasterBand(3), std::vector<uint8_t>{20, 0});

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(2 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 2, 1, out_buf.data(), GDT_Byte));

  // Both cases must yield 0.
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(0, out_buf[i * 2 + 0]);
    EXPECT_EQ(0, out_buf[i * 2 + 1]);
  }
}

// ---------------------------------------------------------------------------
// 8. Negative Weights & Bit Depth Clamping
// ---------------------------------------------------------------------------

TEST_F(GdalPansharpenTest, NegativeWeightsNoBitDepth) {
  // Negative weight with nBitDepth = 0 calls WeightedBrovey3<T, T, FALSE>.
  auto setup = CreateStandardSetup(2, 1, 2, 1, 3, GDT_Byte, GDT_Byte);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint8_t>(40));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(50));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));

  setup.options->padfWeights[0] = 0.8;
  setup.options->padfWeights[1] = -0.2;  // Negative weight.
  setup.options->padfWeights[2] = 0.4;
  setup.options->nBitDepth = 0;

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(2 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 2, 1, out_buf.data(), GDT_Byte));

  // pseudo_pan = 0.8*50 - 0.2*20 + 0.4*30 = 40 - 4 + 12 = 48.0.
  // factor = 40 / 48.0 = 5.0 / 6.0.
  const uint8_t exp1 = static_cast<uint8_t>(50.0 * (5.0 / 6.0) + 0.5);
  const uint8_t exp2 = static_cast<uint8_t>(20.0 * (5.0 / 6.0) + 0.5);
  const uint8_t exp3 = static_cast<uint8_t>(30.0 * (5.0 / 6.0) + 0.5);
  EXPECT_EQ(exp1, out_buf[0]);
  EXPECT_EQ(exp2, out_buf[2]);
  EXPECT_EQ(exp3, out_buf[4]);
}

TEST_F(GdalPansharpenTest, NegativeWeightsWithBitDepthClamping) {
  // Negative weight with nBitDepth = 7 (max 127) calls WeightedBrovey3<T, T,
  // TRUE>.
  auto setup = CreateStandardSetup(2, 1, 2, 1, 3, GDT_Byte, GDT_Byte);
  // High pan value will produce values exceeding 127.
  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint8_t>(200));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(80));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));

  setup.options->padfWeights[0] = 0.8;
  setup.options->padfWeights[1] = -0.2;
  setup.options->padfWeights[2] = 0.4;
  setup.options->nBitDepth = 7;

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(2 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 2, 1, out_buf.data(), GDT_Byte));

  // pseudo_pan = 0.8*80 - 0.2*20 + 0.4*30 = 64 - 4 + 12 = 72.0.
  // factor = 200 / 72.0 = 2.7777.
  // Band 1: 80 * 2.7777 = 222 > 127 -> clamped to 127!
  EXPECT_EQ(127, out_buf[0]);
  EXPECT_EQ(127, out_buf[1]);
}

TEST_F(GdalPansharpenTest, OvershootClampingByte) {
  // Byte with nBitDepth = 6 (max 63) exercises ClampValues for Byte.
  auto setup =
      CreateStandardSetup(8, 8, 4, 4, 3, GDT_Byte, GDT_Byte, GRIORA_Cubic);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint8_t>(60));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(50));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(50));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(50));

  setup.options->nBitDepth = 6;
  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(8 * 8 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 8, 8, out_buf.data(), GDT_Byte));

  // Every pixel must be clamped <= 63.
  for (const auto val : out_buf) {
    EXPECT_LE(val, 63);
  }
}

TEST_F(GdalPansharpenTest, OvershootClampingUInt16) {
  // UInt16 with nBitDepth = 11 (max 2047) exercises ClampValues for UInt16.
  auto setup = CreateStandardSetup(8, 8, 4, 4, 3, GDT_UInt16, GDT_UInt16,
                                   GRIORA_CubicSpline);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint16_t>(2000));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint16_t>(1800));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint16_t>(1800));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint16_t>(1800));

  setup.options->nBitDepth = 11;
  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint16_t> out_buf(8 * 8 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 8, 8, out_buf.data(), GDT_UInt16));

  for (const auto val : out_buf) {
    EXPECT_LE(val, 2047);
  }
}

TEST_F(GdalPansharpenTest, OvershootClampingWithMatchingNBITS) {
  // When NBITS metadata matches nBitDepth, ClampValues is skipped.
  auto setup =
      CreateStandardSetup(8, 8, 4, 4, 3, GDT_Byte, GDT_Byte, GRIORA_Cubic);
  for (int b = 1; b <= 3; ++b) {
    setup.ms_ds->GetRasterBand(b)->SetMetadataItem("NBITS", "6",
                                                   "IMAGE_STRUCTURE");
  }
  setup.options->nBitDepth = 6;
  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(8 * 8 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 8, 8, out_buf.data(), GDT_Byte));
}

// ---------------------------------------------------------------------------
// 9. Data Types & Buffer Conversions
// ---------------------------------------------------------------------------

TEST_F(GdalPansharpenTest, DataTypesUInt16PanUInt16Buf) {
  auto setup = CreateStandardSetup(4, 4, 2, 2, 3, GDT_UInt16, GDT_UInt16);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint16_t>(400));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint16_t>(100));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint16_t>(200));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint16_t>(300));

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint16_t> out_buf(4 * 4 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 4, 4, out_buf.data(), GDT_UInt16));

  // pseudo_pan = 200. factor = 2.0.
  for (int j = 0; j < 16; ++j) {
    EXPECT_EQ(200, out_buf[j]);
    EXPECT_EQ(400, out_buf[16 + j]);
    EXPECT_EQ(600, out_buf[32 + j]);
  }
}

TEST_F(GdalPansharpenTest, DataTypesFloat32PanFloat32Buf) {
  auto setup = CreateStandardSetup(4, 4, 2, 2, 3, GDT_Float32, GDT_Float32);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), 40.0f);
  FillBandConstant(setup.ms_ds->GetRasterBand(1), 10.0f);
  FillBandConstant(setup.ms_ds->GetRasterBand(2), 20.0f);
  FillBandConstant(setup.ms_ds->GetRasterBand(3), 30.0f);

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<float> out_buf(4 * 4 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 4, 4, out_buf.data(), GDT_Float32));

  for (int j = 0; j < 16; ++j) {
    EXPECT_FLOAT_EQ(20.0f, out_buf[j]);
    EXPECT_FLOAT_EQ(40.0f, out_buf[16 + j]);
    EXPECT_FLOAT_EQ(60.0f, out_buf[32 + j]);
  }
}

TEST_F(GdalPansharpenTest, DataTypesFloat64PanFloat64Buf) {
  auto setup = CreateStandardSetup(4, 4, 2, 2, 3, GDT_Float64, GDT_Float64);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), 40.0);
  FillBandConstant(setup.ms_ds->GetRasterBand(1), 10.0);
  FillBandConstant(setup.ms_ds->GetRasterBand(2), 20.0);
  FillBandConstant(setup.ms_ds->GetRasterBand(3), 30.0);

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<double> out_buf(4 * 4 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 4, 4, out_buf.data(), GDT_Float64));

  for (int j = 0; j < 16; ++j) {
    EXPECT_DOUBLE_EQ(20.0, out_buf[j]);
    EXPECT_DOUBLE_EQ(40.0, out_buf[16 + j]);
    EXPECT_DOUBLE_EQ(60.0, out_buf[32 + j]);
  }
}

TEST_F(GdalPansharpenTest, DataTypesBytePanFloat32Buf) {
  // Byte pan with Float32 output buffer exercises the temp buffer conversion.
  auto setup = CreateStandardSetup(4, 4, 2, 2, 3, GDT_Byte, GDT_Byte);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint8_t>(40));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<float> out_buf(4 * 4 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 4, 4, out_buf.data(), GDT_Float32));

  for (int j = 0; j < 16; ++j) {
    EXPECT_FLOAT_EQ(20.0f, out_buf[j]);
    EXPECT_FLOAT_EQ(40.0f, out_buf[16 + j]);
    EXPECT_FLOAT_EQ(60.0f, out_buf[32 + j]);
  }
}

TEST_F(GdalPansharpenTest, DataTypesBytePanInt16Buf) {
  // Byte pan with Int16 output buffer exercises the temp buffer conversion.
  auto setup = CreateStandardSetup(4, 4, 2, 2, 3, GDT_Byte, GDT_Byte);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint8_t>(40));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<int16_t> out_buf(4 * 4 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 4, 4, out_buf.data(), GDT_Int16));

  for (int j = 0; j < 16; ++j) {
    EXPECT_EQ(20, out_buf[j]);
    EXPECT_EQ(40, out_buf[16 + j]);
    EXPECT_EQ(60, out_buf[32 + j]);
  }
}

TEST_F(GdalPansharpenTest, DataTypesUInt16PanFloat64Buf) {
  // UInt16 pan with Float64 output buffer exercises the temp buffer conversion.
  auto setup = CreateStandardSetup(4, 4, 2, 2, 3, GDT_UInt16, GDT_UInt16);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint16_t>(400));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint16_t>(100));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint16_t>(200));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint16_t>(300));

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<double> out_buf(4 * 4 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 4, 4, out_buf.data(), GDT_Float64));

  for (int j = 0; j < 16; ++j) {
    EXPECT_DOUBLE_EQ(200.0, out_buf[j]);
    EXPECT_DOUBLE_EQ(400.0, out_buf[16 + j]);
    EXPECT_DOUBLE_EQ(600.0, out_buf[32 + j]);
  }
}

TEST_F(GdalPansharpenTest, DataTypesFloat64PanByteBuf) {
  // Float64 pan with Byte output buffer.
  auto setup = CreateStandardSetup(4, 4, 2, 2, 3, GDT_Float64, GDT_Float64);
  FillBandConstant(setup.pan_ds->GetRasterBand(1), 40.0);
  FillBandConstant(setup.ms_ds->GetRasterBand(1), 10.0);
  FillBandConstant(setup.ms_ds->GetRasterBand(2), 20.0);
  FillBandConstant(setup.ms_ds->GetRasterBand(3), 30.0);

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(4 * 4 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 4, 4, out_buf.data(), GDT_Byte));

  for (int j = 0; j < 16; ++j) {
    EXPECT_EQ(20, out_buf[j]);
    EXPECT_EQ(40, out_buf[16 + j]);
    EXPECT_EQ(60, out_buf[32 + j]);
  }
}

// ---------------------------------------------------------------------------
// 10. WeightedBroveyWithNoData Handling
// ---------------------------------------------------------------------------

TEST_F(GdalPansharpenTest, NoDataIntegerMinNoData) {
  // UInt16 with dfNoData = 0.0 (minimum value of UInt16).
  // validValue is min + 1 = 1.
  auto setup = CreateStandardSetup(4, 1, 4, 1, 3, GDT_UInt16, GDT_UInt16);

  // Pixel 0: pan is nodata (0) -> output is nodata (0).
  // Pixel 1: spectral band 1 is nodata (0) -> output is nodata (0).
  // Pixel 2: normal pixel (pan = 40, spectral = 20, 20, 20) -> output 40,
  // 40, 40. Pixel 3: value rounds to 0, which equals nodata -> mapped to
  // validValue (1)!
  //          spectral = 1, pseudo_pan = 1000, pan = 1 -> 1 * (1/1000) = 0.001
  //          -> 0 -> mapped to 1.
  FillBand(setup.pan_ds->GetRasterBand(1), std::vector<uint16_t>{0, 40, 40, 1});
  FillBand(setup.ms_ds->GetRasterBand(1), std::vector<uint16_t>{20, 0, 20, 1});
  FillBand(setup.ms_ds->GetRasterBand(2),
           std::vector<uint16_t>{20, 20, 20, 1000});
  FillBand(setup.ms_ds->GetRasterBand(3),
           std::vector<uint16_t>{20, 20, 20, 1000});

  setup.options->bHasNoData = TRUE;
  setup.options->dfNoData = 0.0;
  setup.options->nBitDepth = 10;  // max 1023

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint16_t> out_buf(4 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 4, 1, out_buf.data(), GDT_UInt16));

  // Pixel 0: nodata
  EXPECT_EQ(0, out_buf[0]);
  EXPECT_EQ(0, out_buf[4]);
  EXPECT_EQ(0, out_buf[8]);

  // Pixel 1: nodata
  EXPECT_EQ(0, out_buf[1]);
  EXPECT_EQ(0, out_buf[5]);
  EXPECT_EQ(0, out_buf[9]);

  // Pixel 2: valid sharpened
  EXPECT_EQ(40, out_buf[2]);
  EXPECT_EQ(40, out_buf[6]);
  EXPECT_EQ(40, out_buf[10]);

  // Pixel 3: band 0 would be 0 (noData), so it is replaced with validValue (1)!
  EXPECT_EQ(1, out_buf[3]);
}

TEST_F(GdalPansharpenTest, NoDataIntegerNonMinNoData) {
  // Byte with dfNoData = 255.0 (non-minimum value).
  // validValue is noData - 1 = 254.
  auto setup = CreateStandardSetup(3, 1, 3, 1, 3, GDT_Byte, GDT_Byte);

  // Pixel 0: pan is nodata (255) -> output 255.
  // Pixel 1: spectral band is nodata (255) -> output 255.
  // Pixel 2: computed value equals 255 -> mapped to validValue (254)!
  FillBand(setup.pan_ds->GetRasterBand(1), std::vector<uint8_t>{255, 40, 150});
  FillBand(setup.ms_ds->GetRasterBand(1), std::vector<uint8_t>{20, 255, 170});
  FillBand(setup.ms_ds->GetRasterBand(2), std::vector<uint8_t>{20, 20, 65});
  FillBand(setup.ms_ds->GetRasterBand(3), std::vector<uint8_t>{20, 20, 65});

  setup.options->bHasNoData = TRUE;
  setup.options->dfNoData = 255.0;

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<uint8_t> out_buf(3 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 3, 1, out_buf.data(), GDT_Byte));

  // Pixel 0: nodata (255)
  EXPECT_EQ(255, out_buf[0]);
  // Pixel 1: nodata (255)
  EXPECT_EQ(255, out_buf[1]);
  // Pixel 2: pseudo-pan = 100, factor = 1.5, 170 * 1.5 = 255 -> mapped to 254!
  EXPECT_EQ(254, out_buf[2]);
}

TEST_F(GdalPansharpenTest, NoDataFloatingPoint) {
  // Float64 with dfNoData = -9999.0 exercises floating point validValue logic.
  auto setup = CreateStandardSetup(3, 1, 3, 1, 3, GDT_Float64, GDT_Float64);

  // Pixel 0: pan is nodata (-9999.0).
  // Pixel 1: spectral is nodata (-9999.0).
  // Pixel 2: valid pixel.
  FillBand(setup.pan_ds->GetRasterBand(1),
           std::vector<double>{-9999.0, 40.0, 40.0});
  FillBand(setup.ms_ds->GetRasterBand(1),
           std::vector<double>{10.0, -9999.0, 10.0});
  FillBand(setup.ms_ds->GetRasterBand(2),
           std::vector<double>{20.0, 20.0, 20.0});
  FillBand(setup.ms_ds->GetRasterBand(3),
           std::vector<double>{30.0, 30.0, 30.0});

  setup.options->bHasNoData = TRUE;
  setup.options->dfNoData = -9999.0;

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(setup.options.get()));

  std::vector<double> out_buf(3 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 3, 1, out_buf.data(), GDT_Float64));

  EXPECT_DOUBLE_EQ(-9999.0, out_buf[0]);
  EXPECT_DOUBLE_EQ(-9999.0, out_buf[1]);
  EXPECT_DOUBLE_EQ(20.0, out_buf[2]);
}

// ---------------------------------------------------------------------------
// 11. C API Verification
// ---------------------------------------------------------------------------

TEST_F(GdalPansharpenTest, CApiLifecycleAndProcessing) {
  // 1. Create with invalid options (hPanchroBand == nullptr) fails.
  {
    auto setup = CreateStandardSetup();
    setup.options->hPanchroBand = nullptr;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreatePansharpenOperation(setup.options.get()));
  }

  // 2. Create with valid options.
  auto setup = CreateStandardSetup();
  FillBandConstant(setup.pan_ds->GetRasterBand(1), static_cast<uint8_t>(40));
  FillBandConstant(setup.ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(setup.ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(setup.ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));

  GDALPansharpenOperationH op_h =
      GDALCreatePansharpenOperation(setup.options.get());
  ASSERT_NE(nullptr, op_h);

  // 3. Process region through C API.
  std::vector<uint8_t> out_buf(8 * 8 * 3);
  EXPECT_EQ(CE_None, GDALPansharpenProcessRegion(op_h, 0, 0, 8, 8,
                                                 out_buf.data(), GDT_Byte));
  EXPECT_EQ(20, out_buf[0]);
  EXPECT_EQ(40, out_buf[64]);
  EXPECT_EQ(60, out_buf[128]);

  // 4. Destroy operation.
  GDALDestroyPansharpenOperation(op_h);

  // 5. Destroy nullptr is a safe no-op.
  GDALDestroyPansharpenOperation(nullptr);
}

// ---------------------------------------------------------------------------
// 12. Virtual Filesystem Processing & Cleanup
// ---------------------------------------------------------------------------

TEST_F(GdalPansharpenTest, VsimemDatasetProcessingAndCleanup) {
  constexpr char kTestDir[] = "/vsimem/gdalpansharpen_test/vsimem_cleanup";
  absl::Cleanup cleanup = [kTestDir] { VSIRmdirRecursive(kTestDir); };

  GDALRegister_GTiff();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);

  const std::string pan_path = std::string(kTestDir) + "/pan.tif";
  const std::string ms_path = std::string(kTestDir) + "/ms.tif";

  const double pan_gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  const double ms_gt[6] = {0.0, 2.0, 0.0, 0.0, 0.0, -2.0};

  GDALDatasetUniquePtr pan_ds(
      drv->Create(pan_path.c_str(), 8, 8, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, pan_ds);
  pan_ds->SetGeoTransform(GDALGeoTransform(pan_gt));
  FillBandConstant(pan_ds->GetRasterBand(1), static_cast<uint8_t>(40));

  GDALDatasetUniquePtr ms_ds(
      drv->Create(ms_path.c_str(), 4, 4, 3, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ms_ds);
  ms_ds->SetGeoTransform(GDALGeoTransform(ms_gt));
  FillBandConstant(ms_ds->GetRasterBand(1), static_cast<uint8_t>(10));
  FillBandConstant(ms_ds->GetRasterBand(2), static_cast<uint8_t>(20));
  FillBandConstant(ms_ds->GetRasterBand(3), static_cast<uint8_t>(30));

  auto* opts = GDALCreatePansharpenOptions();
  opts->ePansharpenAlg = GDAL_PSH_WEIGHTED_BROVEY;
  opts->eResampleAlg = GRIORA_NearestNeighbour;
  opts->hPanchroBand = pan_ds->GetRasterBand(1);
  opts->nInputSpectralBands = 3;
  opts->pahInputSpectralBands =
      static_cast<GDALRasterBandH*>(CPLMalloc(sizeof(GDALRasterBandH) * 3));
  for (int i = 0; i < 3; ++i) {
    opts->pahInputSpectralBands[i] = ms_ds->GetRasterBand(i + 1);
  }
  opts->nWeightCount = 3;
  opts->padfWeights = static_cast<double*>(CPLMalloc(sizeof(double) * 3));
  opts->padfWeights[0] = 1.0 / 3.0;
  opts->padfWeights[1] = 1.0 / 3.0;
  opts->padfWeights[2] = 1.0 / 3.0;
  opts->nOutPansharpenedBands = 3;
  opts->panOutPansharpenedBands = static_cast<int*>(CPLMalloc(sizeof(int) * 3));
  opts->panOutPansharpenedBands[0] = 0;
  opts->panOutPansharpenedBands[1] = 1;
  opts->panOutPansharpenedBands[2] = 2;

  std::unique_ptr<GDALPansharpenOptions,
                  decltype(&GDALDestroyPansharpenOptions)>
      options_holder(opts, GDALDestroyPansharpenOptions);

  GDALPansharpenOperation op;
  ASSERT_EQ(CE_None, op.Initialize(options_holder.get()));

  std::vector<uint8_t> out_buf(8 * 8 * 3);
  EXPECT_EQ(CE_None, op.ProcessRegion(0, 0, 8, 8, out_buf.data(), GDT_Byte));

  EXPECT_EQ(20, out_buf[0]);
  EXPECT_EQ(40, out_buf[64]);
  EXPECT_EQ(60, out_buf[128]);

  // Close datasets before filesystem cleanup.
  pan_ds.reset();
  ms_ds.reset();
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered branches are OOM guards and
// specialized data-type paths.
