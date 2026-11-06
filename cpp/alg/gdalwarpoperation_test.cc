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
// Tests GDALWarpOperation (alg/gdalwarpoperation.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalwarpoperation.cpp
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalwarper.h

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/alg/gdalwarper.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

// Custom deleter for GDALWarpOptions.
struct WarpOptionsDeleter {
  void operator()(GDALWarpOptions* psOptions) const {
    GDALDestroyWarpOptions(psOptions);
  }
};
using WarpOptionsUniquePtr =
    std::unique_ptr<GDALWarpOptions, WarpOptionsDeleter>;

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
int AllFailingTransformer(void* /*pTransformArg*/, int /*bDstToSrc*/,
                          int nPointCount, double* /*x*/, double* /*y*/,
                          double* /*z*/, int* panSuccess) {
  if (panSuccess != nullptr) {
    for (int i = 0; i < nPointCount; ++i) {
      panSuccess[i] = FALSE;
    }
  }
  return FALSE;
}

// Transformer where destination corner points fail (used in
// ComputeSourceWindow).
int CornerFailingTransformer(void* /*pTransformArg*/, int bDstToSrc,
                             int nPointCount, double* /*x*/, double* /*y*/,
                             double* /*z*/, int* panSuccess) {
  if (panSuccess != nullptr) {
    for (int i = 0; i < nPointCount; ++i) {
      panSuccess[i] = TRUE;
    }
    if (bDstToSrc && nPointCount == 4) {
      panSuccess[1] = FALSE;
    }
  }
  return TRUE;
}

// Transformer injecting NaN coordinates.
int NanCoordTransformer(void* /*pTransformArg*/, int /*bDstToSrc*/,
                        int nPointCount, double* x, double* /*y*/,
                        double* /*z*/, int* panSuccess) {
  if (panSuccess != nullptr) {
    for (int i = 0; i < nPointCount; ++i) {
      panSuccess[i] = TRUE;
    }
  }
  if (nPointCount > 0) {
    x[0] = std::numeric_limits<double>::quiet_NaN();
  }
  return TRUE;
}

// Transformer returning coordinates outside normal projection space (> 1e7)
// to trigger CHECK_WITH_INVERT_PROJ re-transform logic.
int BogusCoordTransformer(void* /*pTransformArg*/, int /*bDstToSrc*/,
                          int nPointCount, double* x, double* y, double* /*z*/,
                          int* panSuccess) {
  if (panSuccess != nullptr) {
    for (int i = 0; i < nPointCount; ++i) {
      panSuccess[i] = TRUE;
    }
  }
  for (int i = 0; i < nPointCount; ++i) {
    x[i] = 1e8;
    y[i] = 1e8;
  }
  return TRUE;
}

// Transformer placing source window entirely outside raster bounds.
int OutOfBoundsTransformer(void* /*pTransformArg*/, int /*bDstToSrc*/,
                           int nPointCount, double* x, double* y, double* /*z*/,
                           int* panSuccess) {
  if (panSuccess != nullptr) {
    for (int i = 0; i < nPointCount; ++i) {
      panSuccess[i] = TRUE;
    }
  }
  for (int i = 0; i < nPointCount; ++i) {
    x[i] = 5000.0;
    y[i] = 5000.0;
  }
  return TRUE;
}

// Transformer returning coordinates that cover >90% of image dimensions.
int LargeCoverageTransformer(void* /*pTransformArg*/, int /*bDstToSrc*/,
                             int nPointCount, double* x, double* y,
                             double* /*z*/, int* panSuccess) {
  if (panSuccess != nullptr) {
    for (int i = 0; i < nPointCount; ++i) {
      panSuccess[i] = TRUE;
    }
  }
  for (int i = 0; i < nPointCount; ++i) {
    x[i] = (i % 2 == 0) ? 0.0 : 19.5;
    y[i] = (i % 2 == 0) ? 0.0 : 19.5;
  }
  return TRUE;
}

// Dummy mask function for density mask testing.
int DummyMaskFunc(void* /*pMaskFuncArg*/, int /*nBandCount*/,
                  GDALDataType /*eType*/, int /*nXOff*/, int /*nYOff*/,
                  int /*nXSize*/, int /*nYSize*/, GByte** /*papabyImageData*/,
                  int /*bMaskIsFloat*/, void* /*pMask*/) {
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

// Pre- and post-warp chunk processors.
CPLErr PreWarpSuccess(void* /*pKern*/, void* pArg) {
  int* call_count = static_cast<int*>(pArg);
  if (call_count != nullptr) ++(*call_count);
  return CE_None;
}

CPLErr PreWarpFailure(void* /*pKern*/, void* /*pArg*/) {
  CPLError(CE_Failure, CPLE_AppDefined, "Injected pre-warp failure");
  return CE_Failure;
}

CPLErr PostWarpSuccess(void* /*pKern*/, void* pArg) {
  int* call_count = static_cast<int*>(pArg);
  if (call_count != nullptr) ++(*call_count);
  return CE_None;
}

CPLErr PostWarpFailure(void* /*pKern*/, void* /*pArg*/) {
  CPLError(CE_Failure, CPLE_AppDefined, "Injected post-warp failure");
  return CE_Failure;
}

// Helper creating an in-memory dataset via the MEM driver.
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

// Helper to read 8-bit pixels from a band into a std::vector.
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

// Helper to write 8-bit pixels to a band.
void WriteBandValues(GDALDataset* ds, int band_num, int width, int height,
                     const std::vector<uint8_t>& buf) {
  GDALRasterBand* band = ds->GetRasterBand(band_num);
  if (band != nullptr) {
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, width, height,
                                      const_cast<uint8_t*>(buf.data()), width,
                                      height, GDT_Byte, 0, 0, nullptr));
  }
}

// Helper to write 32-bit float pixels to a band.
void WriteBandValuesFloat(GDALDataset* ds, int band_num, int width, int height,
                          const std::vector<float>& buf) {
  GDALRasterBand* band = ds->GetRasterBand(band_num);
  if (band != nullptr) {
    EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, width, height,
                                      const_cast<float*>(buf.data()), width,
                                      height, GDT_Float32, 0, 0, nullptr));
  }
}

// Helper creating default warp options with proper memory allocation.
WarpOptionsUniquePtr CreateDefaultWarpOptions(GDALDataset* src_ds,
                                              GDALDataset* dst_ds,
                                              int band_count = 1) {
  GDALWarpOptions* psOptions = GDALCreateWarpOptions();
  if (psOptions == nullptr) return nullptr;
  psOptions->hSrcDS = GDALDataset::ToHandle(src_ds);
  psOptions->hDstDS = GDALDataset::ToHandle(dst_ds);
  psOptions->nBandCount = band_count;
  if (band_count > 0) {
    psOptions->panSrcBands =
        static_cast<int*>(CPLMalloc(sizeof(int) * band_count));
    psOptions->panDstBands =
        static_cast<int*>(CPLMalloc(sizeof(int) * band_count));
    for (int i = 0; i < band_count; ++i) {
      psOptions->panSrcBands[i] = i + 1;
      psOptions->panDstBands[i] = i + 1;
    }
  }
  psOptions->pfnTransformer = IdentityTransformer;
  psOptions->pTransformerArg = nullptr;
  psOptions->pfnProgress = GDALDummyProgress;
  return WarpOptionsUniquePtr(psOptions);
}

class GdalWarpOperationTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }
};

// -----------------------------------------------------------------------------
// Options Validation & Initialization Tests
// -----------------------------------------------------------------------------

TEST_F(GdalWarpOperationTest, ValidateOptionsDefaultUninitialized) {
  GDALWarpOperation op;
  EXPECT_EQ(nullptr, op.GetOptions());
}

