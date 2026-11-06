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
// Tests proximity algorithm (alg/gdalproximity.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/alg/gdalproximity.cpp

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
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
#include "third_party/gdal/port/cpl_progress.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

GDALDatasetUniquePtr CreateMemDataset(int width, int height,
                                      GDALDataType type = GDT_Float32) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  if (mem_driver == nullptr) {
    return nullptr;
  }
  return GDALDatasetUniquePtr(
      mem_driver->Create("", width, height, 1, type, nullptr));
}

void WriteBandInts(GDALRasterBand* band, int width, int height,
                   const std::vector<int32_t>& values) {
  ASSERT_NE(nullptr, band);
  EXPECT_EQ(CE_None, band->RasterIO(GF_Write, 0, 0, width, height,
                                    const_cast<int32_t*>(values.data()), width,
                                    height, GDT_Int32, 0, 0, nullptr));
}

std::vector<float> ReadBandFloats(GDALRasterBand* band, int width, int height) {
  std::vector<float> values(width * height, -9999.0f);
  if (band != nullptr) {
    EXPECT_EQ(CE_None,
              band->RasterIO(GF_Read, 0, 0, width, height, values.data(), width,
                             height, GDT_Float32, 0, 0, nullptr));
  }
  return values;
}

class GdalProximityTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }
};

