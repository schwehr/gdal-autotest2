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
// Tests GDALGroup::SubsetDimensionFromSelection
// (gdalmultidim_subsetdimension.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalmultidim_subsetdimension.cpp

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;

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

class GdalMultidimSubsetDimensionTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }
};

TEST_F(GdalMultidimSubsetDimensionTest, ErrorCasesAndSubsetOperations) {
  // 1. m_pSelf not set.
  UninitializedSelfGroup uninit_grp;
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, uninit_grp.SubsetDimensionFromSelection("/a=1"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Create MEM multidim dataset.
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->CreateMultiDimensional("test", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto rg = ds->GetRootGroup();
  ASSERT_NE(nullptr, rg);

  auto dim_b = rg->CreateDimension("bands", "SPECTRAL", "UP", 4);
  auto dim_x = rg->CreateDimension("x", "HORIZONTAL_X", "EAST", 2);
  ASSERT_NE(nullptr, dim_b);
  ASSERT_NE(nullptr, dim_x);

  // Indexing variable for dim_b.
  auto idx_var = rg->CreateMDArray(
      "bands", {dim_b}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
  ASSERT_NE(nullptr, idx_var);
  constexpr std::array<double, 4> kWavelengths = {400.0, 500.0, 600.0, 700.0};
  constexpr std::array<GUInt64, 1> kStart1 = {0};
  constexpr std::array<size_t, 1> kCount1 = {4};
  EXPECT_TRUE(idx_var->Write(kStart1.data(), kCount1.data(), nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Float64),
                             kWavelengths.data()));
  EXPECT_TRUE(dim_b->SetIndexingVariable(idx_var));

  // Selector array: [1, 0, 1, 1] -> indices 0, 2, 3 match =1.
  auto sel_ar = rg->CreateMDArray(
      "good_wl", {dim_b}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, sel_ar);
  constexpr std::array<int32_t, 4> kSelVals = {1, 0, 1, 1};
  EXPECT_TRUE(sel_ar->Write(kStart1.data(), kCount1.data(), nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Int32),
                            kSelVals.data()));

  // All-same selector array: [1, 1, 1, 1].
  auto all_ar = rg->CreateMDArray(
      "all_wl", {dim_b}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, all_ar);
  constexpr std::array<int32_t, 4> kAllVals = {1, 1, 1, 1};
  EXPECT_TRUE(all_ar->Write(kStart1.data(), kCount1.data(), nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Int32),
                            kAllVals.data()));

  // String array.
  auto str_ar = rg->CreateMDArray(
      "str_ar", {dim_b}, GDALExtendedDataType::CreateString(), nullptr);
  ASSERT_NE(nullptr, str_ar);

  // 2D array with patched dim FIRST: (bands, x).
  auto ar_first =
      rg->CreateMDArray("ar_first", {dim_b, dim_x},
                        GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, ar_first);
  constexpr std::array<int32_t, 8> kFirstVals = {10, 11, 20, 21,
                                                 30, 31, 40, 41};
  constexpr std::array<GUInt64, 2> kStart2 = {0, 0};
  constexpr std::array<size_t, 2> kCount2First = {4, 2};
  EXPECT_TRUE(ar_first->Write(kStart2.data(), kCount2First.data(), nullptr,
                              nullptr, GDALExtendedDataType::Create(GDT_Int32),
                              kFirstVals.data()));
  EXPECT_TRUE(ar_first->SetUnit("dn"));
  constexpr int32_t kNoData = -1;
  EXPECT_TRUE(ar_first->SetRawNoDataValue(&kNoData));
  auto srs = std::make_shared<OGRSpatialReference>();
  ASSERT_EQ(OGRERR_NONE, srs->importFromEPSG(4326));
  EXPECT_TRUE(ar_first->SetSpatialRef(srs.get()));
  EXPECT_NE(nullptr,
            ar_first->CreateAttribute(
                "arr_attr", {}, GDALExtendedDataType::CreateString(), nullptr));

  // 2D array with patched dim SECOND: (x, bands).
  auto ar_second =
      rg->CreateMDArray("ar_second", {dim_x, dim_b},
                        GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, ar_second);
  constexpr std::array<int32_t, 8> kSecondVals = {100, 200, 300, 400,
                                                  101, 201, 301, 401};
  constexpr std::array<size_t, 2> kCount2Second = {2, 4};
  EXPECT_TRUE(ar_second->Write(kStart2.data(), kCount2Second.data(), nullptr,
                               nullptr, GDALExtendedDataType::Create(GDT_Int32),
                               kSecondVals.data()));

  // Unpatched array (only dim_x).
  auto ar_unpatched =
      rg->CreateMDArray("ar_unpatched", {dim_x},
                        GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, ar_unpatched);

  // Subgroup and group attribute.
  EXPECT_NE(nullptr,
            rg->CreateAttribute("grp_attr", {},
                                GDALExtendedDataType::CreateString(), nullptr));
  auto sub_grp = rg->CreateGroup("sub");
  ASSERT_NE(nullptr, sub_grp);
  auto sub_ar = sub_grp->CreateMDArray(
      "sub_ar", {dim_b}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, sub_ar);

  // Error cases in SubsetDimensionFromSelection.
  constexpr std::string_view kBadSelections[] = {
      "no_equal_sign",   "/good_wl=not_a_number",
      "/non_existent=1", "/ar_first=1",
      "/str_ar=1",       "/good_wl=999",
  };
  for (std::string_view selection : kBadSelections) {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, rg->SubsetDimensionFromSelection(std::string(selection)))
        << selection;
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType()) << selection;
    CPLErrorReset();
  }

  // All values match -> returns self.
  EXPECT_EQ(rg, rg->SubsetDimensionFromSelection("/all_wl=1"));

  // Valid subsetting (matches 3 of 4 elements: indices 0, 2, 3).
  auto sub_root = rg->SubsetDimensionFromSelection("/good_wl=1");
  ASSERT_NE(nullptr, sub_root);
  EXPECT_NE(rg, sub_root);

  // Group methods.
  EXPECT_FALSE(sub_root->GetMDArrayNames().empty());
  EXPECT_THAT(sub_root->GetGroupNames(), ElementsAre("sub"));
  EXPECT_NE(nullptr, sub_root->GetAttribute("grp_attr"));
  EXPECT_EQ(1U, sub_root->GetAttributes().size());

  auto dims = sub_root->GetDimensions();
  ASSERT_EQ(2U, dims.size());
  EXPECT_EQ(3U, dims[0]->GetSize());
  ASSERT_NE(nullptr, dims[0]->GetIndexingVariable());
  EXPECT_EQ(3U, dims[0]->GetIndexingVariable()->GetDimensions()[0]->GetSize());

  auto opened_sub = sub_root->OpenGroup("sub");
  ASSERT_NE(nullptr, opened_sub);
  auto opened_sub_ar = opened_sub->OpenMDArray("sub_ar");
  ASSERT_NE(nullptr, opened_sub_ar);
  EXPECT_EQ(3U, opened_sub_ar->GetDimensions()[0]->GetSize());

  // Unpatched array opened from subset group returns original array.
  auto opened_unpatched = sub_root->OpenMDArray("ar_unpatched");
  EXPECT_EQ(ar_unpatched, opened_unpatched);

  // Patched array with patched dim FIRST: (bands[3], x[2]).
  auto sub_first = sub_root->OpenMDArray("ar_first");
  ASSERT_NE(nullptr, sub_first);
  EXPECT_FALSE(sub_first->IsWritable());
  EXPECT_EQ(ar_first->GetFilename(), sub_first->GetFilename());
  EXPECT_EQ(3U, sub_first->GetDimensions()[0]->GetSize());
  EXPECT_EQ(GDT_Int32, sub_first->GetDataType().GetNumericDataType());
  EXPECT_EQ("dn", sub_first->GetUnit());
  EXPECT_NE(nullptr, sub_first->GetSpatialRef());
  EXPECT_NE(nullptr, sub_first->GetRawNoDataValue());
  EXPECT_EQ(2U, sub_first->GetBlockSize().size());
  EXPECT_NE(nullptr, sub_first->GetAttribute("arr_attr"));
  EXPECT_EQ(1U, sub_first->GetAttributes().size());
  EXPECT_NE(nullptr, sub_first->GetRootGroup());

  // Read sub_first forward (arrayStep > 0).
  std::array<int32_t, 6> out_first = {};
  constexpr std::array<size_t, 2> kCountFirst = {3, 2};
  constexpr std::array<GInt64, 2> kStepPos = {1, 1};
  constexpr std::array<GPtrDiff_t, 2> kStrideFirst = {2, 1};
  EXPECT_TRUE(sub_first->Read(
      kStart2.data(), kCountFirst.data(), kStepPos.data(), kStrideFirst.data(),
      GDALExtendedDataType::Create(GDT_Int32), out_first.data()));
  // Expected rows from old indices 0, 2, 3: (10,11), (30,31), (40,41).
  EXPECT_THAT(out_first, ElementsAre(10, 11, 30, 31, 40, 41));

  // Read sub_first backward (arrayStep < 0).
  std::array<int32_t, 6> out_first_rev = {};
  constexpr std::array<GUInt64, 2> kStartFirstRev = {2, 0};
  constexpr std::array<GInt64, 2> kStepFirstNeg = {-1, 1};
  EXPECT_TRUE(sub_first->Read(kStartFirstRev.data(), kCountFirst.data(),
                              kStepFirstNeg.data(), kStrideFirst.data(),
                              GDALExtendedDataType::Create(GDT_Int32),
                              out_first_rev.data()));
  EXPECT_THAT(out_first_rev, ElementsAre(40, 41, 30, 31, 10, 11));

  // Patched array with patched dim SECOND: (x[2], bands[3]).
  auto sub_second = sub_root->OpenMDArray("ar_second");
  ASSERT_NE(nullptr, sub_second);
  std::array<int32_t, 6> out_second = {};
  constexpr std::array<size_t, 2> kCountSecond = {2, 3};
  constexpr std::array<GPtrDiff_t, 2> kStrideSecond = {3, 1};
  EXPECT_TRUE(sub_second->Read(kStart2.data(), kCountSecond.data(),
                               kStepPos.data(), kStrideSecond.data(),
                               GDALExtendedDataType::Create(GDT_Int32),
                               out_second.data()));
  // Expected: row 0: 100, 300, 400; row 1: 101, 301, 401.
  EXPECT_THAT(out_second, ElementsAre(100, 300, 400, 101, 301, 401));

  // Read sub_second backward on dim 1 (arrayStep < 0).
  std::array<int32_t, 6> out_second_rev = {};
  constexpr std::array<GUInt64, 2> kStartSecondRev = {0, 2};
  constexpr std::array<GInt64, 2> kStepSecondNeg = {1, -1};
  EXPECT_TRUE(sub_second->Read(kStartSecondRev.data(), kCountSecond.data(),
                               kStepSecondNeg.data(), kStrideSecond.data(),
                               GDALExtendedDataType::Create(GDT_Int32),
                               out_second_rev.data()));
  EXPECT_THAT(out_second_rev, ElementsAre(400, 300, 100, 401, 301, 101));
}

}  // namespace
}  // namespace autotest2
