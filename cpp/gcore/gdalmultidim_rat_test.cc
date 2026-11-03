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
// Unit tests for gcore/gdalmultidim_rat.cpp.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalmultidim_rat.cpp

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/gcore/gdalmultidim_priv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
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

  void set_return_null_string(bool value) { m_return_null_string = value; }

 protected:
  bool IRead(const GUInt64* /*arrayStartIdx*/, const size_t* /*count*/,
             const GInt64* /*arrayStep*/, const GPtrDiff_t* /*bufferStride*/,
             const GDALExtendedDataType& bufferDataType,
             void* pDstBuffer) const override {
    if (m_return_null_string && bufferDataType.GetClass() == GEDTC_STRING) {
      *static_cast<char**>(pDstBuffer) = nullptr;
      return true;
    }
    return false;
  }

 private:
  bool m_return_null_string = false;
  std::string m_filename;
  std::vector<std::shared_ptr<GDALDimension>> m_dims;
  GDALExtendedDataType m_dt;
};

TEST(GdalMultidimRatTest, ErrorCasesAndCreation) {
  GDALRegister_MEM();
  GDALDriver* const drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->CreateMultiDimensional("test", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto rg = ds->GetRootGroup();
  ASSERT_NE(nullptr, rg);

  auto dim1 = rg->CreateDimension("dim1", "", "", 3);
  auto dim2 = rg->CreateDimension("dim2", "", "", 3);

  auto ar_1d_1 = rg->CreateMDArray(
      "ar1", {dim1}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  auto ar_1d_2 = rg->CreateMDArray(
      "ar2", {dim2}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  auto ar_2d = rg->CreateMDArray(
      "ar2d", {dim1, dim2}, GDALExtendedDataType::Create(GDT_Int32), nullptr);

  auto sub = rg->CreateGroup("sub");
  auto sub_dim = sub->CreateDimension("dim1", "", "", 4);
  auto ar_sub = sub->CreateMDArray(
      "ar_sub", {sub_dim}, GDALExtendedDataType::Create(GDT_Int32), nullptr);

  {
    WithQuietHandler quiet;
    CPLErrorReset();

    // Empty arrays
    EXPECT_EQ(nullptr, GDALCreateRasterAttributeTableFromMDArrays(GRTT_THEMATIC,
                                                                  {}, {}));

    // Mismatched usages size
    EXPECT_EQ(nullptr,
              GDALCreateRasterAttributeTableFromMDArrays(
                  GRTT_THEMATIC, {ar_1d_1}, {GFU_Generic, GFU_MinMax}));

    // 2D array
    EXPECT_EQ(nullptr, GDALCreateRasterAttributeTableFromMDArrays(GRTT_THEMATIC,
                                                                  {ar_2d}, {}));

    // Different dimension full name
    EXPECT_EQ(nullptr, GDALCreateRasterAttributeTableFromMDArrays(
                           GRTT_THEMATIC, {ar_1d_1, ar_1d_2}, {}));

    // Different dimension size
    EXPECT_EQ(nullptr, GDALCreateRasterAttributeTableFromMDArrays(
                           GRTT_THEMATIC, {ar_1d_1, ar_sub}, {}));

    // C API validation
    EXPECT_EQ(nullptr, GDALCreateRasterAttributeTableFromMDArrays(
                           GRTT_THEMATIC, 1, nullptr, nullptr));
    GDALMDArrayH null_handle = nullptr;
    EXPECT_EQ(nullptr, GDALCreateRasterAttributeTableFromMDArrays(
                           GRTT_THEMATIC, 1, &null_handle, nullptr));

    CPLErrorReset();
  }

  GDALMDArrayHS hs(ar_1d_1);
  GDALMDArrayH valid_handle = &hs;
  GDALRATFieldUsage usage = GFU_Name;
  GDALRasterAttributeTableH h_rat = GDALCreateRasterAttributeTableFromMDArrays(
      GRTT_THEMATIC, 1, &valid_handle, &usage);
  ASSERT_NE(nullptr, h_rat);
  absl::Cleanup destroy_rat = [h_rat]() {
    GDALDestroyRasterAttributeTable(h_rat);
  };
  EXPECT_EQ(GFU_Name, GDALRATGetUsageOfCol(h_rat, 0));
}

TEST(GdalMultidimRatTest, FullOperations) {
  GDALRegister_MEM();
  GDALDriver* const drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  GDALDatasetUniquePtr ds(
      drv->CreateMultiDimensional("test", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto rg = ds->GetRootGroup();
  ASSERT_NE(nullptr, rg);
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

  constexpr auto start = std::to_array<GUInt64>({0});
  constexpr auto count = std::to_array<size_t>({2});
  constexpr auto ints = std::to_array<int>({1, 0});
  EXPECT_TRUE(ar_int->Write(start.data(), count.data(), nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Int32),
                            ints.data()));
  constexpr auto reals = std::to_array<double>({1.5, 2.5});
  EXPECT_TRUE(ar_real->Write(start.data(), count.data(), nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Float64),
                             reals.data()));
  constexpr auto strs = std::to_array<const char*>({"alpha", "beta"});
  EXPECT_TRUE(ar_str->Write(start.data(), count.data(), nullptr, nullptr,
                            GDALExtendedDataType::CreateString(), strs.data()));
  constexpr auto dts =
      std::to_array<const char*>({"2025-05-20T12:34:56Z", "invalid"});
  EXPECT_TRUE(ar_dt->Write(start.data(), count.data(), nullptr, nullptr,
                           GDALExtendedDataType::CreateString(), dts.data()));
  constexpr auto wkts = std::to_array<const char*>({"POINT (1 2)", ""});
  EXPECT_TRUE(ar_wkt->Write(start.data(), count.data(), nullptr, nullptr,
                            GDALExtendedDataType::CreateString(), wkts.data()));

  const std::vector<std::shared_ptr<GDALMDArray>> arrays = {
      ar_int, ar_real, ar_uint32, ar_str, ar_dt, ar_wkt};
  const std::vector<GDALRATFieldUsage> usages = {
      GFU_PixelCount, GFU_Min, GFU_Max, GFU_Name, GFU_Generic, GFU_Generic};

  std::unique_ptr<GDALRasterAttributeTable> rat(
      GDALCreateRasterAttributeTableFromMDArrays(GRTT_ATHEMATIC, arrays,
                                                 usages));
  ASSERT_NE(nullptr, rat);

  std::unique_ptr<GDALRasterAttributeTable> cloned(rat->Clone());
  ASSERT_NE(nullptr, cloned);
  EXPECT_EQ(GRTT_ATHEMATIC, cloned->GetTableType());
  EXPECT_EQ(6, cloned->GetColumnCount());
  EXPECT_EQ(2, cloned->GetRowCount());

  // Column metadata
  EXPECT_EQ(nullptr, rat->GetNameOfCol(-1));
  EXPECT_EQ(nullptr, rat->GetNameOfCol(6));
  EXPECT_STREQ("int_col", rat->GetNameOfCol(0));

  EXPECT_EQ(GFU_Generic, rat->GetUsageOfCol(-1));
  EXPECT_EQ(GFU_Generic, rat->GetUsageOfCol(6));
  EXPECT_EQ(GFU_PixelCount, rat->GetUsageOfCol(0));

  // Empty usages vector defaults to GFU_Generic
  std::unique_ptr<GDALRasterAttributeTable> rat_no_usages(
      GDALCreateRasterAttributeTableFromMDArrays(GRTT_THEMATIC, {ar_int}, {}));
  ASSERT_NE(nullptr, rat_no_usages);
  EXPECT_EQ(GFU_Generic, rat_no_usages->GetUsageOfCol(0));

  EXPECT_EQ(GFT_Integer, rat->GetTypeOfCol(-1));
  EXPECT_EQ(GFT_Integer, rat->GetTypeOfCol(6));
  EXPECT_EQ(GFT_Integer, rat->GetTypeOfCol(0));
  EXPECT_EQ(GFT_Real, rat->GetTypeOfCol(1));
  EXPECT_EQ(GFT_Real, rat->GetTypeOfCol(2));
  EXPECT_EQ(GFT_String, rat->GetTypeOfCol(3));

  EXPECT_EQ(3, rat->GetColOfUsage(GFU_Name));
  EXPECT_EQ(-1, rat->GetColOfUsage(GFU_Red));

  // GetValueAsString / Int / Double / Boolean / DateTime / WKBGeometry
  EXPECT_EQ(nullptr, rat->GetValueAsString(-1, 3));
  EXPECT_EQ(nullptr, rat->GetValueAsString(2, 3));
  EXPECT_EQ(nullptr, rat->GetValueAsString(0, -1));
  EXPECT_EQ(nullptr, rat->GetValueAsString(0, 6));
  EXPECT_STREQ("alpha", rat->GetValueAsString(0, 3));

  EXPECT_EQ(0, rat->GetValueAsInt(-1, 0));
  EXPECT_EQ(0, rat->GetValueAsInt(2, 0));
  EXPECT_EQ(0, rat->GetValueAsInt(0, -1));
  EXPECT_EQ(0, rat->GetValueAsInt(0, 6));
  EXPECT_EQ(1, rat->GetValueAsInt(0, 0));

  EXPECT_DOUBLE_EQ(0.0, rat->GetValueAsDouble(-1, 1));
  EXPECT_DOUBLE_EQ(0.0, rat->GetValueAsDouble(2, 1));
  EXPECT_DOUBLE_EQ(0.0, rat->GetValueAsDouble(0, -1));
  EXPECT_DOUBLE_EQ(0.0, rat->GetValueAsDouble(0, 6));
  EXPECT_DOUBLE_EQ(1.5, rat->GetValueAsDouble(0, 1));

  EXPECT_TRUE(rat->GetValueAsBoolean(0, 0));
  EXPECT_FALSE(rat->GetValueAsBoolean(1, 0));
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(rat->GetValueAsBoolean(-1, 0));
    CPLErrorReset();
  }

  const GDALRATDateTime dt = rat->GetValueAsDateTime(0, 4);
  EXPECT_TRUE(dt.bIsValid);
  EXPECT_EQ(2025, dt.nYear);

  size_t wkb_size = 0;
  const GByte* const wkb = rat->GetValueAsWKBGeometry(0, 5, wkb_size);
  EXPECT_NE(nullptr, wkb);
  EXPECT_GT(wkb_size, 0U);
  const GByte* const wkb_empty = rat->GetValueAsWKBGeometry(1, 5, wkb_size);
  EXPECT_EQ(nullptr, wkb_empty);
  EXPECT_EQ(0U, wkb_size);

  // ValuesIO valid and invalid cases
  std::array<double, 2> df_vals = {};
  std::array<int, 2> n_vals = {};
  std::array<char*, 2> str_vals = {};
  std::array<bool, 2> b_vals = {};
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Write, 1, 0, 2, df_vals.data()));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, 1, -1, 2, df_vals.data()));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, 1, 0, 0, df_vals.data()));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, 1, 1, 2, df_vals.data()));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, -1, 0, 2, df_vals.data()));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, 6, 0, 2, df_vals.data()));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, rat->ValuesIO(GF_Read, 1, 0, 2, df_vals.data()));
  EXPECT_DOUBLE_EQ(1.5, df_vals[0]);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Write, 0, 0, 2, n_vals.data()));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, 0, -1, 2, n_vals.data()));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, 0, 0, 0, n_vals.data()));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, 0, 1, 2, n_vals.data()));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, -1, 0, 2, n_vals.data()));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, 6, 0, 2, n_vals.data()));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, rat->ValuesIO(GF_Read, 0, 0, 2, n_vals.data()));
  EXPECT_EQ(1, n_vals[0]);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Write, 3, 0, 2, str_vals.data()));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, 3, -1, 2, str_vals.data()));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, 3, 0, 0, str_vals.data()));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, 3, 1, 2, str_vals.data()));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, -1, 0, 2, str_vals.data()));
    EXPECT_EQ(CE_Failure, rat->ValuesIO(GF_Read, 6, 0, 2, str_vals.data()));
    CPLErrorReset();
  }
  EXPECT_EQ(CE_None, rat->ValuesIO(GF_Read, 3, 0, 2, str_vals.data()));
  std::unique_ptr<char, CplFreeDeleter> owned_str0(str_vals[0]);
  std::unique_ptr<char, CplFreeDeleter> owned_str1(str_vals[1]);
  EXPECT_STREQ("alpha", owned_str0.get());

  EXPECT_EQ(CE_None, rat->ValuesIO(GF_Read, 0, 0, 2, b_vals.data()));
  EXPECT_TRUE(b_vals[0]);

  // Unsupported mutation methods and failing MDArray read paths
  auto failing_ar = std::make_shared<FailingMDArray>(
      dim, GDALExtendedDataType::CreateString());
  std::unique_ptr<GDALRasterAttributeTable> fail_rat(
      GDALCreateRasterAttributeTableFromMDArrays(GRTT_THEMATIC, {failing_ar},
                                                 {}));
  ASSERT_NE(nullptr, fail_rat);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, rat->SetValue(0, 0, "x"));
    EXPECT_EQ(CE_Failure, rat->SetValue(0, 0, 1));
    EXPECT_EQ(CE_Failure, rat->SetValue(0, 0, 1.0));
    EXPECT_EQ(CE_Failure, rat->SetValue(0, 0, true));
    EXPECT_EQ(CE_Failure, rat->SetValue(0, 0, dt));
    EXPECT_EQ(CE_Failure, rat->SetValue(0, 0, "wkb", 3));
    EXPECT_EQ(FALSE, rat->ChangesAreWrittenToFile());
    EXPECT_EQ(CE_Failure, rat->SetTableType(GRTT_THEMATIC));
    rat->RemoveStatistics();

    EXPECT_EQ(nullptr, fail_rat->GetValueAsString(0, 0));
    EXPECT_EQ(0, fail_rat->GetValueAsInt(0, 0));
    EXPECT_DOUBLE_EQ(0.0, fail_rat->GetValueAsDouble(0, 0));
    EXPECT_EQ(CE_Failure, fail_rat->ValuesIO(GF_Read, 0, 0, 1, df_vals.data()));
    EXPECT_EQ(CE_Failure, fail_rat->ValuesIO(GF_Read, 0, 0, 1, n_vals.data()));
    EXPECT_EQ(CE_Failure,
              fail_rat->ValuesIO(GF_Read, 0, 0, 1, str_vals.data()));
    CPLErrorReset();
  }

  failing_ar->set_return_null_string(true);
  EXPECT_EQ(nullptr, fail_rat->GetValueAsString(0, 0));
}

}  // namespace
}  // namespace autotest2
