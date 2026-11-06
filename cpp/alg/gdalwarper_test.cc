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
// Tests GDAL warper convenience APIs (alg/gdalwarper.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalwarper.cpp
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalwarper.h

#include "third_party/gdal/alg/gdalwarper.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
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
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_mask.h"
#include "third_party/gdal/port/cpl_minixml.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

// Custom deleter for GDALWarpOptions RAII management.
struct WarpOptionsDeleter {
  void operator()(GDALWarpOptions* psOptions) const {
    GDALDestroyWarpOptions(psOptions);
  }
};
using WarpOptionsUniquePtr =
    std::unique_ptr<GDALWarpOptions, WarpOptionsDeleter>;

// Helper to create an in-memory dataset using the MEM driver.
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

// Helper to set standard WGS 84 geographic SRS and geotransform.
void SetWgs84GeoTransform(GDALDataset* ds, double pixel_size = 1.0) {
  const double gt[6] = {0.0, pixel_size, 0.0, 10.0, 0.0, -pixel_size};
  ds->SetGeoTransform(gt);
  OGRSpatialReference srs;
  srs.importFromEPSG(4326);
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  ds->SetSpatialRef(&srs);
}

// Helper to fill band pixels from a double vector.
void FillBand(GDALDataset* ds, int band_num,
              const std::vector<double>& values) {
  GDALRasterBand* band = ds->GetRasterBand(band_num);
  ASSERT_NE(nullptr, band);
  int nx = ds->GetRasterXSize();
  int ny = ds->GetRasterYSize();
  ASSERT_EQ(nx * ny, static_cast<int>(values.size()));
  EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, nx, ny,
                                    const_cast<double*>(values.data()), nx, ny,
                                    GDT_Float64, 0, 0, nullptr));
}

// Helper to fill byte band pixels from a uint8_t vector.
void FillByteBand(GDALDataset* ds, int band_num,
                  const std::vector<uint8_t>& values) {
  GDALRasterBand* band = ds->GetRasterBand(band_num);
  ASSERT_NE(nullptr, band);
  int nx = ds->GetRasterXSize();
  int ny = ds->GetRasterYSize();
  ASSERT_EQ(nx * ny, static_cast<int>(values.size()));
  EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, nx, ny,
                                    const_cast<uint8_t*>(values.data()), nx, ny,
                                    GDT_Byte, 0, 0, nullptr));
}

// Progress tracking structure for progress reporting tests.
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

// =============================================================================
// GDALGetWarpResampleAlg Tests
// =============================================================================

TEST(GdalWarperTest, GetWarpResampleAlgSupported) {
  struct TestCase {
    std::string_view name;
    GDALResampleAlg expected_alg;
  };
  constexpr TestCase kCases[] = {
      {.name = "near", .expected_alg = GRA_NearestNeighbour},
      {.name = "nearest", .expected_alg = GRA_NearestNeighbour},
      {.name = "bilinear", .expected_alg = GRA_Bilinear},
      {.name = "cubic", .expected_alg = GRA_Cubic},
      {.name = "cubicspline", .expected_alg = GRA_CubicSpline},
      {.name = "lanczos", .expected_alg = GRA_Lanczos},
      {.name = "average", .expected_alg = GRA_Average},
      {.name = "rms", .expected_alg = GRA_RMS},
      {.name = "mode", .expected_alg = GRA_Mode},
      {.name = "max", .expected_alg = GRA_Max},
      {.name = "min", .expected_alg = GRA_Min},
      {.name = "med", .expected_alg = GRA_Med},
      {.name = "q1", .expected_alg = GRA_Q1},
      {.name = "q3", .expected_alg = GRA_Q3},
      {.name = "sum", .expected_alg = GRA_Sum},
  };
  for (const auto& tc : kCases) {
    GDALResampleAlg alg = GRA_NearestNeighbour;
    EXPECT_TRUE(GDALGetWarpResampleAlg(std::string(tc.name).c_str(), alg,
                                       /*bThrow=*/false));
    EXPECT_EQ(tc.expected_alg, alg);
  }
}

TEST(GdalWarperTest, GetWarpResampleAlgInvalid) {
  GDALResampleAlg alg = GRA_NearestNeighbour;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(
        GDALGetWarpResampleAlg("unsupported_method", alg, /*bThrow=*/false));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_IllegalArg, CPLGetLastErrorNo());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("unsupported_method"));
  }
  EXPECT_THROW(
      GDALGetWarpResampleAlg("unsupported_method", alg, /*bThrow=*/true),
      std::invalid_argument);
}

// =============================================================================
// GDALWarpGetOptionList Tests
// =============================================================================

TEST(GdalWarperTest, GetOptionList) {
  const char* options_xml = GDALWarpGetOptionList();
  ASSERT_NE(nullptr, options_xml);
  EXPECT_THAT(options_xml, HasSubstr("<OptionList>"));
  EXPECT_THAT(options_xml, HasSubstr("name='INIT_DEST'"));
  EXPECT_THAT(options_xml, HasSubstr("name='WRITE_FLUSH'"));
  EXPECT_THAT(options_xml, HasSubstr("name='SKIP_NOSOURCE'"));
  EXPECT_THAT(options_xml, HasSubstr("name='UNIFIED_SRC_NODATA'"));
  EXPECT_THAT(options_xml, HasSubstr("name='CUTLINE'"));
  EXPECT_THAT(options_xml, HasSubstr("name='CUTLINE_BLEND_DIST'"));
  EXPECT_THAT(options_xml, HasSubstr("name='CUTLINE_ALL_TOUCHED'"));
  EXPECT_THAT(options_xml, HasSubstr("name='XSCALE'"));
  EXPECT_THAT(options_xml, HasSubstr("name='YSCALE'"));
  EXPECT_THAT(options_xml, HasSubstr("name='OPTIMIZE_SIZE'"));
  EXPECT_THAT(options_xml, HasSubstr("name='NUM_THREADS'"));
  EXPECT_THAT(options_xml, HasSubstr("name='STREAMABLE_OUTPUT'"));
  EXPECT_THAT(options_xml, HasSubstr("name='SRC_COORD_PRECISION'"));
  EXPECT_THAT(options_xml, HasSubstr("name='SRC_ALPHA_MAX'"));
  EXPECT_THAT(options_xml, HasSubstr("name='DST_ALPHA_MAX'"));
  EXPECT_THAT(options_xml, HasSubstr("name='SAMPLE_GRID'"));
  EXPECT_THAT(options_xml, HasSubstr("name='SAMPLE_STEPS'"));
  EXPECT_THAT(options_xml, HasSubstr("name='SOURCE_EXTRA'"));
  EXPECT_THAT(options_xml, HasSubstr("name='APPLY_VERTICAL_SHIFT'"));
  EXPECT_THAT(options_xml, HasSubstr("name='MULT_FACTOR_VERTICAL_SHIFT'"));
  EXPECT_THAT(options_xml, HasSubstr("name='EXCLUDED_VALUES'"));
  EXPECT_THAT(options_xml, HasSubstr("name='EXCLUDED_VALUES_PCT_THRESHOLD'"));
  EXPECT_THAT(options_xml, HasSubstr("name='NODATA_VALUES_PCT_THRESHOLD'"));
  EXPECT_THAT(options_xml, HasSubstr("name='MODE_TIES'"));
}

// =============================================================================
// GDALCreateWarpOptions, GDALDestroyWarpOptions, GDALCloneWarpOptions Tests
// =============================================================================