TEST_F(GdalWarpOperationTest, ValidateOptionsMemoryLimitTooSmall) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, dst_ds);

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
  options->dfWarpMemoryLimit = 50000.0;  // Must be >= 100000.0

  GDALWarpOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("is unreasonably small"));
  CPLErrorReset();
  EXPECT_EQ(nullptr, op.GetOptions());
}

TEST_F(GdalWarpOperationTest, ValidateOptionsResampleAlgorithm) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, dst_ds);

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
  options->eResampleAlg = static_cast<GDALResampleAlg>(15);

  GDALWarpOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("is not a supported value"));
  CPLErrorReset();
}

TEST_F(GdalWarpOperationTest, ValidateOptionsWorkingDataType) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, dst_ds);

  // 1. Unknown data type (GDT_TypeCount).
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->eWorkingDataType = GDT_TypeCount;

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("is not a supported value"));
    CPLErrorReset();
  }

  // 2. Complex data type paired with min/max/qnt algorithms.
  constexpr GDALResampleAlg kIncompatibleComplexAlgs[] = {
      GRA_Max, GRA_Min, GRA_Med, GRA_Q1, GRA_Q3,
  };
  for (GDALResampleAlg alg : kIncompatibleComplexAlgs) {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->eWorkingDataType = GDT_CFloat32;
    options->eResampleAlg = alg;

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("not supported for complex valued data"));
    CPLErrorReset();
  }
}

TEST_F(GdalWarpOperationTest, ValidateOptionsSourceDatasetNull) {
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, dst_ds);

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(nullptr, dst_ds.get(), 1);

  GDALWarpOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("hSrcDS is not set"));
  CPLErrorReset();
}

TEST_F(GdalWarpOperationTest, ValidateOptionsBandCountZeroMismatchedDatasets) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 2);
  ASSERT_NE(nullptr, dst_ds);

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 0);

  GDALWarpOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("nBandCount=0, no bands configured!"));
  CPLErrorReset();
}

TEST_F(GdalWarpOperationTest, ValidateOptionsPanBandsNull) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, dst_ds);

  // 1. panSrcBands is NULL.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->eWorkingDataType = GDT_Byte;
    CPLFree(options->panSrcBands);
    options->panSrcBands = nullptr;

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("panSrcBands is NULL"));
    CPLErrorReset();
  }

  // 2. panDstBands is NULL while hDstDS != nullptr.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->eWorkingDataType = GDT_Byte;
    CPLFree(options->panDstBands);
    options->panDstBands = nullptr;

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("panDstBands is NULL"));
    CPLErrorReset();
  }
}

TEST_F(GdalWarpOperationTest, ValidateOptionsBandIndicesOutOfRange) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, dst_ds);

  // 1. Source band < 1.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->panSrcBands[0] = 0;

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("out of range for dataset"));
    CPLErrorReset();
  }

  // 2. Source band > band count.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->panSrcBands[0] = 99;

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("out of range for dataset"));
    CPLErrorReset();
  }

  // 3. Destination band < 1.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->panDstBands[0] = 0;

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("out of range for dataset"));
    CPLErrorReset();
  }

  // 4. Destination band > band count.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->panDstBands[0] = 99;

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("out of range for dataset"));
    CPLErrorReset();
  }
}

TEST_F(GdalWarpOperationTest, ValidateOptionsReadOnlyDestination) {
  GDALRegister_GTiff();
  constexpr char kRoPath[] =
      "/vsimem/gdalwarpoperation_test/ValidateOptionsReadOnlyDestination/"
      "test.tif";
  absl::Cleanup cleanup_dir = [] {
    VSIRmdirRecursive(
        "/vsimem/gdalwarpoperation_test/ValidateOptionsReadOnlyDestination");
  };

  GDALDriver* tif_driver = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, tif_driver);
  GDALDatasetUniquePtr created_ds(
      tif_driver->Create(kRoPath, 10, 10, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, created_ds);
  created_ds.reset();  // Flush and close.

  GDALDatasetUniquePtr ro_ds(GDALDataset::FromHandle(GDALOpenEx(
      kRoPath, GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr, nullptr, nullptr)));
  ASSERT_NE(nullptr, ro_ds);
  ASSERT_EQ(GA_ReadOnly, ro_ds->GetRasterBand(1)->GetAccess());

  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), ro_ds.get(), 1);

  GDALWarpOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Destination band 1 appears to be read-only"));
  CPLErrorReset();
}

TEST_F(GdalWarpOperationTest, ValidateOptionsProgressAndTransformerNull) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, dst_ds);

  // 1. pfnProgress is NULL.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->pfnProgress = nullptr;

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("pfnProgress is NULL"));
    CPLErrorReset();
  }

  // 2. pfnTransformer is NULL and not provided in Initialize().
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->pfnTransformer = nullptr;

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("pfnTransformer is NULL"));
    CPLErrorReset();
  }
}

TEST_F(GdalWarpOperationTest, ValidateOptionsSampleStepsIllegal) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, dst_ds);

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
  options->papszWarpOptions =
      CSLSetNameValue(options->papszWarpOptions, "SAMPLE_STEPS", "1");

  GDALWarpOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("SAMPLE_STEPS warp option has illegal value"));
  CPLErrorReset();
}

TEST_F(GdalWarpOperationTest, ValidateOptionsAlphaBandsOutOfRange) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, dst_ds);

  // 1. Source alpha band out of range.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->nSrcAlphaBand = 5;

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("nSrcAlphaBand = 5 ... out of range for dataset"));
    CPLErrorReset();
  }

  // 2. Destination alpha band out of range.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->nDstAlphaBand = 5;

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("nDstAlphaBand = 5 ... out of range for dataset"));
    CPLErrorReset();
  }
}

TEST_F(GdalWarpOperationTest, ValidateOptionsAlphaBandDensityMaskConflict) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 2);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 2);
  ASSERT_NE(nullptr, dst_ds);

  // 1. Source alpha and source density mask func provided.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->nSrcAlphaBand = 2;
    options->pfnSrcDensityMaskFunc = DummyMaskFunc;

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("pfnSrcDensityMaskFunc provided as well as a SrcAlphaBand"));
    CPLErrorReset();
  }

  // 2. Destination alpha and destination density mask func provided.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->nDstAlphaBand = 2;
    options->pfnDstDensityMaskFunc = DummyMaskFunc;

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("pfnDstDensityMaskFunc provided as well as a DstAlphaBand"));
    CPLErrorReset();
  }
}

TEST_F(GdalWarpOperationTest,
       ValidateOptionsPerDatasetMaskWithSourceNodataWarning) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, dst_ds);

  ASSERT_EQ(CE_None, src_ds->CreateMaskBand(GMF_PER_DATASET));

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
  GDALWarpInitSrcNoDataReal(options.get(), 0.0);

  GDALWarpOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_None, op.Initialize(options.get()));
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Source dataset has both a per-dataset mask band"));
  CPLErrorReset();
}

TEST_F(GdalWarpOperationTest, ValidateOptionsEmptySourceWindowWithoutInitDest) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, dst_ds);

  // 1. ERROR_OUT_IF_EMPTY_SOURCE_WINDOW=FALSE without INIT_DEST fails.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions = CSLSetNameValue(
        options->papszWarpOptions, "ERROR_OUT_IF_EMPTY_SOURCE_WINDOW", "FALSE");

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("ERROR_OUT_IF_EMPTY_SOURCE_WINDOW=FALSE can only be "
                          "used if INIT_DEST is set"));
    CPLErrorReset();
  }

  // 2. With INIT_DEST set, initialization succeeds.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions = CSLSetNameValue(
        options->papszWarpOptions, "ERROR_OUT_IF_EMPTY_SOURCE_WINDOW", "FALSE");
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "INIT_DEST", "0");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
  }
}

