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
// Tests GDALMDArray::GetMeshGrid (gcore/gdalmultidim_meshgrid.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalmultidim_meshgrid.cpp

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;

TEST(GdalMultidimMeshgridTest, MeshGridXYAndIJAndStrings) {
  GDALRegister_MEM();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  GDALDatasetUniquePtr ds(
      mem_driver->CreateMultiDimensional("test_mg", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  std::shared_ptr<GDALGroup> root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto dim_x = root->CreateDimension("x", "", "", 2);
  auto dim_y = root->CreateDimension("y", "", "", 3);
  auto dim_z = root->CreateDimension("z", "", "", 2);

  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto ar_x = root->CreateMDArray("x", {dim_x}, dt_f64, nullptr);
  ASSERT_NE(nullptr, ar_x);
  auto ar_y = root->CreateMDArray("y", {dim_y}, dt_f64, nullptr);
  ASSERT_NE(nullptr, ar_y);

  constexpr auto kValsX = std::to_array<double>({1.0, 2.0});
  constexpr auto kValsY = std::to_array<double>({3.0, 4.0, 5.0});
  constexpr auto kStart0 = std::to_array<GUInt64>({0});
  constexpr auto kCountX = std::to_array<size_t>({2});
  constexpr auto kCountY = std::to_array<size_t>({3});
  ASSERT_TRUE(ar_x->Write(kStart0.data(), kCountX.data(), nullptr, nullptr,
                          dt_f64, kValsX.data()));
  ASSERT_TRUE(ar_y->Write(kStart0.data(), kCountY.data(), nullptr, nullptr,
                          dt_f64, kValsY.data()));

  EXPECT_TRUE(ar_x->SetUnit("deg_east"));
  EXPECT_TRUE(ar_x->SetOffset(10.0));
  EXPECT_TRUE(ar_x->SetScale(2.0));
  constexpr double kNoData = -999.0;
  EXPECT_TRUE(ar_x->SetRawNoDataValue(&kNoData));
  auto attr = ar_x->CreateAttribute("attr1", {}, dt_f64, nullptr);
  ASSERT_NE(nullptr, attr);

  // 1. Default INDEXING=xy on 2D.
  auto grid_xy = GDALMDArray::GetMeshGrid({ar_x, ar_y}, nullptr);
  ASSERT_EQ(2u, grid_xy.size());
  EXPECT_FALSE(grid_xy[0]->IsWritable());
  EXPECT_EQ(ar_x->GetFilename(), grid_xy[0]->GetFilename());
  EXPECT_EQ(2u, grid_xy[0]->GetDimensions().size());
  EXPECT_EQ(dt_f64, grid_xy[0]->GetDataType());
  EXPECT_NE(nullptr, grid_xy[0]->GetAttribute("attr1"));
  EXPECT_EQ(1u, grid_xy[0]->GetAttributes().size());
  EXPECT_EQ("deg_east", grid_xy[0]->GetUnit());
  EXPECT_NE(nullptr, grid_xy[0]->GetRawNoDataValue());
  bool has_offset = false;
  GDALDataType storage_type = GDT_Unknown;
  EXPECT_DOUBLE_EQ(10.0, grid_xy[0]->GetOffset(&has_offset, &storage_type));
  EXPECT_TRUE(has_offset);
  bool has_scale = false;
  EXPECT_DOUBLE_EQ(2.0, grid_xy[0]->GetScale(&has_scale, &storage_type));
  EXPECT_TRUE(has_scale);

  // Shape for xy is (N_y=3, N_x=2).
  constexpr auto kStart2d = std::to_array<GUInt64>({0, 0});
  constexpr auto kCountXY = std::to_array<size_t>({3, 2});
  std::array<double, 6> xm_xy = {};
  std::array<double, 6> ym_xy = {};
  EXPECT_TRUE(grid_xy[0]->Read(kStart2d.data(), kCountXY.data(), nullptr,
                               nullptr, dt_f64, xm_xy.data()));
  EXPECT_TRUE(grid_xy[1]->Read(kStart2d.data(), kCountXY.data(), nullptr,
                               nullptr, dt_f64, ym_xy.data()));
  EXPECT_THAT(xm_xy, ElementsAre(1.0, 2.0, 1.0, 2.0, 1.0, 2.0));
  EXPECT_THAT(ym_xy, ElementsAre(3.0, 3.0, 4.0, 4.0, 5.0, 5.0));

  // 2. INDEXING=ij on 2D.
  CPLStringList ij_opts;
  ij_opts.SetNameValue("INDEXING", "ij");
  auto grid_ij = GDALMDArray::GetMeshGrid({ar_x, ar_y}, ij_opts.List());
  ASSERT_EQ(2u, grid_ij.size());
  constexpr auto kCountIJ = std::to_array<size_t>({2, 3});
  std::array<double, 6> xm_ij = {};
  std::array<double, 6> ym_ij = {};
  EXPECT_TRUE(grid_ij[0]->Read(kStart2d.data(), kCountIJ.data(), nullptr,
                               nullptr, dt_f64, xm_ij.data()));
  EXPECT_TRUE(grid_ij[1]->Read(kStart2d.data(), kCountIJ.data(), nullptr,
                               nullptr, dt_f64, ym_ij.data()));
  EXPECT_THAT(xm_ij, ElementsAre(1.0, 1.0, 1.0, 2.0, 2.0, 2.0));
  EXPECT_THAT(ym_ij, ElementsAre(3.0, 4.0, 5.0, 3.0, 4.0, 5.0));

  // 3. String array (NeedsFreeDynamicMemory() == true).
  auto dt_str = GDALExtendedDataType::CreateString();
  auto ar_s = root->CreateMDArray("s", {dim_z}, dt_str, nullptr);
  ASSERT_NE(nullptr, ar_s);
  constexpr auto kSVals = std::to_array<const char*>({"alpha", "beta"});
  ASSERT_TRUE(ar_s->Write(kStart0.data(), kCountX.data(), nullptr, nullptr,
                          dt_str, kSVals.data()));

  auto grid_s = GDALMDArray::GetMeshGrid({ar_s, ar_x}, ij_opts.List());
  ASSERT_EQ(2u, grid_s.size());
  constexpr auto kCountSX = std::to_array<size_t>({2, 2});
  std::array<char*, 4> out_strs = {};
  absl::Cleanup free_strs = [&out_strs] {
    for (char* p : out_strs) {
      CPLFree(p);
    }
  };
  ASSERT_TRUE(grid_s[0]->Read(kStart2d.data(), kCountSX.data(), nullptr,
                              nullptr, dt_str, out_strs.data()));
  EXPECT_STREQ("alpha", out_strs[0]);
  EXPECT_STREQ("alpha", out_strs[1]);
  EXPECT_STREQ("beta", out_strs[2]);
  EXPECT_STREQ("beta", out_strs[3]);

  // 4. Error cases: non-1D input array and invalid INDEXING option.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_TRUE(GDALMDArray::GetMeshGrid({grid_xy[0]}, nullptr).empty());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

    CPLErrorReset();
    CPLStringList bad_opts;
    bad_opts.SetNameValue("INDEXING", "invalid");
    EXPECT_TRUE(
        GDALMDArray::GetMeshGrid({ar_x, ar_y}, bad_opts.List()).empty());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