TEST(GdalWarperTest, CreateDestroyCloneWarpOptions) {
  // 1. Destroying nullptr is safe.
  GDALDestroyWarpOptions(nullptr);

  // 2. Default creation values.
  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  ASSERT_NE(nullptr, options);
  EXPECT_EQ(0, options->nBandCount);
  EXPECT_EQ(GRA_NearestNeighbour, options->eResampleAlg);
  EXPECT_EQ(GDALDummyProgress, options->pfnProgress);
  EXPECT_EQ(GDT_Unknown, options->eWorkingDataType);
  EXPECT_EQ(GWKTS_First, options->eTieStrategy);
  EXPECT_EQ(nullptr, options->papszWarpOptions);
  EXPECT_EQ(nullptr, options->panSrcBands);
  EXPECT_EQ(nullptr, options->panDstBands);
  EXPECT_EQ(nullptr, options->padfSrcNoDataReal);
  EXPECT_EQ(nullptr, options->padfSrcNoDataImag);
  EXPECT_EQ(nullptr, options->padfDstNoDataReal);
  EXPECT_EQ(nullptr, options->padfDstNoDataImag);
  EXPECT_EQ(nullptr, options->hCutline);
  EXPECT_DOUBLE_EQ(0.0, options->dfCutlineBlendDist);

  // 3. Populate all fields.
  options->dfWarpMemoryLimit = 1234567.0;
  options->eResampleAlg = GRA_Cubic;
  options->eWorkingDataType = GDT_Float32;
  options->eTieStrategy = GWKTS_Max;
  options->papszWarpOptions =
      CSLSetNameValue(options->papszWarpOptions, "INIT_DEST", "0");
  options->nBandCount = 2;
  options->panSrcBands = static_cast<int*>(CPLMalloc(2 * sizeof(int)));
  options->panSrcBands[0] = 1;
  options->panSrcBands[1] = 2;
  options->panDstBands = static_cast<int*>(CPLMalloc(2 * sizeof(int)));
  options->panDstBands[0] = 2;
  options->panDstBands[1] = 1;
  options->padfSrcNoDataReal =
      static_cast<double*>(CPLMalloc(2 * sizeof(double)));
  options->padfSrcNoDataReal[0] = -999.0;
  options->padfSrcNoDataReal[1] = -888.0;
  options->padfSrcNoDataImag =
      static_cast<double*>(CPLMalloc(2 * sizeof(double)));
  options->padfSrcNoDataImag[0] = 1.0;
  options->padfSrcNoDataImag[1] = 2.0;
  options->padfDstNoDataReal =
      static_cast<double*>(CPLMalloc(2 * sizeof(double)));
  options->padfDstNoDataReal[0] = 0.0;
  options->padfDstNoDataReal[1] = -1.0;
  options->padfDstNoDataImag =
      static_cast<double*>(CPLMalloc(2 * sizeof(double)));
  options->padfDstNoDataImag[0] = 3.0;
  options->padfDstNoDataImag[1] = 4.0;
  options->nSrcAlphaBand = 2;
  options->nDstAlphaBand = 2;
  options->dfCutlineBlendDist = 12.5;

  OGRGeometryH hPolygon = OGR_G_CreateGeometry(wkbPolygon);
  OGRGeometryH hRing = OGR_G_CreateGeometry(wkbLinearRing);
  OGR_G_AddPoint_2D(hRing, 0, 0);
  OGR_G_AddPoint_2D(hRing, 0, 10);
  OGR_G_AddPoint_2D(hRing, 10, 10);
  OGR_G_AddPoint_2D(hRing, 10, 0);
  OGR_G_AddPoint_2D(hRing, 0, 0);
  OGR_G_AddGeometryDirectly(hPolygon, hRing);
  options->hCutline = hPolygon;

  // 4. Clone options and verify complete fidelity.
  WarpOptionsUniquePtr clone(GDALCloneWarpOptions(options.get()));
  ASSERT_NE(nullptr, clone);
  EXPECT_DOUBLE_EQ(1234567.0, clone->dfWarpMemoryLimit);
  EXPECT_EQ(GRA_Cubic, clone->eResampleAlg);
  EXPECT_EQ(GDT_Float32, clone->eWorkingDataType);
  EXPECT_EQ(GWKTS_Max, clone->eTieStrategy);
  EXPECT_STREQ("0", CSLFetchNameValue(clone->papszWarpOptions, "INIT_DEST"));
  EXPECT_EQ(2, clone->nBandCount);
  ASSERT_NE(nullptr, clone->panSrcBands);
  EXPECT_EQ(1, clone->panSrcBands[0]);
  EXPECT_EQ(2, clone->panSrcBands[1]);
  ASSERT_NE(nullptr, clone->panDstBands);
  EXPECT_EQ(2, clone->panDstBands[0]);
  EXPECT_EQ(1, clone->panDstBands[1]);
  ASSERT_NE(nullptr, clone->padfSrcNoDataReal);
  EXPECT_DOUBLE_EQ(-999.0, clone->padfSrcNoDataReal[0]);
  EXPECT_DOUBLE_EQ(-888.0, clone->padfSrcNoDataReal[1]);
  ASSERT_NE(nullptr, clone->padfSrcNoDataImag);
  EXPECT_DOUBLE_EQ(1.0, clone->padfSrcNoDataImag[0]);
  EXPECT_DOUBLE_EQ(2.0, clone->padfSrcNoDataImag[1]);
  ASSERT_NE(nullptr, clone->padfDstNoDataReal);
  EXPECT_DOUBLE_EQ(0.0, clone->padfDstNoDataReal[0]);
  EXPECT_DOUBLE_EQ(-1.0, clone->padfDstNoDataReal[1]);
  ASSERT_NE(nullptr, clone->padfDstNoDataImag);
  EXPECT_DOUBLE_EQ(3.0, clone->padfDstNoDataImag[0]);
  EXPECT_DOUBLE_EQ(4.0, clone->padfDstNoDataImag[1]);
  EXPECT_EQ(2, clone->nSrcAlphaBand);
  EXPECT_EQ(2, clone->nDstAlphaBand);
  EXPECT_DOUBLE_EQ(12.5, clone->dfCutlineBlendDist);
  ASSERT_NE(nullptr, clone->hCutline);
  EXPECT_NE(options->hCutline, clone->hCutline);
}

// =============================================================================
// NoData Initializer Helpers Tests
// =============================================================================

TEST(GdalWarperTest, InitNoDataHelpers) {
  // 1. Calling with nullptr option pointer logs error and returns safely.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALWarpInitDstNoDataReal(nullptr, 0.0);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
    GDALWarpInitSrcNoDataReal(nullptr, 0.0);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
    GDALWarpInitNoDataReal(nullptr, 0.0);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
    GDALWarpInitDstNoDataImag(nullptr, 0.0);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
    GDALWarpInitSrcNoDataImag(nullptr, 0.0);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 2. Calling with nBandCount == 0 does nothing.
  {
    WarpOptionsUniquePtr options(GDALCreateWarpOptions());
    options->nBandCount = 0;
    GDALWarpInitDstNoDataReal(options.get(), 1.0);
    EXPECT_EQ(nullptr, options->padfDstNoDataReal);
    GDALWarpInitSrcNoDataReal(options.get(), 1.0);
    EXPECT_EQ(nullptr, options->padfSrcNoDataReal);
    GDALWarpInitDstNoDataImag(options.get(), 1.0);
    EXPECT_EQ(nullptr, options->padfDstNoDataImag);
    GDALWarpInitSrcNoDataImag(options.get(), 1.0);
    EXPECT_EQ(nullptr, options->padfSrcNoDataImag);
  }

  // 3. Normal initialization for multiple bands.
  {
    WarpOptionsUniquePtr options(GDALCreateWarpOptions());
    options->nBandCount = 3;
    GDALWarpInitNoDataReal(options.get(), -999.0);
    ASSERT_NE(nullptr, options->padfDstNoDataReal);
    ASSERT_NE(nullptr, options->padfSrcNoDataReal);
    for (int i = 0; i < 3; ++i) {
      EXPECT_DOUBLE_EQ(-999.0, options->padfDstNoDataReal[i]);
      EXPECT_DOUBLE_EQ(-999.0, options->padfSrcNoDataReal[i]);
    }

    // 4. Repeated call does not overwrite existing array.
    GDALWarpInitDstNoDataReal(options.get(), 100.0);
    EXPECT_DOUBLE_EQ(-999.0, options->padfDstNoDataReal[0]);
    GDALWarpInitSrcNoDataReal(options.get(), 100.0);
    EXPECT_DOUBLE_EQ(-999.0, options->padfSrcNoDataReal[0]);

    // 5. Imaginary components.
    GDALWarpInitDstNoDataImag(options.get(), 1.5);
    GDALWarpInitSrcNoDataImag(options.get(), 2.5);
    ASSERT_NE(nullptr, options->padfDstNoDataImag);
    ASSERT_NE(nullptr, options->padfSrcNoDataImag);
    for (int i = 0; i < 3; ++i) {
      EXPECT_DOUBLE_EQ(1.5, options->padfDstNoDataImag[i]);
      EXPECT_DOUBLE_EQ(2.5, options->padfSrcNoDataImag[i]);
    }
    // Repeated call does not overwrite.
    GDALWarpInitDstNoDataImag(options.get(), 99.0);
    EXPECT_DOUBLE_EQ(1.5, options->padfDstNoDataImag[0]);
    GDALWarpInitSrcNoDataImag(options.get(), 99.0);
    EXPECT_DOUBLE_EQ(2.5, options->padfSrcNoDataImag[0]);
  }
}

// =============================================================================
// GDALWarpResolveWorkingDataType Tests
// =============================================================================

TEST(GdalWarperTest, ResolveWorkingDataTypeDefaultAndNoOp) {
  // nullptr options.
  GDALWarpResolveWorkingDataType(nullptr);

  // Already specified working data type is kept untouched.
  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  options->eWorkingDataType = GDT_Float64;
  GDALWarpResolveWorkingDataType(options.get());
  EXPECT_EQ(GDT_Float64, options->eWorkingDataType);
}

TEST(GdalWarperTest, ResolveWorkingDataTypeInvalidSrcNoData) {
  // If all src nodata values cannot be represented in the source data type,
  // they are freed and cleared.
  GDALDatasetUniquePtr src_ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);

  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  options->hSrcDS = src_ds.get();
  options->nBandCount = 1;
  options->panSrcBands = static_cast<int*>(CPLMalloc(sizeof(int)));
  options->panSrcBands[0] = 1;
  options->padfSrcNoDataReal = static_cast<double*>(CPLMalloc(sizeof(double)));
  options->padfSrcNoDataReal[0] = 99999.0;  // Out of range for GDT_Byte
  options->padfSrcNoDataImag = static_cast<double*>(CPLMalloc(sizeof(double)));
  options->padfSrcNoDataImag[0] = 0.0;

  GDALWarpResolveWorkingDataType(options.get());
  EXPECT_EQ(nullptr, options->padfSrcNoDataReal);
  EXPECT_EQ(nullptr, options->padfSrcNoDataImag);
  EXPECT_EQ(GDT_Byte, options->eWorkingDataType);
}

TEST(GdalWarperTest, ResolveWorkingDataTypeUnionTypes) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(4, 4, 1, GDT_Int16);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(4, 4, 1, GDT_UInt16);
  ASSERT_NE(nullptr, src_ds);
  ASSERT_NE(nullptr, dst_ds);

  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  options->hSrcDS = src_ds.get();
  options->hDstDS = dst_ds.get();
  options->nBandCount = 1;
  options->panSrcBands = static_cast<int*>(CPLMalloc(sizeof(int)));
  options->panSrcBands[0] = 1;
  options->panDstBands = static_cast<int*>(CPLMalloc(sizeof(int)));
  options->panDstBands[0] = 1;

  // Int16 and UInt16 union to Int32.
  GDALWarpResolveWorkingDataType(options.get());
  EXPECT_EQ(GDT_Int32, options->eWorkingDataType);

  // Now test with non-zero imaginary nodata promoting to complex type.
  WarpOptionsUniquePtr options_complex(GDALCreateWarpOptions());
  options_complex->hSrcDS = src_ds.get();
  options_complex->hDstDS = dst_ds.get();
  options_complex->nBandCount = 1;
  options_complex->panSrcBands = static_cast<int*>(CPLMalloc(sizeof(int)));
  options_complex->panSrcBands[0] = 1;
  options_complex->panDstBands = static_cast<int*>(CPLMalloc(sizeof(int)));
  options_complex->panDstBands[0] = 1;
  options_complex->padfSrcNoDataReal =
      static_cast<double*>(CPLMalloc(sizeof(double)));
  options_complex->padfSrcNoDataReal[0] = 0.0;
  options_complex->padfSrcNoDataImag =
      static_cast<double*>(CPLMalloc(sizeof(double)));
  options_complex->padfSrcNoDataImag[0] = 1.0;
  options_complex->padfDstNoDataReal =
      static_cast<double*>(CPLMalloc(sizeof(double)));
  options_complex->padfDstNoDataReal[0] = 0.0;
  options_complex->padfDstNoDataImag =
      static_cast<double*>(CPLMalloc(sizeof(double)));
  options_complex->padfDstNoDataImag[0] = 2.0;

  GDALWarpResolveWorkingDataType(options_complex.get());
  EXPECT_EQ(GDT_CInt32, options_complex->eWorkingDataType);
}

