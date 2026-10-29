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
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"

namespace gdal {
namespace {

class UninitializedSelfGroup final : public GDALGroup {
 public:
  UninitializedSelfGroup() : GDALGroup("/", "uninit") {}
  std::vector<std::string> GetMDArrayNames(CSLConstList) const override {
    return {};
  }
  std::shared_ptr<GDALMDArray> OpenMDArray(const std::string&,
                                           CSLConstList) const override {
    return nullptr;
  }
};

TEST(GdalMultidimSubsetDimensionTest, ErrorCasesAndSubsetOperations) {
  GDALAllRegister();
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // 1. m_pSelf not set
  UninitializedSelfGroup uninit_grp;
  EXPECT_EQ(uninit_grp.SubsetDimensionFromSelection("/a=1"), nullptr);

  // Create MEM multidim dataset
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(drv, nullptr);
  std::unique_ptr<GDALDataset> ds(
      drv->CreateMultiDimensional("test", nullptr, nullptr));
  ASSERT_NE(ds, nullptr);
  auto rg = ds->GetRootGroup();
  ASSERT_NE(rg, nullptr);

  auto dim_b = rg->CreateDimension("bands", "SPECTRAL", "UP", 4);
  auto dim_x = rg->CreateDimension("x", "HORIZONTAL_X", "EAST", 2);

  // Indexing variable for dim_b
  auto idx_var = rg->CreateMDArray(
      "bands", {dim_b}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
  double wl[4] = {400.0, 500.0, 600.0, 700.0};
  const GUInt64 start1[1] = {0};
  const size_t count1[1] = {4};
  EXPECT_TRUE(idx_var->Write(start1, count1, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Float64), wl));
  EXPECT_TRUE(dim_b->SetIndexingVariable(idx_var));

  // Selector array: [1, 0, 1, 1] -> indices 0, 2, 3 match =1
  auto sel_ar = rg->CreateMDArray(
      "good_wl", {dim_b}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  int32_t sel_vals[4] = {1, 0, 1, 1};
  EXPECT_TRUE(sel_ar->Write(start1, count1, nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Int32), sel_vals));

  // All-same selector array: [1, 1, 1, 1]
  auto all_ar = rg->CreateMDArray(
      "all_wl", {dim_b}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  int32_t all_vals[4] = {1, 1, 1, 1};
  EXPECT_TRUE(all_ar->Write(start1, count1, nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Int32), all_vals));

  // String array
  auto str_ar = rg->CreateMDArray(
      "str_ar", {dim_b}, GDALExtendedDataType::CreateString(), nullptr);

  // 2D array with patched dim FIRST: (bands, x)
  auto ar_first =
      rg->CreateMDArray("ar_first", {dim_b, dim_x},
                        GDALExtendedDataType::Create(GDT_Int32), nullptr);
  int32_t first_vals[8] = {10, 11, 20, 21, 30, 31, 40, 41};
  const GUInt64 start2_first[2] = {0, 0};
  const size_t count2_first[2] = {4, 2};
  EXPECT_TRUE(ar_first->Write(start2_first, count2_first, nullptr, nullptr,
                              GDALExtendedDataType::Create(GDT_Int32),
                              first_vals));
  EXPECT_TRUE(ar_first->SetUnit("dn"));
  int32_t nodata = -1;
  EXPECT_TRUE(ar_first->SetRawNoDataValue(&nodata));
  auto srs = std::make_shared<OGRSpatialReference>();
  srs->importFromEPSG(4326);
  EXPECT_TRUE(ar_first->SetSpatialRef(srs.get()));
  EXPECT_NE(ar_first->CreateAttribute(
                "arr_attr", {}, GDALExtendedDataType::CreateString(), nullptr),
            nullptr);

  // 2D array with patched dim SECOND: (x, bands)
  auto ar_second =
      rg->CreateMDArray("ar_second", {dim_x, dim_b},
                        GDALExtendedDataType::Create(GDT_Int32), nullptr);
  int32_t second_vals[8] = {100, 200, 300, 400, 101, 201, 301, 401};
  const GUInt64 start2_second[2] = {0, 0};
  const size_t count2_second[2] = {2, 4};
  EXPECT_TRUE(ar_second->Write(start2_second, count2_second, nullptr, nullptr,
                               GDALExtendedDataType::Create(GDT_Int32),
                               second_vals));

  // Unpatched array (only dim_x)
  auto ar_unpatched =
      rg->CreateMDArray("ar_unpatched", {dim_x},
                        GDALExtendedDataType::Create(GDT_Int32), nullptr);

  // Subgroup and group attribute
  EXPECT_NE(rg->CreateAttribute("grp_attr", {},
                                GDALExtendedDataType::CreateString(), nullptr),
            nullptr);
  auto sub_grp = rg->CreateGroup("sub");
  ASSERT_NE(sub_grp, nullptr);
  auto sub_ar = sub_grp->CreateMDArray(
      "sub_ar", {dim_b}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(sub_ar, nullptr);

  // Error cases in SubsetDimensionFromSelection
  EXPECT_EQ(rg->SubsetDimensionFromSelection("no_equal_sign"), nullptr);
  EXPECT_EQ(rg->SubsetDimensionFromSelection("/good_wl=not_a_number"), nullptr);
  EXPECT_EQ(rg->SubsetDimensionFromSelection("/non_existent=1"), nullptr);
  EXPECT_EQ(rg->SubsetDimensionFromSelection("/ar_first=1"), nullptr);
  EXPECT_EQ(rg->SubsetDimensionFromSelection("/str_ar=1"), nullptr);
  EXPECT_EQ(rg->SubsetDimensionFromSelection("/good_wl=999"), nullptr);

  // All values match -> returns self
  EXPECT_EQ(rg->SubsetDimensionFromSelection("/all_wl=1"), rg);

  // Valid subsetting (matches 3 of 4 elements: indices 0, 2, 3)
  auto sub_root = rg->SubsetDimensionFromSelection("/good_wl=1");
  ASSERT_NE(sub_root, nullptr);
  EXPECT_NE(sub_root, rg);

  // Group methods
  EXPECT_FALSE(sub_root->GetMDArrayNames().empty());
  EXPECT_EQ(sub_root->GetGroupNames(), std::vector<std::string>{"sub"});
  EXPECT_NE(sub_root->GetAttribute("grp_attr"), nullptr);
  EXPECT_EQ(sub_root->GetAttributes().size(), 1u);

  auto dims = sub_root->GetDimensions();
  ASSERT_EQ(dims.size(), 2u);
  EXPECT_EQ(dims[0]->GetSize(), 3u);
  ASSERT_NE(dims[0]->GetIndexingVariable(), nullptr);
  EXPECT_EQ(dims[0]->GetIndexingVariable()->GetDimensions()[0]->GetSize(), 3u);

  auto opened_sub = sub_root->OpenGroup("sub");
  ASSERT_NE(opened_sub, nullptr);
  auto opened_sub_ar = opened_sub->OpenMDArray("sub_ar");
  ASSERT_NE(opened_sub_ar, nullptr);
  EXPECT_EQ(opened_sub_ar->GetDimensions()[0]->GetSize(), 3u);

  // Unpatched array opened from subset group returns original array
  auto opened_unpatched = sub_root->OpenMDArray("ar_unpatched");
  EXPECT_EQ(opened_unpatched, ar_unpatched);

  // Patched array with patched dim FIRST: (bands[3], x[2])
  auto sub_first = sub_root->OpenMDArray("ar_first");
  ASSERT_NE(sub_first, nullptr);
  EXPECT_FALSE(sub_first->IsWritable());
  EXPECT_EQ(sub_first->GetFilename(), ar_first->GetFilename());
  EXPECT_EQ(sub_first->GetDimensions()[0]->GetSize(), 3u);
  EXPECT_EQ(sub_first->GetDataType().GetNumericDataType(), GDT_Int32);
  EXPECT_EQ(sub_first->GetUnit(), "dn");
  EXPECT_NE(sub_first->GetSpatialRef(), nullptr);
  EXPECT_NE(sub_first->GetRawNoDataValue(), nullptr);
  EXPECT_EQ(sub_first->GetBlockSize().size(), 2u);
  EXPECT_NE(sub_first->GetAttribute("arr_attr"), nullptr);
  EXPECT_EQ(sub_first->GetAttributes().size(), 1u);
  EXPECT_NE(sub_first->GetRootGroup(), nullptr);

  // Read sub_first forward (arrayStep > 0)
  int32_t out_first[6] = {0};
  const GUInt64 s_f[2] = {0, 0};
  const size_t c_f[2] = {3, 2};
  const GInt64 step_pos[2] = {1, 1};
  const GPtrDiff_t stride_f[2] = {2, 1};
  EXPECT_TRUE(sub_first->Read(s_f, c_f, step_pos, stride_f,
                              GDALExtendedDataType::Create(GDT_Int32),
                              out_first));
  // Expected rows from old indices 0, 2, 3: (10,11), (30,31), (40,41)
  EXPECT_THAT(out_first, testing::ElementsAre(10, 11, 30, 31, 40, 41));

  // Read sub_first backward (arrayStep < 0)
  int32_t out_first_rev[6] = {0};
  const GUInt64 s_f_rev[2] = {2, 0};
  const GInt64 step_neg[2] = {-1, 1};
  EXPECT_TRUE(sub_first->Read(s_f_rev, c_f, step_neg, stride_f,
                              GDALExtendedDataType::Create(GDT_Int32),
                              out_first_rev));
  EXPECT_THAT(out_first_rev, testing::ElementsAre(40, 41, 30, 31, 10, 11));

  // Patched array with patched dim SECOND: (x[2], bands[3])
  auto sub_second = sub_root->OpenMDArray("ar_second");
  ASSERT_NE(sub_second, nullptr);
  int32_t out_second[6] = {0};
  const GUInt64 s_s[2] = {0, 0};
  const size_t c_s[2] = {2, 3};
  const GPtrDiff_t stride_s[2] = {3, 1};
  EXPECT_TRUE(sub_second->Read(s_s, c_s, step_pos, stride_s,
                               GDALExtendedDataType::Create(GDT_Int32),
                               out_second));
  // Expected: row 0: 100, 300, 400; row 1: 101, 301, 401
  EXPECT_THAT(out_second, testing::ElementsAre(100, 300, 400, 101, 301, 401));

  // Read sub_second backward on dim 1 (arrayStep < 0)
  int32_t out_second_rev[6] = {0};
  const GUInt64 s_s_rev[2] = {0, 2};
  const GInt64 step_s_neg[2] = {1, -1};
  EXPECT_TRUE(sub_second->Read(s_s_rev, c_s, step_s_neg, stride_s,
                               GDALExtendedDataType::Create(GDT_Int32),
                               out_second_rev));
  EXPECT_THAT(out_second_rev,
              testing::ElementsAre(400, 300, 100, 401, 301, 101));
}

}  // namespace
}  // namespace gdal