// -----------------------------------------------------------------------------
// Tie Strategy, Alpha Max & Cutline Initialization Tests
// -----------------------------------------------------------------------------

TEST_F(GdalWarpOperationTest, TieStrategyConfiguration) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, dst_ds);

  constexpr struct {
    std::string_view name;
    GWKTieStrategy expected;
  } kStrategies[] = {
      {.name = "FIRST", .expected = GWKTS_First},
      {.name = "MIN", .expected = GWKTS_Min},
      {.name = "MAX", .expected = GWKTS_Max},
  };

  for (const auto& item : kStrategies) {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions = CSLSetNameValue(
        options->papszWarpOptions, "MODE_TIES", std::string(item.name).c_str());

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    ASSERT_NE(nullptr, op.GetOptions());
    EXPECT_EQ(item.expected, op.GetOptions()->eTieStrategy);
  }

  // Unknown value triggers failure.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions = CSLSetNameValue(
        options->papszWarpOptions, "MODE_TIES", "UNKNOWN_STRATEGY");

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    // GDALValidateOptions emits a CE_Warning on unrecognized / invalid options
    // following the CE_Failure from SetTieStrategy.
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("MODE_TIES"));
    CPLErrorReset();
  }
}

TEST_F(GdalWarpOperationTest, AlphaMaxAutomaticConfiguration) {
  // 1. NBITS metadata sets alpha max.
  {
    GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 2, GDT_UInt16);
    ASSERT_NE(nullptr, src_ds);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 2, GDT_UInt16);
    ASSERT_NE(nullptr, dst_ds);

    src_ds->GetRasterBand(2)->SetMetadataItem("NBITS", "12", "IMAGE_STRUCTURE");
    dst_ds->GetRasterBand(2)->SetMetadataItem("NBITS", "10", "IMAGE_STRUCTURE");

    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->nSrcAlphaBand = 2;
    options->nDstAlphaBand = 2;
    options->eWorkingDataType = GDT_UInt16;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    ASSERT_NE(nullptr, op.GetOptions());
    EXPECT_STREQ("4095", CSLFetchNameValue(op.GetOptions()->papszWarpOptions,
                                           "SRC_ALPHA_MAX"));
    EXPECT_STREQ("1023", CSLFetchNameValue(op.GetOptions()->papszWarpOptions,
                                           "DST_ALPHA_MAX"));
  }

  // 2. GDT_Int16 sets alpha max to 32767.
  {
    GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 2, GDT_Int16);
    ASSERT_NE(nullptr, src_ds);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 2, GDT_Int16);
    ASSERT_NE(nullptr, dst_ds);

    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->nSrcAlphaBand = 2;
    options->nDstAlphaBand = 2;
    options->eWorkingDataType = GDT_Int16;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    ASSERT_NE(nullptr, op.GetOptions());
    EXPECT_STREQ("32767", CSLFetchNameValue(op.GetOptions()->papszWarpOptions,
                                            "SRC_ALPHA_MAX"));
    EXPECT_STREQ("32767", CSLFetchNameValue(op.GetOptions()->papszWarpOptions,
                                            "DST_ALPHA_MAX"));
  }

  // 3. GDT_UInt16 sets alpha max to 65535.
  {
    GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 2, GDT_UInt16);
    ASSERT_NE(nullptr, src_ds);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 2, GDT_UInt16);
    ASSERT_NE(nullptr, dst_ds);

    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->nSrcAlphaBand = 2;
    options->nDstAlphaBand = 2;
    options->eWorkingDataType = GDT_UInt16;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    ASSERT_NE(nullptr, op.GetOptions());
    EXPECT_STREQ("65535", CSLFetchNameValue(op.GetOptions()->papszWarpOptions,
                                            "SRC_ALPHA_MAX"));
    EXPECT_STREQ("65535", CSLFetchNameValue(op.GetOptions()->papszWarpOptions,
                                            "DST_ALPHA_MAX"));
  }
}

TEST_F(GdalWarpOperationTest, CutlineOptionParsing) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, dst_ds);

  // 1. Valid cutline WKT and CUTLINE_BLEND_DIST.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "CUTLINE",
                        "POLYGON((0 0, 0 10, 10 10, 10 0, 0 0))");
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "CUTLINE_BLEND_DIST", "2.5");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    ASSERT_NE(nullptr, op.GetOptions());
    EXPECT_NE(nullptr, op.GetOptions()->hCutline);
    EXPECT_DOUBLE_EQ(2.5, op.GetOptions()->dfCutlineBlendDist);
  }

  // 2. Corrupt cutline WKT fails.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions = CSLSetNameValue(options->papszWarpOptions,
                                                "CUTLINE", "CORRUPT_WKT(0 0)");

    GDALWarpOperation op;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(options.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Failed to parse CUTLINE geometry wkt"));
    CPLErrorReset();
  }
}

TEST_F(GdalWarpOperationTest, InitializeWithOwnedTransformer) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, dst_ds);

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
  options->pfnTransformer = nullptr;
  options->pTransformerArg = nullptr;

  GDALWarpOperation op;
  EXPECT_EQ(CE_None, op.Initialize(options.get(), IdentityTransformer,
                                   GDALTransformerArgUniquePtr{nullptr}));
  ASSERT_NE(nullptr, op.GetOptions());
  EXPECT_EQ(IdentityTransformer, op.GetOptions()->pfnTransformer);
}

TEST_F(GdalWarpOperationTest, DefaultBandMappingWhenBandCountZero) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 2);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 2);
  ASSERT_NE(nullptr, dst_ds);

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 0);

  GDALWarpOperation op;
  EXPECT_EQ(CE_None, op.Initialize(options.get()));
  ASSERT_NE(nullptr, op.GetOptions());
  EXPECT_EQ(2, op.GetOptions()->nBandCount);
  ASSERT_NE(nullptr, op.GetOptions()->panSrcBands);
  ASSERT_NE(nullptr, op.GetOptions()->panDstBands);
  EXPECT_EQ(1, op.GetOptions()->panSrcBands[0]);
  EXPECT_EQ(2, op.GetOptions()->panSrcBands[1]);
  EXPECT_EQ(1, op.GetOptions()->panDstBands[0]);
  EXPECT_EQ(2, op.GetOptions()->panDstBands[1]);
}

TEST_F(GdalWarpOperationTest, ReinitializationAndWipeOptions) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, dst_ds);

  GDALWarpOperation op;

  // First initialization fails validation and wipes options cleanly.
  WarpOptionsUniquePtr opt1 =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
  opt1->dfWarpMemoryLimit = 50000.0;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.Initialize(opt1.get()));
    CPLErrorReset();
  }
  EXPECT_EQ(nullptr, op.GetOptions());

  // Second initialization succeeds.
  WarpOptionsUniquePtr opt2 =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
  opt2->eResampleAlg = GRA_Bilinear;
  EXPECT_EQ(CE_None, op.Initialize(opt2.get()));
  ASSERT_NE(nullptr, op.GetOptions());
  EXPECT_EQ(GRA_Bilinear, op.GetOptions()->eResampleAlg);
}

// -----------------------------------------------------------------------------
// Destination Buffer Lifecycle Tests
// -----------------------------------------------------------------------------