TEST(GdalWarperTest, ResolveWorkingDataTypeVerticalShift) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(4, 4, 1, GDT_Int16);
  ASSERT_NE(nullptr, src_ds);

  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  options->hSrcDS = src_ds.get();
  options->nBandCount = 1;
  options->panSrcBands = static_cast<int*>(CPLMalloc(sizeof(int)));
  options->panSrcBands[0] = 1;
  options->papszWarpOptions =
      CSLSetNameValue(options->papszWarpOptions, "APPLY_VERTICAL_SHIFT", "YES");
  options->papszWarpOptions = CSLSetNameValue(
      options->papszWarpOptions, "MULT_FACTOR_VERTICAL_SHIFT", "0.5");

  GDALWarpResolveWorkingDataType(options.get());
  EXPECT_EQ(GDT_Float32, options->eWorkingDataType);
}

// =============================================================================
// GDALWarpInitDefaultBandMapping Tests
// =============================================================================

TEST(GdalWarperTest, InitDefaultBandMapping) {
  // If nBandCount != 0, it does nothing.
  {
    WarpOptionsUniquePtr options(GDALCreateWarpOptions());
    options->nBandCount = 2;
    GDALWarpInitDefaultBandMapping(options.get(), 5);
    EXPECT_EQ(2, options->nBandCount);
    EXPECT_EQ(nullptr, options->panSrcBands);
    EXPECT_EQ(nullptr, options->panDstBands);
  }

  // If nBandCount == 0, initializes src and dst mappings to 1..n.
  {
    WarpOptionsUniquePtr options(GDALCreateWarpOptions());
    GDALWarpInitDefaultBandMapping(options.get(), 4);
    EXPECT_EQ(4, options->nBandCount);
    ASSERT_NE(nullptr, options->panSrcBands);
    ASSERT_NE(nullptr, options->panDstBands);
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(i + 1, options->panSrcBands[i]);
      EXPECT_EQ(i + 1, options->panDstBands[i]);
    }
  }
}

// =============================================================================
// GDALWarpNoDataMasker Tests
// =============================================================================

TEST(GdalWarperTest, NoDataMaskerInvalidArgs) {
  double nodata[2] = {0.0, 0.0};
  uint8_t image[4] = {0, 1, 2, 3};
  GByte* p_image = image;
  GUInt32 mask = 0xFFFFFFFF;
  int all_valid = FALSE;

  WithQuietHandler quiet;
  CPLErrorReset();

  // nBandCount != 1 fails.
  EXPECT_EQ(CE_Failure, GDALWarpNoDataMasker(
                            nodata, /*nBandCount=*/2, GDT_Byte, 0, 0, 2, 2,
                            &p_image, /*bMaskIsFloat=*/0, &mask, &all_valid));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  // bMaskIsFloat != 0 fails.
  EXPECT_EQ(CE_Failure, GDALWarpNoDataMasker(
                            nodata, /*nBandCount=*/1, GDT_Byte, 0, 0, 2, 2,
                            &p_image, /*bMaskIsFloat=*/1, &mask, &all_valid));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST(GdalWarperTest, NoDataMaskerByte) {
  // 1. Out-of-range nodata values: all valid.
  {
    double nodata_neg[2] = {-1.0, 0.0};
    uint8_t img[4] = {0, 1, 2, 3};
    GByte* p_img = img;
    GUInt32 mask = 0xFFFFFFFF;
    int all_valid = FALSE;
    EXPECT_EQ(CE_None, GDALWarpNoDataMasker(nodata_neg, 1, GDT_Byte, 0, 0, 2, 2,
                                            &p_img, 0, &mask, &all_valid));
    EXPECT_EQ(TRUE, all_valid);

    double nodata_large[2] = {300.0, 0.0};
    EXPECT_EQ(CE_None, GDALWarpNoDataMasker(nodata_large, 1, GDT_Byte, 0, 0, 2,
                                            2, &p_img, 0, &mask, &all_valid));
    EXPECT_EQ(TRUE, all_valid);

    double nodata_imag[2] = {2.0, 1.0};
    EXPECT_EQ(CE_None, GDALWarpNoDataMasker(nodata_imag, 1, GDT_Byte, 0, 0, 2,
                                            2, &p_img, 0, &mask, &all_valid));
    EXPECT_EQ(TRUE, all_valid);
  }

  // 2. In-range matching and non-matching.
  {
    double nodata[2] = {2.0, 0.0};
    uint8_t img[4] = {1, 2, 3, 2};
    GByte* p_img = img;
    GUInt32 mask = 0xFFFFFFFF;
    int all_valid = TRUE;
    EXPECT_EQ(CE_None, GDALWarpNoDataMasker(nodata, 1, GDT_Byte, 0, 0, 2, 2,
                                            &p_img, 0, &mask, &all_valid));
    EXPECT_EQ(FALSE, all_valid);
    EXPECT_TRUE(CPLMaskGet(&mask, 0));
    EXPECT_FALSE(CPLMaskGet(&mask, 1));
    EXPECT_TRUE(CPLMaskGet(&mask, 2));
    EXPECT_FALSE(CPLMaskGet(&mask, 3));
  }
}

TEST(GdalWarperTest, NoDataMaskerInt16AndUInt16) {
  // Int16
  {
    double nodata_out[2] = {-50000.0, 0.0};
    int16_t img[2] = {100, -100};
    GByte* p_img = reinterpret_cast<GByte*>(img);
    GUInt32 mask = 0xFFFFFFFF;
    int all_valid = FALSE;
    EXPECT_EQ(CE_None, GDALWarpNoDataMasker(nodata_out, 1, GDT_Int16, 0, 0, 2,
                                            1, &p_img, 0, &mask, &all_valid));
    EXPECT_EQ(TRUE, all_valid);

    double nodata_in[2] = {-100.0, 0.0};
    EXPECT_EQ(CE_None, GDALWarpNoDataMasker(nodata_in, 1, GDT_Int16, 0, 0, 2, 1,
                                            &p_img, 0, &mask, &all_valid));
    EXPECT_EQ(FALSE, all_valid);
    EXPECT_TRUE(CPLMaskGet(&mask, 0));
    EXPECT_FALSE(CPLMaskGet(&mask, 1));
  }

  // UInt16
  {
    double nodata_out[2] = {70000.0, 0.0};
    uint16_t img[2] = {500, 1000};
    GByte* p_img = reinterpret_cast<GByte*>(img);
    GUInt32 mask = 0xFFFFFFFF;
    int all_valid = FALSE;
    EXPECT_EQ(CE_None, GDALWarpNoDataMasker(nodata_out, 1, GDT_UInt16, 0, 0, 2,
                                            1, &p_img, 0, &mask, &all_valid));
    EXPECT_EQ(TRUE, all_valid);

    double nodata_in[2] = {500.0, 0.0};
    EXPECT_EQ(CE_None, GDALWarpNoDataMasker(nodata_in, 1, GDT_UInt16, 0, 0, 2,
                                            1, &p_img, 0, &mask, &all_valid));
    EXPECT_EQ(FALSE, all_valid);
    EXPECT_FALSE(CPLMaskGet(&mask, 0));
    EXPECT_TRUE(CPLMaskGet(&mask, 1));
  }
}

TEST(GdalWarperTest, NoDataMaskerFloat32) {
  // Non-zero imaginary component: all valid.
  {
    double nodata_imag[2] = {0.0, 1.0};
    float img[2] = {0.0f, 1.0f};
    GByte* p_img = reinterpret_cast<GByte*>(img);
    GUInt32 mask = 0xFFFFFFFF;
    int all_valid = FALSE;
    EXPECT_EQ(CE_None,
              GDALWarpNoDataMasker(nodata_imag, 1, GDT_Float32, 0, 0, 2, 1,
                                   &p_img, 0, &mask, &all_valid));
    EXPECT_EQ(TRUE, all_valid);
  }

  // Normal float comparison.
  {
    double nodata[2] = {-999.0, 0.0};
    float img[3] = {1.0f, -999.0f, 2.0f};
    GByte* p_img = reinterpret_cast<GByte*>(img);
    GUInt32 mask = 0xFFFFFFFF;
    int all_valid = TRUE;
    EXPECT_EQ(CE_None, GDALWarpNoDataMasker(nodata, 1, GDT_Float32, 0, 0, 3, 1,
                                            &p_img, 0, &mask, &all_valid));
    EXPECT_EQ(FALSE, all_valid);
    EXPECT_TRUE(CPLMaskGet(&mask, 0));
    EXPECT_FALSE(CPLMaskGet(&mask, 1));
    EXPECT_TRUE(CPLMaskGet(&mask, 2));
  }

  // NaN nodata comparison.
  {
    double nodata_nan[2] = {std::numeric_limits<double>::quiet_NaN(), 0.0};
    float img[3] = {1.0f, std::numeric_limits<float>::quiet_NaN(), 2.0f};
    GByte* p_img = reinterpret_cast<GByte*>(img);
    GUInt32 mask = 0xFFFFFFFF;
    int all_valid = TRUE;
    EXPECT_EQ(CE_None, GDALWarpNoDataMasker(nodata_nan, 1, GDT_Float32, 0, 0, 3,
                                            1, &p_img, 0, &mask, &all_valid));
    EXPECT_EQ(FALSE, all_valid);
    EXPECT_TRUE(CPLMaskGet(&mask, 0));
    EXPECT_FALSE(CPLMaskGet(&mask, 1));
    EXPECT_TRUE(CPLMaskGet(&mask, 2));
  }
}

TEST(GdalWarperTest, NoDataMaskerFloat64) {
  // Non-zero imaginary component.
  {
    double nodata_imag[2] = {0.0, 1.0};
    double img[2] = {0.0, 1.0};
    GByte* p_img = reinterpret_cast<GByte*>(img);
    GUInt32 mask = 0xFFFFFFFF;
    int all_valid = FALSE;
    EXPECT_EQ(CE_None,
              GDALWarpNoDataMasker(nodata_imag, 1, GDT_Float64, 0, 0, 2, 1,
                                   &p_img, 0, &mask, &all_valid));
    EXPECT_EQ(TRUE, all_valid);
  }

  // Normal double comparison.
  {
    double nodata[2] = {-999.0, 0.0};
    double img[3] = {1.0, -999.0, 2.0};
    GByte* p_img = reinterpret_cast<GByte*>(img);
    GUInt32 mask = 0xFFFFFFFF;
    int all_valid = TRUE;
    EXPECT_EQ(CE_None, GDALWarpNoDataMasker(nodata, 1, GDT_Float64, 0, 0, 3, 1,
                                            &p_img, 0, &mask, &all_valid));
    EXPECT_EQ(FALSE, all_valid);
    EXPECT_TRUE(CPLMaskGet(&mask, 0));
    EXPECT_FALSE(CPLMaskGet(&mask, 1));
    EXPECT_TRUE(CPLMaskGet(&mask, 2));
  }

  // NaN double comparison.
  {
    double nodata_nan[2] = {std::numeric_limits<double>::quiet_NaN(), 0.0};
    double img[3] = {1.0, std::numeric_limits<double>::quiet_NaN(), 2.0};
    GByte* p_img = reinterpret_cast<GByte*>(img);
    GUInt32 mask = 0xFFFFFFFF;
    int all_valid = TRUE;
    EXPECT_EQ(CE_None, GDALWarpNoDataMasker(nodata_nan, 1, GDT_Float64, 0, 0, 3,
                                            1, &p_img, 0, &mask, &all_valid));
    EXPECT_EQ(FALSE, all_valid);
    EXPECT_TRUE(CPLMaskGet(&mask, 0));
    EXPECT_FALSE(CPLMaskGet(&mask, 1));
    EXPECT_TRUE(CPLMaskGet(&mask, 2));
  }
}

TEST(GdalWarperTest, NoDataMaskerDefaultInt32) {
  // Uses GDALCopyWords path with GDT_CFloat64 buffer.
  double nodata[2] = {-12345.0, 0.0};
  int32_t img[6] = {10, -12345, 30, -12345, 50, 60};
  GByte* p_img = reinterpret_cast<GByte*>(img);
  GUInt32 mask = 0xFFFFFFFF;
  int all_valid = TRUE;
  EXPECT_EQ(CE_None, GDALWarpNoDataMasker(nodata, 1, GDT_Int32, 0, 0, 3, 2,
                                          &p_img, 0, &mask, &all_valid));
  EXPECT_EQ(FALSE, all_valid);
  EXPECT_TRUE(CPLMaskGet(&mask, 0));
  EXPECT_FALSE(CPLMaskGet(&mask, 1));
  EXPECT_TRUE(CPLMaskGet(&mask, 2));
  EXPECT_FALSE(CPLMaskGet(&mask, 3));
  EXPECT_TRUE(CPLMaskGet(&mask, 4));
  EXPECT_TRUE(CPLMaskGet(&mask, 5));

  // NaN nodata in default path.
  double nodata_nan[2] = {std::numeric_limits<double>::quiet_NaN(), 0.0};
  mask = 0xFFFFFFFF;
  all_valid = FALSE;
  EXPECT_EQ(CE_None, GDALWarpNoDataMasker(nodata_nan, 1, GDT_Int32, 0, 0, 3, 2,
                                          &p_img, 0, &mask, &all_valid));
  EXPECT_EQ(TRUE, all_valid);  // No NaN in int32 data, so all remain valid.
}

// =============================================================================
// GDALWarpSrcAlphaMasker Tests
// =============================================================================

TEST(GdalWarperTest, SrcAlphaMaskerInvalidBand) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);

  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  options->hSrcDS = src_ds.get();
  options->nSrcAlphaBand = 99;  // Non-existent band

  std::vector<float> mask(16, 0.0f);
  int all_opaque = FALSE;
  EXPECT_EQ(CE_Failure, GDALWarpSrcAlphaMasker(
                            options.get(), 1, GDT_Byte, 0, 0, 4, 4, nullptr,
                            /*bMaskIsFloat=*/1, mask.data(), &all_opaque));
}

