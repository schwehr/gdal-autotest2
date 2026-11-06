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
// Tests GDAL warp kernel algorithms (alg/gdalwarpkernel.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalwarpkernel.cpp
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalwarper.h

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/alg/gdal_alg.h"
#include "third_party/gdal/alg/gdal_alg_priv.h"
#include "third_party/gdal/alg/gdalwarper.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_mask.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

template <typename T>
constexpr GDALDataType GetGDALDataType() {
  if constexpr (std::is_same_v<T, GByte>) {
    return GDT_Byte;
  } else if constexpr (std::is_same_v<T, GInt8>) {
    return GDT_Int8;
  } else if constexpr (std::is_same_v<T, GInt16>) {
    return GDT_Int16;
  } else if constexpr (std::is_same_v<T, GUInt16>) {
    return GDT_UInt16;
  } else if constexpr (std::is_same_v<T, GInt32>) {
    return GDT_Int32;
  } else if constexpr (std::is_same_v<T, GUInt32>) {
    return GDT_UInt32;
  } else if constexpr (std::is_same_v<T, std::int64_t>) {
    return GDT_Int64;
  } else if constexpr (std::is_same_v<T, std::uint64_t>) {
    return GDT_UInt64;
  } else if constexpr (std::is_same_v<T, float>) {
    return GDT_Float32;
  } else if constexpr (std::is_same_v<T, double>) {
    return GDT_Float64;
  } else if constexpr (std::is_same_v<T, std::complex<GInt16>>) {
    return GDT_CInt16;
  } else if constexpr (std::is_same_v<T, std::complex<GInt32>>) {
    return GDT_CInt32;
  } else if constexpr (std::is_same_v<T, std::complex<float>>) {
    return GDT_CFloat32;
  } else if constexpr (std::is_same_v<T, std::complex<double>>) {
    return GDT_CFloat64;
  } else {
    return GDT_Unknown;
  }
}

// Configurable transformer for warping tests.
struct TestTransformData {
  double scale_x = 1.0;
  double scale_y = 1.0;
  double offset_x = 0.0;
  double offset_y = 0.0;
  bool fail_corner = false;
  int fail_corner_x = -1;
  int fail_corner_y = -1;
  bool wrap_x = false;
  bool non_convex_quad = false;
  bool self_crossing_quad = false;
  double vertical_shift_z = 0.0;
  bool fail_all = false;
  bool fail_forward_x_ge_2 = false;
  bool fail_forward_y_ge_2 = false;
};

int TestTransformer(void* pTransformerArg, int bDstToSrc, int nPointCount,
                    double* x, double* y, double* z, int* panSuccess) {
  auto* data = static_cast<TestTransformData*>(pTransformerArg);
  for (int i = 0; i < nPointCount; ++i) {
    if (data != nullptr && data->fail_all) {
      panSuccess[i] = FALSE;
      continue;
    }
    panSuccess[i] = TRUE;
    if (data == nullptr) {
      continue;
    }

    if (bDstToSrc) {
      x[i] = (x[i] - data->offset_x) / data->scale_x;
      y[i] = (y[i] - data->offset_y) / data->scale_y;
      if (z != nullptr) {
        z[i] = data->vertical_shift_z;
      }
    } else {
      if (data->fail_corner && data->fail_corner_x >= 0 &&
          data->fail_corner_y >= 0) {
        if (std::abs(x[i] - data->fail_corner_x) < 1e-5 &&
            std::abs(y[i] - data->fail_corner_y) < 1e-5) {
          panSuccess[i] = FALSE;
          continue;
        }
      }

      if (data->fail_forward_x_ge_2 && x[i] >= 2.0) {
        panSuccess[i] = FALSE;
        continue;
      }
      if (data->fail_forward_y_ge_2 && y[i] >= 2.0) {
        panSuccess[i] = FALSE;
        continue;
      }

      if (data->wrap_x && x[i] >= 2.0) {
        x[i] = (x[i] * data->scale_x + data->offset_x) - 100.0;
      } else {
        x[i] = x[i] * data->scale_x + data->offset_x;
      }
      y[i] = y[i] * data->scale_y + data->offset_y;

      if (data->non_convex_quad && std::abs(x[i] - 1.0) < 1e-5 &&
          std::abs(y[i] - 1.0) < 1e-5) {
        x[i] = 0.3;
        y[i] = 0.3;
      } else if (data->self_crossing_quad && std::abs(x[i] - 1.0) < 1e-5 &&
                 std::abs(y[i] - 0.0) < 1e-5) {
        y[i] = 2.0;
      }

      if (z != nullptr) {
        z[i] = data->vertical_shift_z;
      }
    }
  }
  return TRUE;
}

// Test harness managing memory and settings for a GDALWarpKernel instance.
template <typename T>
class WarpKernelHarness {
 public:
  GDALWarpKernel kernel;
  CPLStringList options;
  TestTransformData transform_data;
  int src_w;
  int src_h;
  int dst_w;
  int dst_h;
  int bands;
  std::vector<std::vector<T>> src_bands;
  std::vector<std::vector<T>> dst_bands;
  std::vector<GByte*> src_ptrs;
  std::vector<GByte*> dst_ptrs;
  std::vector<GUInt32> unified_src_valid;
  std::vector<float> unified_src_density;
  std::vector<std::vector<GUInt32>> per_band_src_valid;
  std::vector<GUInt32*> per_band_src_valid_ptrs;
  std::vector<GUInt32> dst_valid;
  std::vector<float> dst_density;
  std::vector<double> dst_nodata;

  WarpKernelHarness(int sw, int sh, int dw, int dh, int b = 1,
                    GDALResampleAlg resample = GRA_NearestNeighbour)
      : src_w(sw), src_h(sh), dst_w(dw), dst_h(dh), bands(b) {
    kernel.nSrcXSize = src_w;
    kernel.nSrcYSize = src_h;
    kernel.nDstXSize = dst_w;
    kernel.nDstYSize = dst_h;
    kernel.nBands = bands;
    kernel.eWorkingDataType = GetGDALDataType<T>();
    kernel.eResample = resample;
    kernel.pfnTransformer = TestTransformer;
    kernel.pTransformerArg = &transform_data;

    transform_data.scale_x = static_cast<double>(dst_w) / src_w;
    transform_data.scale_y = static_cast<double>(dst_h) / src_h;

    // Allocate src buffers with extra elements padding for safety
    const size_t src_size =
        static_cast<size_t>(src_w) * src_h + WARP_EXTRA_ELTS + 16;
    src_bands.resize(bands, std::vector<T>(src_size, T{}));
    src_ptrs.resize(bands);
    for (int i = 0; i < bands; ++i) {
      src_ptrs[i] = reinterpret_cast<GByte*>(src_bands[i].data());
    }
    kernel.papabySrcImage = src_ptrs.data();

    // Allocate dst buffers
    const size_t dst_size = static_cast<size_t>(dst_w) * dst_h + 16;
    dst_bands.resize(bands, std::vector<T>(dst_size, T{}));
    dst_ptrs.resize(bands);
    for (int i = 0; i < bands; ++i) {
      dst_ptrs[i] = reinterpret_cast<GByte*>(dst_bands[i].data());
    }
    kernel.papabyDstImage = dst_ptrs.data();
  }

  void SetOption(const char* name, const char* value) {
    options.SetNameValue(name, value);
    kernel.papszWarpOptions = options.List();
  }

  void SetExtraEltsOption() {
    options.SetNameValue("EXTRA_ELTS", std::to_string(WARP_EXTRA_ELTS).c_str());
    kernel.papszWarpOptions = options.List();
  }

  void EnableUnifiedSrcValid(bool all_valid = true) {
    const size_t nMaskWords =
        (static_cast<size_t>(src_w) * src_h + WARP_EXTRA_ELTS + 47) / 32;
    unified_src_valid.assign(nMaskWords, all_valid ? 0xFFFFFFFFU : 0x0U);
    kernel.panUnifiedSrcValid = unified_src_valid.data();
  }

  void EnableUnifiedSrcDensity(float default_val = 1.0f) {
    const size_t nElts =
        static_cast<size_t>(src_w) * src_h + WARP_EXTRA_ELTS + 16;
    unified_src_density.assign(nElts, default_val);
    kernel.pafUnifiedSrcDensity = unified_src_density.data();
  }

  void EnablePerBandSrcValid(bool all_valid = true) {
    const size_t nMaskWords =
        (static_cast<size_t>(src_w) * src_h + WARP_EXTRA_ELTS + 47) / 32;
    per_band_src_valid.resize(
        bands,
        std::vector<GUInt32>(nMaskWords, all_valid ? 0xFFFFFFFFU : 0x0U));
    per_band_src_valid_ptrs.resize(bands);
    for (int i = 0; i < bands; ++i) {
      per_band_src_valid_ptrs[i] = per_band_src_valid[i].data();
    }
    kernel.papanBandSrcValid = per_band_src_valid_ptrs.data();
  }

  void EnableDstValid(bool all_valid = false) {
    const size_t nMaskWords = (static_cast<size_t>(dst_w) * dst_h + 47) / 32;
    dst_valid.assign(nMaskWords, all_valid ? 0xFFFFFFFFU : 0x0U);
    kernel.panDstValid = dst_valid.data();
  }

  void EnableDstDensity(float default_val = 0.0f) {
    const size_t nElts = static_cast<size_t>(dst_w) * dst_h + 16;
    dst_density.assign(nElts, default_val);
    kernel.pafDstDensity = dst_density.data();
  }

  void SetDstNoData(double val) {
    dst_nodata.assign(bands, val);
    kernel.padfDstNoDataReal = dst_nodata.data();
  }

  void SetSrcPixel(int band, int x, int y, T val) {
    src_bands[band][static_cast<size_t>(y) * src_w + x] = val;
  }

  T GetDstPixel(int band, int x, int y) const {
    return dst_bands[band][static_cast<size_t>(y) * dst_w + x];
  }

  CPLErr PerformWarp() { return kernel.PerformWarp(); }
};

// Helper for GenImgProjTransformer integration with MEM datasets.
struct GenImgProjTransformerHelper {
  GDALDatasetUniquePtr src_ds;
  GDALDatasetUniquePtr dst_ds;
  void* transform_arg = nullptr;