TEST_F(GdalWarpOperationTest, DestinationBufferInitialization) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(10, 10, 1);
  ASSERT_NE(nullptr, dst_ds);

  // 1. INIT_DEST unset -> returns CE_None, *pbWasInitialized = FALSE.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));

    int was_init = -1;
    void* buf = op.CreateDestinationBuffer(10, 10, &was_init);
    ASSERT_NE(nullptr, buf);
    EXPECT_EQ(FALSE, was_init);
    GDALWarpOperation::DestroyDestinationBuffer(buf);
  }

  // 2. INIT_DEST=NO_DATA without nodata defined -> warning.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "INIT_DEST", "NO_DATA");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));

    int was_init = -1;
    WithQuietHandler quiet;
    CPLErrorReset();
    void* buf = op.CreateDestinationBuffer(10, 10, &was_init);
    ASSERT_NE(nullptr, buf);
    EXPECT_EQ(TRUE, was_init);
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("INIT_DEST was set to NO_DATA, but a NoData value "
                          "was not defined"));
    CPLErrorReset();
    GDALWarpOperation::DestroyDestinationBuffer(buf);
  }

  // 3. INIT_DEST=NO_DATA with nodata real and imag defined.
  {
    GDALDatasetUniquePtr src_c_ds = CreateMemDataset(10, 10, 1, GDT_CFloat64);
    GDALDatasetUniquePtr dst_c_ds = CreateMemDataset(10, 10, 1, GDT_CFloat64);

    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_c_ds.get(), dst_c_ds.get(), 1);
    options->eWorkingDataType = GDT_CFloat64;
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "INIT_DEST", "NO_DATA");
    GDALWarpInitDstNoDataReal(options.get(), 42.0);
    GDALWarpInitDstNoDataImag(options.get(), 7.0);

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));

    int was_init = -1;
    void* buf = op.CreateDestinationBuffer(10, 10, &was_init);
    ASSERT_NE(nullptr, buf);
    EXPECT_EQ(TRUE, was_init);
    const double* cvals = static_cast<const double*>(buf);
    EXPECT_DOUBLE_EQ(42.0, cvals[0]);
    EXPECT_DOUBLE_EQ(7.0, cvals[1]);
    GDALWarpOperation::DestroyDestinationBuffer(buf);
  }

  // 4. INIT_DEST=255 with GDT_Byte.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "INIT_DEST", "255");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));

    int was_init = -1;
    void* buf = op.CreateDestinationBuffer(10, 10, &was_init);
    ASSERT_NE(nullptr, buf);
    EXPECT_EQ(TRUE, was_init);
    const uint8_t* bytes = static_cast<const uint8_t*>(buf);
    EXPECT_EQ(255, bytes[0]);
    EXPECT_EQ(255, bytes[99]);
    GDALWarpOperation::DestroyDestinationBuffer(buf);
  }

  // 5. INIT_DEST=0 with GDT_Byte.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "INIT_DEST", "0");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));

    int was_init = -1;
    void* buf = op.CreateDestinationBuffer(10, 10, &was_init);
    ASSERT_NE(nullptr, buf);
    EXPECT_EQ(TRUE, was_init);
    const uint8_t* bytes = static_cast<const uint8_t*>(buf);
    EXPECT_EQ(0, bytes[0]);
    EXPECT_EQ(0, bytes[99]);
    GDALWarpOperation::DestroyDestinationBuffer(buf);
  }

  // 6. INIT_DEST=123.5 with GDT_Float64.
  {
    GDALDatasetUniquePtr src_f_ds = CreateMemDataset(10, 10, 1, GDT_Float64);
    GDALDatasetUniquePtr dst_f_ds = CreateMemDataset(10, 10, 1, GDT_Float64);

    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_f_ds.get(), dst_f_ds.get(), 1);
    options->eWorkingDataType = GDT_Float64;
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "INIT_DEST", "123.5");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));

    int was_init = -1;
    void* buf = op.CreateDestinationBuffer(10, 10, &was_init);
    ASSERT_NE(nullptr, buf);
    EXPECT_EQ(TRUE, was_init);
    const double* dvals = static_cast<const double*>(buf);
    EXPECT_DOUBLE_EQ(123.5, dvals[0]);
    GDALWarpOperation::DestroyDestinationBuffer(buf);
  }

  // 7. INIT_DEST=10+20i complex string.
  {
    GDALDatasetUniquePtr src_c_ds = CreateMemDataset(10, 10, 1, GDT_CFloat64);
    GDALDatasetUniquePtr dst_c_ds = CreateMemDataset(10, 10, 1, GDT_CFloat64);

    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_c_ds.get(), dst_c_ds.get(), 1);
    options->eWorkingDataType = GDT_CFloat64;
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "INIT_DEST", "10+20i");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));

    int was_init = -1;
    void* buf = op.CreateDestinationBuffer(10, 10, &was_init);
    ASSERT_NE(nullptr, buf);
    EXPECT_EQ(TRUE, was_init);
    const double* cvals = static_cast<const double*>(buf);
    EXPECT_DOUBLE_EQ(10.0, cvals[0]);
    EXPECT_DOUBLE_EQ(20.0, cvals[1]);
    GDALWarpOperation::DestroyDestinationBuffer(buf);
  }

  // 8. Invalid INIT_DEST string fails parsing and returns nullptr.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "INIT_DEST", "INVALID_INIT");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));

    WithQuietHandler quiet;
    CPLErrorReset();
    void* buf = op.CreateDestinationBuffer(10, 10, nullptr);
    EXPECT_EQ(nullptr, buf);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Error parsing INIT_DEST"));
    CPLErrorReset();
  }
}

// -----------------------------------------------------------------------------
// ChunkAndWarpImage Execution & Chunking Tests
// -----------------------------------------------------------------------------

TEST_F(GdalWarpOperationTest, ChunkAndWarpImageSingleChunk) {
  constexpr int kWidth = 20;
  constexpr int kHeight = 20;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  std::vector<uint8_t> src_pixels(kWidth * kHeight, 0);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    src_pixels[i] = static_cast<uint8_t>((i % 250) + 1);
  }
  WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_pixels);

  ProgressTracker tracker;
  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
  options->pfnProgress = RecordingProgress;
  options->pProgressArg = &tracker;

  GDALWarpOperation op;
  EXPECT_EQ(CE_None, op.Initialize(options.get()));
  EXPECT_EQ(CE_None, op.ChunkAndWarpImage(0, 0, kWidth, kHeight));

  EXPECT_EQ(src_pixels, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
  ASSERT_FALSE(tracker.progress_values.empty());
  EXPECT_DOUBLE_EQ(1.0, tracker.progress_values.back());
}

TEST_F(GdalWarpOperationTest, ChunkAndWarpImageCancellation) {
  constexpr int kWidth = 20;
  constexpr int kHeight = 20;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  ProgressTracker tracker;
  tracker.fail_threshold = 0.5;  // Cancel midway

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
  options->pfnProgress = RecordingProgress;
  options->pProgressArg = &tracker;

  GDALWarpOperation op;
  EXPECT_EQ(CE_None, op.Initialize(options.get()));
  EXPECT_EQ(CE_Failure, op.ChunkAndWarpImage(0, 0, kWidth, kHeight));
}