TEST(GdalWarperTest, SrcAlphaMaskerByte) {
  // 6x6 dataset (36 pixels) exercises SSE chunk loop (24 pixels) and remainder.
  GDALDatasetUniquePtr src_ds = CreateMemDataset(6, 6, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);

  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  options->hSrcDS = src_ds.get();
  options->nSrcAlphaBand = 1;

  // 1. All opaque pixels (255).
  std::vector<uint8_t> all_255(36, 255);
  FillByteBand(src_ds.get(), 1, all_255);
  std::vector<float> mask(36, 0.0f);
  int all_opaque = FALSE;
  EXPECT_EQ(CE_None,
            GDALWarpSrcAlphaMasker(options.get(), 1, GDT_Byte, 0, 0, 6, 6,
                                   nullptr, 1, mask.data(), &all_opaque));
  EXPECT_EQ(TRUE, all_opaque);
  for (float v : mask) {
    EXPECT_FLOAT_EQ(1.0f, v);
  }

  // 2. Partially opaque pixels.
  std::vector<uint8_t> mixed(36, 255);
  mixed[0] = 0;
  mixed[25] = 128;
  FillByteBand(src_ds.get(), 1, mixed);
  all_opaque = TRUE;
  EXPECT_EQ(CE_None,
            GDALWarpSrcAlphaMasker(options.get(), 1, GDT_Byte, 0, 0, 6, 6,
                                   nullptr, 1, mask.data(), &all_opaque));
  EXPECT_EQ(FALSE, all_opaque);
  EXPECT_FLOAT_EQ(0.0f, mask[0]);
  EXPECT_NEAR(128.0f / 255.0f, mask[25], 1e-4f);

  // 3. Custom SRC_ALPHA_MAX option.
  options->papszWarpOptions =
      CSLSetNameValue(options->papszWarpOptions, "SRC_ALPHA_MAX", "100");
  std::vector<uint8_t> vals(36, 50);
  FillByteBand(src_ds.get(), 1, vals);
  EXPECT_EQ(CE_None,
            GDALWarpSrcAlphaMasker(options.get(), 1, GDT_Byte, 0, 0, 6, 6,
                                   nullptr, 1, mask.data(), &all_opaque));
  EXPECT_NEAR(0.5f, mask[0], 1e-4f);
}

TEST(GdalWarperTest, SrcAlphaMaskerUInt16) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(6, 6, 1, GDT_UInt16);
  ASSERT_NE(nullptr, src_ds);

  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  options->hSrcDS = src_ds.get();
  options->nSrcAlphaBand = 1;
  options->papszWarpOptions =
      CSLSetNameValue(options->papszWarpOptions, "SRC_ALPHA_MAX", "65535");

  std::vector<double> vals(36, 65535.0);
  vals[10] = 32767.0;
  FillBand(src_ds.get(), 1, vals);

  std::vector<float> mask(36, 0.0f);
  int all_opaque = TRUE;
  EXPECT_EQ(CE_None,
            GDALWarpSrcAlphaMasker(options.get(), 1, GDT_UInt16, 0, 0, 6, 6,
                                   nullptr, 1, mask.data(), &all_opaque));
  EXPECT_EQ(FALSE, all_opaque);
  EXPECT_FLOAT_EQ(1.0f, mask[0]);
  EXPECT_NEAR(32767.0f / 65535.0f, mask[10], 1e-4f);
}

TEST(GdalWarperTest, SrcAlphaMaskerFloat32) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(6, 6, 1, GDT_Float32);
  ASSERT_NE(nullptr, src_ds);

  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  options->hSrcDS = src_ds.get();
  options->nSrcAlphaBand = 1;
  options->papszWarpOptions =
      CSLSetNameValue(options->papszWarpOptions, "SRC_ALPHA_MAX", "1.0");

  std::vector<double> vals(36, 1.0);
  vals[5] = 0.5;
  FillBand(src_ds.get(), 1, vals);

  std::vector<float> mask(36, 0.0f);
  int all_opaque = TRUE;
  EXPECT_EQ(CE_None,
            GDALWarpSrcAlphaMasker(options.get(), 1, GDT_Float32, 0, 0, 6, 6,
                                   nullptr, 1, mask.data(), &all_opaque));
  EXPECT_EQ(FALSE, all_opaque);
  EXPECT_FLOAT_EQ(1.0f, mask[0]);
  EXPECT_FLOAT_EQ(0.5f, mask[5]);
}

// =============================================================================
// GDALWarpSrcMaskMasker Tests
// =============================================================================