  GenImgProjTransformerHelper(int src_w, int src_h, int dst_w, int dst_h) {
    GDALRegister_MEM();
    GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
    if (mem_driver != nullptr) {
      src_ds.reset(mem_driver->Create("", src_w, src_h, 1, GDT_Byte, nullptr));
      dst_ds.reset(mem_driver->Create("", dst_w, dst_h, 1, GDT_Byte, nullptr));
      if (src_ds && dst_ds) {
        const double gt_src[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
        const double gt_dst[6] = {0.0, static_cast<double>(src_w) / dst_w,
                                  0.0, 10.0,
                                  0.0, -static_cast<double>(src_h) / dst_h};
        src_ds->SetGeoTransform(gt_src);
        dst_ds->SetGeoTransform(gt_dst);
        transform_arg = GDALCreateGenImgProjTransformer2(src_ds.get(),
                                                         dst_ds.get(), nullptr);
      }
    }
  }

  ~GenImgProjTransformerHelper() {
    if (transform_arg != nullptr) {
      GDALDestroyGenImgProjTransformer(transform_arg);
    }
  }
};

// ====================================================================
// Group 1: Filter Functions and Radii
// ====================================================================

TEST(GdalWarpKernelTest, FilterRadiusAndFunctions) {
  // Test GWKGetFilterRadius across all known resampling algorithms.
  EXPECT_EQ(0, GWKGetFilterRadius(GRA_NearestNeighbour));
  EXPECT_EQ(1, GWKGetFilterRadius(GRA_Bilinear));
  EXPECT_EQ(2, GWKGetFilterRadius(GRA_Cubic));
  EXPECT_EQ(2, GWKGetFilterRadius(GRA_CubicSpline));
  EXPECT_EQ(3, GWKGetFilterRadius(GRA_Lanczos));
  EXPECT_EQ(0, GWKGetFilterRadius(GRA_Average));
  EXPECT_EQ(0, GWKGetFilterRadius(GRA_Mode));
  EXPECT_EQ(0,
            GWKGetFilterRadius(static_cast<GDALResampleAlg>(7)));  // Reserved
  EXPECT_EQ(0, GWKGetFilterRadius(GRA_Max));
  EXPECT_EQ(0, GWKGetFilterRadius(GRA_Min));
  EXPECT_EQ(0, GWKGetFilterRadius(GRA_Med));
  EXPECT_EQ(0, GWKGetFilterRadius(GRA_Q1));
  EXPECT_EQ(0, GWKGetFilterRadius(GRA_Q3));
  EXPECT_EQ(0, GWKGetFilterRadius(GRA_Sum));
  EXPECT_EQ(0, GWKGetFilterRadius(GRA_RMS));

  // Test GWKGetFilterFunc.
  EXPECT_EQ(nullptr, GWKGetFilterFunc(GRA_NearestNeighbour));
  EXPECT_EQ(nullptr, GWKGetFilterFunc(GRA_Average));
  EXPECT_EQ(nullptr, GWKGetFilterFunc(GRA_Mode));
  EXPECT_EQ(nullptr, GWKGetFilterFunc(GRA_Sum));

  FilterFuncType pfnBilinear = GWKGetFilterFunc(GRA_Bilinear);
  ASSERT_NE(nullptr, pfnBilinear);
  EXPECT_DOUBLE_EQ(1.0, pfnBilinear(0.0));
  EXPECT_DOUBLE_EQ(0.5, pfnBilinear(0.5));
  EXPECT_DOUBLE_EQ(0.0, pfnBilinear(1.0));
  EXPECT_DOUBLE_EQ(0.0, pfnBilinear(1.5));

  FilterFuncType pfnCubic = GWKGetFilterFunc(GRA_Cubic);
  ASSERT_NE(nullptr, pfnCubic);
  EXPECT_DOUBLE_EQ(1.0, pfnCubic(0.0));
  EXPECT_NEAR(0.5625, pfnCubic(0.5), 1e-4);
  EXPECT_DOUBLE_EQ(0.0, pfnCubic(1.0));
  EXPECT_NEAR(-0.0625, pfnCubic(1.5), 1e-4);
  EXPECT_DOUBLE_EQ(0.0, pfnCubic(2.0));
  EXPECT_DOUBLE_EQ(0.0, pfnCubic(2.5));

  FilterFuncType pfnBSpline = GWKGetFilterFunc(GRA_CubicSpline);
  ASSERT_NE(nullptr, pfnBSpline);
  EXPECT_DOUBLE_EQ(4.0, pfnBSpline(0.0));
  EXPECT_DOUBLE_EQ(2.875, pfnBSpline(0.5));
  EXPECT_DOUBLE_EQ(1.0, pfnBSpline(1.0));
  EXPECT_DOUBLE_EQ(0.125, pfnBSpline(1.5));
  EXPECT_DOUBLE_EQ(0.0, pfnBSpline(2.0));

  FilterFuncType pfnLanczos = GWKGetFilterFunc(GRA_Lanczos);
  ASSERT_NE(nullptr, pfnLanczos);
  EXPECT_DOUBLE_EQ(1.0, pfnLanczos(0.0));
  EXPECT_NEAR(0.6079, pfnLanczos(0.5), 1e-3);
  EXPECT_NEAR(0.0, pfnLanczos(1.0), 1e-6);
  EXPECT_NEAR(0.0, pfnLanczos(2.0), 1e-6);
  EXPECT_NEAR(0.0, pfnLanczos(3.0), 1e-6);

  // Test GWKGetFilterFunc4Values.
  EXPECT_EQ(nullptr, GWKGetFilterFunc4Values(GRA_NearestNeighbour));
  EXPECT_EQ(nullptr, GWKGetFilterFunc4Values(GRA_Average));

  FilterFunc4ValuesType pfnBilinear4 = GWKGetFilterFunc4Values(GRA_Bilinear);
  ASSERT_NE(nullptr, pfnBilinear4);
  double vals[4] = {0.0, 0.5, 1.0, 1.5};
  double sum4 = pfnBilinear4(vals);
  EXPECT_DOUBLE_EQ(1.5, sum4);

  FilterFunc4ValuesType pfnCubic4 = GWKGetFilterFunc4Values(GRA_Cubic);
  ASSERT_NE(nullptr, pfnCubic4);
  double vals_cubic[4] = {0.0, 0.5, 1.0, 1.5};
  EXPECT_GT(pfnCubic4(vals_cubic), 0.0);

  FilterFunc4ValuesType pfnBSpline4 = GWKGetFilterFunc4Values(GRA_CubicSpline);
  ASSERT_NE(nullptr, pfnBSpline4);
  double vals_bspline[4] = {0.0, 0.5, 1.0, 1.5};
  EXPECT_GT(pfnBSpline4(vals_bspline), 0.0);

  FilterFunc4ValuesType pfnLanczos4 = GWKGetFilterFunc4Values(GRA_Lanczos);
  ASSERT_NE(nullptr, pfnLanczos4);
  double vals_lanczos[4] = {0.0, 0.5, 1.0, 1.5};
  EXPECT_GT(pfnLanczos4(vals_lanczos), 0.0);
}

// ====================================================================
// Group 2: GDALWarpKernel Validation & Preconditions
// ====================================================================

TEST(GdalWarpKernelTest, ValidateDefaultsAndSuccess) {
  GDALWarpKernel kernel;
  EXPECT_EQ(GDT_Unknown, kernel.eWorkingDataType);
  EXPECT_EQ(0, kernel.nBands);
  EXPECT_EQ(1.0, kernel.dfXScale);
  EXPECT_EQ(1.0, kernel.dfYScale);
  EXPECT_EQ(GWKTS_First, kernel.eTieStrategy);
  EXPECT_EQ(GDALDummyProgress, kernel.pfnProgress);

  EXPECT_EQ(CE_None, kernel.Validate());
}

TEST(GdalWarpKernelTest, ValidateUnsupportedResample) {
  GDALWarpKernel kernel;
  // 15 is within the 4-bit range of the 15-value unscoped enum, safely
  // exceeding GRA_LAST_VALUE (14).
  kernel.eResample = static_cast<GDALResampleAlg>(15);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, kernel.Validate());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unsupported resampling method"));
}

TEST(GdalWarpKernelTest, ValidateExcludedValues) {
  GDALWarpKernel kernel;
  kernel.nBands = 3;

  // Invalid: 2 tokens for 3 bands
  CPLStringList options_bad;
  options_bad.SetNameValue("EXCLUDED_VALUES", "10,20");
  kernel.papszWarpOptions = options_bad.List();

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, kernel.Validate());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("EXCLUDED_VALUES should contain"));
  }

  // Valid: 6 tokens for 3 bands (2 tuples)
  CPLStringList options_good;
  options_good.SetNameValue("EXCLUDED_VALUES", "(10,20,30),(40,50,60)");
  kernel.papszWarpOptions = options_good.List();
  EXPECT_EQ(CE_None, kernel.Validate());
  EXPECT_EQ(2, kernel.m_aadfExcludedValues.size());
  EXPECT_EQ(3, kernel.m_aadfExcludedValues[0].size());
  EXPECT_DOUBLE_EQ(10.0, kernel.m_aadfExcludedValues[0][0]);
  EXPECT_DOUBLE_EQ(50.0, kernel.m_aadfExcludedValues[1][1]);
}

TEST(GdalWarpKernelTest, PerformWarpEmptyDimensions) {
  GDALWarpKernel kernel;
  kernel.nSrcXSize = 0;
  kernel.nSrcYSize = 10;
  EXPECT_EQ(CE_None, kernel.PerformWarp());

  // Failure when progress callback returns FALSE
  kernel.pfnProgress = [](double, const char*, void*) -> int { return FALSE; };
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, kernel.PerformWarp());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User terminated"));
  }
}