TEST_F(GdalWarpOperationTest, ChunkAndWarpImageMultiChunkSplitting) {
  // Low memory limit forces division of destination window.
  // 1. Wide dataset (splits horizontally along X when OPTIMIZE_SIZE=NO).
  {
    constexpr int kWidth = 400;
    constexpr int kHeight = 200;
    GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
    ASSERT_NE(nullptr, src_ds);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
    ASSERT_NE(nullptr, dst_ds);

    std::vector<uint8_t> src_pixels(kWidth * kHeight, 0);
    for (int i = 0; i < kWidth * kHeight; ++i) {
      src_pixels[i] = static_cast<uint8_t>((i % 250) + 1);
    }
    WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_pixels);

    ProgressTracker tracker;
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->dfWarpMemoryLimit = 100000.0;
    options->pfnProgress = RecordingProgress;
    options->pProgressArg = &tracker;
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "OPTIMIZE_SIZE", "NO");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    EXPECT_EQ(CE_None, op.ChunkAndWarpImage(0, 0, kWidth, kHeight));
    EXPECT_EQ(src_pixels, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
    EXPECT_GE(tracker.progress_values.size(), 2u);
  }

  // 2. Tall dataset (splits vertically along Y when OPTIMIZE_SIZE=YES).
  {
    constexpr int kWidth = 200;
    constexpr int kHeight = 400;
    GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
    ASSERT_NE(nullptr, src_ds);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
    ASSERT_NE(nullptr, dst_ds);

    std::vector<uint8_t> src_pixels(kWidth * kHeight, 0);
    for (int i = 0; i < kWidth * kHeight; ++i) {
      src_pixels[i] = static_cast<uint8_t>((i % 250) + 1);
    }
    WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_pixels);

    ProgressTracker tracker;
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->dfWarpMemoryLimit = 100000.0;
    options->pfnProgress = RecordingProgress;
    options->pProgressArg = &tracker;
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "OPTIMIZE_SIZE", "YES");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    EXPECT_EQ(CE_None, op.ChunkAndWarpImage(0, 0, kWidth, kHeight));
    EXPECT_EQ(src_pixels, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
    EXPECT_GE(tracker.progress_values.size(), 2u);
  }

  // 3. STREAMABLE_OUTPUT=YES.
  {
    constexpr int kWidth = 200;
    constexpr int kHeight = 400;
    GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
    ASSERT_NE(nullptr, src_ds);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
    ASSERT_NE(nullptr, dst_ds);

    ProgressTracker tracker;
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->dfWarpMemoryLimit = 100000.0;
    options->pfnProgress = RecordingProgress;
    options->pProgressArg = &tracker;
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "STREAMABLE_OUTPUT", "YES");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    EXPECT_EQ(CE_None, op.ChunkAndWarpImage(0, 0, kWidth, kHeight));
    EXPECT_GE(tracker.progress_values.size(), 2u);
  }
}

TEST_F(GdalWarpOperationTest, ChunkAndWarpImageSkipNoSource) {
  constexpr int kWidth = 20;
  constexpr int kHeight = 20;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
  options->pfnTransformer = OutOfBoundsTransformer;
  options->papszWarpOptions =
      CSLSetNameValue(options->papszWarpOptions, "SKIP_NOSOURCE", "YES");

  GDALWarpOperation op;
  EXPECT_EQ(CE_None, op.Initialize(options.get()));
  EXPECT_EQ(CE_None, op.ChunkAndWarpImage(0, 0, kWidth, kHeight));
}

// -----------------------------------------------------------------------------
// ChunkAndWarpMulti Multithreaded Warping Tests
// -----------------------------------------------------------------------------

TEST_F(GdalWarpOperationTest, ChunkAndWarpMultiExecution) {
  constexpr int kWidth = 400;
  constexpr int kHeight = 400;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  std::vector<uint8_t> src_pixels(kWidth * kHeight, 0);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    src_pixels[i] = static_cast<uint8_t>((i % 250) + 1);
  }
  WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_pixels);

  ProgressTracker tracker;
  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
  options->dfWarpMemoryLimit = 100000.0;  // Forces 4+ chunks
  options->pfnProgress = RecordingProgress;
  options->pProgressArg = &tracker;

  GDALWarpOperation op;
  EXPECT_EQ(CE_None, op.Initialize(options.get()));
  EXPECT_EQ(CE_None, op.ChunkAndWarpMulti(0, 0, kWidth, kHeight));

  EXPECT_EQ(src_pixels, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
  ASSERT_FALSE(tracker.progress_values.empty());
  EXPECT_DOUBLE_EQ(1.0, tracker.progress_values.back());
  EXPECT_GE(tracker.progress_values.size(), 4u);
}

TEST_F(GdalWarpOperationTest, ChunkAndWarpMultiCancellation) {
  constexpr int kWidth = 400;
  constexpr int kHeight = 400;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  ProgressTracker tracker;
  tracker.fail_threshold = 0.2;  // Cancel early in multithreaded loop

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
  options->dfWarpMemoryLimit = 100000.0;
  options->pfnProgress = RecordingProgress;
  options->pProgressArg = &tracker;

  GDALWarpOperation op;
  EXPECT_EQ(CE_None, op.Initialize(options.get()));
  EXPECT_EQ(CE_Failure, op.ChunkAndWarpMulti(0, 0, kWidth, kHeight));
}

// -----------------------------------------------------------------------------
// WarpRegion Tests
// -----------------------------------------------------------------------------

TEST_F(GdalWarpOperationTest, WarpRegionSingleBandAndMultiBand) {
  constexpr int kWidth = 10;
  constexpr int kHeight = 10;

  // 1. Single band read and write.
  {
    GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
    ASSERT_NE(nullptr, src_ds);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
    ASSERT_NE(nullptr, dst_ds);

    std::vector<uint8_t> src_pixels(kWidth * kHeight, 77);
    WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_pixels);

    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "WRITE_FLUSH", "YES");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    EXPECT_EQ(CE_None,
              op.WarpRegion(0, 0, kWidth, kHeight, 0, 0, kWidth, kHeight));
    EXPECT_EQ(src_pixels, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
  }

  // 2. Multi-band read and write.
  {
    GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 2);
    ASSERT_NE(nullptr, src_ds);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 2);
    ASSERT_NE(nullptr, dst_ds);

    std::vector<uint8_t> b1(kWidth * kHeight, 11);
    std::vector<uint8_t> b2(kWidth * kHeight, 22);
    WriteBandValues(src_ds.get(), 1, kWidth, kHeight, b1);
    WriteBandValues(src_ds.get(), 2, kWidth, kHeight, b2);

    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 2);

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    EXPECT_EQ(CE_None, op.WarpRegion(0, 0, kWidth, kHeight, 0, 0, kWidth,
                                     kHeight, 0.0, 0.0, 0.0, 1.0));
    EXPECT_EQ(b1, ReadBandValues(dst_ds.get(), 1, kWidth, kHeight));
    EXPECT_EQ(b2, ReadBandValues(dst_ds.get(), 2, kWidth, kHeight));
  }

  // 3. Early return when nSrcXSize == 0.
  {
    GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
    ASSERT_NE(nullptr, src_ds);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
    ASSERT_NE(nullptr, dst_ds);

    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    EXPECT_EQ(CE_None,
              op.WarpRegion(0, 0, kWidth, kHeight, 0, 0, 0, 0, 0.0, 1.0));
  }
}

TEST_F(GdalWarpOperationTest, WarpRegionReadFailure) {
  constexpr int kWidth = 10;
  constexpr int kHeight = 10;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);

  GDALWarpOperation op;
  EXPECT_EQ(CE_None, op.Initialize(options.get()));

  // Attempting to read destination window out of bounds causes RasterIO
  // failure.
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, op.WarpRegion(100, 100, kWidth, kHeight));
  CPLErrorReset();
}

// -----------------------------------------------------------------------------
// WarpRegionToBuffer Tests (Processors, Masks & Resampling)
// -----------------------------------------------------------------------------