TEST(GdalWarperTest, SrcMaskMaskerBasic) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  GDALRasterBand* band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  EXPECT_EQ(CE_None, band->SetNoDataValue(0));

  std::vector<uint8_t> data(16, 10);
  data[0] = 0;  // Nodata -> mask pixel will be 0
  data[5] = 0;
  FillByteBand(src_ds.get(), 1, data);

  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  options->hSrcDS = src_ds.get();
  options->nBandCount = 1;
  options->panSrcBands = static_cast<int*>(CPLMalloc(sizeof(int)));
  options->panSrcBands[0] = 1;

  GUInt32 panMask = 0xFFFFFFFF;
  EXPECT_EQ(CE_None,
            GDALWarpSrcMaskMasker(options.get(), 1, GDT_Byte, 0, 0, 4, 4,
                                  nullptr, /*bMaskIsFloat=*/0, &panMask));
  EXPECT_FALSE(CPLMaskGet(&panMask, 0));
  EXPECT_TRUE(CPLMaskGet(&panMask, 1));
  EXPECT_FALSE(CPLMaskGet(&panMask, 5));
}

// =============================================================================
// GDALWarpDstAlphaMasker Tests
// =============================================================================

TEST(GdalWarperTest, DstAlphaMaskerInvalidBand) {
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, dst_ds);

  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  options->hDstDS = dst_ds.get();
  options->nDstAlphaBand = 99;

  std::vector<float> mask(16, 0.0f);
  EXPECT_EQ(CE_Failure, GDALWarpDstAlphaMasker(options.get(), 1, GDT_Byte, 0, 0,
                                               4, 4, nullptr, 1, mask.data()));
}

TEST(GdalWarperTest, DstAlphaMaskerInitDest) {
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, dst_ds);

  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  options->hDstDS = dst_ds.get();
  options->nDstAlphaBand = 1;
  options->papszWarpOptions =
      CSLSetNameValue(options->papszWarpOptions, "INIT_DEST", "0");

  std::vector<float> mask(16, 1.0f);
  EXPECT_EQ(CE_None,
            GDALWarpDstAlphaMasker(options.get(), /*nBandCount=*/1, GDT_Byte, 0,
                                   0, 4, 4, nullptr, 1, mask.data()));
  for (float v : mask) {
    EXPECT_FLOAT_EQ(0.0f, v);
  }
}

TEST(GdalWarperTest, DstAlphaMaskerReadByteAndUInt16) {
  // Byte reading (36 pixels to exercise SSE 32-pixel chunk + remainder).
  {
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(6, 6, 1, GDT_Byte);
    ASSERT_NE(nullptr, dst_ds);
    std::vector<uint8_t> vals(36, 255);
    vals[0] = 0;
    vals[33] = 128;
    FillByteBand(dst_ds.get(), 1, vals);

    WarpOptionsUniquePtr options(GDALCreateWarpOptions());
    options->hDstDS = dst_ds.get();
    options->nDstAlphaBand = 1;

    std::vector<float> mask(36, 0.0f);
    EXPECT_EQ(CE_None, GDALWarpDstAlphaMasker(options.get(), 1, GDT_Byte, 0, 0,
                                              6, 6, nullptr, 1, mask.data()));
    EXPECT_FLOAT_EQ(0.0f, mask[0]);
    EXPECT_FLOAT_EQ(1.0f, mask[1]);
    EXPECT_NEAR(128.0f / 255.0f, mask[33], 1e-4f);
  }

  // UInt16 reading.
  {
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(6, 6, 1, GDT_UInt16);
    ASSERT_NE(nullptr, dst_ds);
    std::vector<double> vals(36, 65535.0);
    vals[34] = 32767.0;
    FillBand(dst_ds.get(), 1, vals);

    WarpOptionsUniquePtr options(GDALCreateWarpOptions());
    options->hDstDS = dst_ds.get();
    options->nDstAlphaBand = 1;
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "DST_ALPHA_MAX", "65535");

    std::vector<float> mask(36, 0.0f);
    EXPECT_EQ(CE_None,
              GDALWarpDstAlphaMasker(options.get(), 1, GDT_UInt16, 0, 0, 6, 6,
                                     nullptr, 1, mask.data()));
    EXPECT_FLOAT_EQ(1.0f, mask[0]);
    EXPECT_NEAR(32767.0f / 65535.0f, mask[34], 1e-4f);
  }
}

TEST(GdalWarperTest, DstAlphaMaskerReadFloat32) {
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(6, 6, 1, GDT_Float32);
  ASSERT_NE(nullptr, dst_ds);
  std::vector<double> vals(36, 1.0);
  vals[10] = 0.5;
  FillBand(dst_ds.get(), 1, vals);

  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  options->hDstDS = dst_ds.get();
  options->nDstAlphaBand = 1;
  options->papszWarpOptions =
      CSLSetNameValue(options->papszWarpOptions, "DST_ALPHA_MAX", "1.0");

  std::vector<float> mask(36, 0.0f);
  EXPECT_EQ(CE_None, GDALWarpDstAlphaMasker(options.get(), 1, GDT_Float32, 0, 0,
                                            6, 6, nullptr, 1, mask.data()));
  EXPECT_FLOAT_EQ(1.0f, mask[0]);
  EXPECT_FLOAT_EQ(0.5f, mask[10]);
}

TEST(GdalWarperTest, DstAlphaMaskerWriteByteAndInt16) {
  // Byte writing (nBandCount < 0 triggers write mode).
  {
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(6, 6, 1, GDT_Byte);
    ASSERT_NE(nullptr, dst_ds);

    WarpOptionsUniquePtr options(GDALCreateWarpOptions());
    options->hDstDS = dst_ds.get();
    options->nDstAlphaBand = 1;

    std::vector<float> mask(36, 1.0f);
    mask[0] = 0.0f;
    mask[33] = 0.5f;

    EXPECT_EQ(CE_None,
              GDALWarpDstAlphaMasker(options.get(), /*nBandCount=*/-1, GDT_Byte,
                                     0, 0, 6, 6, nullptr, 1, mask.data()));

    GDALRasterBand* band = dst_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    std::vector<uint8_t> read_back(36, 0);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 6, 6, read_back.data(), 6,
                                      6, GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(0, read_back[0]);
    EXPECT_EQ(255, read_back[1]);
    EXPECT_EQ(127, read_back[33]);
  }

  // Int16 writing.
  {
    GDALDatasetUniquePtr dst_ds = CreateMemDataset(6, 6, 1, GDT_Int16);
    ASSERT_NE(nullptr, dst_ds);

    WarpOptionsUniquePtr options(GDALCreateWarpOptions());
    options->hDstDS = dst_ds.get();
    options->nDstAlphaBand = 1;
    options->papszWarpOptions =
        CSLSetNameValue(options->papszWarpOptions, "DST_ALPHA_MAX", "32767");

    std::vector<float> mask(36, 1.0f);
    mask[5] = 0.5f;

    EXPECT_EQ(CE_None,
              GDALWarpDstAlphaMasker(options.get(), -1, GDT_Int16, 0, 0, 6, 6,
                                     nullptr, 1, mask.data()));
    GDALRasterBand* band = dst_ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    std::vector<int16_t> read_back(36, 0);
    EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 6, 6, read_back.data(), 6,
                                      6, GDT_Int16, 0, 0, nullptr));
    EXPECT_EQ(32767, read_back[0]);
    EXPECT_EQ(16383, read_back[5]);
  }
}

TEST(GdalWarperTest, DstAlphaMaskerWriteFloat32) {
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(6, 6, 1, GDT_Float32);
  ASSERT_NE(nullptr, dst_ds);

  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  options->hDstDS = dst_ds.get();
  options->nDstAlphaBand = 1;
  // GDALWarpDstAlphaMasker truncates to int before writing float.
  options->papszWarpOptions =
      CSLSetNameValue(options->papszWarpOptions, "DST_ALPHA_MAX", "100.0");

  std::vector<float> mask(36, 1.0f);
  mask[0] = 0.0f;
  mask[5] = 0.5f;

  EXPECT_EQ(CE_None, GDALWarpDstAlphaMasker(options.get(), -1, GDT_Float32, 0,
                                            0, 6, 6, nullptr, 1, mask.data()));
  GDALRasterBand* band = dst_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  std::vector<float> read_back(36, 0.0f);
  EXPECT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 6, 6, read_back.data(), 6, 6,
                                    GDT_Float32, 0, 0, nullptr));
  EXPECT_FLOAT_EQ(0.0f, read_back[0]);
  EXPECT_FLOAT_EQ(100.0f, read_back[1]);
  EXPECT_FLOAT_EQ(50.0f, read_back[5]);
}

// =============================================================================
// GDALSerializeWarpOptions Tests
// =============================================================================

TEST(GdalWarperTest, SerializeWarpOptionsBasic) {
  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  options->dfWarpMemoryLimit = 50000000.0;
  options->eResampleAlg = GRA_Bilinear;
  options->eWorkingDataType = GDT_Float32;

  CPLXMLTreeCloser tree(GDALSerializeWarpOptions(options.get()));
  ASSERT_NE(nullptr, tree.get());

  char* xml_str = CPLSerializeXMLTree(tree.get());
  ASSERT_NE(nullptr, xml_str);
  absl::Cleanup xml_cleanup = [xml_str] { CPLFree(xml_str); };

  EXPECT_THAT(xml_str, HasSubstr("<GDALWarpOptions>"));
  EXPECT_THAT(xml_str, HasSubstr("<WarpMemoryLimit>5e+07</WarpMemoryLimit>"));
  EXPECT_THAT(xml_str, HasSubstr("<ResampleAlg>Bilinear</ResampleAlg>"));
  EXPECT_THAT(xml_str, HasSubstr("<WorkingDataType>Float32</WorkingDataType>"));
}