TEST(GdalWarpKernelTest, PerformWarpMissingExtraElts) {
  WarpKernelHarness<float> harness(20, 20, 10, 10, 1, GRA_Cubic);
  // Downsampling cubic without EXTRA_ELTS=1 must fail
  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, harness.PerformWarp());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Source arrays must have WARP_EXTRA_ELTS"));

  // CubicSpline without EXTRA_ELTS=1 must fail
  harness.kernel.eResample = GRA_CubicSpline;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, harness.PerformWarp());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Source arrays must have WARP_EXTRA_ELTS"));

  // Lanczos without EXTRA_ELTS=1 must fail
  harness.kernel.eResample = GRA_Lanczos;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, harness.PerformWarp());
  EXPECT_THAT(CPLGetLastErrorMsg(),
              HasSubstr("Source arrays must have WARP_EXTRA_ELTS"));

  // With EXTRA_ELTS defined, it succeeds
  harness.SetExtraEltsOption();
  EXPECT_EQ(CE_None, harness.PerformWarp());
}

// ====================================================================
// Group 3: Scale Computation and Overrides
// ====================================================================

TEST(GdalWarpKernelTest, ScaleOptionsAndGridSampling) {
  WarpKernelHarness<GByte> harness(10, 10, 10, 10, 1, GRA_NearestNeighbour);

  // Explicit XSCALE and YSCALE
  harness.SetOption("XSCALE", "2.5");
  harness.SetOption("YSCALE", "3.5");
  EXPECT_EQ(CE_None, harness.PerformWarp());
  EXPECT_DOUBLE_EQ(2.5, harness.kernel.dfXScale);
  EXPECT_DOUBLE_EQ(3.5, harness.kernel.dfYScale);

  // XSCALE=FROM_GRID_SAMPLING
  harness.options.Clear();
  harness.SetOption("XSCALE", "FROM_GRID_SAMPLING");
  EXPECT_EQ(CE_None, harness.PerformWarp());
  EXPECT_GT(harness.kernel.dfXScale, 0.0);

  // Large ratio (dfYScale / dfXScale > 100) auto-triggers grid sampling
  WarpKernelHarness<GByte> harness_antimeridian(100, 1, 1, 200, 1,
                                                GRA_NearestNeighbour);
  EXPECT_EQ(CE_None, harness_antimeridian.PerformWarp());
  EXPECT_GT(harness_antimeridian.kernel.dfXScale, 0.0);
}

// ====================================================================
// Group 4: All 16 Fast Paths (bNoMasksOrDstDensityOnly == true)
// ====================================================================

TEST(GdalWarpKernelTest, FastPathByte) {
  // 1. Nearest
  {
    WarpKernelHarness<GByte> h(4, 4, 4, 4, 1, GRA_NearestNeighbour);
    h.SetSrcPixel(0, 1, 1, 42);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(42, h.GetDstPixel(0, 1, 1));
  }
  // 2. Bilinear (1:1 and 2x downsampling)
  {
    WarpKernelHarness<GByte> h(4, 4, 4, 4, 1, GRA_Bilinear);
    h.SetSrcPixel(0, 1, 1, 100);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0);
  }
  {
    WarpKernelHarness<GByte> h(8, 8, 4, 4, 1, GRA_Bilinear);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 2, 2, 100);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0);
  }
  // 3. Cubic (1:1 and 2x downsampling)
  {
    WarpKernelHarness<GByte> h(6, 6, 6, 6, 1, GRA_Cubic);
    h.SetSrcPixel(0, 2, 2, 100);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0);
  }
  {
    WarpKernelHarness<GByte> h(12, 12, 6, 6, 1, GRA_Cubic);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 4, 4, 100);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0);
  }
  // 4. CubicSpline (1:1 and 2x downsampling)
  {
    WarpKernelHarness<GByte> h(6, 6, 6, 6, 1, GRA_CubicSpline);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 2, 2, 100);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0);
  }
  {
    WarpKernelHarness<GByte> h(12, 12, 6, 6, 1, GRA_CubicSpline);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 4, 4, 100);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0);
  }
  // 5. Multi-band Cubic Byte (exercises GWKCubicResampleNoMasks4MultiBandT)
  {
    WarpKernelHarness<GByte> h(8, 8, 8, 8, 3, GRA_Cubic);
    h.SetSrcPixel(0, 3, 3, 50);
    h.SetSrcPixel(1, 3, 3, 75);
    h.SetSrcPixel(2, 3, 3, 100);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 3, 3), 0);
    EXPECT_GT(h.GetDstPixel(1, 3, 3), 0);
    EXPECT_GT(h.GetDstPixel(2, 3, 3), 0);
  }
}

TEST(GdalWarpKernelTest, FastPathInt16) {
  // 1. Nearest
  {
    WarpKernelHarness<GInt16> h(4, 4, 4, 4, 1, GRA_NearestNeighbour);
    h.SetSrcPixel(0, 1, 1, -1234);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(-1234, h.GetDstPixel(0, 1, 1));
  }
  // 2. Bilinear (1:1 and downsampling)
  {
    WarpKernelHarness<GInt16> h(4, 4, 4, 4, 1, GRA_Bilinear);
    h.SetSrcPixel(0, 1, 1, 2000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0);
  }
  {
    WarpKernelHarness<GInt16> h(8, 8, 4, 4, 1, GRA_Bilinear);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 2, 2, 2000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0);
  }
  // 3. Cubic (1:1 and downsampling)
  {
    WarpKernelHarness<GInt16> h(6, 6, 6, 6, 1, GRA_Cubic);
    h.SetSrcPixel(0, 2, 2, 3000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0);
  }
  {
    WarpKernelHarness<GInt16> h(12, 12, 6, 6, 1, GRA_Cubic);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 4, 4, 3000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0);
  }
  // 4. CubicSpline (1:1 and downsampling)
  {
    WarpKernelHarness<GInt16> h(6, 6, 6, 6, 1, GRA_CubicSpline);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 2, 2, 4000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0);
  }
  {
    WarpKernelHarness<GInt16> h(12, 12, 6, 6, 1, GRA_CubicSpline);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 4, 4, 4000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0);
  }
}

TEST(GdalWarpKernelTest, FastPathUInt16) {
  // 1. Nearest
  {
    WarpKernelHarness<GUInt16> h(4, 4, 4, 4, 1, GRA_NearestNeighbour);
    h.SetSrcPixel(0, 1, 1, 55000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(55000, h.GetDstPixel(0, 1, 1));
  }
  // 2. Bilinear (1:1 and downsampling)
  {
    WarpKernelHarness<GUInt16> h(4, 4, 4, 4, 1, GRA_Bilinear);
    h.SetSrcPixel(0, 1, 1, 40000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0);
  }
  {
    WarpKernelHarness<GUInt16> h(8, 8, 4, 4, 1, GRA_Bilinear);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 2, 2, 40000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0);
  }
  // 3. Cubic (1:1 and downsampling)
  {
    WarpKernelHarness<GUInt16> h(6, 6, 6, 6, 1, GRA_Cubic);
    h.SetSrcPixel(0, 2, 2, 50000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0);
  }
  {
    WarpKernelHarness<GUInt16> h(12, 12, 6, 6, 1, GRA_Cubic);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 4, 4, 50000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0);
  }
  // 4. CubicSpline (1:1 and downsampling)
  {
    WarpKernelHarness<GUInt16> h(6, 6, 6, 6, 1, GRA_CubicSpline);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 2, 2, 60000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0);
  }
  {
    WarpKernelHarness<GUInt16> h(12, 12, 6, 6, 1, GRA_CubicSpline);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 4, 4, 60000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0);
  }
  // 5. Multi-band Cubic UInt16
  {
    WarpKernelHarness<GUInt16> h(8, 8, 8, 8, 3, GRA_Cubic);
    h.SetSrcPixel(0, 3, 3, 10000);
    h.SetSrcPixel(1, 3, 3, 20000);
    h.SetSrcPixel(2, 3, 3, 30000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 3, 3), 0);
    EXPECT_GT(h.GetDstPixel(1, 3, 3), 0);
    EXPECT_GT(h.GetDstPixel(2, 3, 3), 0);
  }
}

TEST(GdalWarpKernelTest, FastPathFloat32) {
  // 1. Nearest
  {
    WarpKernelHarness<float> h(4, 4, 4, 4, 1, GRA_NearestNeighbour);
    h.SetSrcPixel(0, 1, 1, 3.1415f);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_FLOAT_EQ(3.1415f, h.GetDstPixel(0, 1, 1));
  }
  // 2. Bilinear (1:1 and downsampling)
  {
    WarpKernelHarness<float> h(4, 4, 4, 4, 1, GRA_Bilinear);
    h.SetSrcPixel(0, 1, 1, 10.0f);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0.0f);
  }
  {
    WarpKernelHarness<float> h(8, 8, 4, 4, 1, GRA_Bilinear);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 2, 2, 10.0f);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0.0f);
  }
  // 3. Cubic (1:1 and downsampling)
  {
    WarpKernelHarness<float> h(6, 6, 6, 6, 1, GRA_Cubic);
    h.SetSrcPixel(0, 2, 2, 20.0f);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0.0f);
  }
  {
    WarpKernelHarness<float> h(12, 12, 6, 6, 1, GRA_Cubic);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 4, 4, 20.0f);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0.0f);
  }
}

TEST(GdalWarpKernelTest, FastPathFloat64) {
  // Bilinear (1:1 and downsampling)
  {
    WarpKernelHarness<double> h(4, 4, 4, 4, 1, GRA_Bilinear);
    h.SetSrcPixel(0, 1, 1, 100.5);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0.0);
  }
  {
    WarpKernelHarness<double> h(8, 8, 4, 4, 1, GRA_Bilinear);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 2, 2, 100.5);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0.0);
  }
  // Cubic with pafDstDensity
  {
    WarpKernelHarness<double> h(6, 6, 6, 6, 1, GRA_Cubic);
    h.EnableDstDensity(0.5f);
    h.SetSrcPixel(0, 2, 2, 200.5);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0.0);
    EXPECT_GT(h.dst_density[2 * 6 + 2], 0.0f);
  }
}

// ====================================================================
// Group 5: Masked Nearest Neighbour Fast Paths
// ====================================================================