TEST_F(GdalProximityTest, NullPointers) {
  GDALDatasetUniquePtr ds = CreateMemDataset(3, 3, GDT_Float32);
  ASSERT_NE(nullptr, ds);
  GDALRasterBand* band = ds->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALComputeProximity(nullptr, band, nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALComputeProximity(band, nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

TEST_F(GdalProximityTest, SizeMismatch) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(3, 3, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr prox_ds_x = CreateMemDataset(4, 3, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds_x);
  GDALDatasetUniquePtr prox_ds_y = CreateMemDataset(3, 4, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds_y);

  GDALRasterBand* src_band = src_ds->GetRasterBand(1);
  GDALRasterBand* prox_band_x = prox_ds_x->GetRasterBand(1);
  GDALRasterBand* prox_band_y = prox_ds_y->GetRasterBand(1);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALComputeProximity(src_band, prox_band_x, nullptr,
                                               nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Source and proximity bands are not the same size"));
    CPLErrorReset();
  }

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALComputeProximity(src_band, prox_band_y, nullptr,
                                               nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Source and proximity bands are not the same size"));
    CPLErrorReset();
  }
}

TEST_F(GdalProximityTest, InvalidDistUnits) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(3, 3, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr prox_ds = CreateMemDataset(3, 3, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds);

  CPLStringList options;
  options.SetNameValue("DISTUNITS", "INVALID_UNIT");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_Failure, GDALComputeProximity(src_ds->GetRasterBand(1),
                                             prox_ds->GetRasterBand(1),
                                             options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unrecognized DISTUNITS value"));
  CPLErrorReset();
}

TEST_F(GdalProximityTest, UnsignedProximityBandRequiresGTiff) {
  GDALDatasetUniquePtr src_ds = CreateMemDataset(3, 3, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);

  // GDT_Byte working proximity requires GTiff driver, which is not registered.
  GDALDatasetUniquePtr prox_byte_ds = CreateMemDataset(3, 3, GDT_Byte);
  ASSERT_NE(nullptr, prox_byte_ds);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALComputeProximity(src_ds->GetRasterBand(1),
                                               prox_byte_ds->GetRasterBand(1),
                                               nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GDALComputeProximity needs GTiff driver"));
    CPLErrorReset();
  }

  // GDT_UInt16 working proximity also requires GTiff driver.
  GDALDatasetUniquePtr prox_uint16_ds = CreateMemDataset(3, 3, GDT_UInt16);
  ASSERT_NE(nullptr, prox_uint16_ds);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALComputeProximity(src_ds->GetRasterBand(1),
                                               prox_uint16_ds->GetRasterBand(1),
                                               nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GDALComputeProximity needs GTiff driver"));
    CPLErrorReset();
  }

  // GDT_UInt32 working proximity also requires GTiff driver.
  GDALDatasetUniquePtr prox_uint32_ds = CreateMemDataset(3, 3, GDT_UInt32);
  ASSERT_NE(nullptr, prox_uint32_ds);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALComputeProximity(src_ds->GetRasterBand(1),
                                               prox_uint32_ds->GetRasterBand(1),
                                               nullptr, nullptr, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("GDALComputeProximity needs GTiff driver"));
    CPLErrorReset();
  }
}

TEST_F(GdalProximityTest, ProgressCancellation) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr prox_ds = CreateMemDataset(kWidth, kHeight, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds);

  std::vector<int32_t> src_data(kWidth * kHeight, 0);
  src_data[0] = 1;
  WriteBandInts(src_ds->GetRasterBand(1), kWidth, kHeight, src_data);

  // 1. Cancel at initialization (dfComplete == 0.0).
  auto cancel_at_start = [](double /*dfComplete*/, const char* /*pszMessage*/,
                            void* /*pData*/) -> int { return 0; };
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure,
              GDALComputeProximity(src_ds->GetRasterBand(1),
                                   prox_ds->GetRasterBand(1), nullptr,
                                   cancel_at_start, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User terminated"));
    CPLErrorReset();
  }

  // 2. Cancel during pass 1 (top to bottom).
  auto cancel_pass1 = [](double dfComplete, const char* /*pszMessage*/,
                         void* /*pData*/) -> int {
    return dfComplete > 0.1 ? 0 : 1;
  };
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALComputeProximity(src_ds->GetRasterBand(1),
                                               prox_ds->GetRasterBand(1),
                                               nullptr, cancel_pass1, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User terminated"));
    CPLErrorReset();
  }

  // 3. Cancel during pass 2 (bottom to top).
  auto cancel_pass2 = [](double dfComplete, const char* /*pszMessage*/,
                         void* /*pData*/) -> int {
    return dfComplete > 0.6 ? 0 : 1;
  };
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, GDALComputeProximity(src_ds->GetRasterBand(1),
                                               prox_ds->GetRasterBand(1),
                                               nullptr, cancel_pass2, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("User terminated"));
    CPLErrorReset();
  }
}

TEST_F(GdalProximityTest, BasicProximityCalculation) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr prox_ds = CreateMemDataset(kWidth, kHeight, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds);

  // Target at center (1, 1).
  std::vector<int32_t> src_data = {
      0, 0, 0, 0, 1, 0, 0, 0, 0,
  };
  WriteBandInts(src_ds->GetRasterBand(1), kWidth, kHeight, src_data);

  EXPECT_EQ(CE_None, GDALComputeProximity(src_ds->GetRasterBand(1),
                                          prox_ds->GetRasterBand(1), nullptr,
                                          GDALDummyProgress, nullptr));

  std::vector<float> prox =
      ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
  const float sqrt2 = std::sqrt(2.0f);
  EXPECT_NEAR(sqrt2, prox[0], 1e-4);
  EXPECT_NEAR(1.0f, prox[1], 1e-4);
  EXPECT_NEAR(sqrt2, prox[2], 1e-4);

  EXPECT_NEAR(1.0f, prox[3], 1e-4);
  EXPECT_NEAR(0.0f, prox[4], 1e-4);
  EXPECT_NEAR(1.0f, prox[5], 1e-4);

  EXPECT_NEAR(sqrt2, prox[6], 1e-4);
  EXPECT_NEAR(1.0f, prox[7], 1e-4);
  EXPECT_NEAR(sqrt2, prox[8], 1e-4);
}