TEST(GdalWarperTest, SerializeWarpOptionsAllResampleAlgs) {
  struct TestCase {
    GDALResampleAlg alg;
    std::string_view expected_str;
  };
  constexpr TestCase kCases[] = {
      {.alg = GRA_NearestNeighbour, .expected_str = "NearestNeighbour"},
      {.alg = GRA_Bilinear, .expected_str = "Bilinear"},
      {.alg = GRA_Cubic, .expected_str = "Cubic"},
      {.alg = GRA_CubicSpline, .expected_str = "CubicSpline"},
      {.alg = GRA_Lanczos, .expected_str = "Lanczos"},
      {.alg = GRA_Average, .expected_str = "Average"},
      {.alg = GRA_RMS, .expected_str = "RootMeanSquare"},
      {.alg = GRA_Mode, .expected_str = "Mode"},
      {.alg = GRA_Max, .expected_str = "Maximum"},
      {.alg = GRA_Min, .expected_str = "Minimum"},
      {.alg = GRA_Med, .expected_str = "Median"},
      {.alg = GRA_Q1, .expected_str = "Quartile1"},
      {.alg = GRA_Q3, .expected_str = "Quartile3"},
      {.alg = GRA_Sum, .expected_str = "Sum"},
      {.alg = static_cast<GDALResampleAlg>(15), .expected_str = "Unknown"},
  };

  for (const auto& tc : kCases) {
    WarpOptionsUniquePtr options(GDALCreateWarpOptions());
    options->eResampleAlg = tc.alg;
    CPLXMLTreeCloser tree(GDALSerializeWarpOptions(options.get()));
    ASSERT_NE(nullptr, tree.get());
    const char* val = CPLGetXMLValue(tree.get(), "ResampleAlg", nullptr);
    ASSERT_NE(nullptr, val);
    EXPECT_EQ(tc.expected_str, val);
  }
}