TEST(GdalWarpKernelTest, MaskedNearestNeighbour) {
  // 1. Byte (GWKNearestByte)
  {
    WarpKernelHarness<GByte> h(4, 4, 4, 4, 1, GRA_NearestNeighbour);
    h.EnableUnifiedSrcValid(true);
    h.EnableDstValid(false);
    h.SetSrcPixel(0, 1, 1, 88);
    // Invalidate pixel (1, 2)
    CPLMaskClear(h.unified_src_valid.data(), 2 * 4 + 1);

    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(88, h.GetDstPixel(0, 1, 1));
    EXPECT_TRUE(CPLMaskGet(h.dst_valid.data(), 1 * 4 + 1));
    EXPECT_FALSE(CPLMaskGet(h.dst_valid.data(), 2 * 4 + 1));
  }
  // 2. Short (GWKNearestShort)
  {
    WarpKernelHarness<GInt16> h(4, 4, 4, 4, 1, GRA_NearestNeighbour);
    h.EnableUnifiedSrcValid(true);
    h.SetSrcPixel(0, 1, 1, -555);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(-555, h.GetDstPixel(0, 1, 1));
  }
  // 3. Unsigned Short (GWKNearestUnsignedShort)
  {
    WarpKernelHarness<GUInt16> h(4, 4, 4, 4, 1, GRA_NearestNeighbour);
    h.EnableUnifiedSrcValid(true);
    h.SetSrcPixel(0, 1, 1, 45000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(45000, h.GetDstPixel(0, 1, 1));
  }
  // 4. Float (GWKNearestFloat)
  {
    WarpKernelHarness<float> h(4, 4, 4, 4, 1, GRA_NearestNeighbour);
    h.EnableUnifiedSrcValid(true);
    h.SetSrcPixel(0, 1, 1, 123.456f);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_FLOAT_EQ(123.456f, h.GetDstPixel(0, 1, 1));
  }
  // 5. Int32 and Float64 masked nearest (GWKRealCase)
  {
    WarpKernelHarness<GInt32> h(4, 4, 4, 4, 1, GRA_NearestNeighbour);
    h.EnableUnifiedSrcValid(true);
    h.SetSrcPixel(0, 1, 1, 12345678);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(12345678, h.GetDstPixel(0, 1, 1));
  }
  {
    WarpKernelHarness<double> h(4, 4, 4, 4, 1, GRA_NearestNeighbour);
    h.EnableUnifiedSrcValid(true);
    h.SetSrcPixel(0, 1, 1, 987654.321);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_DOUBLE_EQ(987654.321, h.GetDstPixel(0, 1, 1));
  }
}

// ====================================================================
// Group 6: GWKRealCase (Real data types with masks & filters)
// ====================================================================

TEST(GdalWarpKernelTest, RealCaseMasksAndFilters) {
  // Bilinear with masks
  {
    WarpKernelHarness<float> h(4, 4, 4, 4, 1, GRA_Bilinear);
    h.EnableUnifiedSrcValid(true);
    h.EnableUnifiedSrcDensity(1.0f);
    h.EnableDstValid(false);
    h.EnableDstDensity(0.0f);
    h.SetSrcPixel(0, 1, 1, 15.0f);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0.0f);
    EXPECT_TRUE(CPLMaskGet(h.dst_valid.data(), 1 * 4 + 1));
    EXPECT_GT(h.dst_density[1 * 4 + 1], 0.0f);
  }

  // Cubic with bSrcMaskIsDensity (Byte, UInt16, Float32)
  {
    WarpKernelHarness<GByte> h(6, 6, 6, 6, 1, GRA_Cubic);
    h.EnableUnifiedSrcDensity(1.0f);
    h.SetSrcPixel(0, 2, 2, 70);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0);
  }
  {
    WarpKernelHarness<GUInt16> h(6, 6, 6, 6, 1, GRA_Cubic);
    h.EnableUnifiedSrcDensity(1.0f);
    h.SetSrcPixel(0, 2, 2, 35000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0);
  }
  {
    WarpKernelHarness<float> h(6, 6, 6, 6, 1, GRA_Cubic);
    h.EnableUnifiedSrcDensity(1.0f);
    h.SetSrcPixel(0, 2, 2, 55.5f);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0.0f);
  }

  // CubicSpline with unified mask
  {
    WarpKernelHarness<float> h(6, 6, 6, 6, 1, GRA_CubicSpline);
    h.SetExtraEltsOption();
    h.EnableUnifiedSrcValid(true);
    h.SetSrcPixel(0, 2, 2, 80.0f);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0.0f);
  }

  // Lanczos with unified mask (upsampling and downsampling)
  {
    WarpKernelHarness<float> h(6, 6, 6, 6, 1, GRA_Lanczos);
    h.SetExtraEltsOption();
    h.EnableUnifiedSrcValid(true);
    h.SetSrcPixel(0, 2, 2, 90.0f);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0.0f);
  }
  {
    WarpKernelHarness<float> h(12, 12, 6, 6, 1, GRA_Lanczos);
    h.SetExtraEltsOption();
    h.EnableUnifiedSrcValid(true);
    h.SetSrcPixel(0, 4, 4, 90.0f);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2), 0.0f);
  }

  // Other types: GInt8, GUInt32, std::int64_t, std::uint64_t
  {
    WarpKernelHarness<GInt8> h(4, 4, 4, 4, 1, GRA_Bilinear);
    h.EnableUnifiedSrcValid(true);
    h.SetSrcPixel(0, 1, 1, 25);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0);
  }
  {
    WarpKernelHarness<GUInt32> h(4, 4, 4, 4, 1, GRA_Bilinear);
    h.EnableUnifiedSrcValid(true);
    h.SetSrcPixel(0, 1, 1, 1000000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0);
  }
  {
    WarpKernelHarness<std::int64_t> h(4, 4, 4, 4, 1, GRA_Bilinear);
    h.EnableUnifiedSrcValid(true);
    h.SetSrcPixel(0, 1, 1, 123456789012LL);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0);
  }
  {
    WarpKernelHarness<std::uint64_t> h(4, 4, 4, 4, 1, GRA_Bilinear);
    h.EnableUnifiedSrcValid(true);
    h.SetSrcPixel(0, 1, 1, 987654321098ULL);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0);
  }
}

// ====================================================================
// Group 7: GWKGeneralCase (Complex data types and USE_GENERAL_CASE)
// ====================================================================

TEST(GdalWarpKernelTest, GeneralCaseComplexTypes) {
  // GDT_CInt16
  {
    WarpKernelHarness<std::complex<GInt16>> h(4, 4, 4, 4, 1,
                                              GRA_NearestNeighbour);
    h.SetSrcPixel(0, 1, 1, {10, -20});
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(10, h.GetDstPixel(0, 1, 1).real());
    EXPECT_EQ(-20, h.GetDstPixel(0, 1, 1).imag());
  }
  // GDT_CInt32 Bilinear
  {
    WarpKernelHarness<std::complex<GInt32>> h(4, 4, 4, 4, 1, GRA_Bilinear);
    h.SetSrcPixel(0, 1, 1, {1000, 2000});
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1).real(), 0);
  }
  // GDT_CFloat32 Cubic, CubicSpline, Lanczos (1:1 and downsampling)
  {
    WarpKernelHarness<std::complex<float>> h(6, 6, 6, 6, 1, GRA_Cubic);
    h.SetSrcPixel(0, 2, 2, {50.0f, -50.0f});
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2).real(), 0.0f);
  }
  {
    WarpKernelHarness<std::complex<float>> h(12, 12, 6, 6, 1, GRA_Cubic);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 4, 4, {50.0f, -50.0f});
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2).real(), 0.0f);
  }
  {
    WarpKernelHarness<std::complex<float>> h(6, 6, 6, 6, 1, GRA_CubicSpline);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 2, 2, {60.0f, 60.0f});
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2).real(), 0.0f);
  }
  {
    WarpKernelHarness<std::complex<float>> h(6, 6, 6, 6, 1, GRA_Lanczos);
    h.SetExtraEltsOption();
    h.SetSrcPixel(0, 2, 2, {70.0f, -70.0f});
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 2, 2).real(), 0.0f);
  }
  // GDT_CFloat64 with masks
  {
    WarpKernelHarness<std::complex<double>> h(4, 4, 4, 4, 1, GRA_Bilinear);
    h.EnableUnifiedSrcValid(true);
    h.EnableUnifiedSrcDensity(1.0f);
    h.SetSrcPixel(0, 1, 1, {123.4, 567.8});
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1).real(), 0.0);
  }
  // USE_GENERAL_CASE=YES on real data
  {
    WarpKernelHarness<float> h(4, 4, 4, 4, 1, GRA_Bilinear);
    h.SetOption("USE_GENERAL_CASE", "YES");
    h.SetSrcPixel(0, 1, 1, 45.0f);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0.0f);
  }
}

// ====================================================================
// Group 8: Vertical Shift, Coord Precision, Edge Adjustments & NoData
// ====================================================================

TEST(GdalWarpKernelTest, VerticalShiftAndCoordPrecision) {
  // Vertical shift
  WarpKernelHarness<float> h(4, 4, 4, 4, 1, GRA_Bilinear);
  h.SetOption("APPLY_VERTICAL_SHIFT", "YES");
  h.SetOption("MULT_FACTOR_VERTICAL_SHIFT", "2.0");
  h.SetOption("MULT_FACTOR_VERTICAL_SHIFT_PIPELINE", "0.5");
  h.transform_data.vertical_shift_z = 10.0;
  h.SetSrcPixel(0, 1, 1, 50.0f);

  EXPECT_EQ(CE_None, h.PerformWarp());
  // 50.0 * 2.0 - 10.0 * 0.5 = 95.0
  EXPECT_NEAR(95.0f, h.GetDstPixel(0, 1, 1), 5.0f);

  // Coord precision
  h.SetOption("SRC_COORD_PRECISION", "0.25");
  h.SetOption("ERROR_THRESHOLD", "0.05");
  EXPECT_EQ(CE_None, h.PerformWarp());
}

TEST(GdalWarpKernelTest, EdgeAdjustmentCornerReprojectFails) {
  // Test GWKOneSourceCornerFailsToReproject edge adjustment
  WarpKernelHarness<GByte> h(4, 4, 4, 4, 1, GRA_NearestNeighbour);
  h.transform_data.fail_corner = true;
  h.transform_data.fail_corner_x = 4;
  h.transform_data.fail_corner_y = 4;
  h.EnableUnifiedSrcValid(true);
  h.EnableUnifiedSrcDensity(1.0f);
  h.SetSrcPixel(0, 1, 1, 77);

  EXPECT_EQ(CE_None, h.PerformWarp());
  EXPECT_EQ(77, h.GetDstPixel(0, 1, 1));
}