TEST_F(GdalProximityTest, AllTargetsAndNoTargets) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr prox_ds = CreateMemDataset(kWidth, kHeight, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds);

  // 1. All targets -> all distances 0.
  std::vector<int32_t> all_targets(kWidth * kHeight, 1);
  WriteBandInts(src_ds->GetRasterBand(1), kWidth, kHeight, all_targets);
  EXPECT_EQ(CE_None, GDALComputeProximity(src_ds->GetRasterBand(1),
                                          prox_ds->GetRasterBand(1), nullptr,
                                          nullptr, nullptr));
  std::vector<float> prox =
      ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    EXPECT_FLOAT_EQ(0.0f, prox[i]);
  }

  // 2. No targets -> all distances default NODATA (65535).
  std::vector<int32_t> no_targets(kWidth * kHeight, 0);
  WriteBandInts(src_ds->GetRasterBand(1), kWidth, kHeight, no_targets);
  EXPECT_EQ(CE_None, GDALComputeProximity(src_ds->GetRasterBand(1),
                                          prox_ds->GetRasterBand(1), nullptr,
                                          nullptr, nullptr));
  prox = ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    EXPECT_FLOAT_EQ(65535.0f, prox[i]);
  }
}

TEST_F(GdalProximityTest, MaxDistOption) {
  constexpr int kWidth = 5;
  constexpr int kHeight = 5;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr prox_ds = CreateMemDataset(kWidth, kHeight, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds);

  // Target at top-left (0, 0).
  std::vector<int32_t> src_data(kWidth * kHeight, 0);
  src_data[0] = 1;
  WriteBandInts(src_ds->GetRasterBand(1), kWidth, kHeight, src_data);

  CPLStringList options;
  options.SetNameValue("MAXDIST", "2");

  EXPECT_EQ(CE_None, GDALComputeProximity(src_ds->GetRasterBand(1),
                                          prox_ds->GetRasterBand(1),
                                          options.List(), nullptr, nullptr));

  std::vector<float> prox =
      ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
  EXPECT_FLOAT_EQ(0.0f, prox[0]);         // (0, 0)
  EXPECT_NEAR(1.0f, prox[1], 1e-4);       // (1, 0)
  EXPECT_NEAR(2.0f, prox[2], 1e-4);       // (2, 0)
  EXPECT_FLOAT_EQ(65535.0f, prox[3]);     // (3, 0) beyond MAXDIST
  EXPECT_NEAR(1.0f, prox[kWidth], 1e-4);  // (0, 1)
  EXPECT_NEAR(std::sqrt(2.0f), prox[kWidth + 1], 1e-4);   // (1, 1)
  EXPECT_FLOAT_EQ(65535.0f, prox[kWidth * kHeight - 1]);  // (4, 4)
}

TEST_F(GdalProximityTest, NoDataOptions) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr prox_ds = CreateMemDataset(kWidth, kHeight, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds);

  std::vector<int32_t> src_data(kWidth * kHeight, 0);
  src_data[0] = 1;
  WriteBandInts(src_ds->GetRasterBand(1), kWidth, kHeight, src_data);

  // 1. Explicit NODATA option in options list.
  {
    CPLStringList options;
    options.SetNameValue("MAXDIST", "1");
    options.SetNameValue("NODATA", "-999");
    EXPECT_EQ(CE_None, GDALComputeProximity(src_ds->GetRasterBand(1),
                                            prox_ds->GetRasterBand(1),
                                            options.List(), nullptr, nullptr));
    std::vector<float> prox =
        ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
    EXPECT_FLOAT_EQ(0.0f, prox[0]);
    EXPECT_NEAR(1.0f, prox[1], 1e-4);
    EXPECT_FLOAT_EQ(-999.0f, prox[2]);
    EXPECT_FLOAT_EQ(-999.0f, prox[kWidth * kHeight - 1]);
  }

  // 2. NODATA not provided in options, but proximity band has a nodata value.
  {
    EXPECT_EQ(CE_None, prox_ds->GetRasterBand(1)->SetNoDataValue(-50.0));
    CPLStringList options;
    options.SetNameValue("MAXDIST", "1");
    EXPECT_EQ(CE_None, GDALComputeProximity(src_ds->GetRasterBand(1),
                                            prox_ds->GetRasterBand(1),
                                            options.List(), nullptr, nullptr));
    std::vector<float> prox =
        ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
    EXPECT_FLOAT_EQ(0.0f, prox[0]);
    EXPECT_NEAR(1.0f, prox[1], 1e-4);
    EXPECT_FLOAT_EQ(-50.0f, prox[2]);
    EXPECT_FLOAT_EQ(-50.0f, prox[kWidth * kHeight - 1]);
  }
}