TEST_F(GdalWarpOperationTest, WarpRegionToBufferProcessors) {
  constexpr int kWidth = 10;
  constexpr int kHeight = 10;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  // 1. Successful pre- and post-warp processors.
  {
    int pre_calls = 0;
    int post_calls = 0;
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->pfnPreWarpChunkProcessor = PreWarpSuccess;
    options->pPreWarpProcessorArg = &pre_calls;
    options->pfnPostWarpChunkProcessor = PostWarpSuccess;
    options->pPostWarpProcessorArg = &post_calls;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 0);
    EXPECT_EQ(CE_None, op.WarpRegionToBuffer(0, 0, kWidth, kHeight, buf.data(),
                                             GDT_Byte));
    EXPECT_EQ(1, pre_calls);
    EXPECT_EQ(1, post_calls);
  }

  // 2. Pre-warp processor failure aborts operation.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->pfnPreWarpChunkProcessor = PreWarpFailure;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 0);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.WarpRegionToBuffer(0, 0, kWidth, kHeight,
                                                buf.data(), GDT_Byte));
    CPLErrorReset();
  }

  // 3. Post-warp processor failure returns error.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->pfnPostWarpChunkProcessor = PostWarpFailure;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 0);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, op.WarpRegionToBuffer(0, 0, kWidth, kHeight,
                                                buf.data(), GDT_Byte));
    CPLErrorReset();
  }
}

TEST_F(GdalWarpOperationTest, WarpRegionToBufferAlphaBands) {
  constexpr int kWidth = 10;
  constexpr int kHeight = 10;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 2);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 2);
  ASSERT_NE(nullptr, dst_ds);

  std::vector<uint8_t> data(kWidth * kHeight, 50);
  std::vector<uint8_t> all_opaque(kWidth * kHeight, 255);
  std::vector<uint8_t> semi_transparent(kWidth * kHeight, 128);
  WriteBandValues(src_ds.get(), 1, kWidth, kHeight, data);

  // 1. Source alpha all opaque (bOutAllOpaque == TRUE).
  {
    WriteBandValues(src_ds.get(), 2, kWidth, kHeight, all_opaque);
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->nSrcAlphaBand = 2;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 0);
    EXPECT_EQ(CE_None, op.WarpRegionToBuffer(0, 0, kWidth, kHeight, buf.data(),
                                             GDT_Byte));
    EXPECT_EQ(50, buf[0]);
  }

  // 2. Source alpha semi-transparent (density mask retained).
  {
    WriteBandValues(src_ds.get(), 2, kWidth, kHeight, semi_transparent);
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->nSrcAlphaBand = 2;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 0);
    EXPECT_EQ(CE_None, op.WarpRegionToBuffer(0, 0, kWidth, kHeight, buf.data(),
                                             GDT_Byte));
    EXPECT_EQ(25, buf[0]);
  }

  // 3. Destination alpha band.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->nDstAlphaBand = 2;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 0);
    EXPECT_EQ(CE_None, op.WarpRegionToBuffer(0, 0, kWidth, kHeight, buf.data(),
                                             GDT_Byte));
  }
}

TEST_F(GdalWarpOperationTest, WarpRegionToBufferCutline) {
  constexpr int kWidth = 20;
  constexpr int kHeight = 20;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  std::vector<uint8_t> src_pixels(kWidth * kHeight, 100);
  WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_pixels);

  // 1. Cutline fully enclosing chunk (GCMVF_CHUNK_FULLY_WITHIN_CUTLINE).
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "CUTLINE",
                        "POLYGON((-10 -10, -10 30, 30 30, 30 -10, -10 -10))");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 0);
    EXPECT_EQ(CE_None, op.WarpRegionToBuffer(0, 0, kWidth, kHeight, buf.data(),
                                             GDT_Byte));
    EXPECT_EQ(100, buf[0]);
  }

  // 2. Cutline partially intersecting chunk.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "CUTLINE",
                        "POLYGON((0 0, 0 10, 10 10, 10 0, 0 0))");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 0);
    EXPECT_EQ(CE_None, op.WarpRegionToBuffer(0, 0, kWidth, kHeight, buf.data(),
                                             GDT_Byte));
    EXPECT_EQ(100, buf[0]);
    EXPECT_EQ(0, buf[15 * kWidth + 15]);
  }
}

TEST_F(GdalWarpOperationTest, WarpRegionToBufferNoData) {
  constexpr int kWidth = 10;
  constexpr int kHeight = 10;

  // 1. Single band source nodata - all valid (mask freed).
  {
    GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
    std::vector<uint8_t> pixels(kWidth * kHeight, 5);
    WriteBandValues(src_ds.get(), 1, kWidth, kHeight, pixels);

    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    GDALWarpInitSrcNoDataReal(options.get(), 0.0);  // Nodata is 0, pixels are 5

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 0);
    EXPECT_EQ(CE_None, op.WarpRegionToBuffer(0, 0, kWidth, kHeight, buf.data(),
                                             GDT_Byte));
    EXPECT_EQ(5, buf[0]);
  }

  // 2. Single band source nodata - some invalid (mask transferred).
  {
    GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
    std::vector<uint8_t> pixels(kWidth * kHeight, 0);  // All 0 == nodata
    WriteBandValues(src_ds.get(), 1, kWidth, kHeight, pixels);

    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    GDALWarpInitSrcNoDataReal(options.get(), 0.0);

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 77);
    EXPECT_EQ(CE_None, op.WarpRegionToBuffer(0, 0, kWidth, kHeight, buf.data(),
                                             GDT_Byte));
    EXPECT_EQ(77, buf[0]);
  }

  // 3. Multi-band source nodata - default unified.
  {
    GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 2);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 2);
    std::vector<uint8_t> pixels(kWidth * kHeight, 0);
    WriteBandValues(src_ds.get(), 1, kWidth, kHeight, pixels);
    WriteBandValues(src_ds.get(), 2, kWidth, kHeight, pixels);

    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 2);
    GDALWarpInitSrcNoDataReal(options.get(), 0.0);

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(2 * kWidth * kHeight, 77);
    EXPECT_EQ(CE_None, op.WarpRegionToBuffer(0, 0, kWidth, kHeight, buf.data(),
                                             GDT_Byte));
    EXPECT_EQ(77, buf[0]);
  }

  // 4. Multi-band source nodata - UNIFIED_SRC_NODATA=PARTIAL.
  {
    GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 2);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 2);
    std::vector<uint8_t> pixels(kWidth * kHeight, 0);
    WriteBandValues(src_ds.get(), 1, kWidth, kHeight, pixels);
    WriteBandValues(src_ds.get(), 2, kWidth, kHeight, pixels);

    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 2);
    GDALWarpInitSrcNoDataReal(options.get(), 0.0);
    options->papszWarpOptions = CSLSetNameValue(
        options->papszWarpOptions, "UNIFIED_SRC_NODATA", "PARTIAL");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(2 * kWidth * kHeight, 77);
    EXPECT_EQ(CE_None, op.WarpRegionToBuffer(0, 0, kWidth, kHeight, buf.data(),
                                             GDT_Byte));
    EXPECT_EQ(77, buf[0]);
  }

  // 5. Destination nodata - single band all valid.
  {
    GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    GDALWarpInitDstNoDataReal(options.get(), 0.0);

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 100);  // All 100 != 0
    EXPECT_EQ(CE_None, op.WarpRegionToBuffer(0, 0, kWidth, kHeight, buf.data(),
                                             GDT_Byte));
  }

  // 6. Destination nodata - multi-band.
  {
    GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 2);
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 2);
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 2);
    GDALWarpInitDstNoDataReal(options.get(), 0.0);

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(2 * kWidth * kHeight, 0);  // All 0 == nodata
    EXPECT_EQ(CE_None, op.WarpRegionToBuffer(0, 0, kWidth, kHeight, buf.data(),
                                             GDT_Byte));
  }
}

