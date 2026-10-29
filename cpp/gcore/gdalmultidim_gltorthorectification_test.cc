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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"

namespace gdal {
namespace {

class GltHelper : public GDALMDArray {
 public:
  using GDALMDArray::CreateGLTOrthorectified;
};

TEST(GdalMultidimGltOrthorectificationTest, Ortho2DAnd3D) {
  GDALAllRegister();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(drv, nullptr);
  std::unique_ptr<GDALDataset> ds(
      drv->CreateMultiDimensional("test", nullptr, nullptr));
  ASSERT_NE(ds, nullptr);
  auto rg = ds->GetRootGroup();
  ASSERT_NE(rg, nullptr);

  auto dim_y = rg->CreateDimension("y", "", "", 2);
  auto dim_x = rg->CreateDimension("x", "", "", 2);
  auto dim_b = rg->CreateDimension("bands", "", "", 2);

  auto glt_dim_y = rg->CreateDimension("glt_y_dim", "", "", 2);
  auto glt_dim_x = rg->CreateDimension("glt_x_dim", "", "", 2);

  auto parent_3d =
      rg->CreateMDArray("parent3d", {dim_y, dim_x, dim_b},
                        GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(parent_3d, nullptr);
  int32_t nodata = -999;
  EXPECT_TRUE(parent_3d->SetRawNoDataValue(&nodata));
  EXPECT_TRUE(parent_3d->SetUnit("W/m2"));
  EXPECT_TRUE(parent_3d->SetOffset(1.5));
  EXPECT_TRUE(parent_3d->SetScale(2.5));
  auto attr = parent_3d->CreateAttribute(
      "desc", {}, GDALExtendedDataType::CreateString(), nullptr);
  ASSERT_NE(attr, nullptr);

  // Fill parent_3d: shape (2, 2, 2)
  int32_t parent_vals[8] = {10, 11, 20, 21, 30, 31, 40, 41};
  const GUInt64 start3[3] = {0, 0, 0};
  const size_t count3[3] = {2, 2, 2};
  EXPECT_TRUE(parent_3d->Write(start3, count3, nullptr, nullptr,
                               GDALExtendedDataType::Create(GDT_Int32),
                               parent_vals));

  // Create /sensor_band_parameters/good_wavelengths with [1, 0]
  auto sbp = rg->CreateGroup("sensor_band_parameters");
  auto good_wl =
      sbp->CreateMDArray("good_wavelengths", {dim_b},
                         GDALExtendedDataType::Create(GDT_Byte), nullptr);
  GByte wl_vals[2] = {1, 0};
  const GUInt64 start1[1] = {0};
  const size_t count1[1] = {2};
  EXPECT_TRUE(good_wl->Write(start1, count1, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Byte), wl_vals));

  // Create GLT X and Y arrays (1-based index -> offset = -1)
  // Pixel (0,0) -> (1, 1) -> src (0,0)
  // Pixel (0,1) -> (2, 1) -> src (0,1)
  // Pixel (1,0) -> (0, 0) -> out of bounds (-1, -1) -> nodata!
  // Pixel (1,1) -> (2, 2) -> src (1,1)
  auto glt_x =
      rg->CreateMDArray("glt_x", {glt_dim_y, glt_dim_x},
                        GDALExtendedDataType::Create(GDT_Int32), nullptr);
  auto glt_y =
      rg->CreateMDArray("glt_y", {glt_dim_y, glt_dim_x},
                        GDALExtendedDataType::Create(GDT_Int32), nullptr);
  int32_t gx_vals[4] = {1, 2, 0, 2};
  int32_t gy_vals[4] = {1, 1, 0, 2};
  const GUInt64 start2[2] = {0, 0};
  const size_t count2[2] = {2, 2};
  EXPECT_TRUE(glt_x->Write(start2, count2, nullptr, nullptr,
                           GDALExtendedDataType::Create(GDT_Int32), gx_vals));
  EXPECT_TRUE(glt_y->Write(start2, count2, nullptr, nullptr,
                           GDALExtendedDataType::Create(GDT_Int32), gy_vals));

  std::vector<double> gt = {-120.0, 0.01, 0.0, 35.0, 0.0, -0.01};
  auto ortho = GltHelper::CreateGLTOrthorectified(parent_3d, rg, glt_x, glt_y,
                                                  -1, gt, nullptr);
  ASSERT_NE(ortho, nullptr);

  // Metadata methods
  EXPECT_FALSE(ortho->IsWritable());
  EXPECT_EQ(ortho->GetFilename(), parent_3d->GetFilename());
  EXPECT_EQ(ortho->GetDimensions().size(), 3u);
  EXPECT_EQ(ortho->GetDataType().GetNumericDataType(), GDT_Int32);
  EXPECT_NE(ortho->GetSpatialRef(), nullptr);
  EXPECT_EQ(ortho->GetBlockSize().size(), 3u);
  EXPECT_NE(ortho->GetAttribute("desc"), nullptr);
  EXPECT_EQ(ortho->GetAttributes().size(), 1u);
  EXPECT_EQ(ortho->GetUnit(), "W/m2");
  EXPECT_NE(ortho->GetRawNoDataValue(), nullptr);
  bool has_val = false;
  GDALDataType storage_dt = GDT_Unknown;
  EXPECT_DOUBLE_EQ(ortho->GetOffset(&has_val, &storage_dt), 1.5);
  EXPECT_TRUE(has_val);
  EXPECT_DOUBLE_EQ(ortho->GetScale(&has_val, &storage_dt), 2.5);
  EXPECT_TRUE(has_val);

  // Non-numeric read fails
  char* str_buf = nullptr;
  const size_t count_one[3] = {1, 1, 1};
  EXPECT_FALSE(ortho->Read(start3, count_one, nullptr, nullptr,
                           GDALExtendedDataType::CreateString(), &str_buf));

  // Read all bands (band 0 is valid, band 1 is invalid -> filled with nodata)
  int32_t out_vals[8] = {0};
  EXPECT_TRUE(ortho->Read(start3, count3, nullptr, nullptr,
                          GDALExtendedDataType::Create(GDT_Int32), out_vals));
  // (0,0) -> src (0,0): band 0 = 10, band 1 = -999
  EXPECT_EQ(out_vals[0], 10);
  EXPECT_EQ(out_vals[1], -999);
  // (0,1) -> src (0,1): band 0 = 20, band 1 = -999
  EXPECT_EQ(out_vals[2], 20);
  EXPECT_EQ(out_vals[3], -999);
  // (1,0) -> out of bounds: band 0 = -999, band 1 = -999
  EXPECT_EQ(out_vals[4], -999);
  EXPECT_EQ(out_vals[5], -999);
  // (1,1) -> src (1,1): band 0 = 40, band 1 = -999
  EXPECT_EQ(out_vals[6], 40);
  EXPECT_EQ(out_vals[7], -999);

  // Read only invalid band (band 1) -> hits nCountInvalid == count[2] early
  // exit
  const GUInt64 start_inv_band[3] = {0, 0, 1};
  const size_t count_inv_band[3] = {2, 2, 1};
  int32_t inv_vals[4] = {0};
  EXPECT_TRUE(ortho->Read(start_inv_band, count_inv_band, nullptr, nullptr,
                          GDALExtendedDataType::Create(GDT_Int32), inv_vals));
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(inv_vals[i], -999);
  }