TEST(GdalWarperTest, SerializeWarpOptionsAdvancedFields) {
  GDALRegister_GTiff();
  const std::string dir = "/vsimem/gdalwarper_test/adv";
  const std::string src_path = dir + "/src.tif";
  const std::string dst_path = dir + "/dst.tif";
  absl::Cleanup dir_cleanup = [dir] { VSIRmdirRecursive(dir.c_str()); };

  GDALDriver* gtiff_driver = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);
  {
    GDALDatasetUniquePtr src_ds_create(
        gtiff_driver->Create(src_path.c_str(), 4, 4, 3, GDT_Byte, nullptr));
    GDALDatasetUniquePtr dst_ds_create(
        gtiff_driver->Create(dst_path.c_str(), 4, 4, 3, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, src_ds_create);
    ASSERT_NE(nullptr, dst_ds_create);
    SetWgs84GeoTransform(src_ds_create.get());
    SetWgs84GeoTransform(dst_ds_create.get());
  }
  GDALDatasetUniquePtr src_ds(
      GDALDataset::Open(src_path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  GDALDatasetUniquePtr dst_ds(
      GDALDataset::Open(dst_path.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, src_ds);
  ASSERT_NE(nullptr, dst_ds);

  void* hTransformArg = GDALCreateGenImgProjTransformer(
      src_ds.get(), nullptr, dst_ds.get(), nullptr, TRUE, 1000.0, 0);
  ASSERT_NE(nullptr, hTransformArg);
  absl::Cleanup transform_cleanup = [hTransformArg] {
    GDALDestroyGenImgProjTransformer(hTransformArg);
  };

  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  options->hSrcDS = src_ds.get();
  options->hDstDS = dst_ds.get();
  options->pfnTransformer = GDALGenImgProjTransform;
  options->pTransformerArg = hTransformArg;
  options->papszWarpOptions =
      CSLSetNameValue(options->papszWarpOptions, "INIT_DEST", "0");
  // Internal option EXTRA_ELTS and CUTLINE option should not be serialized
  options->papszWarpOptions =
      CSLSetNameValue(options->papszWarpOptions, "EXTRA_ELTS", "1");
  options->papszWarpOptions =
      CSLSetNameValue(options->papszWarpOptions, "CUTLINE", "POLYGON(...)");

  options->nBandCount = 3;
  options->panSrcBands = static_cast<int*>(CPLMalloc(3 * sizeof(int)));
  options->panSrcBands[0] = 1;
  options->panSrcBands[1] = 2;
  options->panSrcBands[2] = 3;
  options->panDstBands = static_cast<int*>(CPLMalloc(3 * sizeof(int)));
  options->panDstBands[0] = 1;
  options->panDstBands[1] = 2;
  options->panDstBands[2] = 3;

  options->padfSrcNoDataReal =
      static_cast<double*>(CPLMalloc(3 * sizeof(double)));
  options->padfSrcNoDataReal[0] = -999.0;
  options->padfSrcNoDataReal[1] = 0.0;
  options->padfSrcNoDataReal[2] = 123.0;

  options->padfSrcNoDataImag =
      static_cast<double*>(CPLMalloc(3 * sizeof(double)));
  options->padfSrcNoDataImag[0] = 1.0;
  options->padfSrcNoDataImag[1] = std::numeric_limits<double>::quiet_NaN();
  options->padfSrcNoDataImag[2] = 0.0;

  options->padfDstNoDataReal =
      static_cast<double*>(CPLMalloc(3 * sizeof(double)));
  options->padfDstNoDataReal[0] = -888.0;
  options->padfDstNoDataReal[1] = 0.0;
  options->padfDstNoDataReal[2] = 456.0;

  options->padfDstNoDataImag =
      static_cast<double*>(CPLMalloc(3 * sizeof(double)));
  options->padfDstNoDataImag[0] = 2.0;
  options->padfDstNoDataImag[1] = std::numeric_limits<double>::quiet_NaN();
  options->padfDstNoDataImag[2] = 0.0;

  options->nSrcAlphaBand = 2;
  options->nDstAlphaBand = 2;
  options->dfCutlineBlendDist = 5.5;

  OGRGeometryH hPolygon = OGR_G_CreateGeometry(wkbPolygon);
  OGRGeometryH hRing = OGR_G_CreateGeometry(wkbLinearRing);
  OGR_G_AddPoint_2D(hRing, 0, 0);
  OGR_G_AddPoint_2D(hRing, 0, 1);
  OGR_G_AddPoint_2D(hRing, 1, 1);
  OGR_G_AddPoint_2D(hRing, 1, 0);
  OGR_G_AddPoint_2D(hRing, 0, 0);
  OGR_G_AddGeometryDirectly(hPolygon, hRing);
  options->hCutline = hPolygon;

  CPLXMLTreeCloser tree(GDALSerializeWarpOptions(options.get()));
  ASSERT_NE(nullptr, tree.get());

  char* xml_str = CPLSerializeXMLTree(tree.get());
  ASSERT_NE(nullptr, xml_str);
  absl::Cleanup xml_cleanup = [xml_str] { CPLFree(xml_str); };

  EXPECT_THAT(xml_str, HasSubstr(src_path));
  EXPECT_THAT(xml_str, HasSubstr(dst_path));
  EXPECT_THAT(xml_str, HasSubstr("<Option name=\"INIT_DEST\">0</Option>"));
  EXPECT_THAT(xml_str, Not(HasSubstr("EXTRA_ELTS")));
  EXPECT_THAT(xml_str, HasSubstr("<Transformer>"));
  EXPECT_THAT(xml_str, HasSubstr("<SrcAlphaBand>2</SrcAlphaBand>"));
  EXPECT_THAT(xml_str, HasSubstr("<DstAlphaBand>2</DstAlphaBand>"));
  EXPECT_THAT(xml_str, HasSubstr("<CutlineBlendDist>5.5</CutlineBlendDist>"));
  EXPECT_THAT(xml_str,
              HasSubstr("<Cutline>POLYGON ((0 0,0 1,1 1,1 0,0 0))</Cutline>"));
  EXPECT_THAT(xml_str, HasSubstr("<SrcNoDataImag>nan</SrcNoDataImag>"));
  EXPECT_THAT(xml_str, HasSubstr("<DstNoDataImag>nan</DstNoDataImag>"));
  EXPECT_THAT(xml_str, HasSubstr("<SrcNoDataImag>0</SrcNoDataImag>"));
  EXPECT_THAT(xml_str, HasSubstr("<DstNoDataImag>0</DstNoDataImag>"));

  // Release the destination dataset handle before deserializing so that
  // GDALDeserializeWarpOptions can re-open dst.tif without concurrent update
  // handles.
  options->hDstDS = nullptr;
  dst_ds.reset();

  // Verify deserialization round-trip.
  WarpOptionsUniquePtr deserialized(GDALDeserializeWarpOptions(tree.get()));
  ASSERT_NE(nullptr, deserialized);
  EXPECT_EQ(3, deserialized->nBandCount);
  EXPECT_EQ(2, deserialized->nSrcAlphaBand);
  EXPECT_EQ(2, deserialized->nDstAlphaBand);
  EXPECT_DOUBLE_EQ(5.5, deserialized->dfCutlineBlendDist);
  EXPECT_NE(nullptr, deserialized->hCutline);
  if (deserialized->hSrcDS != nullptr) {
    GDALClose(deserialized->hSrcDS);
    deserialized->hSrcDS = nullptr;
  }
  if (deserialized->hDstDS != nullptr) {
    GDALClose(deserialized->hDstDS);
    deserialized->hDstDS = nullptr;
  }
  if (deserialized->pTransformerArg != nullptr) {
    GDALDestroyTransformer(deserialized->pTransformerArg);
    deserialized->pTransformerArg = nullptr;
  }
}

// =============================================================================
// GDALDeserializeWarpOptions Tests
// =============================================================================

TEST(GdalWarperTest, DeserializeWarpOptionsErrors) {
  // 1. nullptr node.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALDeserializeWarpOptions(nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. Wrong node type (not CXT_Element).
  {
    CPLXMLTreeCloser node(
        CPLCreateXMLNode(nullptr, CXT_Text, "GDALWarpOptions"));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALDeserializeWarpOptions(node.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 3. Wrong element name.
  {
    CPLXMLTreeCloser node(
        CPLCreateXMLNode(nullptr, CXT_Element, "NotWarpOptions"));
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALDeserializeWarpOptions(node.get()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 4. Unrecognized ResampleAlg.
  {
    const char* xml =
        "<GDALWarpOptions>"
        "  <ResampleAlg>InvalidAlg</ResampleAlg>"
        "</GDALWarpOptions>";
    CPLXMLTreeCloser tree(CPLParseXMLString(xml));
    ASSERT_NE(nullptr, tree.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    WarpOptionsUniquePtr options(GDALDeserializeWarpOptions(tree.get()));
    EXPECT_EQ(nullptr, options);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("InvalidAlg"));
  }
}

TEST(GdalWarperTest, DeserializeWarpOptionsAllResampleAlgs) {
  struct TestCase {
    std::string_view alg_str;
    GDALResampleAlg expected_alg;
  };
  constexpr TestCase kCases[] = {
      {.alg_str = "NearestNeighbour", .expected_alg = GRA_NearestNeighbour},
      {.alg_str = "Bilinear", .expected_alg = GRA_Bilinear},
      {.alg_str = "Cubic", .expected_alg = GRA_Cubic},
      {.alg_str = "CubicSpline", .expected_alg = GRA_CubicSpline},
      {.alg_str = "Lanczos", .expected_alg = GRA_Lanczos},
      {.alg_str = "Average", .expected_alg = GRA_Average},
      {.alg_str = "RootMeanSquare", .expected_alg = GRA_RMS},
      {.alg_str = "Mode", .expected_alg = GRA_Mode},
      {.alg_str = "Maximum", .expected_alg = GRA_Max},
      {.alg_str = "Minimum", .expected_alg = GRA_Min},
      {.alg_str = "Median", .expected_alg = GRA_Med},
      {.alg_str = "Quartile1", .expected_alg = GRA_Q1},
      {.alg_str = "Quartile3", .expected_alg = GRA_Q3},
      {.alg_str = "Sum", .expected_alg = GRA_Sum},
      {.alg_str = "Default", .expected_alg = GRA_NearestNeighbour},
  };

  for (const auto& tc : kCases) {
    std::string xml = std::string("<GDALWarpOptions><ResampleAlg>") +
                      std::string(tc.alg_str) +
                      "</ResampleAlg></GDALWarpOptions>";
    CPLXMLTreeCloser tree(CPLParseXMLString(xml.c_str()));
    ASSERT_NE(nullptr, tree.get());
    WarpOptionsUniquePtr options(GDALDeserializeWarpOptions(tree.get()));
    ASSERT_NE(nullptr, options);
    EXPECT_EQ(tc.expected_alg, options->eResampleAlg);
  }
}

TEST(GdalWarperTest, DeserializeWarpOptionsFloatNormalization) {
  // Tests NormalizeValue lambda handling of float limits.
  std::string lowest_float = CPLString().Printf(
      "%.16g", static_cast<double>(std::numeric_limits<float>::lowest()));
  std::string max_float = CPLString().Printf(
      "%.16g", static_cast<double>(std::numeric_limits<float>::max()));

  std::string xml =
      "<GDALWarpOptions>"
      "  <WorkingDataType>Float32</WorkingDataType>"
      "  <BandList>"
      "    <BandMapping src='1' dst='2'>"
      "      <SrcNoDataReal>" +
      lowest_float +
      "</SrcNoDataReal>"
      "      <SrcNoDataImag>1.25</SrcNoDataImag>"
      "      <DstNoDataReal>" +
      max_float +
      "</DstNoDataReal>"
      "      <DstNoDataImag>2.5</DstNoDataImag>"
      "    </BandMapping>"
      "  </BandList>"
      "</GDALWarpOptions>";

  CPLXMLTreeCloser tree(CPLParseXMLString(xml.c_str()));
  ASSERT_NE(nullptr, tree.get());
  WarpOptionsUniquePtr options(GDALDeserializeWarpOptions(tree.get()));
  ASSERT_NE(nullptr, options);

  EXPECT_EQ(1, options->nBandCount);
  ASSERT_NE(nullptr, options->panSrcBands);
  EXPECT_EQ(1, options->panSrcBands[0]);
  ASSERT_NE(nullptr, options->panDstBands);
  EXPECT_EQ(2, options->panDstBands[0]);
  ASSERT_NE(nullptr, options->padfSrcNoDataReal);
  EXPECT_DOUBLE_EQ(static_cast<double>(std::numeric_limits<float>::lowest()),
                   options->padfSrcNoDataReal[0]);
  ASSERT_NE(nullptr, options->padfSrcNoDataImag);
  EXPECT_DOUBLE_EQ(1.25, options->padfSrcNoDataImag[0]);
  ASSERT_NE(nullptr, options->padfDstNoDataReal);
  EXPECT_DOUBLE_EQ(static_cast<double>(std::numeric_limits<float>::max()),
                   options->padfDstNoDataReal[0]);
  ASSERT_NE(nullptr, options->padfDstNoDataImag);
  EXPECT_DOUBLE_EQ(2.5, options->padfDstNoDataImag[0]);
}

TEST(GdalWarperTest, DeserializeWarpOptionsDatasetsAndGeoLoc) {
  GDALRegister_GTiff();
  const std::string dir = "/vsimem/gdalwarper_test/deserialize_datasets";
  const std::string src_path = dir + "/src.tif";
  const std::string dst_path = dir + "/dst.tif";
  absl::Cleanup dir_cleanup = [dir] { VSIRmdirRecursive(dir.c_str()); };

  GDALDriver* gtiff_driver = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);
  {
    GDALDatasetUniquePtr ds(
        gtiff_driver->Create(src_path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    SetWgs84GeoTransform(ds.get());
  }
  {
    GDALDatasetUniquePtr ds(
        gtiff_driver->Create(dst_path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    SetWgs84GeoTransform(ds.get());
  }

  std::string xml =
      "<GDALWarpOptions>"
      "  <SourceDataset>" +
      src_path +
      "</SourceDataset>"
      "  <DestinationDataset>" +
      dst_path +
      "</DestinationDataset>"
      "  <Transformer>"
      "    <GeoLocTransformer/>"
      "  </Transformer>"
      "  <Cutline>POLYGON((0 0,0 1,1 1,1 0,0 0))</Cutline>"
      "  <CutlineBlendDist>3.5</CutlineBlendDist>"
      "  <SrcAlphaBand>1</SrcAlphaBand>"
      "  <DstAlphaBand>1</DstAlphaBand>"
      "</GDALWarpOptions>";

  CPLXMLTreeCloser tree(CPLParseXMLString(xml.c_str()));
  ASSERT_NE(nullptr, tree.get());
  WarpOptionsUniquePtr options(GDALDeserializeWarpOptions(tree.get()));
  ASSERT_NE(nullptr, options);

  EXPECT_NE(nullptr, options->hSrcDS);
  EXPECT_NE(nullptr, options->hDstDS);
  EXPECT_NE(nullptr, options->hCutline);
  EXPECT_DOUBLE_EQ(3.5, options->dfCutlineBlendDist);
  EXPECT_EQ(1, options->nSrcAlphaBand);
  EXPECT_EQ(1, options->nDstAlphaBand);

  // Close opened datasets before destroying options.
  if (options->hSrcDS != nullptr) {
    GDALClose(options->hSrcDS);
    options->hSrcDS = nullptr;
  }
  if (options->hDstDS != nullptr) {
    GDALClose(options->hDstDS);
    options->hDstDS = nullptr;
  }
}

TEST(GdalWarperTest, DeserializeWarpOptionsErrorCleanup) {
  // When an error occurs during deserialization, cleanup branch destroys
  // opened datasets, transformer, and options, returning nullptr.
  GDALRegister_GTiff();
  const std::string dir = "/vsimem/gdalwarper_test/deserialize_error";
  const std::string src_path = dir + "/src.tif";
  const std::string dst_path = dir + "/dst.tif";
  absl::Cleanup dir_cleanup = [dir] { VSIRmdirRecursive(dir.c_str()); };

  GDALDriver* gtiff_driver = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_driver);
  {
    GDALDatasetUniquePtr ds(
        gtiff_driver->Create(src_path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    SetWgs84GeoTransform(ds.get());
  }
  {
    GDALDatasetUniquePtr ds(
        gtiff_driver->Create(dst_path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, ds);
    SetWgs84GeoTransform(ds.get());
  }

  std::string xml =
      "<GDALWarpOptions>"
      "  <SourceDataset>" +
      src_path +
      "</SourceDataset>"
      "  <DestinationDataset>" +
      dst_path +
      "</DestinationDataset>"
      "  <Transformer>"
      "    <InvalidTransformer/>"
      "  </Transformer>"
      "</GDALWarpOptions>";

  CPLXMLTreeCloser tree(CPLParseXMLString(xml.c_str()));
  ASSERT_NE(nullptr, tree.get());

  WithQuietHandler quiet;
  CPLErrorReset();
  WarpOptionsUniquePtr options(GDALDeserializeWarpOptions(tree.get()));
  EXPECT_EQ(nullptr, options);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

// =============================================================================
// GDALReprojectImage Tests
// =============================================================================

TEST(GdalWarperTest, ReprojectImageBasic) {
  constexpr int kSize = 4;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kSize, kSize, 1, GDT_Byte);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kSize, kSize, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  ASSERT_NE(nullptr, dst_ds);

  SetWgs84GeoTransform(src_ds.get());
  SetWgs84GeoTransform(dst_ds.get());

  std::vector<uint8_t> src_pixels = {10, 20,  30,  40,  50,  60,  70,  80,
                                     90, 100, 110, 120, 130, 140, 150, 160};
  FillByteBand(src_ds.get(), 1, src_pixels);

  EXPECT_EQ(CE_None, GDALReprojectImage(src_ds.get(), nullptr, dst_ds.get(),
                                        nullptr, GRA_NearestNeighbour, 0.0, 0.0,
                                        nullptr, nullptr, nullptr));

  std::vector<uint8_t> dst_pixels(16, 0);
  GDALRasterBand* dst_band = dst_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, dst_band);
  EXPECT_EQ(CE_None,
            dst_band->RasterIO(GF_Read, 0, 0, kSize, kSize, dst_pixels.data(),
                               kSize, kSize, GDT_Byte, 0, 0, nullptr));

  for (size_t i = 0; i < src_pixels.size(); ++i) {
    EXPECT_EQ(src_pixels[i], dst_pixels[i]);
  }
}

TEST(GdalWarperTest, ReprojectImageApproxTransformer) {
  // dfMaxError > 0 exercises GDALCreateApproxTransformer and its cleanup.
  constexpr int kSize = 4;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kSize, kSize, 1, GDT_Byte);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kSize, kSize, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  ASSERT_NE(nullptr, dst_ds);

  SetWgs84GeoTransform(src_ds.get());
  SetWgs84GeoTransform(dst_ds.get());

  std::vector<uint8_t> src_pixels(16, 42);
  FillByteBand(src_ds.get(), 1, src_pixels);

  EXPECT_EQ(CE_None,
            GDALReprojectImage(src_ds.get(), nullptr, dst_ds.get(), nullptr,
                               GRA_NearestNeighbour, 0.0, /*dfMaxError=*/0.25,
                               nullptr, nullptr, nullptr));

  std::vector<uint8_t> dst_pixels(16, 0);
  GDALRasterBand* dst_band = dst_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, dst_band);
  EXPECT_EQ(CE_None,
            dst_band->RasterIO(GF_Read, 0, 0, kSize, kSize, dst_pixels.data(),
                               kSize, kSize, GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(42, dst_pixels[0]);
}

TEST(GdalWarperTest, ReprojectImageWithOptionsAndAlpha) {
  constexpr int kSize = 4;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kSize, kSize, 2, GDT_Byte);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kSize, kSize, 2, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  ASSERT_NE(nullptr, dst_ds);

  SetWgs84GeoTransform(src_ds.get());
  SetWgs84GeoTransform(dst_ds.get());

  // Set band 2 as alpha on both source and destination datasets.
  GDALRasterBand* src_alpha = src_ds->GetRasterBand(2);
  ASSERT_NE(nullptr, src_alpha);
  EXPECT_EQ(CE_None, src_alpha->SetColorInterpretation(GCI_AlphaBand));

  GDALRasterBand* dst_alpha = dst_ds->GetRasterBand(2);
  ASSERT_NE(nullptr, dst_alpha);
  EXPECT_EQ(CE_None, dst_alpha->SetColorInterpretation(GCI_AlphaBand));

  std::vector<uint8_t> src_data(16, 100);
  std::vector<uint8_t> alpha_data(16, 255);
  FillByteBand(src_ds.get(), 1, src_data);
  FillByteBand(src_ds.get(), 2, alpha_data);

  WarpOptionsUniquePtr options(GDALCreateWarpOptions());
  options->papszWarpOptions =
      CSLSetNameValue(options->papszWarpOptions, "INIT_DEST", "0");

  EXPECT_EQ(CE_None, GDALReprojectImage(src_ds.get(), nullptr, dst_ds.get(),
                                        nullptr, GRA_Bilinear, 0.0, 0.0,
                                        nullptr, nullptr, options.get()));

  std::vector<uint8_t> dst_data(16, 0);
  GDALRasterBand* dst_band = dst_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, dst_band);
  EXPECT_EQ(CE_None,
            dst_band->RasterIO(GF_Read, 0, 0, kSize, kSize, dst_data.data(),
                               kSize, kSize, GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(100, dst_data[0]);
}

TEST(GdalWarperTest, ReprojectImageWithNoData) {
  constexpr int kSize = 4;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kSize, kSize, 1, GDT_Byte);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kSize, kSize, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  ASSERT_NE(nullptr, dst_ds);

  SetWgs84GeoTransform(src_ds.get());
  SetWgs84GeoTransform(dst_ds.get());

  GDALRasterBand* src_band = src_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, src_band);
  EXPECT_EQ(CE_None, src_band->SetNoDataValue(0.0));

  GDALRasterBand* dst_band = dst_ds->GetRasterBand(1);
  ASSERT_NE(nullptr, dst_band);
  EXPECT_EQ(CE_None, dst_band->SetNoDataValue(0.0));

  std::vector<uint8_t> src_data(16, 50);
  src_data[0] = 0;  // Nodata pixel
  FillByteBand(src_ds.get(), 1, src_data);

  EXPECT_EQ(CE_None, GDALReprojectImage(src_ds.get(), nullptr, dst_ds.get(),
                                        nullptr, GRA_NearestNeighbour, 0.0, 0.0,
                                        nullptr, nullptr, nullptr));

  std::vector<uint8_t> dst_data(16, 0);
  EXPECT_EQ(CE_None,
            dst_band->RasterIO(GF_Read, 0, 0, kSize, kSize, dst_data.data(),
                               kSize, kSize, GDT_Byte, 0, 0, nullptr));
  EXPECT_EQ(0, dst_data[0]);
  EXPECT_EQ(50, dst_data[1]);
}

TEST(GdalWarperTest, ReprojectImageProgressAndCancellation) {
  constexpr int kSize = 4;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kSize, kSize, 1, GDT_Byte);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kSize, kSize, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  ASSERT_NE(nullptr, dst_ds);

  SetWgs84GeoTransform(src_ds.get());
  SetWgs84GeoTransform(dst_ds.get());

  // 1. Successful progress callback.
  ProgressTracker tracker;
  EXPECT_EQ(CE_None, GDALReprojectImage(src_ds.get(), nullptr, dst_ds.get(),
                                        nullptr, GRA_NearestNeighbour, 0.0, 0.0,
                                        RecordingProgress, &tracker, nullptr));
  EXPECT_FALSE(tracker.progress_values.empty());

  // 2. Cancellation via progress returning FALSE.
  ProgressTracker fail_tracker;
  fail_tracker.fail_threshold = 0.0;  // Cancel immediately on first call
  EXPECT_EQ(CE_Failure,
            GDALReprojectImage(src_ds.get(), nullptr, dst_ds.get(), nullptr,
                               GRA_NearestNeighbour, 0.0, 0.0,
                               RecordingProgress, &fail_tracker, nullptr));
}

TEST(GdalWarperTest, ReprojectImageInvalidSrs) {
  constexpr int kSize = 4;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kSize, kSize, 1, GDT_Byte);
  GDALDatasetUniquePtr dst_ds = CreateMemDataset(kSize, kSize, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  ASSERT_NE(nullptr, dst_ds);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALReprojectImage(src_ds.get(), "INVALID_SRS_STRING", dst_ds.get(),
                               nullptr, GRA_NearestNeighbour, 0.0, 0.0, nullptr,
                               nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

// =============================================================================
// GDALCreateAndReprojectImage Tests
// =============================================================================

TEST(GdalWarperTest, CreateAndReprojectImageNullSrc) {
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALCreateAndReprojectImage(
                nullptr, nullptr, "/vsimem/out.tif", nullptr, nullptr, nullptr,
                GRA_NearestNeighbour, 0.0, 0.0, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST(GdalWarperTest, CreateAndReprojectImageMissingDriver) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  SetWgs84GeoTransform(src_ds.get());

  // Ensure GTiff driver is not registered for this test.
  GDALDriver* gtiff_driver = GetGDALDriverManager()->GetDriverByName("GTiff");
  if (gtiff_driver != nullptr) {
    GetGDALDriverManager()->DeregisterDriver(gtiff_driver);
  }
  absl::Cleanup reregister = [gtiff_driver] {
    if (gtiff_driver != nullptr) {
      GetGDALDriverManager()->RegisterDriver(gtiff_driver);
    }
  };

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALCreateAndReprojectImage(
                            src_ds.get(), nullptr, "/vsimem/out.tif", nullptr,
                            nullptr, nullptr, GRA_NearestNeighbour, 0.0, 0.0,
                            nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("needs GTiff driver"));
}

TEST(GdalWarperTest, CreateAndReprojectImageMemDriver) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  SetWgs84GeoTransform(src_ds.get());
  std::vector<uint8_t> data(16, 77);
  FillByteBand(src_ds.get(), 1, data);

  GDALDriverH mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  // Calling with pszSrcWKT == nullptr and pszDstWKT == nullptr defaults from
  // src_ds.
  EXPECT_EQ(CE_None, GDALCreateAndReprojectImage(src_ds.get(), nullptr, "",
                                                 nullptr, mem_driver, nullptr,
                                                 GRA_NearestNeighbour, 0.0, 0.0,
                                                 nullptr, nullptr, nullptr));
}

TEST(GdalWarperTest, CreateAndReprojectImageGTiffDriver) {
  GDALRegister_GTiff();
  const std::string dir = "/vsimem/gdalwarper_test/create_reproject";
  const std::string dst_path = dir + "/out.tif";
  absl::Cleanup dir_cleanup = [dir] { VSIRmdirRecursive(dir.c_str()); };

  GDALDatasetUniquePtr src_ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  SetWgs84GeoTransform(src_ds.get());
  std::vector<uint8_t> data(16, 88);
  FillByteBand(src_ds.get(), 1, data);

  // When GTiff is registered, passing hDstDriver == nullptr defaults to GTiff.
  EXPECT_EQ(CE_None, GDALCreateAndReprojectImage(
                         src_ds.get(), nullptr, dst_path.c_str(), nullptr,
                         nullptr, nullptr, GRA_NearestNeighbour, 0.0, 0.0,
                         nullptr, nullptr, nullptr));

  VSIStatBufL sStat;
  EXPECT_EQ(0, VSIStatL(dst_path.c_str(), &sStat));
}

TEST(GdalWarperTest, CreateAndReprojectImageInvalidWkt) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(4, 4, 1, GDT_Byte);
  ASSERT_NE(nullptr, src_ds);
  SetWgs84GeoTransform(src_ds.get());

  GDALDriverH mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure,
            GDALCreateAndReprojectImage(
                src_ds.get(), "INVALID_WKT", "", nullptr, mem_driver, nullptr,
                GRA_NearestNeighbour, 0.0, 0.0, nullptr, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered branches are OOM allocation guards
// and defensive error paths.