TEST_F(GdalWarpOperationTest, WarpRegionToBufferSourceDatasetMask) {
  constexpr int kWidth = 10;
  constexpr int kHeight = 10;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  ASSERT_EQ(CE_None, src_ds->CreateMaskBand(GMF_PER_DATASET));

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);

  GDALWarpOperation op;
  EXPECT_EQ(CE_None, op.Initialize(options.get()));
  std::vector<uint8_t> buf(kWidth * kHeight, 0);
  EXPECT_EQ(CE_None,
            op.WarpRegionToBuffer(0, 0, kWidth, kHeight, buf.data(), GDT_Byte));
}

TEST_F(GdalWarpOperationTest, ResamplingAlgorithmsCoverage) {
  constexpr int kWidth = 10;
  constexpr int kHeight = 10;
  GDALDatasetUniquePtr src_ds =
      CreateMemDataset(kWidth, kHeight, 1, GDT_Float32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds =
      CreateMemDataset(kWidth, kHeight, 1, GDT_Float32);
  ASSERT_NE(nullptr, dst_ds);

  std::vector<float> src_pixels(kWidth * kHeight, 1.0f);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    src_pixels[i] = static_cast<float>(i + 1);
  }
  WriteBandValuesFloat(src_ds.get(), 1, kWidth, kHeight, src_pixels);

  constexpr GDALResampleAlg kAlgorithms[] = {
      GRA_NearestNeighbour,
      GRA_Bilinear,
      GRA_Cubic,
      GRA_CubicSpline,
      GRA_Lanczos,
      GRA_Average,
      GRA_RMS,
      GRA_Mode,
      GRA_Max,
      GRA_Min,
      GRA_Med,
      GRA_Q1,
      GRA_Q3,
      GRA_Sum,
  };

  for (GDALResampleAlg alg : kAlgorithms) {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->eResampleAlg = alg;
    options->eWorkingDataType = GDT_Float32;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<float> buf(kWidth * kHeight, 0.0f);
    EXPECT_EQ(CE_None, op.WarpRegionToBuffer(0, 0, kWidth, kHeight, buf.data(),
                                             GDT_Float32));
    // GRA_CubicSpline is an approximating B-spline filter that blends neighbor
    // pixels at (0, 0) to 3.2f; all other algorithms yield 1.0f.
    const float expected = (alg == GRA_CubicSpline) ? 3.2f : 1.0f;
    EXPECT_NEAR(expected, buf[0], 1e-3f);
  }
}

TEST_F(GdalWarpOperationTest, TranslationOnPixelBoundariesOptimization) {
  constexpr int kWidth = 20;
  constexpr int kHeight = 20;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  double src_gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
  src_ds->SetGeoTransform(src_gt);
  double dst_gt[6] = {5.0, 1.0, 0.0, -5.0, 0.0, -1.0};
  dst_ds->SetGeoTransform(dst_gt);

  void* pTransformArg = GDALCreateGenImgProjTransformer2(
      GDALDataset::ToHandle(src_ds.get()), GDALDataset::ToHandle(dst_ds.get()),
      nullptr);
  ASSERT_NE(nullptr, pTransformArg);
  absl::Cleanup destroy_transformer = [pTransformArg] {
    GDALDestroyGenImgProjTransformer(pTransformArg);
  };

  std::vector<uint8_t> src_pixels(kWidth * kHeight, 0);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    src_pixels[i] = static_cast<uint8_t>((i % 250) + 1);
  }
  WriteBandValues(src_ds.get(), 1, kWidth, kHeight, src_pixels);

  // 1. With translation optimization enabled (default).
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->pfnTransformer = GDALGenImgProjTransform;
    options->pTransformerArg = pTransformArg;
    options->eResampleAlg = GRA_Cubic;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    EXPECT_EQ(CE_None, op.ChunkAndWarpImage(0, 0, 10, 10));
  }

  // 2. With GDAL_WARP_USE_TRANSLATION_OPTIM=NO.
  {
    CPLSetConfigOption("GDAL_WARP_USE_TRANSLATION_OPTIM", "NO");
    absl::Cleanup reset_opt = [] {
      CPLSetConfigOption("GDAL_WARP_USE_TRANSLATION_OPTIM", nullptr);
    };

    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->pfnTransformer = GDALGenImgProjTransform;
    options->pTransformerArg = pTransformArg;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    EXPECT_EQ(CE_None, op.ChunkAndWarpImage(0, 0, 10, 10));
  }
}

// -----------------------------------------------------------------------------
// ComputeSourceWindow & Transformer Edge Case Tests
// -----------------------------------------------------------------------------

TEST_F(GdalWarpOperationTest, SampleStepsAndGridSampling) {
  constexpr int kWidth = 20;
  constexpr int kHeight = 20;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  // 1. SAMPLE_GRID=YES with default SAMPLE_STEPS.
  // Run twice to test privateData->nStepCount == 0 then != 0.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "SAMPLE_GRID", "YES");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    EXPECT_EQ(CE_None, op.ChunkAndWarpImage(0, 0, 10, 10));
    EXPECT_EQ(CE_None, op.ChunkAndWarpImage(10, 0, 10, 10));
  }

  // 2. SAMPLE_STEPS=ALL with SAMPLE_GRID=YES.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "SAMPLE_GRID", "YES");
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "SAMPLE_STEPS", "ALL");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    EXPECT_EQ(CE_None, op.ChunkAndWarpImage(0, 0, 10, 10));
  }

  // 3. SAMPLE_STEPS=5 with SAMPLE_GRID=YES.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "SAMPLE_GRID", "YES");
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "SAMPLE_STEPS", "5");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    EXPECT_EQ(CE_None, op.ChunkAndWarpImage(0, 0, 10, 10));
  }

  // 4. SAMPLE_STEPS=ALL with edge sampling (!bUseGrid).
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "SAMPLE_STEPS", "ALL");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    EXPECT_EQ(CE_None, op.ChunkAndWarpImage(0, 0, 10, 10));
  }
}

TEST_F(GdalWarpOperationTest, TransformerEdgeCases) {
  constexpr int kWidth = 20;
  constexpr int kHeight = 20;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  // 1. Destination corner failure triggers bAll = true.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->pfnTransformer = CornerFailingTransformer;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 0);
    EXPECT_EQ(CE_None,
              op.WarpRegionToBuffer(0, 0, 10, 10, buf.data(), GDT_Byte));
  }

  // 2. NaN coordinates in transformer.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->pfnTransformer = NanCoordTransformer;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 0);
    EXPECT_EQ(CE_None,
              op.WarpRegionToBuffer(0, 0, 10, 10, buf.data(), GDT_Byte));
  }

  // 3. Partial failure transformer triggers fallback to grid.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->pfnTransformer = PartialFailureTransformer;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 0);
    EXPECT_EQ(CE_None,
              op.WarpRegionToBuffer(0, 0, 10, 10, buf.data(), GDT_Byte));
  }

  // 4. Bogus coordinates (> 1e6) trigger CHECK_WITH_INVERT_PROJ.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->pfnTransformer = BogusCoordTransformer;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 0);
    EXPECT_EQ(CE_None,
              op.WarpRegionToBuffer(0, 0, 10, 10, buf.data(), GDT_Byte));
  }

  // 5. Out of bounds coordinates -> returns CE_None and zero source window.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->pfnTransformer = OutOfBoundsTransformer;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    EXPECT_EQ(CE_None, op.WarpRegion(0, 0, 10, 10));
  }

  // 6. Large coverage (> 90% of raster size) snaps to full raster.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->pfnTransformer = LargeCoverageTransformer;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 0);
    EXPECT_EQ(CE_None,
              op.WarpRegionToBuffer(0, 0, 10, 10, buf.data(), GDT_Byte));
  }

  // 7. All points fail: ERROR_OUT_IF_EMPTY_SOURCE_WINDOW=TRUE (default).
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->pfnTransformer = AllFailingTransformer;

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 0);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              op.WarpRegionToBuffer(0, 0, 10, 10, buf.data(), GDT_Byte));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(
        CPLGetLastErrorMsg(),
        HasSubstr("failed to transform, unable to compute output bounds"));
    CPLErrorReset();
  }

  // 8. All points fail: ERROR_OUT_IF_EMPTY_SOURCE_WINDOW=FALSE.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    options->pfnTransformer = AllFailingTransformer;
    options->papszWarpOptions = CSLSetNameValue(
        options->papszWarpOptions, "ERROR_OUT_IF_EMPTY_SOURCE_WINDOW", "FALSE");
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "INIT_DEST", "0");

    GDALWarpOperation op;
    EXPECT_EQ(CE_None, op.Initialize(options.get()));
    std::vector<uint8_t> buf(kWidth * kHeight, 0);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_None,
              op.WarpRegionToBuffer(0, 0, 10, 10, buf.data(), GDT_Byte));
    CPLErrorReset();
  }
}