TEST(GdalWarpKernelTest, AvoidNoDataWarningAndAdjustment) {
  // Destination pixel value collision with padfDstNoDataReal
  WarpKernelHarness<GByte> h(4, 4, 4, 4, 1, GRA_NearestNeighbour);
  h.EnableDstValid();
  h.SetDstNoData(100.0);
  h.SetSrcPixel(0, 1, 1, 100);

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_None, h.PerformWarp());
  // The value 100 should have been changed to 99 to avoid colliding with nodata
  EXPECT_EQ(99, h.GetDstPixel(0, 1, 1));
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("changed to"));
}

// ====================================================================
// Group 9: Statistical Resamplers (GWKAverageOrMode)
// ====================================================================

TEST(GdalWarpKernelTest, StatisticalAverage) {
  // Real data average
  WarpKernelHarness<float> h(8, 8, 4, 4, 1, GRA_Average);
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      h.SetSrcPixel(0, x, y, 10.0f);
    }
  }
  EXPECT_EQ(CE_None, h.PerformWarp());
  EXPECT_NEAR(10.0f, h.GetDstPixel(0, 1, 1), 1e-3f);

  // Complex data average
  WarpKernelHarness<std::complex<float>> h_cplx(8, 8, 4, 4, 1, GRA_Average);
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      h_cplx.SetSrcPixel(0, x, y, {10.0f, 20.0f});
    }
  }
  EXPECT_EQ(CE_None, h_cplx.PerformWarp());
  EXPECT_NEAR(10.0f, h_cplx.GetDstPixel(0, 1, 1).real(), 1e-3f);
  EXPECT_NEAR(20.0f, h_cplx.GetDstPixel(0, 1, 1).imag(), 1e-3f);

  // Average with EXCLUDED_VALUES and thresholds
  WarpKernelHarness<float> h_excl(4, 4, 2, 2, 1, GRA_Average);
  h_excl.SetOption("EXCLUDED_VALUES", "0");
  h_excl.SetOption("EXCLUDED_VALUES_PCT_THRESHOLD", "50");
  h_excl.SetOption("NODATA_VALUES_PCT_THRESHOLD", "50");
  h_excl.SetSrcPixel(0, 0, 0, 0.0f);
  h_excl.SetSrcPixel(0, 0, 1, 10.0f);
  h_excl.SetSrcPixel(0, 1, 0, 10.0f);
  h_excl.SetSrcPixel(0, 1, 1, 10.0f);
  EXPECT_EQ(CE_None, h_excl.PerformWarp());
  EXPECT_GT(h_excl.GetDstPixel(0, 0, 0), 0.0f);
}

TEST(GdalWarpKernelTest, StatisticalRMS) {
  // RMS real
  WarpKernelHarness<float> h(4, 4, 2, 2, 1, GRA_RMS);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      h.SetSrcPixel(0, x, y, 4.0f);
    }
  }
  EXPECT_EQ(CE_None, h.PerformWarp());
  EXPECT_NEAR(4.0f, h.GetDstPixel(0, 0, 0), 1e-3f);

  // RMS complex
  WarpKernelHarness<std::complex<float>> h_cplx(4, 4, 2, 2, 1, GRA_RMS);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      h_cplx.SetSrcPixel(0, x, y, {3.0f, 4.0f});
    }
  }
  EXPECT_EQ(CE_None, h_cplx.PerformWarp());
  EXPECT_GT(h_cplx.GetDstPixel(0, 0, 0).real(), 0.0f);
}

TEST(GdalWarpKernelTest, StatisticalModeAllTypesAndTies) {
  // Byte (256 bins)
  {
    WarpKernelHarness<GByte> h(4, 4, 2, 2, 1, GRA_Mode);
    h.SetSrcPixel(0, 0, 0, 5);
    h.SetSrcPixel(0, 0, 1, 5);
    h.SetSrcPixel(0, 1, 0, 9);
    h.SetSrcPixel(0, 1, 1, 9);
    h.kernel.eTieStrategy = GWKTS_Min;
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(5, h.GetDstPixel(0, 0, 0));

    h.kernel.eTieStrategy = GWKTS_Max;
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(9, h.GetDstPixel(0, 0, 0));

    h.kernel.eTieStrategy = GWKTS_First;
    EXPECT_EQ(CE_None, h.PerformWarp());
  }

  // GInt8 (256 bins, offset 128)
  {
    WarpKernelHarness<GInt8> h(4, 4, 2, 2, 1, GRA_Mode);
    h.SetSrcPixel(0, 0, 0, -10);
    h.SetSrcPixel(0, 0, 1, -10);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(-10, h.GetDstPixel(0, 0, 0));
  }

  // GUInt16 (65536 bins)
  {
    WarpKernelHarness<GUInt16> h(4, 4, 2, 2, 1, GRA_Mode);
    h.SetSrcPixel(0, 0, 0, 40000);
    h.SetSrcPixel(0, 0, 1, 40000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(40000, h.GetDstPixel(0, 0, 0));
  }

  // GInt16 (65536 bins, offset 32768)
  {
    WarpKernelHarness<GInt16> h(4, 4, 2, 2, 1, GRA_Mode);
    h.SetSrcPixel(0, 0, 0, -5000);
    h.SetSrcPixel(0, 0, 1, -5000);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(-5000, h.GetDstPixel(0, 0, 0));
  }

  // GWKModeRealType: Int32, UInt32, Int64, UInt64, Float32, Float64
  {
    WarpKernelHarness<GInt32> h(4, 4, 2, 2, 1, GRA_Mode);
    h.SetSrcPixel(0, 0, 0, 123456);
    h.SetSrcPixel(0, 0, 1, 123456);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(123456, h.GetDstPixel(0, 0, 0));
  }
  {
    WarpKernelHarness<GUInt32> h(4, 4, 2, 2, 1, GRA_Mode);
    h.SetSrcPixel(0, 0, 0, 999999);
    h.SetSrcPixel(0, 0, 1, 999999);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(999999, h.GetDstPixel(0, 0, 0));
  }
  {
    WarpKernelHarness<std::int64_t> h(4, 4, 2, 2, 1, GRA_Mode);
    h.SetSrcPixel(0, 0, 0, 888888888888LL);
    h.SetSrcPixel(0, 0, 1, 888888888888LL);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(888888888888LL, h.GetDstPixel(0, 0, 0));
  }
  {
    WarpKernelHarness<std::uint64_t> h(4, 4, 2, 2, 1, GRA_Mode);
    h.SetSrcPixel(0, 0, 0, 777777777777ULL);
    h.SetSrcPixel(0, 0, 1, 777777777777ULL);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_EQ(777777777777ULL, h.GetDstPixel(0, 0, 0));
  }
  {
    WarpKernelHarness<float> h(4, 4, 2, 2, 1, GRA_Mode);
    h.SetSrcPixel(0, 0, 0, 3.5f);
    h.SetSrcPixel(0, 0, 1, 3.5f);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_FLOAT_EQ(3.5f, h.GetDstPixel(0, 0, 0));
  }
  {
    WarpKernelHarness<double> h(4, 4, 2, 2, 1, GRA_Mode);
    h.SetSrcPixel(0, 0, 0, 7.25);
    h.SetSrcPixel(0, 0, 1, 7.25);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_DOUBLE_EQ(7.25, h.GetDstPixel(0, 0, 0));
  }

  // GWKModeComplexType
  {
    WarpKernelHarness<std::complex<float>> h(4, 4, 2, 2, 1, GRA_Mode);
    h.SetSrcPixel(0, 0, 0, {1.0f, 2.0f});
    h.SetSrcPixel(0, 0, 1, {1.0f, 2.0f});
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_FLOAT_EQ(1.0f, h.GetDstPixel(0, 0, 0).real());
    EXPECT_FLOAT_EQ(2.0f, h.GetDstPixel(0, 0, 0).imag());
  }

  // Mode with vertical shift (redirects to GWKModeComplexType)
  {
    WarpKernelHarness<GByte> h(4, 4, 2, 2, 1, GRA_Mode);
    h.kernel.bApplyVerticalShift = true;
    h.SetSrcPixel(0, 0, 0, 12);
    h.SetSrcPixel(0, 0, 1, 12);
    EXPECT_EQ(CE_None, h.PerformWarp());
  }
}

TEST(GdalWarpKernelTest, StatisticalExtremaAndQuantiles) {
  // Max and Min
  {
    WarpKernelHarness<float> h(4, 4, 2, 2, 1, GRA_Max);
    h.SetSrcPixel(0, 0, 0, 10.0f);
    h.SetSrcPixel(0, 0, 1, 50.0f);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_FLOAT_EQ(50.0f, h.GetDstPixel(0, 0, 0));
  }
  {
    WarpKernelHarness<float> h(4, 4, 2, 2, 1, GRA_Min);
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) {
        h.SetSrcPixel(0, x, y, 100.0f);
      }
    }
    h.SetSrcPixel(0, 0, 0, 10.0f);
    h.SetSrcPixel(0, 0, 1, 50.0f);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_FLOAT_EQ(10.0f, h.GetDstPixel(0, 0, 0));
  }

  // Median, Q1, Q3
  {
    WarpKernelHarness<float> h(4, 4, 2, 2, 1, GRA_Med);
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) {
        h.SetSrcPixel(0, x, y, static_cast<float>(y * 4 + x + 1));
      }
    }
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 0, 0), 0.0f);
  }
  {
    WarpKernelHarness<float> h(4, 4, 2, 2, 1, GRA_Q1);
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) {
        h.SetSrcPixel(0, x, y, static_cast<float>(y * 4 + x + 1));
      }
    }
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 0, 0), 0.0f);
  }
  {
    WarpKernelHarness<float> h(4, 4, 2, 2, 1, GRA_Q3);
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) {
        h.SetSrcPixel(0, x, y, static_cast<float>(y * 4 + x + 1));
      }
    }
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 0, 0), 0.0f);
  }
}