TEST_F(GdalProximityTest, FixedBufValOption) {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr prox_ds = CreateMemDataset(kWidth, kHeight, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds);

  std::vector<int32_t> src_data(kWidth * kHeight, 0);
  src_data[0] = 1;
  WriteBandInts(src_ds->GetRasterBand(1), kWidth, kHeight, src_data);

  CPLStringList options;
  options.SetNameValue("MAXDIST", "2");
  options.SetNameValue("FIXED_BUF_VAL", "42");
  options.SetNameValue("NODATA", "0");

  EXPECT_EQ(CE_None, GDALComputeProximity(src_ds->GetRasterBand(1),
                                          prox_ds->GetRasterBand(1),
                                          options.List(), nullptr, nullptr));

  std::vector<float> prox =
      ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
  // Target itself remains 0.0.
  EXPECT_FLOAT_EQ(0.0f, prox[0]);
  // Pixels within MAXDIST receive fixed value 42.0.
  EXPECT_FLOAT_EQ(42.0f, prox[1]);
  EXPECT_FLOAT_EQ(42.0f, prox[2]);
  EXPECT_FLOAT_EQ(42.0f, prox[kWidth]);
  EXPECT_FLOAT_EQ(42.0f, prox[kWidth + 1]);
  // Pixels beyond MAXDIST receive NODATA (0.0).
  EXPECT_FLOAT_EQ(0.0f, prox[3]);
  EXPECT_FLOAT_EQ(0.0f, prox[kWidth * kHeight - 1]);
}

TEST_F(GdalProximityTest, ValuesOption) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr prox_ds = CreateMemDataset(kWidth, kHeight, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds);

  // Src has values 1, 2, 3:
  // 0, 1, 0
  // 0, 3, 0
  // 0, 2, 0
  std::vector<int32_t> src_data = {
      0, 1, 0, 0, 3, 0, 0, 2, 0,
  };
  WriteBandInts(src_ds->GetRasterBand(1), kWidth, kHeight, src_data);

  // Target values are 1 and 2 only; 3 is ignored as target.
  CPLStringList options;
  options.SetNameValue("VALUES", "1,2");

  EXPECT_EQ(CE_None, GDALComputeProximity(src_ds->GetRasterBand(1),
                                          prox_ds->GetRasterBand(1),
                                          options.List(), nullptr, nullptr));

  std::vector<float> prox =
      ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
  EXPECT_NEAR(0.0f, prox[1], 1e-4);  // (1, 0) value 1 -> target
  EXPECT_NEAR(0.0f, prox[7], 1e-4);  // (1, 2) value 2 -> target
  EXPECT_NEAR(1.0f, prox[4],
              1e-4);  // (1, 1) value 3 -> distance 1.0 to targets
  EXPECT_NEAR(1.0f, prox[0], 1e-4);  // (0, 0) -> distance 1.0 to (1, 0)
}

