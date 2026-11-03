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
// Tests multidim GLT orthorectification
// (gcore/gdalmultidim_gltorthorectification.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalmultidim_gltorthorectification.cpp

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;

class GltHelper : public GDALMDArray {
 public:
  using GDALMDArray::CreateGLTOrthorectified;
};

TEST(GdalMultidimGltOrthorectificationTest, Ortho2DAnd3D) {
  GDALRegister_MEM();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->CreateMultiDimensional("test", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  std::shared_ptr<GDALGroup> rg = ds->GetRootGroup();
  ASSERT_NE(nullptr, rg);

  auto dim_y = rg->CreateDimension("y", "", "", 2);
  auto dim_x = rg->CreateDimension("x", "", "", 2);
  auto dim_b = rg->CreateDimension("bands", "", "", 2);

  auto glt_dim_y = rg->CreateDimension("glt_y_dim", "", "", 2);
  auto glt_dim_x = rg->CreateDimension("glt_x_dim", "", "", 2);

  auto parent_3d =
      rg->CreateMDArray("parent3d", {dim_y, dim_x, dim_b},
                        GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, parent_3d);
  constexpr int32_t kNoData = -999;
  EXPECT_TRUE(parent_3d->SetRawNoDataValue(&kNoData));
  EXPECT_TRUE(parent_3d->SetUnit("W/m2"));
  EXPECT_TRUE(parent_3d->SetOffset(1.5));
  EXPECT_TRUE(parent_3d->SetScale(2.5));
  auto attr = parent_3d->CreateAttribute(
      "desc", {}, GDALExtendedDataType::CreateString(), nullptr);
  ASSERT_NE(nullptr, attr);

  // Fill parent_3d: shape (2, 2, 2).
  constexpr auto kParentVals =
      std::to_array<int32_t>({10, 11, 20, 21, 30, 31, 40, 41});
  constexpr auto kStart3 = std::to_array<GUInt64>({0, 0, 0});
  constexpr auto kCount3 = std::to_array<size_t>({2, 2, 2});
  ASSERT_TRUE(parent_3d->Write(kStart3.data(), kCount3.data(), nullptr, nullptr,
                               GDALExtendedDataType::Create(GDT_Int32),
                               kParentVals.data()));

  // Create /sensor_band_parameters/good_wavelengths with [1, 0].
  auto sbp = rg->CreateGroup("sensor_band_parameters");
  ASSERT_NE(nullptr, sbp);
  auto good_wl =
      sbp->CreateMDArray("good_wavelengths", {dim_b},
                         GDALExtendedDataType::Create(GDT_Byte), nullptr);
  ASSERT_NE(nullptr, good_wl);
  constexpr auto kWlVals = std::to_array<GByte>({1, 0});
  constexpr auto kStart1 = std::to_array<GUInt64>({0});
  constexpr auto kCount1 = std::to_array<size_t>({2});
  ASSERT_TRUE(good_wl->Write(kStart1.data(), kCount1.data(), nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Byte),
                             kWlVals.data()));

  // Create GLT X and Y arrays (1-based index -> offset = -1).
  // Pixel (0,0) -> (1, 1) -> src (0,0)
  // Pixel (0,1) -> (2, 1) -> src (0,1)
  // Pixel (1,0) -> (0, 0) -> out of bounds (-1, -1) -> nodata
  // Pixel (1,1) -> (2, 2) -> src (1,1)
  auto glt_x =
      rg->CreateMDArray("glt_x", {glt_dim_y, glt_dim_x},
                        GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, glt_x);
  auto glt_y =
      rg->CreateMDArray("glt_y", {glt_dim_y, glt_dim_x},
                        GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, glt_y);
  constexpr auto kGxVals = std::to_array<int32_t>({1, 2, 0, 2});
  constexpr auto kGyVals = std::to_array<int32_t>({1, 1, 0, 2});
  constexpr auto kStart2 = std::to_array<GUInt64>({0, 0});
  constexpr auto kCount2 = std::to_array<size_t>({2, 2});
  ASSERT_TRUE(glt_x->Write(kStart2.data(), kCount2.data(), nullptr, nullptr,
                           GDALExtendedDataType::Create(GDT_Int32),
                           kGxVals.data()));
  ASSERT_TRUE(glt_y->Write(kStart2.data(), kCount2.data(), nullptr, nullptr,
                           GDALExtendedDataType::Create(GDT_Int32),
                           kGyVals.data()));

  const std::vector<double> gt = {-120.0, 0.01, 0.0, 35.0, 0.0, -0.01};
  auto ortho = GltHelper::CreateGLTOrthorectified(parent_3d, rg, glt_x, glt_y,
                                                  -1, gt, nullptr);
  ASSERT_NE(nullptr, ortho);

  // Metadata methods.
  EXPECT_FALSE(ortho->IsWritable());
  EXPECT_EQ(parent_3d->GetFilename(), ortho->GetFilename());
  EXPECT_EQ(3u, ortho->GetDimensions().size());
  EXPECT_EQ(GDT_Int32, ortho->GetDataType().GetNumericDataType());
  EXPECT_NE(nullptr, ortho->GetSpatialRef());
  EXPECT_EQ(3u, ortho->GetBlockSize().size());
  EXPECT_NE(nullptr, ortho->GetAttribute("desc"));
  EXPECT_EQ(1u, ortho->GetAttributes().size());
  EXPECT_EQ("W/m2", ortho->GetUnit());
  EXPECT_NE(nullptr, ortho->GetRawNoDataValue());
  bool has_val = false;
  GDALDataType storage_dt = GDT_Unknown;
  EXPECT_DOUBLE_EQ(1.5, ortho->GetOffset(&has_val, &storage_dt));
  EXPECT_TRUE(has_val);
  EXPECT_DOUBLE_EQ(2.5, ortho->GetScale(&has_val, &storage_dt));
  EXPECT_TRUE(has_val);

  // Non-numeric read fails without emitting CPLError.
  {
    CPLErrorReset();
    char* str_buf_raw = nullptr;
    constexpr auto kCountOne = std::to_array<size_t>({1, 1, 1});
    EXPECT_FALSE(ortho->Read(kStart3.data(), kCountOne.data(), nullptr, nullptr,
                             GDALExtendedDataType::CreateString(),
                             &str_buf_raw));
    std::unique_ptr<char, CplFreeDeleter> str_buf(str_buf_raw);
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
  }

  // Read all bands (band 0 is valid, band 1 is invalid -> filled with nodata).
  std::array<int32_t, 8> out_vals = {};
  ASSERT_TRUE(ortho->Read(kStart3.data(), kCount3.data(), nullptr, nullptr,
                          GDALExtendedDataType::Create(GDT_Int32),
                          out_vals.data()));
  EXPECT_THAT(out_vals, ElementsAre(10, -999, 20, -999, -999, -999, 40, -999));

  // Read only invalid band (band 1) -> hits nCountInvalid == count[2] early
  // exit.
  constexpr auto kStartInvBand = std::to_array<GUInt64>({0, 0, 1});
  constexpr auto kCountInvBand = std::to_array<size_t>({2, 2, 1});
  std::array<int32_t, 4> inv_vals = {};
  ASSERT_TRUE(ortho->Read(kStartInvBand.data(), kCountInvBand.data(), nullptr,
                          nullptr, GDALExtendedDataType::Create(GDT_Int32),
                          inv_vals.data()));
  EXPECT_THAT(inv_vals, ElementsAre(-999, -999, -999, -999));

  // Read only pixel (1,0) where GLT is out-of-bounds -> hits nMinX > nMaxX
  // early exit.
  constexpr auto kStartOob = std::to_array<GUInt64>({1, 0, 0});
  constexpr auto kCountOob = std::to_array<size_t>({1, 1, 1});
  int32_t oob_val = 0;
  ASSERT_TRUE(ortho->Read(kStartOob.data(), kCountOob.data(), nullptr, nullptr,
                          GDALExtendedDataType::Create(GDT_Int32), &oob_val));
  EXPECT_EQ(-999, oob_val);

  // Also test 2D parent array (no nodata set -> out-of-bounds zero-filled).
  auto parent_2d =
      rg->CreateMDArray("parent2d", {dim_y, dim_x},
                        GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, parent_2d);
  constexpr auto kP2dVals = std::to_array<int32_t>({100, 200, 300, 400});
  ASSERT_TRUE(parent_2d->Write(kStart2.data(), kCount2.data(), nullptr, nullptr,
                               GDALExtendedDataType::Create(GDT_Int32),
                               kP2dVals.data()));
  auto ortho_2d = GltHelper::CreateGLTOrthorectified(parent_2d, rg, glt_x,
                                                     glt_y, -1, gt, nullptr);
  ASSERT_NE(nullptr, ortho_2d);
  std::array<int32_t, 4> out_2d = {-1, -1, -1, -1};
  ASSERT_TRUE(ortho_2d->Read(kStart2.data(), kCount2.data(), nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Int32),
                             out_2d.data()));
  EXPECT_THAT(out_2d, ElementsAre(100, 200, 0, 400));
}

}  // namespace
}  // namespace autotest2