// ====================================================================
// Group 10: GWKSumPreserving (GRA_Sum)
// ====================================================================

TEST(GdalWarpKernelTest, SumPreservingAffineAndGeneral) {
  // 1. Affine optimization (GDAL_WARP_USE_AFFINE_OPTIMIZATION="YES")
  {
    CPLSetConfigOption("GDAL_WARP_USE_AFFINE_OPTIMIZATION", "YES");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("GDAL_WARP_USE_AFFINE_OPTIMIZATION", nullptr);
    };

    GenImgProjTransformerHelper helper(4, 4, 4, 4);
    WarpKernelHarness<float> h(4, 4, 4, 4, 1, GRA_Sum);
    h.kernel.pfnTransformer = GDALGenImgProjTransform;
    h.kernel.pTransformerArg = helper.transform_arg;
    h.SetSrcPixel(0, 1, 1, 100.0f);

    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0.0f);
  }

  // 2. Non-affine polygon clipping (GDAL_WARP_USE_AFFINE_OPTIMIZATION="NO")
  {
    CPLSetConfigOption("GDAL_WARP_USE_AFFINE_OPTIMIZATION", "NO");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("GDAL_WARP_USE_AFFINE_OPTIMIZATION", nullptr);
    };

    WarpKernelHarness<float> h(4, 4, 4, 4, 1, GRA_Sum);
    h.SetSrcPixel(0, 1, 1, 100.0f);

    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0.0f);
  }

  // 3. Non-convex quad (exercises !isConvex and two-triangle split)
  {
    CPLSetConfigOption("GDAL_WARP_USE_AFFINE_OPTIMIZATION", "NO");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("GDAL_WARP_USE_AFFINE_OPTIMIZATION", nullptr);
    };

    WarpKernelHarness<float> h(4, 4, 4, 4, 1, GRA_Sum);
    h.transform_data.non_convex_quad = true;
    h.SetSrcPixel(0, 0, 0, 50.0f);

    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 0, 0), 0.0f);
  }

  // 4. Self-crossing quad (exercises self-crossing quad rejection)
  {
    CPLSetConfigOption("GDAL_WARP_USE_AFFINE_OPTIMIZATION", "NO");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("GDAL_WARP_USE_AFFINE_OPTIMIZATION", nullptr);
    };

    WarpKernelHarness<float> h(4, 4, 4, 4, 1, GRA_Sum);
    h.transform_data.self_crossing_quad = true;
    h.SetSrcPixel(0, 0, 0, 50.0f);

    EXPECT_EQ(CE_None, h.PerformWarp());
  }

  // 5. Antimeridian discontinuity (exercises FindDiscontinuity and left/right
  // split)
  {
    CPLSetConfigOption("GDAL_WARP_USE_AFFINE_OPTIMIZATION", "NO");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("GDAL_WARP_USE_AFFINE_OPTIMIZATION", nullptr);
    };

    WarpKernelHarness<float> h(4, 4, 4, 4, 1, GRA_Sum);
    h.transform_data.wrap_x = true;
    h.SetSrcPixel(0, 1, 1, 50.0f);
    h.SetSrcPixel(0, 2, 1, 50.0f);

    EXPECT_EQ(CE_None, h.PerformWarp());
  }

  // 6. With validity and density masks
  {
    CPLSetConfigOption("GDAL_WARP_USE_AFFINE_OPTIMIZATION", "NO");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("GDAL_WARP_USE_AFFINE_OPTIMIZATION", nullptr);
    };

    WarpKernelHarness<float> h(4, 4, 4, 4, 1, GRA_Sum);
    h.EnableUnifiedSrcValid(true);
    h.EnableUnifiedSrcDensity(1.0f);
    h.EnableDstValid(false);
    h.SetSrcPixel(0, 1, 1, 60.0f);

    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 1, 1), 0.0f);
    EXPECT_TRUE(CPLMaskGet(h.dst_valid.data(), 1 * 4 + 1));
  }
}

// ====================================================================
// Group 11: Multithreading & Progress
// ====================================================================

TEST(GdalWarpKernelTest, MultithreadingAndProgress) {
  // Test GWKThreadsCreate and GWKThreadsEnd directly
  {
    CPLStringList thread_options;
    thread_options.SetNameValue("NUM_THREADS", "0");
    void* thread_data0 =
        GWKThreadsCreate(thread_options.List(), nullptr, nullptr);
    GWKThreadsEnd(thread_data0);

    thread_options.SetNameValue("NUM_THREADS", "1");
    void* thread_data1 =
        GWKThreadsCreate(thread_options.List(), nullptr, nullptr);
    GWKThreadsEnd(thread_data1);

    thread_options.SetNameValue("NUM_THREADS", "2");
    void* thread_data2 =
        GWKThreadsCreate(thread_options.List(), nullptr, nullptr);
    GWKThreadsEnd(thread_data2);

    thread_options.SetNameValue("NUM_THREADS", "ALL_CPUS");
    void* thread_data_all =
        GWKThreadsCreate(thread_options.List(), nullptr, nullptr);
    GWKThreadsEnd(thread_data_all);
  }

  // Multithreaded execution with chunk size override
  {
    CPLSetConfigOption("WARP_THREAD_CHUNK_SIZE", "16");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("WARP_THREAD_CHUNK_SIZE", nullptr);
    };

    GenImgProjTransformerHelper helper(8, 8, 8, 8);
    WarpKernelHarness<GByte> h(8, 8, 8, 8, 1, GRA_Bilinear);
    h.kernel.pfnTransformer = GDALGenImgProjTransform;
    h.kernel.pTransformerArg = helper.transform_arg;
    h.SetOption("NUM_THREADS", "2");

    void* psThreadData =
        GWKThreadsCreate(h.kernel.papszWarpOptions, h.kernel.pfnTransformer,
                         h.kernel.pTransformerArg);
    absl::Cleanup cleanup_threads = [psThreadData] {
      GWKThreadsEnd(psThreadData);
    };
    h.kernel.psThreadData = psThreadData;

    int progress_calls = 0;
    h.kernel.pProgress = &progress_calls;
    h.kernel.pfnProgress = [](double, const char*, void* arg) -> int {
      int* count = static_cast<int*>(arg);
      (*count)++;
      return TRUE;
    };

    h.SetSrcPixel(0, 3, 3, 77);
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(h.GetDstPixel(0, 3, 3), 0);
    EXPECT_GT(progress_calls, 0);
  }

  // Multithreaded execution with user cancellation
  {
    CPLSetConfigOption("WARP_THREAD_CHUNK_SIZE", "16");
    absl::Cleanup cleanup = [] {
      CPLSetConfigOption("WARP_THREAD_CHUNK_SIZE", nullptr);
    };

    GenImgProjTransformerHelper helper(8, 8, 8, 8);
    WarpKernelHarness<GByte> h(8, 8, 8, 8, 1, GRA_Bilinear);
    h.kernel.pfnTransformer = GDALGenImgProjTransform;
    h.kernel.pTransformerArg = helper.transform_arg;
    h.SetOption("NUM_THREADS", "2");

    void* psThreadData =
        GWKThreadsCreate(h.kernel.papszWarpOptions, h.kernel.pfnTransformer,
                         h.kernel.pTransformerArg);
    absl::Cleanup cleanup_threads = [psThreadData] {
      GWKThreadsEnd(psThreadData);
    };
    h.kernel.psThreadData = psThreadData;

    int progress_calls = 0;
    h.kernel.pProgress = &progress_calls;
    h.kernel.pfnProgress = [](double dfComplete, const char*,
                              void* arg) -> int {
      int* count = static_cast<int*>(arg);
      (*count)++;
      if (dfComplete > 0.0) {
        return FALSE;
      }
      return TRUE;
    };

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, h.PerformWarp());
  }

  // Progress callback success
  {
    WarpKernelHarness<GByte> h(4, 4, 4, 4, 1, GRA_NearestNeighbour);
    int progress_calls = 0;
    h.kernel.pProgress = &progress_calls;
    h.kernel.pfnProgress = [](double, const char*, void* arg) -> int {
      int* count = static_cast<int*>(arg);
      (*count)++;
      return TRUE;
    };
    EXPECT_EQ(CE_None, h.PerformWarp());
    EXPECT_GT(progress_calls, 0);
  }

  // Progress callback user cancellation
  {
    WarpKernelHarness<GByte> h(4, 4, 4, 4, 1, GRA_NearestNeighbour);
    h.kernel.pfnProgress = [](double, const char*, void*) -> int {
      return FALSE;
    };
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, h.PerformWarp());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User terminated"));
  }
}

// ====================================================================
// Group 12: End-to-End Features via GDALWarpOperation
// ====================================================================