TEST_F(GdalProximityTest, UseInputNoData) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr prox_ds = CreateMemDataset(kWidth, kHeight, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds);

  // 1. USE_INPUT_NODATA=YES and src band has nodata value set.
  EXPECT_EQ(CE_None, src_ds->GetRasterBand(1)->SetNoDataValue(99.0));
  EXPECT_EQ(CE_None, prox_ds->GetRasterBand(1)->SetNoDataValue(-999.0));

  std::vector<int32_t> src_data = {
      0, 0, 0, 0, 1, 99, 0, 0, 0,
  };
  WriteBandInts(src_ds->GetRasterBand(1), kWidth, kHeight, src_data);

  {
    CPLStringList options;
    options.SetNameValue("VALUES", "1");
    options.SetNameValue("USE_INPUT_NODATA", "YES");
    EXPECT_EQ(CE_None, GDALComputeProximity(src_ds->GetRasterBand(1),
                                            prox_ds->GetRasterBand(1),
                                            options.List(), nullptr, nullptr));

    std::vector<float> prox =
        ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
    EXPECT_NEAR(0.0f, prox[4], 1e-4);   // (1, 1) is target
    EXPECT_FLOAT_EQ(-999.0f, prox[5]);  // (2, 1) has input nodata, remains -999
    EXPECT_NEAR(1.0f, prox[3], 1e-4);   // (0, 1) normal distance 1.0
  }

  // 2. USE_INPUT_NODATA=YES when src band has NO nodata set.
  GDALDatasetUniquePtr src_ds_no_nd =
      CreateMemDataset(kWidth, kHeight, GDT_Int32);
  ASSERT_NE(nullptr, src_ds_no_nd);
  WriteBandInts(src_ds_no_nd->GetRasterBand(1), kWidth, kHeight, src_data);
  {
    CPLStringList options;
    options.SetNameValue("VALUES", "1");
    options.SetNameValue("USE_INPUT_NODATA", "YES");
    EXPECT_EQ(CE_None, GDALComputeProximity(src_ds_no_nd->GetRasterBand(1),
                                            prox_ds->GetRasterBand(1),
                                            options.List(), nullptr, nullptr));
    std::vector<float> prox =
        ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
    EXPECT_NEAR(0.0f, prox[4], 1e-4);
    // (2, 1) is not treated as nodata because src band has no nodata set.
    EXPECT_NEAR(1.0f, prox[5], 1e-4);
  }

  // 3. USE_INPUT_NODATA=NO.
  {
    CPLStringList options;
    options.SetNameValue("VALUES", "1");
    options.SetNameValue("USE_INPUT_NODATA", "NO");
    EXPECT_EQ(CE_None, GDALComputeProximity(src_ds->GetRasterBand(1),
                                            prox_ds->GetRasterBand(1),
                                            options.List(), nullptr, nullptr));
    std::vector<float> prox =
        ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
    EXPECT_NEAR(0.0f, prox[4], 1e-4);
    EXPECT_NEAR(1.0f, prox[5], 1e-4);
  }
}

TEST_F(GdalProximityTest, DistUnitsGeoSquarePixels) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr prox_ds = CreateMemDataset(kWidth, kHeight, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds);

  // Square pixel geotransform (10 meters per pixel).
  double geo_transform[6] = {100.0, 10.0, 0.0, 200.0, 0.0, -10.0};
  EXPECT_EQ(CE_None, src_ds->SetGeoTransform(geo_transform));

  // Target at center (1, 1).
  std::vector<int32_t> src_data = {
      0, 0, 0, 0, 1, 0, 0, 0, 0,
  };
  WriteBandInts(src_ds->GetRasterBand(1), kWidth, kHeight, src_data);

  CPLStringList options;
  options.SetNameValue("DISTUNITS", "GEO");

  EXPECT_EQ(CE_None, GDALComputeProximity(src_ds->GetRasterBand(1),
                                          prox_ds->GetRasterBand(1),
                                          options.List(), nullptr, nullptr));

  std::vector<float> prox =
      ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
  // Center is 0.
  EXPECT_FLOAT_EQ(0.0f, prox[4]);
  // Distance 1 pixel * 10 = 10.0 meters.
  EXPECT_NEAR(10.0f, prox[1], 1e-4);
  EXPECT_NEAR(10.0f, prox[3], 1e-4);
  EXPECT_NEAR(10.0f, prox[5], 1e-4);
  EXPECT_NEAR(10.0f, prox[7], 1e-4);
  // Diagonal distance sqrt(2) * 10 = 14.1421... meters.
  const float expected_diag = std::sqrt(2.0f) * 10.0f;
  EXPECT_NEAR(expected_diag, prox[0], 1e-4);
  EXPECT_NEAR(expected_diag, prox[2], 1e-4);
  EXPECT_NEAR(expected_diag, prox[6], 1e-4);
  EXPECT_NEAR(expected_diag, prox[8], 1e-4);
}

