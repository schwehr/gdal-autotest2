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

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/gcore/gdalmultidim_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace gdal {
namespace {

class FailingMDArray final : public GDALMDArray {
 public:
  explicit FailingMDArray(std::shared_ptr<GDALDimension> dim,
                          const GDALExtendedDataType& dt)
      : GDALAbstractMDArray("/", "failing"),
        GDALMDArray("/", "failing"),
        m_dims{std::move(dim)},
        m_dt(dt) {}

  bool IsWritable() const override { return false; }
  const std::string& GetFilename() const override { return m_filename; }
  const std::vector<std::shared_ptr<GDALDimension>>& GetDimensions()
      const override {
    return m_dims;
  }
  const GDALExtendedDataType& GetDataType() const override { return m_dt; }

 protected:
  bool IRead(const GUInt64* arrayStartIdx, const size_t* count,
             const GInt64* arrayStep, const GPtrDiff_t* bufferStride,
             const GDALExtendedDataType& bufferDataType,
             void* pDstBuffer) const override {
    if (m_return_null_string && bufferDataType.GetClass() == GEDTC_STRING) {
      *static_cast<char**>(pDstBuffer) = nullptr;
      return true;
    }
    return false;
  }

 public:
  bool m_return_null_string = false;

 private:
  std::string m_filename;
  std::vector<std::shared_ptr<GDALDimension>> m_dims;
  GDALExtendedDataType m_dt;
};

TEST(GdalMultidimRatTest, ErrorCasesAndCreation) {
  GDALAllRegister();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(drv, nullptr);
  std::unique_ptr<GDALDataset> ds(
      drv->CreateMultiDimensional("test", nullptr, nullptr));
  ASSERT_NE(ds, nullptr);
  auto rg = ds->GetRootGroup();
  ASSERT_NE(rg, nullptr);

  auto dim1 = rg->CreateDimension("dim1", "", "", 3);
  auto dim2 = rg->CreateDimension("dim2", "", "", 3);
  auto dim3 = rg->CreateDimension("dim3", "", "", 4);

  auto ar_1d_1 = rg->CreateMDArray(
      "ar1", {dim1}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  auto ar_1d_2 = rg->CreateMDArray(
      "ar2", {dim2}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  auto ar_1d_3 = rg->CreateMDArray(
      "ar3", {dim3}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  auto ar_2d = rg->CreateMDArray(
      "ar2d", {dim1, dim2}, GDALExtendedDataType::Create(GDT_Int32), nullptr);

  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);

  // Empty arrays
  EXPECT_EQ(GDALCreateRasterAttributeTableFromMDArrays(GRTT_THEMATIC, {}, {}),
            nullptr);

  // Mismatched usages size
  EXPECT_EQ(GDALCreateRasterAttributeTableFromMDArrays(
                GRTT_THEMATIC, {ar_1d_1}, {GFU_Generic, GFU_MinMax}),
            nullptr);

  // 2D array
  EXPECT_EQ(
      GDALCreateRasterAttributeTableFromMDArrays(GRTT_THEMATIC, {ar_2d}, {}),
      nullptr);

  // Different dimension full name
  EXPECT_EQ(GDALCreateRasterAttributeTableFromMDArrays(GRTT_THEMATIC,
                                                       {ar_1d_1, ar_1d_2}, {}),
            nullptr);

  // Different dimension size (same name in subgroup vs root, or different size)
  auto sub = rg->CreateGroup("sub");
  auto sub_dim = sub->CreateDimension("dim1", "", "", 4);
  auto ar_sub = sub->CreateMDArray(
      "ar_sub", {sub_dim}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  EXPECT_EQ(GDALCreateRasterAttributeTableFromMDArrays(GRTT_THEMATIC,
                                                       {ar_1d_1, ar_sub}, {}),
            nullptr);

  // C API validation
  EXPECT_EQ(GDALCreateRasterAttributeTableFromMDArrays(GRTT_THEMATIC, 1,
                                                       nullptr, nullptr),
            nullptr);
  GDALMDArrayH null_handle = nullptr;
  EXPECT_EQ(GDALCreateRasterAttributeTableFromMDArrays(GRTT_THEMATIC, 1,
                                                       &null_handle, nullptr),
            nullptr);

  GDALMDArrayHS hs(ar_1d_1);
  GDALMDArrayH valid_handle = &hs;
  GDALRATFieldUsage usage = GFU_Name;
  GDALRasterAttributeTableH h_rat = GDALCreateRasterAttributeTableFromMDArrays(
      GRTT_THEMATIC, 1, &valid_handle, &usage);
  ASSERT_NE(h_rat, nullptr);
  EXPECT_EQ(GDALRATGetUsageOfCol(h_rat, 0), GFU_Name);
  GDALDestroyRasterAttributeTable(h_rat);
}

TEST(GdalMultidimRatTest, FullOperations) {
  GDALAllRegister();
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(drv, nullptr);
  std::unique_ptr<GDALDataset> ds(
      drv->CreateMultiDimensional("test", nullptr, nullptr));
  auto rg = ds->GetRootGroup();
  auto dim = rg->CreateDimension("dim", "", "", 2);

  auto ar_int = rg->CreateMDArray(
      "int_col", {dim}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  auto ar_real = rg->CreateMDArray(
      "real_col", {dim}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
  auto ar_uint32 = rg->CreateMDArray(
      "uint32_col", {dim}, GDALExtendedDataType::Create(GDT_UInt32), nullptr);
  auto ar_str = rg->CreateMDArray(
      "str_col", {dim}, GDALExtendedDataType::CreateString(), nullptr);
  auto ar_dt = rg->CreateMDArray("dt_col", {dim},
                                 GDALExtendedDataType::CreateString(), nullptr);
  auto ar_wkt = rg->CreateMDArray(
      "wkt_col", {dim}, GDALExtendedDataType::CreateString(), nullptr);

  const GUInt64 start[1] = {0};
  const size_t count[1] = {2};
  int ints[2] = {1, 0};
  EXPECT_TRUE(ar_int->Write(start, count, nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Int32), ints));
  double reals[2] = {1.5, 2.5};
  EXPECT_TRUE(ar_real->Write(start, count, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Float64), reals));
  const char* strs[2] = {"alpha", "beta"};
  EXPECT_TRUE(ar_str->Write(start, count, nullptr, nullptr,
                            GDALExtendedDataType::CreateString(), strs));
  const char* dts[2] = {"2025-05-20T12:34:56Z", "invalid"};
  EXPECT_TRUE(ar_dt->Write(start, count, nullptr, nullptr,
                           GDALExtendedDataType::CreateString(), dts));
  const char* wkts[2] = {"POINT (1 2)", ""};
  EXPECT_TRUE(ar_wkt->Write(start, count, nullptr, nullptr,
                            GDALExtendedDataType::CreateString(), wkts));

  std::vector<std::shared_ptr<GDALMDArray>> arrays = {
      ar_int, ar_real, ar_uint32, ar_str, ar_dt, ar_wkt};
  std::vector<GDALRATFieldUsage> usages = {
      GFU_PixelCount, GFU_Min, GFU_Max, GFU_Name, GFU_Generic, GFU_Generic};

  std::unique_ptr<GDALRasterAttributeTable> rat(
      GDALCreateRasterAttributeTableFromMDArrays(GRTT_ATHEMATIC, arrays,
                                                 usages));
  ASSERT_NE(rat, nullptr);

  std::unique_ptr<GDALRasterAttributeTable> cloned(rat->Clone());
  ASSERT_NE(cloned, nullptr);
  EXPECT_EQ(cloned->GetTableType(), GRTT_ATHEMATIC);
  EXPECT_EQ(cloned->GetColumnCount(), 6);
  EXPECT_EQ(cloned->GetRowCount(), 2);

  // Column metadata
  EXPECT_EQ(rat->GetNameOfCol(-1), nullptr);
  EXPECT_EQ(rat->GetNameOfCol(6), nullptr);
  EXPECT_STREQ(rat->GetNameOfCol(0), "int_col");

  EXPECT_EQ(rat->GetUsageOfCol(-1), GFU_Generic);
  EXPECT_EQ(rat->GetUsageOfCol(6), GFU_Generic);
  EXPECT_EQ(rat->GetUsageOfCol(0), GFU_PixelCount);

  // Also test empty usages vector
  std::unique_ptr<GDALRasterAttributeTable> rat_no_usages(
      GDALCreateRasterAttributeTableFromMDArrays(GRTT_THEMATIC, {ar_int}, {}));
  EXPECT_EQ(rat_no_usages->GetUsageOfCol(0), GFU_Generic);

  EXPECT_EQ(rat->GetTypeOfCol(-1), GFT_Integer);
  EXPECT_EQ(rat->GetTypeOfCol(6), GFT_Integer);
  EXPECT_EQ(rat->GetTypeOfCol(0), GFT_Integer);
  EXPECT_EQ(rat->GetTypeOfCol(1), GFT_Real);
  EXPECT_EQ(rat->GetTypeOfCol(2), GFT_Real);
  EXPECT_EQ(rat->GetTypeOfCol(3), GFT_String);

  EXPECT_EQ(rat->GetColOfUsage(GFU_Name), 3);
  EXPECT_EQ(rat->GetColOfUsage(GFU_Red), -1);

  // GetValueAsString / Int / Double / Boolean / DateTime / WKBGeometry
  EXPECT_EQ(rat->GetValueAsString(-1, 3), nullptr);
  EXPECT_EQ(rat->GetValueAsString(2, 3), nullptr);
  EXPECT_EQ(rat->GetValueAsString(0, -1), nullptr);
  EXPECT_EQ(rat->GetValueAsString(0, 6), nullptr);
  EXPECT_STREQ(rat->GetValueAsString(0, 3), "alpha");

  EXPECT_EQ(rat->GetValueAsInt(-1, 0), 0);
  EXPECT_EQ(rat->GetValueAsInt(2, 0), 0);
  EXPECT_EQ(rat->GetValueAsInt(0, -1), 0);
  EXPECT_EQ(rat->GetValueAsInt(0, 6), 0);
  EXPECT_EQ(rat->GetValueAsInt(0, 0), 1);

  EXPECT_EQ(rat->GetValueAsDouble(-1, 1), 0.0);
  EXPECT_EQ(rat->GetValueAsDouble(2, 1), 0.0);
  EXPECT_EQ(rat->GetValueAsDouble(0, -1), 0.0);
  EXPECT_EQ(rat->GetValueAsDouble(0, 6), 0.0);
  EXPECT_DOUBLE_EQ(rat->GetValueAsDouble(0, 1), 1.5);

  EXPECT_TRUE(rat->GetValueAsBoolean(0, 0));
  EXPECT_FALSE(rat->GetValueAsBoolean(1, 0));
  {
    CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
    EXPECT_FALSE(rat->GetValueAsBoolean(-1, 0));
  }

  GDALRATDateTime dt = rat->GetValueAsDateTime(0, 4);
  EXPECT_TRUE(dt.bIsValid);
  EXPECT_EQ(dt.nYear, 2025);

  size_t wkb_size = 0;
  const GByte* wkb = rat->GetValueAsWKBGeometry(0, 5, wkb_size);
  EXPECT_NE(wkb, nullptr);
  EXPECT_GT(wkb_size, 0u);
  const GByte* wkb_empty = rat->GetValueAsWKBGeometry(1, 5, wkb_size);
  EXPECT_EQ(wkb_empty, nullptr);
  EXPECT_EQ(wkb_size, 0u);

  // ValuesIO valid and invalid cases
  CPLErrorStateBackuper quiet(CPLQuietErrorHandler);
  double df_vals[2] = {0.0, 0.0};
  EXPECT_EQ(rat->ValuesIO(GF_Write, 1, 0, 2, df_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, 1, -1, 2, df_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, 1, 0, 0, df_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, 1, 1, 2, df_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, -1, 0, 2, df_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, 6, 0, 2, df_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, 1, 0, 2, df_vals), CE_None);
  EXPECT_DOUBLE_EQ(df_vals[0], 1.5);

  int n_vals[2] = {0, 0};
  EXPECT_EQ(rat->ValuesIO(GF_Write, 0, 0, 2, n_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, 0, -1, 2, n_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, 0, 0, 0, n_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, 0, 1, 2, n_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, -1, 0, 2, n_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, 6, 0, 2, n_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, 0, 0, 2, n_vals), CE_None);
  EXPECT_EQ(n_vals[0], 1);

  char* str_vals[2] = {nullptr, nullptr};
  EXPECT_EQ(rat->ValuesIO(GF_Write, 3, 0, 2, str_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, 3, -1, 2, str_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, 3, 0, 0, str_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, 3, 1, 2, str_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, -1, 0, 2, str_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, 6, 0, 2, str_vals), CE_Failure);
  EXPECT_EQ(rat->ValuesIO(GF_Read, 3, 0, 2, str_vals), CE_None);
  EXPECT_STREQ(str_vals[0], "alpha");
  CPLFree(str_vals[0]);
  CPLFree(str_vals[1]);

  bool b_vals[2] = {false, false};
  EXPECT_EQ(rat->ValuesIO(GF_Read, 0, 0, 2, b_vals), CE_None);
  EXPECT_TRUE(b_vals[0]);

  // Unsupported mutation methods
  EXPECT_EQ(rat->SetValue(0, 0, "x"), CE_Failure);
  EXPECT_EQ(rat->SetValue(0, 0, 1), CE_Failure);
  EXPECT_EQ(rat->SetValue(0, 0, 1.0), CE_Failure);
  EXPECT_EQ(rat->SetValue(0, 0, true), CE_Failure);
  EXPECT_EQ(rat->SetValue(0, 0, dt), CE_Failure);
  EXPECT_EQ(rat->SetValue(0, 0, "wkb", 3), CE_Failure);
  EXPECT_EQ(rat->ChangesAreWrittenToFile(), FALSE);
  EXPECT_EQ(rat->SetTableType(GRTT_THEMATIC), CE_Failure);
  rat->RemoveStatistics();

  // Failing MDArray read paths
  auto failing_ar = std::make_shared<FailingMDArray>(
      dim, GDALExtendedDataType::CreateString());
  std::unique_ptr<GDALRasterAttributeTable> fail_rat(
      GDALCreateRasterAttributeTableFromMDArrays(GRTT_THEMATIC, {failing_ar},
                                                 {}));
  ASSERT_NE(fail_rat, nullptr);
  EXPECT_EQ(fail_rat->GetValueAsString(0, 0), nullptr);
  EXPECT_EQ(fail_rat->GetValueAsInt(0, 0), 0);
  EXPECT_EQ(fail_rat->GetValueAsDouble(0, 0), 0.0);
  EXPECT_EQ(fail_rat->ValuesIO(GF_Read, 0, 0, 1, df_vals), CE_Failure);
  EXPECT_EQ(fail_rat->ValuesIO(GF_Read, 0, 0, 1, n_vals), CE_Failure);
  EXPECT_EQ(fail_rat->ValuesIO(GF_Read, 0, 0, 1, str_vals), CE_Failure);

  failing_ar->m_return_null_string = true;
  EXPECT_EQ(fail_rat->GetValueAsString(0, 0), nullptr);
}

}  // namespace
}  // namespace gdal