TEST(GdalWarpKernelTest, WarpOperationOptionsIntegration) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  // Test INIT_DEST and UNIFIED_SRC_NODATA
  auto src_ds =
      GDALDatasetUniquePtr(mem_driver->Create("", 4, 4, 2, GDT_Byte, nullptr));
  auto dst_ds =
      GDALDatasetUniquePtr(mem_driver->Create("", 4, 4, 2, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);
  ASSERT_NE(nullptr, dst_ds);

  const double gt[6] = {0.0, 1.0, 0.0, 10.0, 0.0, -1.0};
  src_ds->SetGeoTransform(gt);
  dst_ds->SetGeoTransform(gt);

  std::unique_ptr<GDALWarpOptions, decltype(&GDALDestroyWarpOptions)> psOptions(
      GDALCreateWarpOptions(), GDALDestroyWarpOptions);
  psOptions->hSrcDS = src_ds.get();
  psOptions->hDstDS = dst_ds.get();
  psOptions->nBandCount = 2;
  int* panSrcBands = static_cast<int*>(CPLMalloc(2 * sizeof(int)));
  panSrcBands[0] = 1;
  panSrcBands[1] = 2;
  int* panDstBands = static_cast<int*>(CPLMalloc(2 * sizeof(int)));
  panDstBands[0] = 1;
  panDstBands[1] = 2;
  psOptions->panSrcBands = panSrcBands;
  psOptions->panDstBands = panDstBands;
  psOptions->eResampleAlg = GRA_NearestNeighbour;

  psOptions->papszWarpOptions =
      CSLSetNameValue(psOptions->papszWarpOptions, "INIT_DEST", "0");
  psOptions->papszWarpOptions =
      CSLSetNameValue(psOptions->papszWarpOptions, "UNIFIED_SRC_NODATA", "YES");

  double* padfSrcNoData = static_cast<double*>(CPLMalloc(2 * sizeof(double)));
  padfSrcNoData[0] = 0.0;
  padfSrcNoData[1] = 0.0;
  double* padfDstNoData = static_cast<double*>(CPLMalloc(2 * sizeof(double)));
  padfDstNoData[0] = 0.0;
  padfDstNoData[1] = 0.0;
  psOptions->padfSrcNoDataReal = padfSrcNoData;
  psOptions->padfDstNoDataReal = padfDstNoData;

  void* pTransformerArg =
      GDALCreateGenImgProjTransformer2(src_ds.get(), dst_ds.get(), nullptr);
  absl::Cleanup cleanup_trans = [pTransformerArg] {
    GDALDestroyGenImgProjTransformer(pTransformerArg);
  };
  psOptions->pfnTransformer = GDALGenImgProjTransform;
  psOptions->pTransformerArg = pTransformerArg;

  GDALWarpOperation op;
  EXPECT_EQ(CE_None, op.Initialize(psOptions.get()));
  EXPECT_EQ(CE_None, op.ChunkAndWarpImage(0, 0, 4, 4));

  // Test UNIFIED_SRC_NODATA=PARTIAL
  psOptions->papszWarpOptions = CSLSetNameValue(
      psOptions->papszWarpOptions, "UNIFIED_SRC_NODATA", "PARTIAL");
  GDALWarpOperation op_partial;
  EXPECT_EQ(CE_None, op_partial.Initialize(psOptions.get()));
  EXPECT_EQ(CE_None, op_partial.ChunkAndWarpImage(0, 0, 4, 4));

  // Test UNIFIED_SRC_NODATA=NO
  psOptions->papszWarpOptions =
      CSLSetNameValue(psOptions->papszWarpOptions, "UNIFIED_SRC_NODATA", "NO");
  GDALWarpOperation op_no;
  EXPECT_EQ(CE_None, op_no.Initialize(psOptions.get()));
  EXPECT_EQ(CE_None, op_no.ChunkAndWarpImage(0, 0, 4, 4));

  // Test INIT_DEST=NO_DATA
  psOptions->papszWarpOptions =
      CSLSetNameValue(psOptions->papszWarpOptions, "INIT_DEST", "NO_DATA");
  GDALWarpOperation op_nodata;
  EXPECT_EQ(CE_None, op_nodata.Initialize(psOptions.get()));
  EXPECT_EQ(CE_None, op_nodata.ChunkAndWarpImage(0, 0, 4, 4));
}

TEST(GdalWarpKernelTest, GeneralAndRealCaseAllTypesWithPartialDensity) {
  auto TestTypeGeneral = []<typename T>() {
    // 1a. NearestNeighbour with partial density
    {
      WarpKernelHarness<T> harness(4, 4, 4, 4, 1, GRA_NearestNeighbour);
      harness.SetExtraEltsOption();
      harness.SetOption("USE_GENERAL_CASE", "YES");
      harness.EnableUnifiedSrcDensity(0.7f);
      harness.EnableDstDensity();
      harness.EnableDstValid();
      for (size_t i = 0; i < 16; ++i) {
        if constexpr (std::is_same_v<T, std::complex<GInt16>> ||
                      std::is_same_v<T, std::complex<GInt32>> ||
                      std::is_same_v<T, std::complex<float>> ||
                      std::is_same_v<T, std::complex<double>>) {
          harness.src_bands[0][i] =
              T{static_cast<typename T::value_type>(i + 1),
                static_cast<typename T::value_type>(i + 2)};
        } else {
          harness.src_bands[0][i] = static_cast<T>(i + 1);
        }
      }
      EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
    }
    // 1b. Lanczos with valid masks (exercising GWKGetPixelRow across all
    // types!)
    {
      WarpKernelHarness<T> harness(6, 6, 6, 6, 1, GRA_Lanczos);
      harness.SetExtraEltsOption();
      harness.SetOption("USE_GENERAL_CASE", "YES");
      harness.EnableUnifiedSrcValid(true);
      CPLMaskClear(harness.unified_src_valid.data(), 0);
      harness.EnablePerBandSrcValid(true);
      CPLMaskClear(harness.per_band_src_valid[0].data(), 1);
      harness.EnableDstDensity();
      harness.EnableDstValid();
      for (size_t i = 0; i < 36; ++i) {
        if constexpr (std::is_same_v<T, std::complex<GInt16>> ||
                      std::is_same_v<T, std::complex<GInt32>> ||
                      std::is_same_v<T, std::complex<float>> ||
                      std::is_same_v<T, std::complex<double>>) {
          harness.src_bands[0][i] =
              T{static_cast<typename T::value_type>(i + 1),
                static_cast<typename T::value_type>(i + 2)};
        } else {
          harness.src_bands[0][i] = static_cast<T>((i + 1) * 2);
        }
      }
      EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
    }
  };

  TestTypeGeneral.operator()<GByte>();
  TestTypeGeneral.operator()<GInt8>();
  TestTypeGeneral.operator()<GUInt16>();
  TestTypeGeneral.operator()<GInt16>();
  TestTypeGeneral.operator()<GUInt32>();
  TestTypeGeneral.operator()<GInt32>();
  TestTypeGeneral.operator()<std::uint64_t>();
  TestTypeGeneral.operator()<std::int64_t>();
  TestTypeGeneral.operator()<float>();
  TestTypeGeneral.operator()<double>();
  TestTypeGeneral.operator()<std::complex<GInt16>>();
  TestTypeGeneral.operator()<std::complex<GInt32>>();
  TestTypeGeneral.operator()<std::complex<float>>();
  TestTypeGeneral.operator()<std::complex<double>>();

  // Empty validity masks hitting early return false in GWKGetPixelRow
  {
    WarpKernelHarness<GByte> harness(4, 4, 4, 4, 1, GRA_Lanczos);
    harness.SetExtraEltsOption();
    harness.SetOption("USE_GENERAL_CASE", "YES");
    harness.EnableUnifiedSrcValid(false);
    EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
  }
  {
    WarpKernelHarness<GByte> harness(4, 4, 4, 4, 1, GRA_Lanczos);
    harness.SetExtraEltsOption();
    harness.SetOption("USE_GENERAL_CASE", "YES");
    harness.EnablePerBandSrcValid(false);
    EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
  }

  // GWKRealCase: GWKGetPixelValueReal and GWKSetPixelValueReal across real
  // types
  auto TestTypeReal = []<typename T>() {
    WarpKernelHarness<T> harness(4, 4, 4, 4, 1, GRA_NearestNeighbour);
    harness.SetExtraEltsOption();
    harness.EnableUnifiedSrcDensity(0.7f);
    harness.EnablePerBandSrcValid(true);
    CPLMaskClear(harness.per_band_src_valid[0].data(), 0);
    harness.EnableDstDensity();
    harness.EnableDstValid();
    for (size_t i = 0; i < 16; ++i) {
      harness.src_bands[0][i] = static_cast<T>(i + 1);
    }
    EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
  };

  TestTypeReal.operator()<GByte>();
  TestTypeReal.operator()<GInt8>();
  TestTypeReal.operator()<GUInt16>();
  TestTypeReal.operator()<GInt16>();
  TestTypeReal.operator()<GUInt32>();
  TestTypeReal.operator()<GInt32>();
  TestTypeReal.operator()<std::uint64_t>();
  TestTypeReal.operator()<std::int64_t>();
  TestTypeReal.operator()<float>();
  TestTypeReal.operator()<double>();

  // Dst valid without Dst density in GWKSetPixelValueReal
  {
    WarpKernelHarness<GByte> harness(4, 4, 4, 4, 1, GRA_NearestNeighbour);
    harness.SetExtraEltsOption();
    harness.EnableUnifiedSrcDensity(0.7f);
    harness.EnableDstValid(false);
    for (size_t i = 0; i < 16; ++i)
      harness.src_bands[0][i] = static_cast<GByte>(i + 1);
    EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
  }

  // Float16 and CFloat16 via raw GDALWarpKernel
  {
    GDALWarpKernel kernel;
    kernel.nSrcXSize = 4;
    kernel.nSrcYSize = 4;
    kernel.nDstXSize = 4;
    kernel.nDstYSize = 4;
    kernel.nBands = 1;
    kernel.eWorkingDataType = GDT_Float16;
    kernel.eResample = GRA_NearestNeighbour;
    kernel.pfnTransformer = TestTransformer;
    TestTransformData data;
    data.scale_x = 1.0;
    data.scale_y = 1.0;
    kernel.pTransformerArg = &data;
    CPLStringList options;
    options.SetNameValue("EXTRA_ELTS", "1");
    kernel.papszWarpOptions = options.List();

    std::vector<float> unified_density(32, 0.7f);
    kernel.pafUnifiedSrcDensity = unified_density.data();
    std::vector<float> dst_density(32, 0.2f);
    kernel.pafDstDensity = dst_density.data();
    std::vector<uint8_t> src_f16(256, 0x3c);
    std::vector<uint8_t> dst_f16(256, 0);
    GByte* pSrcF16 = src_f16.data();
    GByte* pDstF16 = dst_f16.data();
    kernel.papabySrcImage = &pSrcF16;
    kernel.papabyDstImage = &pDstF16;
    EXPECT_EQ(CE_None, kernel.PerformWarp());

    // Float16 with GRA_Lanczos and USE_GENERAL_CASE=YES
    options.SetNameValue("USE_GENERAL_CASE", "YES");
    kernel.papszWarpOptions = options.List();
    kernel.eResample = GRA_Lanczos;
    EXPECT_EQ(CE_None, kernel.PerformWarp());

    // CFloat16 with GRA_Lanczos and USE_GENERAL_CASE=YES
    kernel.eWorkingDataType = GDT_CFloat16;
    std::vector<uint8_t> src_cf16(256, 0x3c);
    std::vector<uint8_t> dst_cf16(256, 0);
    GByte* pSrcCF16 = src_cf16.data();
    GByte* pDstCF16 = dst_cf16.data();
    kernel.papabySrcImage = &pSrcCF16;
    kernel.papabyDstImage = &pDstCF16;
    EXPECT_EQ(CE_None, kernel.PerformWarp());
  }

  // Cubic overshoot clamping
  auto TestCubicOvershoot = []<typename T>(T min_val, T max_val) {
    WarpKernelHarness<T> harness(6, 6, 6, 6, 1, GRA_Cubic);
    harness.SetExtraEltsOption();
    harness.SetOption("USE_GENERAL_CASE", "YES");
    harness.transform_data.offset_x = 0.25;
    harness.transform_data.offset_y = 0.25;
    for (int y = 0; y < 6; ++y) {
      for (int x = 0; x < 6; ++x) {
        harness.src_bands[0][y * 6 + x] = (x + y) % 2 == 0 ? min_val : max_val;
      }
    }
    EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
  };

  TestCubicOvershoot.operator()<GInt8>(-128, 127);
  TestCubicOvershoot.operator()<GInt16>(-32768, 32767);
  TestCubicOvershoot.operator()<GUInt16>(0, 65535);
  TestCubicOvershoot.operator()<std::complex<GInt16>>(
      std::complex<GInt16>{-32768, 32767}, std::complex<GInt16>{32767, -32768});
  TestCubicOvershoot.operator()<std::complex<GInt32>>(
      std::complex<GInt32>{-2147483647 - 1, 2147483647},
      std::complex<GInt32>{2147483647, -2147483647 - 1});
}