TEST_F(GdalProximityTest, DistUnitsGeoNonSquarePixels) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr prox_ds = CreateMemDataset(kWidth, kHeight, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds);

  // Non-square pixel geotransform: |10.0| != |-20.0|.
  double geo_transform[6] = {100.0, 10.0, 0.0, 200.0, 0.0, -20.0};
  EXPECT_EQ(CE_None, src_ds->SetGeoTransform(geo_transform));

  std::vector<int32_t> src_data = {
      0, 0, 0, 0, 1, 0, 0, 0, 0,
  };
  WriteBandInts(src_ds->GetRasterBand(1), kWidth, kHeight, src_data);

  CPLStringList options;
  options.SetNameValue("DISTUNITS", "GEO");

  WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(CE_None, GDALComputeProximity(src_ds->GetRasterBand(1),
                                          prox_ds->GetRasterBand(1),
                                          options.List(), nullptr, nullptr));
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Pixels not square"));
  CPLErrorReset();

  std::vector<float> prox =
      ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
  EXPECT_FLOAT_EQ(0.0f, prox[4]);
  // dfDistMult is |adfGeoTransform[1]| = 10.0.
  EXPECT_NEAR(10.0f, prox[1], 1e-4);
}

TEST_F(GdalProximityTest, DistUnitsGeoWithMaxDist) {
  constexpr int kWidth = 5;
  constexpr int kHeight = 5;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr prox_ds = CreateMemDataset(kWidth, kHeight, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds);

  double geo_transform[6] = {0.0, 10.0, 0.0, 0.0, 0.0, -10.0};
  EXPECT_EQ(CE_None, src_ds->SetGeoTransform(geo_transform));

  std::vector<int32_t> src_data(kWidth * kHeight, 0);
  src_data[0] = 1;
  WriteBandInts(src_ds->GetRasterBand(1), kWidth, kHeight, src_data);

  CPLStringList options;
  options.SetNameValue("DISTUNITS", "GEO");
  options.SetNameValue("MAXDIST", "15");  // 15 meters -> 1.5 pixels

  EXPECT_EQ(CE_None, GDALComputeProximity(src_ds->GetRasterBand(1),
                                          prox_ds->GetRasterBand(1),
                                          options.List(), nullptr, nullptr));

  std::vector<float> prox =
      ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
  EXPECT_FLOAT_EQ(0.0f, prox[0]);
  // Distance 1 pixel = 10.0 meters <= 15.0 meters.
  EXPECT_NEAR(10.0f, prox[1], 1e-4);
  // Distance sqrt(2) pixels ~ 14.142 meters <= 15.0 meters.
  EXPECT_NEAR(std::sqrt(2.0f) * 10.0f, prox[kWidth + 1], 1e-4);
  // Distance 2 pixels = 20.0 meters > 15.0 meters -> NODATA.
  EXPECT_FLOAT_EQ(65535.0f, prox[2]);
}