  // Read only pixel (1,0) where GLT is out-of-bounds -> hits nMinX > nMaxX
  // early exit
  const GUInt64 start_oob[3] = {1, 0, 0};
  const size_t count_oob[3] = {1, 1, 1};
  int32_t oob_val = 0;
  EXPECT_TRUE(ortho->Read(start_oob, count_oob, nullptr, nullptr,
                          GDALExtendedDataType::Create(GDT_Int32), &oob_val));
  EXPECT_EQ(oob_val, -999);

  // Also test 2D parent array
  auto parent_2d =
      rg->CreateMDArray("parent2d", {dim_y, dim_x},
                        GDALExtendedDataType::Create(GDT_Int32), nullptr);
  int32_t p2d_vals[4] = {100, 200, 300, 400};
  EXPECT_TRUE(parent_2d->Write(start2, count2, nullptr, nullptr,
                               GDALExtendedDataType::Create(GDT_Int32),
                               p2d_vals));
  auto ortho_2d = GltHelper::CreateGLTOrthorectified(parent_2d, rg, glt_x,
                                                     glt_y, -1, gt, nullptr);
  ASSERT_NE(ortho_2d, nullptr);
  int32_t out_2d[4] = {0};
  EXPECT_TRUE(ortho_2d->Read(start2, count2, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Int32), out_2d));
  EXPECT_EQ(out_2d[0], 100);
  EXPECT_EQ(out_2d[1], 200);
  EXPECT_EQ(out_2d[2], 0);
  EXPECT_EQ(out_2d[3], 400);
}

}  // namespace
}  // namespace gdal