TEST(GdalWarpKernelTest, EdgeOffsetAdjustmentAndSinglePointRetry) {
  // Test single-point fallback retry in GWKCheckAndComputeSrcOffsets
  {
    WarpKernelHarness<GByte> harness(6, 6, 6, 6, 1, GRA_Bilinear);
    harness.SetExtraEltsOption();
    harness.transform_data.fail_forward_x_ge_2 = true;
    harness.transform_data.fail_forward_y_ge_2 = true;
    EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
  }

  // Test GWKAdjustSrcOffsetOnEdge and GWKAdjustSrcOffsetOnEdgeUnifiedSrcDensity
  const int offsets_to_test[] = {7, 11, 5, 1, -1};
  for (bool use_general : {false, true}) {
    for (int valid_neighbor : offsets_to_test) {
      // UnifiedSrcValid variant
      {
        WarpKernelHarness<GByte> harness(5, 5, 5, 5, 1, GRA_NearestNeighbour);
        harness.SetExtraEltsOption();
        if (use_general) harness.SetOption("USE_GENERAL_CASE", "YES");
        harness.transform_data.fail_forward_x_ge_2 = true;
        harness.EnableUnifiedSrcValid(false);
        if (valid_neighbor >= 0) {
          CPLMaskSet(harness.unified_src_valid.data(), valid_neighbor);
        }
        EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
      }
      // UnifiedSrcDensity variant
      {
        WarpKernelHarness<float> harness(5, 5, 5, 5, 1, GRA_NearestNeighbour);
        harness.SetExtraEltsOption();
        if (use_general) harness.SetOption("USE_GENERAL_CASE", "YES");
        harness.transform_data.fail_forward_x_ge_2 = true;
        harness.EnableUnifiedSrcDensity(0.0f);
        if (valid_neighbor >= 0) {
          harness.unified_src_density[valid_neighbor] = 1.0f;
        }
        EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
      }
    }
    // Also test without corner failure (!bOneSourceCornerFailsToReproject ->
    // continue)
    {
      WarpKernelHarness<GByte> harness(5, 5, 5, 5, 1, GRA_NearestNeighbour);
      harness.SetExtraEltsOption();
      if (use_general) harness.SetOption("USE_GENERAL_CASE", "YES");
      harness.EnableUnifiedSrcValid(false);
      EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
    }
    {
      WarpKernelHarness<float> harness(5, 5, 5, 5, 1, GRA_NearestNeighbour);
      harness.SetExtraEltsOption();
      if (use_general) harness.SetOption("USE_GENERAL_CASE", "YES");
      harness.EnableUnifiedSrcDensity(0.0f);
      EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
    }
  }

  // Test NaN handling in transformer
  {
    auto NaNTransformer = [](void* pArg, int bDstToSrc, int nPointCount,
                             double* x, double* y, double* z, int* panSuccess) {
      for (int i = 0; i < nPointCount; ++i) {
        if (i == 1) {
          x[i] = std::numeric_limits<double>::quiet_NaN();
          y[i] = std::numeric_limits<double>::quiet_NaN();
          panSuccess[i] = TRUE;
        } else {
          panSuccess[i] = TRUE;
        }
      }
      return TRUE;
    };
    WarpKernelHarness<GByte> harness(4, 4, 4, 4, 1, GRA_NearestNeighbour);
    harness.SetExtraEltsOption();
    harness.kernel.pfnTransformer = NaNTransformer;
    EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
  }
}

TEST(GdalWarpKernelTest, LanczosUnmaskedByteAndGridScaleSampling) {
  // Downsampling with unmasked Byte Lanczos
  {
    WarpKernelHarness<GByte> harness(12, 12, 4, 4, 1, GRA_Lanczos);
    harness.SetExtraEltsOption();
    for (size_t i = 0; i < 144; ++i) {
      harness.src_bands[0][i] = static_cast<GByte>(i * 4);
    }
    EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
  }

  // Upsampling with unmasked Byte Lanczos hitting SSE2 6-tap fast path
  {
    WarpKernelHarness<GByte> harness(10, 10, 12, 12, 1, GRA_Lanczos);
    harness.SetExtraEltsOption();
    for (size_t i = 0; i < 100; ++i) {
      harness.src_bands[0][i] = static_cast<GByte>((i + 1) * 2);
    }
    EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
  }

  // Odd number of lines in SSE2 fast path
  {
    WarpKernelHarness<GByte> harness(9, 9, 11, 11, 1, GRA_Lanczos);
    harness.SetExtraEltsOption();
    for (size_t i = 0; i < 81; ++i) {
      harness.src_bands[0][i] = static_cast<GByte>((i + 1) * 3);
    }
    EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
  }

  // XSCALE set to FROM_GRID_SAMPLING and explicit YSCALE
  {
    WarpKernelHarness<float> harness(4, 4, 4, 4, 1, GRA_Lanczos);
    harness.SetExtraEltsOption();
    harness.SetOption("XSCALE", "FROM_GRID_SAMPLING");
    harness.SetOption("YSCALE", "1.0");
    for (size_t i = 0; i < 16; ++i) {
      harness.src_bands[0][i] = static_cast<float>(i * 10);
    }
    EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
  }
}

TEST(GdalWarpKernelTest, ModeTieStrategiesAntimeridianAndWorkerLoop) {
  // Test GWKTieStrategy: GWKTS_First, GWKTS_Min, GWKTS_Max
  const GWKTieStrategy strategies[] = {GWKTS_First, GWKTS_Min, GWKTS_Max};

  // 1. GByte (histogram mode, L7780)
  for (GWKTieStrategy strategy : strategies) {
    WarpKernelHarness<GByte> harness(4, 4, 2, 2, 1, GRA_Mode);
    harness.SetExtraEltsOption();
    harness.kernel.eTieStrategy = strategy;
    for (size_t i = 0; i < 16; ++i) {
      harness.src_bands[0][i] = (i % 2 == 0) ? 10 : 20;
    }
    EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
  }

  // 2. float (GWKModeRealType, L6856)
  for (GWKTieStrategy strategy : strategies) {
    WarpKernelHarness<float> harness(4, 4, 2, 2, 1, GRA_Mode);
    harness.SetExtraEltsOption();
    harness.kernel.eTieStrategy = strategy;
    for (size_t i = 0; i < 16; ++i) {
      harness.src_bands[0][i] = (i % 2 == 0) ? 10.5f : 20.5f;
    }
    EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
  }

  // 3. complex<float> (GWKModeComplexType, L7110)
  for (GWKTieStrategy strategy : strategies) {
    WarpKernelHarness<std::complex<float>> harness(4, 4, 2, 2, 1, GRA_Mode);
    harness.SetExtraEltsOption();
    harness.kernel.eTieStrategy = strategy;
    for (size_t i = 0; i < 16; ++i) {
      harness.src_bands[0][i] = (i % 2 == 0) ? std::complex<float>{1.0f, 2.0f}
                                             : std::complex<float>{3.0f, 4.0f};
    }
    EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
  }

  // 4. Vertical shift on Mode (GWKModeComplexType fallback, L7263)
  {
    WarpKernelHarness<GByte> harness(4, 4, 2, 2, 1, GRA_Mode);
    harness.SetExtraEltsOption();
    harness.kernel.bApplyVerticalShift = true;
    for (size_t i = 0; i < 16; ++i) {
      harness.src_bands[0][i] = static_cast<GByte>(i * 10);
    }
    EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
  }

  // 5. Test GWKAverageOrModeComputeSourceCoords with antimeridian wrap (wrap_x
  // = true)
  {
    WarpKernelHarness<GByte> harness(10, 10, 4, 4, 1, GRA_Mode);
    harness.SetExtraEltsOption();
    harness.transform_data.wrap_x = true;
    harness.transform_data.scale_x = 2.0;
    harness.transform_data.scale_y = 2.0;
    for (size_t i = 0; i < 100; ++i) {
      harness.src_bands[0][i] = static_cast<GByte>(i % 5);
    }
    EXPECT_EQ(CE_None, harness.kernel.PerformWarp());
  }

  // 6. Test cancellation in progress callback
  {
    WarpKernelHarness<GByte> harness(8, 8, 8, 8, 1, GRA_Bilinear);
    harness.SetExtraEltsOption();
    auto CancelProgress = [](double dfComplete, const char* pszMsg,
                             void* pData) -> int {
      return dfComplete > 0.0 ? FALSE : TRUE;
    };
    harness.kernel.pfnProgress = CancelProgress;
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_NE(CE_None, harness.kernel.PerformWarp());
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered branches are specialized type/SIMD
// combinations and OOM guards.