TEST_F(GdalProximityTest, DistUnitsPixelExplicit) {
  constexpr int kWidth = 3;
  constexpr int kHeight = 3;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr prox_ds = CreateMemDataset(kWidth, kHeight, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds);

  std::vector<int32_t> src_data = {
      0, 0, 0, 0, 1, 0, 0, 0, 0,
  };
  WriteBandInts(src_ds->GetRasterBand(1), kWidth, kHeight, src_data);

  CPLStringList options;
  options.SetNameValue("DISTUNITS", "PIXEL");

  EXPECT_EQ(CE_None, GDALComputeProximity(src_ds->GetRasterBand(1),
                                          prox_ds->GetRasterBand(1),
                                          options.List(), nullptr, nullptr));

  std::vector<float> prox =
      ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
  EXPECT_NEAR(1.0f, prox[1], 1e-4);
  EXPECT_NEAR(std::sqrt(2.0f), prox[0], 1e-4);
}

TEST_F(GdalProximityTest, BottomUpPassProximity) {
  constexpr int kWidth = 5;
  constexpr int kHeight = 5;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr prox_ds = CreateMemDataset(kWidth, kHeight, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds);

  // Target at bottom-right corner (4, 4).
  std::vector<int32_t> src_data(kWidth * kHeight, 0);
  src_data[kWidth * kHeight - 1] = 1;
  WriteBandInts(src_ds->GetRasterBand(1), kWidth, kHeight, src_data);

  EXPECT_EQ(CE_None, GDALComputeProximity(src_ds->GetRasterBand(1),
                                          prox_ds->GetRasterBand(1), nullptr,
                                          nullptr, nullptr));

  std::vector<float> prox =
      ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
  EXPECT_FLOAT_EQ(0.0f, prox[kWidth * kHeight - 1]);
  EXPECT_NEAR(1.0f, prox[kWidth * kHeight - 2], 1e-4);
  EXPECT_NEAR(1.0f, prox[kWidth * (kHeight - 1) - 1], 1e-4);
  EXPECT_NEAR(std::sqrt(2.0f), prox[kWidth * (kHeight - 1) - 2], 1e-4);
  // Distance to top-left (0, 0) is sqrt(4^2 + 4^2) = sqrt(32) ~ 5.65685.
  EXPECT_NEAR(std::sqrt(32.0f), prox[0], 1e-4);
}

TEST_F(GdalProximityTest, TallImageExceedingMaxDistSquared) {
  // A narrow and tall raster exercises the branch where distance squared
  // exceeds near distance threshold: dfDistSq >= dfNearDistSq in
  // ProcessProximityLine.
  constexpr int kWidth = 2;
  constexpr int kHeight = 30;
  GDALDatasetUniquePtr src_ds = CreateMemDataset(kWidth, kHeight, GDT_Int32);
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetUniquePtr prox_ds = CreateMemDataset(kWidth, kHeight, GDT_Float32);
  ASSERT_NE(nullptr, prox_ds);

  std::vector<int32_t> src_data(kWidth * kHeight, 0);
  src_data[0] = 1;  // Target at (0, 0).
  WriteBandInts(src_ds->GetRasterBand(1), kWidth, kHeight, src_data);

  CPLStringList options;
  options.SetNameValue("MAXDIST", "1");

  EXPECT_EQ(CE_None, GDALComputeProximity(src_ds->GetRasterBand(1),
                                          prox_ds->GetRasterBand(1),
                                          options.List(), nullptr, nullptr));

  std::vector<float> prox =
      ReadBandFloats(prox_ds->GetRasterBand(1), kWidth, kHeight);
  EXPECT_FLOAT_EQ(0.0f, prox[0]);
  EXPECT_NEAR(1.0f, prox[1], 1e-4);
  EXPECT_NEAR(1.0f, prox[2], 1e-4);
  // Lines farther away receive default NODATA (65535).
  EXPECT_FLOAT_EQ(65535.0f, prox[kWidth * kHeight - 1]);
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Remaining uncovered branches are OOM guards and defensive
// error paths.