TEST_F(GdalWarpOperationTest, SrcFillRatioHeuristics) {
  constexpr int kDstSize = 120;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(20, 20, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kDstSize, kDstSize, 1);
  ASSERT_NE(nullptr, dst_ds);

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
  options->pfnTransformer = [](void* /*pTransformArg*/, int /*bDstToSrc*/,
                               int nPointCount, double* x, double* y,
                               double* /*z*/, int* panSuccess) -> int {
    if (panSuccess != nullptr) {
      for (int i = 0; i < nPointCount; ++i) {
        panSuccess[i] = TRUE;
      }
    }
    for (int i = 0; i < nPointCount; ++i) {
      x[i] = x[i] * 5.0 - 100.0;
      y[i] = y[i] * 5.0 - 100.0;
    }
    return TRUE;
  };
  options->papszWarpOptions = CSLSetNameValue(
      options->papszWarpOptions, "SRC_FILL_RATIO_HEURISTICS", "YES");

  GDALWarpOperation op;
  EXPECT_EQ(CE_None, op.Initialize(options.get()));
  EXPECT_EQ(CE_None, op.ChunkAndWarpImage(0, 0, kDstSize, kDstSize));
}

TEST_F(GdalWarpOperationTest, PolarSpecialPointDetection) {
  constexpr int kWidth = 50;
  constexpr int kHeight = 50;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  src_ds->SetProjection(SRS_WKT_WGS84_LAT_LONG);
  double src_gt[6] = {-180.0, 360.0 / kWidth, 0.0, 90.0, 0.0, -180.0 / kHeight};
  src_ds->SetGeoTransform(src_gt);

  OGRSpatialReference srs;
  srs.importFromEPSG(3031);  // Antarctic Polar Stereographic
  char* dst_wkt_raw = nullptr;
  srs.exportToWkt(&dst_wkt_raw);
  std::unique_ptr<char, CplFreeDeleter> dst_wkt(dst_wkt_raw);
  dst_ds->SetProjection(dst_wkt.get());

  double dst_gt[6] = {-100000.0, 4000.0, 0.0, 100000.0, 0.0, -4000.0};
  dst_ds->SetGeoTransform(dst_gt);

  void* pTransformArg = GDALCreateGenImgProjTransformer2(
      GDALDataset::ToHandle(src_ds.get()), GDALDataset::ToHandle(dst_ds.get()),
      nullptr);
  ASSERT_NE(nullptr, pTransformArg);
  absl::Cleanup destroy_transformer = [pTransformArg] {
    GDALDestroyGenImgProjTransformer(pTransformArg);
  };

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
  options->pfnTransformer = GDALGenImgProjTransform;
  options->pTransformerArg = pTransformArg;

  GDALWarpOperation op;
  EXPECT_EQ(CE_None, op.Initialize(options.get()));
  EXPECT_EQ(CE_None, op.ChunkAndWarpImage(0, 0, kWidth, kHeight));
}

// -----------------------------------------------------------------------------
// Options & Miscellaneous Configuration Tests
// -----------------------------------------------------------------------------

TEST_F(GdalWarpOperationTest, SourceExtraOption) {
  constexpr int kWidth = 20;
  constexpr int kHeight = 20;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
  options->papszWarpOptions =
      CSLSetNameValue(options->papszWarpOptions, "SOURCE_EXTRA", "5");

  GDALWarpOperation op;
  EXPECT_EQ(CE_None, op.Initialize(options.get()));
  std::vector<uint8_t> buf(kWidth * kHeight, 0);
  EXPECT_EQ(CE_None, op.WarpRegionToBuffer(0, 0, 10, 10, buf.data(), GDT_Byte));
}

TEST_F(GdalWarpOperationTest, ReportTimingsOption) {
  constexpr int kWidth = 10;
  constexpr int kHeight = 10;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  WarpOptionsUniquePtr options =
      CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
  options->papszWarpOptions =
      CSLSetNameValue(options->papszWarpOptions, "REPORT_TIMINGS", "YES");

  GDALWarpOperation op;
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_None, op.Initialize(options.get()));
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_EQ(CE_None, op.WarpRegion(0, 0, 10, 10));
}

// -----------------------------------------------------------------------------
// C API Wrappers Tests
// -----------------------------------------------------------------------------

TEST_F(GdalWarpOperationTest, CApiWrappers) {
  constexpr int kWidth = 10;
  constexpr int kHeight = 10;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kWidth, kHeight, 1);
  ASSERT_NE(nullptr, dst_ds);

  // 1. Null handle error checks.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALChunkAndWarpImage(nullptr, 0, 0, 10, 10));
    EXPECT_EQ(CE_Failure, GDALChunkAndWarpMulti(nullptr, 0, 0, 10, 10));
    EXPECT_EQ(CE_Failure, GDALWarpRegion(nullptr, 0, 0, 10, 10, 0, 0, 10, 10));
    std::vector<uint8_t> buf(100, 0);
    EXPECT_EQ(CE_Failure,
              GDALWarpRegionToBuffer(nullptr, 0, 0, 10, 10, buf.data(),
                                     GDT_Byte, 0, 0, 10, 10));
    CPLErrorReset();
  }

  // 2. Destroy null handle safely.
  GDALDestroyWarpOperation(nullptr);

  // 3. Create failure with invalid options.
  {
    WarpOptionsUniquePtr bad_options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    bad_options->dfWarpMemoryLimit = 50000.0;  // Too small
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALCreateWarpOperation(bad_options.get()));
    CPLErrorReset();
  }

  // 4. Create, run operations, destroy.
  {
    WarpOptionsUniquePtr options =
        CreateDefaultWarpOptions(src_ds.get(), dst_ds.get(), 1);
    GDALWarpOperationH hOp = GDALCreateWarpOperation(options.get());
    ASSERT_NE(nullptr, hOp);

    EXPECT_EQ(CE_None, GDALChunkAndWarpImage(hOp, 0, 0, 5, 5));
    EXPECT_EQ(CE_None, GDALWarpRegion(hOp, 0, 5, 5, 5, 0, 5, 5, 5));

    std::vector<uint8_t> buf(25, 0);
    EXPECT_EQ(CE_None, GDALWarpRegionToBuffer(hOp, 5, 5, 5, 5, buf.data(),
                                              GDT_Byte, 5, 5, 5, 5));
    EXPECT_EQ(CE_None, GDALChunkAndWarpMulti(hOp, 5, 0, 5, 5));

    GDALDestroyWarpOperation(hOp);
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered branches are OOM allocation guards
// and unreachable defensive checks.
