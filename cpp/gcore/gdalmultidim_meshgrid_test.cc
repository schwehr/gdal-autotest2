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
//
// Tests gcore/gdalmultidim_meshgrid.cpp.

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"

namespace {

TEST(GdalmultidimMeshgridTest, MeshGridXYAndIJAndStrings) {
  GDALAllRegister();
  GDALDriver* mem_driver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_driver);

  std::unique_ptr<GDALDataset> ds(
      mem_driver->CreateMultiDimensional("test_mg", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  std::shared_ptr<GDALGroup> root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto dim_x = root->CreateDimension("x", "", "", 2);
  auto dim_y = root->CreateDimension("y", "", "", 3);
  auto dim_z = root->CreateDimension("z", "", "", 2);

  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto ar_x = root->CreateMDArray("x", {dim_x}, dt_f64, nullptr);
  auto ar_y = root->CreateMDArray("y", {dim_y}, dt_f64, nullptr);

  double vals_x[2] = {1.0, 2.0};
  double vals_y[3] = {3.0, 4.0, 5.0};
  const GUInt64 start0[1] = {0};
  const size_t count_x[1] = {2};
  const size_t count_y[1] = {3};
  ASSERT_TRUE(ar_x->Write(start0, count_x, nullptr, nullptr, dt_f64, vals_x));
  ASSERT_TRUE(ar_y->Write(start0, count_y, nullptr, nullptr, dt_f64, vals_y));

  ar_x->SetUnit("deg_east");
  ar_x->SetOffset(10.0);
  ar_x->SetScale(2.0);
  double nodata = -999.0;
  ar_x->SetRawNoDataValue(&nodata);
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
  const GUInt64 start2d[2] = {0, 0};
  const size_t count_xy[2] = {3, 2};
  std::vector<double> xm_xy(6, 0.0);
  std::vector<double> ym_xy(6, 0.0);
  EXPECT_TRUE(grid_xy[0]->Read(start2d, count_xy, nullptr, nullptr, dt_f64,
                               xm_xy.data()));
  EXPECT_TRUE(grid_xy[1]->Read(start2d, count_xy, nullptr, nullptr, dt_f64,
                               ym_xy.data()));
  EXPECT_THAT(xm_xy, testing::ElementsAre(1.0, 2.0, 1.0, 2.0, 1.0, 2.0));
  EXPECT_THAT(ym_xy, testing::ElementsAre(3.0, 3.0, 4.0, 4.0, 5.0, 5.0));

  // 2. INDEXING=ij on 2D.
  CPLStringList ij_opts;
  ij_opts.SetNameValue("INDEXING", "ij");
  auto grid_ij = GDALMDArray::GetMeshGrid({ar_x, ar_y}, ij_opts.List());
  ASSERT_EQ(2u, grid_ij.size());
  const size_t count_ij[2] = {2, 3};
  std::vector<double> xm_ij(6, 0.0);
  std::vector<double> ym_ij(6, 0.0);
  EXPECT_TRUE(grid_ij[0]->Read(start2d, count_ij, nullptr, nullptr, dt_f64,
                               xm_ij.data()));
  EXPECT_TRUE(grid_ij[1]->Read(start2d, count_ij, nullptr, nullptr, dt_f64,
                               ym_ij.data()));
  EXPECT_THAT(xm_ij, testing::ElementsAre(1.0, 1.0, 1.0, 2.0, 2.0, 2.0));
  EXPECT_THAT(ym_ij, testing::ElementsAre(3.0, 4.0, 5.0, 3.0, 4.0, 5.0));

  // 3. String array (NeedsFreeDynamicMemory() == true).
  auto dt_str = GDALExtendedDataType::CreateString();
  auto ar_s = root->CreateMDArray("s", {dim_z}, dt_str, nullptr);
  const char* s_vals[2] = {"alpha", "beta"};
  ASSERT_TRUE(ar_s->Write(start0, count_x, nullptr, nullptr, dt_str, s_vals));

  auto grid_s = GDALMDArray::GetMeshGrid({ar_s, ar_x}, ij_opts.List());
  ASSERT_EQ(2u, grid_s.size());
  const size_t count_sx[2] = {2, 2};
  char* out_strs[4] = {nullptr, nullptr, nullptr, nullptr};
  EXPECT_TRUE(
      grid_s[0]->Read(start2d, count_sx, nullptr, nullptr, dt_str, out_strs));
  EXPECT_STREQ("alpha", out_strs[0]);
  EXPECT_STREQ("alpha", out_strs[1]);
  EXPECT_STREQ("beta", out_strs[2]);
  EXPECT_STREQ("beta", out_strs[3]);
  for (char* p : out_strs) {
    CPLFree(p);
  }

  // 4. Error cases: non-1D input array and invalid INDEXING option.
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_TRUE(GDALMDArray::GetMeshGrid({grid_xy[0]}, nullptr).empty());
  CPLStringList bad_opts;
  bad_opts.SetNameValue("INDEXING", "invalid");
  EXPECT_TRUE(GDALMDArray::GetMeshGrid({ar_x, ar_y}, bad_opts.List()).empty());
  CPLPopErrorHandler();
}

}  // namespace
