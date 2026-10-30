// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests the netCDF multidimensional raster driver (netcdfmultidim.cpp).
//
// See also:
//   https://gdal.org/drivers/raster/netcdf.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/netcdf_multidim.py

#include <algorithm>
#include <cstdint>
#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <vector>

#include "netcdf.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/frmts/netcdf/netcdfdataset.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

class NetCDFMultiDimTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALAllRegister(); }
};

// netCDFAttribute::IRead() can convert an NC_CHAR attribute to a numeric
// buffer type, but the public GDALAbstractMDArray::Read() rejects
// string->numeric conversions up front, so that code path is only reachable
// through the protected hook. Reach it with the well-defined pointer-to-member
// idiom (no casting of the object to an unrelated type).
class ProtectedMDArrayAccess : public GDALAbstractMDArray {
 public:
  static bool ReadScalar(const GDALAbstractMDArray& arr,
                         const GDALExtendedDataType& bufferDataType,
                         void* pDstBuffer) {
    constexpr auto kIRead = &ProtectedMDArrayAccess::IRead;
    const GUInt64 start0 = 0;
    const size_t count1 = 1;
    const GInt64 step1 = 1;
    const GPtrDiff_t stride1 = 1;
    return (arr.*kIRead)(&start0, &count1, &step1, &stride1, bufferDataType,
                         pDstBuffer);
  }
};

TEST_F(NetCDFMultiDimTest, CreateMultiDimensionalAndBasicHierarchy) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "multidim_basic.nc")
          .string();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  char** papszOptions = nullptr;
  papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "NC4");
  papszOptions = CSLSetNameValue(papszOptions, "CONVENTIONS", "CF-1.8");

  auto poDS = std::unique_ptr<GDALDataset>(poDriver->CreateMultiDimensional(
      filename.c_str(), nullptr, papszOptions));
  CSLDestroy(papszOptions);
  ASSERT_NE(nullptr, poDS);

  auto poRootGroup = poDS->GetRootGroup();
  ASSERT_NE(nullptr, poRootGroup);
  EXPECT_EQ("/", poRootGroup->GetName());
  EXPECT_EQ("/", poRootGroup->GetFullName());

  // 1. Create dimensions
  auto poDimX = poRootGroup->CreateDimension("x", GDAL_DIM_TYPE_HORIZONTAL_X,
                                             "EAST", 10, nullptr);
  ASSERT_NE(nullptr, poDimX);
  EXPECT_EQ("x", poDimX->GetName());
  EXPECT_EQ(10, poDimX->GetSize());
  EXPECT_EQ(GDAL_DIM_TYPE_HORIZONTAL_X, poDimX->GetType());

  auto poDimY = poRootGroup->CreateDimension("y", GDAL_DIM_TYPE_HORIZONTAL_Y,
                                             "NORTH", 8, nullptr);
  ASSERT_NE(nullptr, poDimY);
  EXPECT_EQ("y", poDimY->GetName());
  EXPECT_EQ(8, poDimY->GetSize());

  char** papszDimOpts = nullptr;
  papszDimOpts = CSLSetNameValue(papszDimOpts, "UNLIMITED", "YES");
  auto poDimTime = poRootGroup->CreateDimension("time", GDAL_DIM_TYPE_TEMPORAL,
                                                "", 0, papszDimOpts);
  CSLDestroy(papszDimOpts);
  ASSERT_NE(nullptr, poDimTime);
  EXPECT_EQ("time", poDimTime->GetName());
  EXPECT_EQ(0, poDimTime->GetSize());

  auto apoDims = poRootGroup->GetDimensions();
  EXPECT_EQ(3, apoDims.size());

  // 2. Create attributes on root group
  auto poAttrStr = poRootGroup->CreateAttribute(
      "title", {}, GDALExtendedDataType::CreateString(), nullptr);
  ASSERT_NE(nullptr, poAttrStr);
  EXPECT_TRUE(poAttrStr->Write("Test MultiDim NetCDF"));

  auto poAttrInt = poRootGroup->CreateAttribute(
      "int_val", {}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, poAttrInt);
  int nVal = 42;
  EXPECT_TRUE(poAttrInt->WriteInt(nVal));

  auto poAttrDbl = poRootGroup->CreateAttribute(
      "dbl_val", {}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
  ASSERT_NE(nullptr, poAttrDbl);
  double dfVal = 3.1415926535;
  EXPECT_TRUE(poAttrDbl->Write(dfVal));

  // Multi-value attribute
  auto poAttrArr = poRootGroup->CreateAttribute(
      "vec_ints", {3}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, poAttrArr);
  int anVals[3] = {10, 20, 30};
  EXPECT_TRUE(poAttrArr->Write(anVals, 3));

  // 3. Create MDArrays
  // 1D indexing array for x
  auto poVarX = poRootGroup->CreateMDArray(
      "x", {poDimX}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
  ASSERT_NE(nullptr, poVarX);
  EXPECT_EQ("x", poVarX->GetName());
  EXPECT_EQ(1, poVarX->GetDimensionCount());
  EXPECT_EQ(10, poVarX->GetTotalElementsCount());

  std::vector<double> adfX(10);
  for (int i = 0; i < 10; ++i) adfX[i] = 100.0 + i * 0.5;
  const size_t anCount10[1] = {10};
  const std::vector<GUInt64> anStart0 = {0};
  EXPECT_TRUE(poVarX->Write(anStart0.data(), anCount10, nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Float64),
                            adfX.data()));

  // 2D data array (y, x) with compression and chunking
  char** papszArrayOpts = nullptr;
  papszArrayOpts = CSLSetNameValue(papszArrayOpts, "COMPRESS", "DEFLATE");
  papszArrayOpts = CSLSetNameValue(papszArrayOpts, "ZLEVEL", "6");
  papszArrayOpts = CSLSetNameValue(papszArrayOpts, "BLOCKSIZE", "4,5");
  papszArrayOpts = CSLSetNameValue(papszArrayOpts, "CHECKSUM", "YES");
  auto poVarData = poRootGroup->CreateMDArray(
      "temperature", {poDimY, poDimX},
      GDALExtendedDataType::Create(GDT_Float32), papszArrayOpts);
  CSLDestroy(papszArrayOpts);
  ASSERT_NE(nullptr, poVarData);
  EXPECT_EQ(2, poVarData->GetDimensionCount());
  EXPECT_EQ(80, poVarData->GetTotalElementsCount());

  // Set metadata on array
  EXPECT_TRUE(poVarData->SetNoDataValue(-9999.0));
  EXPECT_TRUE(poVarData->SetOffset(10.0));
  EXPECT_TRUE(poVarData->SetScale(0.1));
  EXPECT_TRUE(poVarData->SetUnit("celsius"));

  OGRSpatialReference oSRS;
  oSRS.importFromEPSG(4326);
  oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  EXPECT_TRUE(poVarData->SetSpatialRef(&oSRS));

  std::vector<float> afData(80);
  for (int i = 0; i < 80; ++i) afData[i] = static_cast<float>(i * 1.5f);
  const size_t anCount8x10[2] = {8, 10};
  const std::vector<GUInt64> anStart0x0 = {0, 0};
  EXPECT_TRUE(poVarData->Write(anStart0x0.data(), anCount8x10, nullptr, nullptr,
                               GDALExtendedDataType::Create(GDT_Float32),
                               afData.data()));

  // 4. Create Subgroup
  auto poSubGroup = poRootGroup->CreateGroup("subgroup", nullptr);
  ASSERT_NE(nullptr, poSubGroup);
  EXPECT_EQ("subgroup", poSubGroup->GetName());
  EXPECT_EQ("/subgroup", poSubGroup->GetFullName());

  auto poSubDim = poSubGroup->CreateDimension("sub_dim", "", "", 5, nullptr);
  ASSERT_NE(nullptr, poSubDim);

  auto poSubArray = poSubGroup->CreateMDArray(
      "sub_array", {poSubDim}, GDALExtendedDataType::Create(GDT_Int16),
      nullptr);
  ASSERT_NE(nullptr, poSubArray);
  std::vector<short> anSubVals = {1, 2, 3, 4, 5};
  const size_t anCount5[1] = {5};
  EXPECT_TRUE(poSubArray->Write(anStart0.data(), anCount5, nullptr, nullptr,
                                GDALExtendedDataType::Create(GDT_Int16),
                                anSubVals.data()));

  poDS.reset();

  // 5. Re-open and verify
  {
    auto poReadDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(filename.c_str(), GDAL_OF_MULTIDIM_RASTER, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    auto poRG = poReadDS->GetRootGroup();
    ASSERT_NE(nullptr, poRG);

    // Verify group names
    auto aosGroups = poRG->GetGroupNames();
    EXPECT_EQ(1, aosGroups.size());
    EXPECT_EQ("subgroup", aosGroups[0]);

    // Verify arrays
    auto aosArrays = poRG->GetMDArrayNames();
    EXPECT_GE(aosArrays.size(), 2);

    auto poReadX = poRG->OpenMDArray("x");
    ASSERT_NE(nullptr, poReadX);
    std::vector<double> adfReadX(10);
    EXPECT_TRUE(poReadX->Read(anStart0.data(), anCount10, nullptr, nullptr,
                              GDALExtendedDataType::Create(GDT_Float64),
                              adfReadX.data()));
    for (int i = 0; i < 10; ++i) {
      EXPECT_DOUBLE_EQ(100.0 + i * 0.5, adfReadX[i]);
    }

    auto poReadData = poRG->OpenMDArray("temperature");
    ASSERT_NE(nullptr, poReadData);
    EXPECT_EQ("celsius", poReadData->GetUnit());

    bool bHasNoData = false;
    double dfNoData = poReadData->GetNoDataValueAsDouble(&bHasNoData);
    EXPECT_TRUE(bHasNoData);
    EXPECT_DOUBLE_EQ(-9999.0, dfNoData);

    bool bHasOffset = false;
    double dfOffset = poReadData->GetOffset(&bHasOffset);
    EXPECT_TRUE(bHasOffset);
    EXPECT_DOUBLE_EQ(10.0, dfOffset);

    bool bHasScale = false;
    double dfScale = poReadData->GetScale(&bHasScale);
    EXPECT_TRUE(bHasScale);
    EXPECT_DOUBLE_EQ(0.1, dfScale);

    auto poReadSRS = poReadData->GetSpatialRef();
    ASSERT_NE(nullptr, poReadSRS);
    EXPECT_TRUE(poReadSRS->IsGeographic());

    std::vector<float> afReadData(80);
    EXPECT_TRUE(poReadData->Read(
        anStart0x0.data(), anCount8x10, nullptr, nullptr,
        GDALExtendedDataType::Create(GDT_Float32), afReadData.data()));
    for (int i = 0; i < 80; ++i) {
      EXPECT_FLOAT_EQ(static_cast<float>(i * 1.5f), afReadData[i]);
    }

    // Verify subgroup
    auto poReadSub = poRG->OpenGroup("subgroup");
    ASSERT_NE(nullptr, poReadSub);
    auto poReadSubArray = poReadSub->OpenMDArray("sub_array");
    ASSERT_NE(nullptr, poReadSubArray);
    std::vector<short> anReadSub(5);
    EXPECT_TRUE(poReadSubArray->Read(
        anStart0.data(), anCount5, nullptr, nullptr,
        GDALExtendedDataType::Create(GDT_Int16), anReadSub.data()));
    for (int i = 0; i < 5; ++i) {
      EXPECT_EQ(i + 1, anReadSub[i]);
    }
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFMultiDimTest, AttributesAllTypesAndOperations) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "multidim_attrs.nc")
          .string();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  {
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->CreateMultiDimensional(filename.c_str(), nullptr, nullptr));
    ASSERT_NE(nullptr, poDS);

    auto poRootGroup = poDS->GetRootGroup();
    ASSERT_NE(nullptr, poRootGroup);

    // String attribute (NC_STRING)
    auto a_str = poRootGroup->CreateAttribute(
        "a_str", {}, GDALExtendedDataType::CreateString(), nullptr);
    ASSERT_NE(nullptr, a_str);
    EXPECT_TRUE(a_str->Write("hello_nc4"));

    // Char attribute (NC_CHAR)
    char** papszCharOpts = nullptr;
    papszCharOpts = CSLSetNameValue(papszCharOpts, "NC_TYPE", "NC_CHAR");
    auto a_char = poRootGroup->CreateAttribute(
        "a_char", {}, GDALExtendedDataType::CreateString(), papszCharOpts);
    CSLDestroy(papszCharOpts);
    ASSERT_NE(nullptr, a_char);
    EXPECT_TRUE(a_char->Write("hello_nc3"));

    // Byte / Int8
    auto a_byte = poRootGroup->CreateAttribute(
        "a_byte", {}, GDALExtendedDataType::Create(GDT_Byte), nullptr);
    ASSERT_NE(nullptr, a_byte);
    EXPECT_TRUE(a_byte->WriteInt(250));

    auto a_int8 = poRootGroup->CreateAttribute(
        "a_int8", {}, GDALExtendedDataType::Create(GDT_Int8), nullptr);
    ASSERT_NE(nullptr, a_int8);
    EXPECT_TRUE(a_int8->WriteInt(-120));

    // Int16 / UInt16
    auto a_int16 = poRootGroup->CreateAttribute(
        "a_int16", {}, GDALExtendedDataType::Create(GDT_Int16), nullptr);
    ASSERT_NE(nullptr, a_int16);
    EXPECT_TRUE(a_int16->WriteInt(-30000));

    auto a_uint16 = poRootGroup->CreateAttribute(
        "a_uint16", {}, GDALExtendedDataType::Create(GDT_UInt16), nullptr);
    ASSERT_NE(nullptr, a_uint16);
    EXPECT_TRUE(a_uint16->WriteInt(60000));

    // Int32 / UInt32
    auto a_int32 = poRootGroup->CreateAttribute(
        "a_int32", {}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
    ASSERT_NE(nullptr, a_int32);
    EXPECT_TRUE(a_int32->WriteInt(-1234567));

    auto a_uint32 = poRootGroup->CreateAttribute(
        "a_uint32", {}, GDALExtendedDataType::Create(GDT_UInt32), nullptr);
    ASSERT_NE(nullptr, a_uint32);
    EXPECT_TRUE(a_uint32->WriteInt64(3000000000LL));

    // Int64 / UInt64
    auto a_int64 = poRootGroup->CreateAttribute(
        "a_int64", {}, GDALExtendedDataType::Create(GDT_Int64), nullptr);
    ASSERT_NE(nullptr, a_int64);
    EXPECT_TRUE(a_int64->WriteInt64(-9000000000000LL));

    auto a_uint64 = poRootGroup->CreateAttribute(
        "a_uint64", {}, GDALExtendedDataType::Create(GDT_UInt64), nullptr);
    ASSERT_NE(nullptr, a_uint64);
    EXPECT_TRUE(a_uint64->Write(1.8e19));

    // Float32 / Float64
    auto a_flt = poRootGroup->CreateAttribute(
        "a_flt", {}, GDALExtendedDataType::Create(GDT_Float32), nullptr);
    ASSERT_NE(nullptr, a_flt);
    EXPECT_TRUE(a_flt->Write(123.456));

    auto a_dbl = poRootGroup->CreateAttribute(
        "a_dbl", {}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
    ASSERT_NE(nullptr, a_dbl);
    EXPECT_TRUE(a_dbl->Write(987.654321));

    // Multi-valued string attribute
    auto a_str_arr = poRootGroup->CreateAttribute(
        "a_str_arr", {3}, GDALExtendedDataType::CreateString(), nullptr);
    ASSERT_NE(nullptr, a_str_arr);
    CPLStringList aosStrings;
    aosStrings.AddString("one");
    aosStrings.AddString("two");
    aosStrings.AddString("three");
    EXPECT_TRUE(a_str_arr->Write(aosStrings.List()));

    // Multi-valued double attribute
    auto a_dbl_arr = poRootGroup->CreateAttribute(
        "a_dbl_arr", {2}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
    ASSERT_NE(nullptr, a_dbl_arr);
    double adfDbls[2] = {1.5, 2.5};
    EXPECT_TRUE(a_dbl_arr->Write(adfDbls, 2));

    // Attribute to delete
    auto a_del = poRootGroup->CreateAttribute(
        "to_delete", {}, GDALExtendedDataType::CreateString(), nullptr);
    ASSERT_NE(nullptr, a_del);
    EXPECT_TRUE(a_del->Write("goodbye"));
    EXPECT_TRUE(poRootGroup->DeleteAttribute("to_delete"));
    EXPECT_EQ(nullptr, poRootGroup->GetAttribute("to_delete"));

    // Update existing attribute
    char** papszUpd = nullptr;
    papszUpd = CSLSetNameValue(papszUpd, "UPDATE_METADATA", "YES");
    auto a_upd = poRootGroup->CreateAttribute(
        "a_int32", {}, GDALExtendedDataType::Create(GDT_Int32), papszUpd);
    CSLDestroy(papszUpd);
    ASSERT_NE(nullptr, a_upd);
    EXPECT_TRUE(a_upd->WriteInt(7654321));

    // Create an array and add an attribute to it
    auto poDim = poRootGroup->CreateDimension("d", "", "", 2, nullptr);
    ASSERT_NE(nullptr, poDim);
    auto poVar = poRootGroup->CreateMDArray(
        "v", {poDim}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
    ASSERT_NE(nullptr, poVar);
    auto a_var = poVar->CreateAttribute(
        "var_att", {}, GDALExtendedDataType::CreateString(), nullptr);
    ASSERT_NE(nullptr, a_var);
    EXPECT_TRUE(a_var->Write("variable attribute value"));
  }

  // Re-open and verify reads
  {
    auto poReadDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(filename.c_str(), GDAL_OF_MULTIDIM_RASTER, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    auto poRG = poReadDS->GetRootGroup();
    ASSERT_NE(nullptr, poRG);

    auto r_str = poRG->GetAttribute("a_str");
    ASSERT_NE(nullptr, r_str);
    EXPECT_STREQ("hello_nc4", r_str->ReadAsString());

    auto r_char = poRG->GetAttribute("a_char");
    ASSERT_NE(nullptr, r_char);
    EXPECT_STREQ("hello_nc3", r_char->ReadAsString());

    auto r_byte = poRG->GetAttribute("a_byte");
    ASSERT_NE(nullptr, r_byte);
    EXPECT_EQ(250, r_byte->ReadAsInt());

    auto r_int8 = poRG->GetAttribute("a_int8");
    ASSERT_NE(nullptr, r_int8);
    EXPECT_EQ(-120, r_int8->ReadAsInt());

    auto r_int16 = poRG->GetAttribute("a_int16");
    ASSERT_NE(nullptr, r_int16);
    EXPECT_EQ(-30000, r_int16->ReadAsInt());

    auto r_uint16 = poRG->GetAttribute("a_uint16");
    ASSERT_NE(nullptr, r_uint16);
    EXPECT_EQ(60000, r_uint16->ReadAsInt());

    auto r_int32 = poRG->GetAttribute("a_int32");
    ASSERT_NE(nullptr, r_int32);
    EXPECT_EQ(7654321, r_int32->ReadAsInt());

    auto r_uint32 = poRG->GetAttribute("a_uint32");
    ASSERT_NE(nullptr, r_uint32);
    EXPECT_EQ(3000000000LL, r_uint32->ReadAsInt64());

    auto r_int64 = poRG->GetAttribute("a_int64");
    ASSERT_NE(nullptr, r_int64);
    EXPECT_EQ(-9000000000000LL, r_int64->ReadAsInt64());

    auto r_flt = poRG->GetAttribute("a_flt");
    ASSERT_NE(nullptr, r_flt);
    EXPECT_NEAR(123.456, r_flt->ReadAsDouble(), 1e-4);

    auto r_dbl = poRG->GetAttribute("a_dbl");
    ASSERT_NE(nullptr, r_dbl);
    EXPECT_NEAR(987.654321, r_dbl->ReadAsDouble(), 1e-6);

    auto r_str_arr = poRG->GetAttribute("a_str_arr");
    ASSERT_NE(nullptr, r_str_arr);
    auto aosReadStrs = r_str_arr->ReadAsStringArray();
    EXPECT_EQ(3, aosReadStrs.size());
    EXPECT_STREQ("one", aosReadStrs[0]);
    EXPECT_STREQ("two", aosReadStrs[1]);
    EXPECT_STREQ("three", aosReadStrs[2]);

    auto r_dbl_arr = poRG->GetAttribute("a_dbl_arr");
    ASSERT_NE(nullptr, r_dbl_arr);
    auto adfReadDbls = r_dbl_arr->ReadAsDoubleArray();
    EXPECT_EQ(2, adfReadDbls.size());
    EXPECT_DOUBLE_EQ(1.5, adfReadDbls[0]);
    EXPECT_DOUBLE_EQ(2.5, adfReadDbls[1]);

    // Check variable attribute
    auto poReadVar = poRG->OpenMDArray("v");
    ASSERT_NE(nullptr, poReadVar);
    auto r_var_att = poReadVar->GetAttribute("var_att");
    ASSERT_NE(nullptr, r_var_att);
    EXPECT_STREQ("variable attribute value", r_var_att->ReadAsString());

    auto aoVarAttrs = poReadVar->GetAttributes();
    EXPECT_GE(aoVarAttrs.size(), 1);
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFMultiDimTest, MDArraysAllDataTypesAndNoData) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "multidim_types.nc")
          .string();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  auto poDS = std::unique_ptr<GDALDataset>(
      poDriver->CreateMultiDimensional(filename.c_str(), nullptr, nullptr));
  ASSERT_NE(nullptr, poDS);

  auto poRootGroup = poDS->GetRootGroup();
  ASSERT_NE(nullptr, poRootGroup);

  auto poDim = poRootGroup->CreateDimension("dim2", "", "", 2, nullptr);
  ASSERT_NE(nullptr, poDim);

  struct TypeTestEntry {
    std::string name;
    GDALDataType eType;
    double dfNoData;
    double dfVal0;
    double dfVal1;
  };

  std::vector<TypeTestEntry> entries = {
      {"v_byte", GDT_Byte, 255.0, 10.0, 20.0},
      {"v_int8", GDT_Int8, -128.0, -10.0, 20.0},
      {"v_int16", GDT_Int16, -32768.0, -100.0, 200.0},
      {"v_uint16", GDT_UInt16, 65535.0, 1000.0, 2000.0},
      {"v_int32", GDT_Int32, -999999.0, -50000.0, 50000.0},
      {"v_uint32", GDT_UInt32, 999999.0, 100000.0, 200000.0},
      {"v_int64", GDT_Int64, -999999.0, -10000000000LL, 10000000000LL},
      {"v_uint64", GDT_UInt64, 999999.0, 10000000000ULL, 20000000000ULL},
      {"v_flt32", GDT_Float32, -9999.0, 1.25, 3.75},
      {"v_flt64", GDT_Float64, -99999.0, 10.125, 20.875},
  };

  for (const auto& entry : entries) {
    auto poArr = poRootGroup->CreateMDArray(
        entry.name, {poDim}, GDALExtendedDataType::Create(entry.eType),
        nullptr);
    ASSERT_NE(nullptr, poArr);

    EXPECT_TRUE(poArr->SetNoDataValue(entry.dfNoData));

    bool bHasNoData = false;
    double dfND = poArr->GetNoDataValueAsDouble(&bHasNoData);
    EXPECT_TRUE(bHasNoData);
    EXPECT_DOUBLE_EQ(entry.dfNoData, dfND);

    // Also test GetRawNoDataValue
    const void* pRawND = poArr->GetRawNoDataValue();
    EXPECT_NE(nullptr, pRawND);

    double vals[2] = {entry.dfVal0, entry.dfVal1};
    const std::vector<GUInt64> start = {0};
    const std::vector<size_t> count = {2};
    EXPECT_TRUE(poArr->Write(start.data(), count.data(), nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Float64), vals));
  }

  // String array (NC_STRING)
  auto poVarStr = poRootGroup->CreateMDArray(
      "v_str", {poDim}, GDALExtendedDataType::CreateString(), nullptr);
  ASSERT_NE(nullptr, poVarStr);
  const char* aszStrs[2] = {"alpha", "beta"};
  const std::vector<GUInt64> start = {0};
  const std::vector<size_t> count = {2};
  EXPECT_TRUE(poVarStr->Write(start.data(), count.data(), nullptr, nullptr,
                              GDALExtendedDataType::CreateString(), aszStrs));

  // Scalar 0D array
  auto poVarScalar = poRootGroup->CreateMDArray(
      "v_scalar", {}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
  ASSERT_NE(nullptr, poVarScalar);
  EXPECT_EQ(0, poVarScalar->GetDimensionCount());
  double dfScalarVal = 42.5;
  EXPECT_TRUE(poVarScalar->Write(nullptr, nullptr, nullptr, nullptr,
                                 GDALExtendedDataType::Create(GDT_Float64),
                                 &dfScalarVal));

  poDS.reset();

  // Re-open and verify
  {
    auto poReadDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(filename.c_str(), GDAL_OF_MULTIDIM_RASTER, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    auto poRG = poReadDS->GetRootGroup();
    ASSERT_NE(nullptr, poRG);

    for (const auto& entry : entries) {
      auto poArr = poRG->OpenMDArray(entry.name);
      ASSERT_NE(nullptr, poArr);

      bool bHasND = false;
      double dfND = poArr->GetNoDataValueAsDouble(&bHasND);
      EXPECT_TRUE(bHasND);
      EXPECT_DOUBLE_EQ(entry.dfNoData, dfND);

      const void* pRawND = poArr->GetRawNoDataValue();
      EXPECT_NE(nullptr, pRawND);

      double readVals[2] = {0, 0};
      EXPECT_TRUE(poArr->Read(start.data(), count.data(), nullptr, nullptr,
                              GDALExtendedDataType::Create(GDT_Float64),
                              readVals));
      EXPECT_DOUBLE_EQ(entry.dfVal0, readVals[0]);
      EXPECT_DOUBLE_EQ(entry.dfVal1, readVals[1]);
    }

    auto poReadStr = poRG->OpenMDArray("v_str");
    ASSERT_NE(nullptr, poReadStr);
    char* aszReadStrs[2] = {nullptr, nullptr};
    EXPECT_TRUE(poReadStr->Read(start.data(), count.data(), nullptr, nullptr,
                                GDALExtendedDataType::CreateString(),
                                aszReadStrs));
    EXPECT_STREQ("alpha", aszReadStrs[0]);
    EXPECT_STREQ("beta", aszReadStrs[1]);
    CPLFree(aszReadStrs[0]);
    CPLFree(aszReadStrs[1]);

    auto poReadScalar = poRG->OpenMDArray("v_scalar");
    ASSERT_NE(nullptr, poReadScalar);
    EXPECT_EQ(0, poReadScalar->GetDimensionCount());
    double dfReadScalar = 0;
    EXPECT_TRUE(poReadScalar->Read(nullptr, nullptr, nullptr, nullptr,
                                   GDALExtendedDataType::Create(GDT_Float64),
                                   &dfReadScalar));
    EXPECT_DOUBLE_EQ(42.5, dfReadScalar);
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFMultiDimTest, ReadWriteStridesAndTypeConversion) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "multidim_strides.nc")
          .string();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  {
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->CreateMultiDimensional(filename.c_str(), nullptr, nullptr));
    ASSERT_NE(nullptr, poDS);

    auto poRootGroup = poDS->GetRootGroup();
    ASSERT_NE(nullptr, poRootGroup);

    auto dimZ = poRootGroup->CreateDimension("z", "", "", 2, nullptr);
    auto dimY = poRootGroup->CreateDimension("y", "", "", 3, nullptr);
    auto dimX = poRootGroup->CreateDimension("x", "", "", 4, nullptr);
    ASSERT_NE(nullptr, dimZ);
    ASSERT_NE(nullptr, dimY);
    ASSERT_NE(nullptr, dimX);

    // 3D array: 2 x 3 x 4 = 24 elements of GDT_Int32
    auto poVar3D = poRootGroup->CreateMDArray(
        "var3d", {dimZ, dimY, dimX}, GDALExtendedDataType::Create(GDT_Int32),
        nullptr);
    ASSERT_NE(nullptr, poVar3D);

    // Raw nodata value (must be set before writing data in NetCDF-4)
    int nRawNoData = -999;
    EXPECT_TRUE(poVar3D->SetRawNoDataValue(&nRawNoData));
    EXPECT_TRUE(poVar3D->SetRawNoDataValue(nullptr));

    std::vector<int> anData3D(24);
    for (int i = 0; i < 24; ++i) anData3D[i] = i + 1;
    const std::vector<GUInt64> start0 = {0, 0, 0};
    const size_t count2x3x4[3] = {2, 3, 4};
    EXPECT_TRUE(poVar3D->Write(start0.data(), count2x3x4, nullptr, nullptr,
                               GDALExtendedDataType::Create(GDT_Int32),
                               anData3D.data()));

    // Statistics (returns CPLErr: CE_None == 0)
    double dfMin = 0, dfMax = 0, dfMean = 0, dfStdDev = 0;
    GUInt64 nValidCount = 0;
    EXPECT_EQ(CE_None, poVar3D->GetStatistics(false, true, &dfMin, &dfMax,
                                              &dfMean, &dfStdDev, &nValidCount,
                                              nullptr, nullptr));
    EXPECT_DOUBLE_EQ(1.0, dfMin);
    EXPECT_DOUBLE_EQ(24.0, dfMax);
    EXPECT_EQ(24, nValidCount);

    EXPECT_EQ(CE_None, poVar3D->GetStatistics(false, false, &dfMin, &dfMax,
                                              &dfMean, &dfStdDev, &nValidCount,
                                              nullptr, nullptr));
    poVar3D->ClearStatistics();

    // Structural info & block size
    EXPECT_NE(nullptr, poRootGroup->GetStructuralInfo());
    EXPECT_FALSE(poVar3D->GetBlockSize().empty());

    // 1D array of GDT_Int32
    auto poVar1D = poRootGroup->CreateMDArray(
        "var1d", {dimX}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
    ASSERT_NE(nullptr, poVar1D);
    std::vector<int> anData1D = {10, 20, 30, 40};
    const std::vector<GUInt64> start1D_0 = {0};
    const size_t count1D_4[1] = {4};
    EXPECT_TRUE(poVar1D->Write(start1D_0.data(), count1D_4, nullptr, nullptr,
                               GDALExtendedDataType::Create(GDT_Int32),
                               anData1D.data()));

    // Write with negative step on 1D array
    const std::vector<GUInt64> start1D_neg = {3};
    const size_t count1D_2[1] = {2};
    const GInt64 step1D_neg[1] = {-2};
    const GPtrDiff_t stride1D[1] = {1};
    int anData1D_sub[2] = {99, 88};
    EXPECT_TRUE(
        poVar1D->Write(start1D_neg.data(), count1D_2, step1D_neg, stride1D,
                       GDALExtendedDataType::Create(GDT_Int32), anData1D_sub));

    // 2D array of GDT_Float32
    auto poVar2D = poRootGroup->CreateMDArray(
        "var2d", {dimY, dimX}, GDALExtendedDataType::Create(GDT_Float32),
        nullptr);
    ASSERT_NE(nullptr, poVar2D);
    std::vector<float> afData2D(12);
    for (int i = 0; i < 12; ++i) afData2D[i] = static_cast<float>(i * 2);
    const std::vector<GUInt64> start2D_0 = {0, 0};
    const size_t count3x4[2] = {3, 4};
    EXPECT_TRUE(poVar2D->Write(start2D_0.data(), count3x4, nullptr, nullptr,
                               GDALExtendedDataType::Create(GDT_Float32),
                               afData2D.data()));

    // Write with negative step on 2D: step = {-1, 1}, start = {2, 0}, count =
    // {2, 4}. Row 2 <- {100..103}, row 1 <- {104..107}.
    const std::vector<GUInt64> start2D_neg = {2, 0};
    const size_t count2x4[2] = {2, 4};
    const GInt64 step2D_neg[2] = {-1, 1};
    const GPtrDiff_t stride2x4[2] = {4, 1};
    std::vector<float> afData2D_sub(8);
    for (int i = 0; i < 8; ++i) afData2D_sub[i] = static_cast<float>(100 + i);
    EXPECT_TRUE(poVar2D->Write(
        start2D_neg.data(), count2x4, step2D_neg, stride2x4,
        GDALExtendedDataType::Create(GDT_Float32), afData2D_sub.data()));

    // Write with negative buffer stride: bufferStride = {-4, 1}, buffer
    // pointer at element 4. Row 0 <- {104..107}, row 1 <- {100..103}.
    const GInt64 step2D_pos[2] = {1, 1};
    const GPtrDiff_t stride2x4_neg[2] = {-4, 1};
    EXPECT_TRUE(poVar2D->Write(
        start2D_0.data(), count2x4, step2D_pos, stride2x4_neg,
        GDALExtendedDataType::Create(GDT_Float32), afData2D_sub.data() + 4));
  }

  // Re-open and verify reads
  {
    auto poReadDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(filename.c_str(), GDAL_OF_MULTIDIM_RASTER, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    auto poRG = poReadDS->GetRootGroup();
    ASSERT_NE(nullptr, poRG);

    auto poVar3D = poRG->OpenMDArray("var3d");
    ASSERT_NE(nullptr, poVar3D);

    // Read full 3D array with conversion to Float64
    std::vector<double> adfData3D(24);
    const std::vector<GUInt64> start0 = {0, 0, 0};
    const size_t count2x3x4[3] = {2, 3, 4};
    EXPECT_TRUE(poVar3D->Read(start0.data(), count2x3x4, nullptr, nullptr,
                              GDALExtendedDataType::Create(GDT_Float64),
                              adfData3D.data()));
    EXPECT_DOUBLE_EQ(1.0, adfData3D[0]);
    EXPECT_DOUBLE_EQ(24.0, adfData3D[23]);

    // Read 3D with step != 1 (e.g. step = {1, 2, 2}, count = {2, 2, 2})
    const size_t count2x2x2[3] = {2, 2, 2};
    const GInt64 step3D[3] = {1, 2, 2};
    const GPtrDiff_t stride2x2x2[3] = {4, 2, 1};
    std::vector<int> anData3D_step(8);
    EXPECT_TRUE(poVar3D->Read(start0.data(), count2x2x2, step3D, stride2x2x2,
                              GDALExtendedDataType::Create(GDT_Int32),
                              anData3D_step.data()));
    EXPECT_THAT(anData3D_step, ElementsAre(1, 3, 9, 11, 13, 15, 21, 23));

    // Read 3D with negative step: step = {1, -1, 1}, start = {0, 2, 0}, count =
    // {2, 3, 4}. The y rows come out reversed.
    const std::vector<GUInt64> start3D_neg = {0, 2, 0};
    const GInt64 step3D_neg[3] = {1, -1, 1};
    const GPtrDiff_t stride2x3x4[3] = {12, 4, 1};
    std::vector<int> anData3D_neg(24);
    EXPECT_TRUE(poVar3D->Read(
        start3D_neg.data(), count2x3x4, step3D_neg, stride2x3x4,
        GDALExtendedDataType::Create(GDT_Int32), anData3D_neg.data()));
    EXPECT_THAT(anData3D_neg,
                ElementsAre(9, 10, 11, 12, 5, 6, 7, 8, 1, 2, 3, 4,  //
                            21, 22, 23, 24, 17, 18, 19, 20, 13, 14, 15, 16));

    // Read 3D with negative buffer stride: bufferStride = {-12, 4, 1} and the
    // buffer pointer at element 12, so the two z slices swap in the buffer.
    const GPtrDiff_t stride3D_negBuf[3] = {-12, 4, 1};
    EXPECT_TRUE(poVar3D->Read(
        start0.data(), count2x3x4, nullptr, stride3D_negBuf,
        GDALExtendedDataType::Create(GDT_Int32), anData3D_neg.data() + 12));
    EXPECT_THAT(anData3D_neg,
                ElementsAre(13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,  //
                            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12));

    // Test AdviseRead and cached read
    EXPECT_TRUE(poVar3D->AdviseRead(start0.data(), count2x3x4, nullptr));
    std::vector<int> anCachedRead(24);
    EXPECT_TRUE(poVar3D->Read(start0.data(), count2x3x4, nullptr, nullptr,
                              GDALExtendedDataType::Create(GDT_Int32),
                              anCachedRead.data()));
    EXPECT_EQ(1, anCachedRead[0]);
    EXPECT_EQ(24, anCachedRead[23]);

    // Read 1D with negative step
    auto poVar1D = poRG->OpenMDArray("var1d");
    ASSERT_NE(nullptr, poVar1D);
    const std::vector<GUInt64> start1D_neg = {3};
    const size_t count1D_2[1] = {2};
    const GInt64 step1D_neg[1] = {-2};
    const GPtrDiff_t stride1D[1] = {1};
    int anRead1D[2] = {0, 0};
    EXPECT_TRUE(poVar1D->Read(start1D_neg.data(), count1D_2, step1D_neg,
                              stride1D, GDALExtendedDataType::Create(GDT_Int32),
                              anRead1D));
    EXPECT_EQ(99, anRead1D[0]);
    EXPECT_EQ(88, anRead1D[1]);

    // Read 2D. After the writes above: row 0 = {104..107}, row 1 = {100..103},
    // row 2 = {100..103}.
    auto poVar2D = poRG->OpenMDArray("var2d");
    ASSERT_NE(nullptr, poVar2D);
    const std::vector<GUInt64> start2D_0 = {0, 0};
    const size_t count3x4[2] = {3, 4};
    std::vector<float> afRead2DFull(12);
    EXPECT_TRUE(poVar2D->Read(start2D_0.data(), count3x4, nullptr, nullptr,
                              GDALExtendedDataType::Create(GDT_Float32),
                              afRead2DFull.data()));
    EXPECT_THAT(afRead2DFull,
                ElementsAre(104, 105, 106, 107, 100, 101, 102, 103,  //
                            100, 101, 102, 103));

    // Read 2D with negative step: rows 2 then 1.
    const std::vector<GUInt64> start2D_neg = {2, 0};
    const size_t count2x4[2] = {2, 4};
    const GInt64 step2D_neg[2] = {-1, 1};
    const GPtrDiff_t stride2x4[2] = {4, 1};
    std::vector<float> afRead2D(8);
    EXPECT_TRUE(poVar2D->Read(
        start2D_neg.data(), count2x4, step2D_neg, stride2x4,
        GDALExtendedDataType::Create(GDT_Float32), afRead2D.data()));
    EXPECT_THAT(afRead2D, ElementsAre(100, 101, 102, 103, 100, 101, 102, 103));
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFMultiDimTest, DimensionsUnlimitedResizeAndIndexing) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "multidim_resize.nc")
          .string();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  {
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->CreateMultiDimensional(filename.c_str(), nullptr, nullptr));
    ASSERT_NE(nullptr, poDS);

    auto poRootGroup = poDS->GetRootGroup();
    ASSERT_NE(nullptr, poRootGroup);

    // Unlimited dimension: UNLIMITED=YES
    char** papszDimOpts = nullptr;
    papszDimOpts = CSLSetNameValue(papszDimOpts, "UNLIMITED", "YES");
    auto dimTime =
        poRootGroup->CreateDimension("time", "", "", 0, papszDimOpts);
    CSLDestroy(papszDimOpts);
    ASSERT_NE(nullptr, dimTime);

    // Fixed dimension
    auto dimX = poRootGroup->CreateDimension("x", "", "", 4, nullptr);
    ASSERT_NE(nullptr, dimX);

    // 1D Indexing variable for time
    auto varTime = poRootGroup->CreateMDArray(
        "time", {dimTime}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
    ASSERT_NE(nullptr, varTime);

    // Verify indexing variable on dimension
    auto poIndexingVar = dimTime->GetIndexingVariable();
    ASSERT_NE(nullptr, poIndexingVar);
    EXPECT_EQ("time", poIndexingVar->GetName());

    // 2D Array using unlimited dimension
    char** papszVarOpts = nullptr;
    papszVarOpts =
        CSLSetNameValue(papszVarOpts, "RAW_DATA_CHUNK_CACHE_SIZE", "1000000");
    papszVarOpts = CSLSetNameValue(papszVarOpts, "CHUNK_SLOTS", "500");
    papszVarOpts = CSLSetNameValue(papszVarOpts, "PREEMPTION", "0.75");
    papszVarOpts = CSLSetNameValue(papszVarOpts, "BLOCKSIZE", "2,4");
    auto varData = poRootGroup->CreateMDArray(
        "data", {dimTime, dimX}, GDALExtendedDataType::Create(GDT_Float32),
        papszVarOpts);
    CSLDestroy(papszVarOpts);
    ASSERT_NE(nullptr, varData);

    // Resize tests:
    // Error: wrong dimension count
    EXPECT_FALSE(varData->Resize({5}, nullptr));
    // Error: cannot grow non-unlimited dimension
    EXPECT_FALSE(varData->Resize({5, 8}, nullptr));

    // Success: grow unlimited dimension
    EXPECT_TRUE(varData->Resize({3, 4}, nullptr));
    EXPECT_EQ(3, dimTime->GetSize());

    // Error: shrinking not supported
    EXPECT_FALSE(varData->Resize({2, 4}, nullptr));

    // Write into resized array
    std::vector<float> afData(12, 42.0f);
    const std::vector<GUInt64> start0 = {0, 0};
    const size_t count3x4[2] = {3, 4};
    EXPECT_TRUE(varData->Write(start0.data(), count3x4, nullptr, nullptr,
                               GDALExtendedDataType::Create(GDT_Float32),
                               afData.data()));

    // Renaming tests
    // Group rename error: empty name, root group
    EXPECT_FALSE(poRootGroup->Rename(""));
    EXPECT_FALSE(poRootGroup->Rename("new_root"));

    // Subgroup creation and rename
    auto poSub = poRootGroup->CreateGroup("sub_orig", nullptr);
    ASSERT_NE(nullptr, poSub);
    auto poSubDim = poSub->CreateDimension("sub_dim", "", "", 2, nullptr);
    ASSERT_NE(nullptr, poSubDim);
    auto poSubVar =
        poSub->CreateMDArray("sub_var", {poSubDim},
                             GDALExtendedDataType::Create(GDT_Int32), nullptr);
    ASSERT_NE(nullptr, poSubVar);
    auto poSubAtt = poSubVar->CreateAttribute(
        "sub_att", {}, GDALExtendedDataType::CreateString(), nullptr);
    ASSERT_NE(nullptr, poSubAtt);
    EXPECT_TRUE(poSubAtt->Write("val"));

    EXPECT_FALSE(poSub->Rename(""));
    EXPECT_TRUE(poSub->Rename("sub_renamed"));
    EXPECT_EQ("sub_renamed", poSub->GetName());

    // Dimension rename
    EXPECT_FALSE(poSubDim->Rename(""));
    EXPECT_TRUE(poSubDim->Rename("sub_dim_renamed"));
    EXPECT_EQ("sub_dim_renamed", poSubDim->GetName());

    // Variable rename
    EXPECT_FALSE(poSubVar->Rename(""));
    EXPECT_TRUE(poSubVar->Rename("sub_var_renamed"));
    EXPECT_EQ("sub_var_renamed", poSubVar->GetName());

    // Attribute rename
    EXPECT_FALSE(poSubAtt->Rename(""));
    EXPECT_TRUE(poSubAtt->Rename("sub_att_renamed"));
    EXPECT_EQ("sub_att_renamed", poSubAtt->GetName());

    // Delete variable attribute
    EXPECT_TRUE(poSubVar->DeleteAttribute("sub_att_renamed"));
    EXPECT_EQ(nullptr, poSubVar->GetAttribute("sub_att_renamed"));
  }

  // Re-open and verify
  {
    auto poReadDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(filename.c_str(), GDAL_OF_MULTIDIM_RASTER, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    auto poRG = poReadDS->GetRootGroup();
    ASSERT_NE(nullptr, poRG);

    auto poData = poRG->OpenMDArray("data");
    ASSERT_NE(nullptr, poData);
    EXPECT_EQ(2, poData->GetDimensionCount());
    EXPECT_EQ(3, poData->GetDimensions()[0]->GetSize());
    EXPECT_EQ(4, poData->GetDimensions()[1]->GetSize());

    std::vector<float> afRead(12);
    const std::vector<GUInt64> start0 = {0, 0};
    const size_t count3x4[2] = {3, 4};
    EXPECT_TRUE(poData->Read(start0.data(), count3x4, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Float32),
                             afRead.data()));
    EXPECT_FLOAT_EQ(42.0f, afRead[0]);
    EXPECT_FLOAT_EQ(42.0f, afRead[11]);
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFMultiDimTest, SpatialRefAndCoordinateVariables) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "multidim_srs.nc")
          .string();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  {
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->CreateMultiDimensional(filename.c_str(), nullptr, nullptr));
    ASSERT_NE(nullptr, poDS);

    auto poRootGroup = poDS->GetRootGroup();
    ASSERT_NE(nullptr, poRootGroup);

    // Dimensions with explicit types: X and Y
    auto dimY = poRootGroup->CreateDimension("y", GDAL_DIM_TYPE_HORIZONTAL_Y,
                                             "", 3, nullptr);
    auto dimX = poRootGroup->CreateDimension("x", GDAL_DIM_TYPE_HORIZONTAL_X,
                                             "", 4, nullptr);
    ASSERT_NE(nullptr, dimY);
    ASSERT_NE(nullptr, dimX);

    auto varY = poRootGroup->CreateMDArray(
        "y", {dimY}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
    auto varX = poRootGroup->CreateMDArray(
        "x", {dimX}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
    ASSERT_NE(nullptr, varY);
    ASSERT_NE(nullptr, varX);

    auto varProj = poRootGroup->CreateMDArray(
        "proj_var", {dimY, dimX}, GDALExtendedDataType::Create(GDT_Float32),
        nullptr);
    ASSERT_NE(nullptr, varProj);

    // Projected SRS (EPSG:32631)
    OGRSpatialReference oSRSProj;
    oSRSProj.importFromEPSG(32631);
    EXPECT_TRUE(varProj->SetSpatialRef(&oSRSProj));

    auto srsGot = varProj->GetSpatialRef();
    ASSERT_NE(nullptr, srsGot);
    EXPECT_TRUE(srsGot->IsProjected());

    // SetSpatialRef(nullptr) removes grid_mapping
    EXPECT_TRUE(varProj->SetSpatialRef(nullptr));
    EXPECT_EQ(nullptr, varProj->GetSpatialRef());

    // Re-set projected SRS
    EXPECT_TRUE(varProj->SetSpatialRef(&oSRSProj));

    // Dimensions without types but with indexing variables
    auto dimRow = poRootGroup->CreateDimension("row", "", "", 3, nullptr);
    auto dimCol = poRootGroup->CreateDimension("col", "", "", 4, nullptr);
    auto varRow = poRootGroup->CreateMDArray(
        "row", {dimRow}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
    auto varCol = poRootGroup->CreateMDArray(
        "col", {dimCol}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
    ASSERT_NE(nullptr, varRow);
    ASSERT_NE(nullptr, varCol);

    auto varProjUntyped = poRootGroup->CreateMDArray(
        "proj_untyped", {dimRow, dimCol},
        GDALExtendedDataType::Create(GDT_Float32), nullptr);
    ASSERT_NE(nullptr, varProjUntyped);
    EXPECT_TRUE(varProjUntyped->SetSpatialRef(&oSRSProj));

    // Geographic SRS (EPSG:4326) on lat/lon
    auto dimLat = poRootGroup->CreateDimension("lat", "", "", 3, nullptr);
    auto dimLon = poRootGroup->CreateDimension("lon", "", "", 4, nullptr);
    auto varLat = poRootGroup->CreateMDArray(
        "lat", {dimLat}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
    auto varLon = poRootGroup->CreateMDArray(
        "lon", {dimLon}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
    ASSERT_NE(nullptr, varLon);
    auto attLonUnits = varLon->CreateAttribute(
        "units", {}, GDALExtendedDataType::CreateString(), nullptr);
    ASSERT_NE(nullptr, attLonUnits);
    EXPECT_TRUE(attLonUnits->Write("degrees_east"));
    auto varGeo = poRootGroup->CreateMDArray(
        "geo_var", {dimLat, dimLon}, GDALExtendedDataType::Create(GDT_Float32),
        nullptr);
    ASSERT_NE(nullptr, varGeo);

    OGRSpatialReference oSRSGeo;
    oSRSGeo.importFromEPSG(4326);
    oSRSGeo.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    EXPECT_TRUE(varGeo->SetSpatialRef(&oSRSGeo));

    // Coordinates attribute with "lat lon"
    auto attCoords1 = varGeo->CreateAttribute(
        "coordinates", {}, GDALExtendedDataType::CreateString(), nullptr);
    ASSERT_NE(nullptr, attCoords1);
    EXPECT_TRUE(attCoords1->Write("lat lon"));
    auto coordVars = varGeo->GetCoordinateVariables();
    EXPECT_EQ(2, coordVars.size());
    auto idxVarLat = dimLat->GetIndexingVariable();
    EXPECT_NE(nullptr, idxVarLat);

    // Array with coordinates attribute in opposite order: "lon lat"
    auto varGeoRev = poRootGroup->CreateMDArray(
        "geo_var_rev", {dimLat, dimLon},
        GDALExtendedDataType::Create(GDT_Float32), nullptr);
    ASSERT_NE(nullptr, varGeoRev);
    auto attCoords2 = varGeoRev->CreateAttribute(
        "coordinates", {}, GDALExtendedDataType::CreateString(), nullptr);
    ASSERT_NE(nullptr, attCoords2);
    EXPECT_TRUE(attCoords2->Write("lon lat"));
    auto idxVarLon = dimLon->GetIndexingVariable();
    EXPECT_NE(nullptr, idxVarLon);
  }

  // Re-open and verify
  {
    auto poReadDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(filename.c_str(), GDAL_OF_MULTIDIM_RASTER, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    auto poRG = poReadDS->GetRootGroup();
    ASSERT_NE(nullptr, poRG);

    auto varProj = poRG->OpenMDArray("proj_var");
    ASSERT_NE(nullptr, varProj);
    auto srs = varProj->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    EXPECT_TRUE(srs->IsProjected());

    auto varGeo = poRG->OpenMDArray("geo_var");
    ASSERT_NE(nullptr, varGeo);
    auto srsGeo = varGeo->GetSpatialRef();
    ASSERT_NE(nullptr, srsGeo);
    EXPECT_TRUE(srsGeo->IsGeographic());
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFMultiDimTest, CompoundAndComplexDataTypes) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "multidim_compound.nc")
          .string();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  {
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->CreateMultiDimensional(filename.c_str(), nullptr, nullptr));
    ASSERT_NE(nullptr, poDS);

    auto poRootGroup = poDS->GetRootGroup();
    ASSERT_NE(nullptr, poRootGroup);

    auto dim = poRootGroup->CreateDimension("d", "", "", 2, nullptr);
    ASSERT_NE(nullptr, dim);

    // 1. Complex types: GDT_CInt16, GDT_CInt32, GDT_CFloat32, GDT_CFloat64
    auto varCInt16 = poRootGroup->CreateMDArray(
        "cint16", {dim}, GDALExtendedDataType::Create(GDT_CInt16), nullptr);
    ASSERT_NE(nullptr, varCInt16);
    short anCInt16[4] = {1, -1, 2, -2};
    const std::vector<GUInt64> start0 = {0};
    const size_t count2[1] = {2};
    EXPECT_TRUE(varCInt16->Write(start0.data(), count2, nullptr, nullptr,
                                 GDALExtendedDataType::Create(GDT_CInt16),
                                 anCInt16));

    auto varCInt32 = poRootGroup->CreateMDArray(
        "cint32", {dim}, GDALExtendedDataType::Create(GDT_CInt32), nullptr);
    ASSERT_NE(nullptr, varCInt32);
    int anCInt32[4] = {100, -100, 200, -200};
    EXPECT_TRUE(varCInt32->Write(start0.data(), count2, nullptr, nullptr,
                                 GDALExtendedDataType::Create(GDT_CInt32),
                                 anCInt32));

    auto varCFlt = poRootGroup->CreateMDArray(
        "cfloat32", {dim}, GDALExtendedDataType::Create(GDT_CFloat32), nullptr);
    ASSERT_NE(nullptr, varCFlt);
    float afCFlt[4] = {1.5f, 2.5f, 3.5f, 4.5f};
    EXPECT_TRUE(varCFlt->Write(start0.data(), count2, nullptr, nullptr,
                               GDALExtendedDataType::Create(GDT_CFloat32),
                               afCFlt));

    auto varCDbl = poRootGroup->CreateMDArray(
        "cfloat64", {dim}, GDALExtendedDataType::Create(GDT_CFloat64), nullptr);
    ASSERT_NE(nullptr, varCDbl);
    double adfCDbl[4] = {10.5, 20.5, 30.5, 40.5};
    EXPECT_TRUE(varCDbl->Write(start0.data(), count2, nullptr, nullptr,
                               GDALExtendedDataType::Create(GDT_CFloat64),
                               adfCDbl));

    // 2. Custom Compound Data Type
    std::vector<std::unique_ptr<GDALEDTComponent>> comps;
    comps.emplace_back(std::unique_ptr<GDALEDTComponent>(new GDALEDTComponent(
        "x", 0, GDALExtendedDataType::Create(GDT_Float32))));
    comps.emplace_back(std::unique_ptr<GDALEDTComponent>(new GDALEDTComponent(
        "y", 4, GDALExtendedDataType::Create(GDT_Float32))));
    auto compoundDT =
        GDALExtendedDataType::Create("MyPoint", 8, std::move(comps));
    EXPECT_EQ(GEDTC_COMPOUND, compoundDT.GetClass());

    auto varCompound =
        poRootGroup->CreateMDArray("points", {dim}, compoundDT, nullptr);
    ASSERT_NE(nullptr, varCompound);

    struct Point {
      float x;
      float y;
    };
    Point pts[2] = {{1.0f, 2.0f}, {3.0f, 4.0f}};
    EXPECT_TRUE(varCompound->Write(start0.data(), count2, nullptr, nullptr,
                                   compoundDT, pts));
  }

  // Re-open and verify
  {
    auto poReadDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(filename.c_str(), GDAL_OF_MULTIDIM_RASTER, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    auto poRG = poReadDS->GetRootGroup();
    ASSERT_NE(nullptr, poRG);

    auto varCInt16 = poRG->OpenMDArray("cint16");
    ASSERT_NE(nullptr, varCInt16);
    short anReadC16[4] = {};
    const std::vector<GUInt64> start0 = {0};
    const size_t count2[1] = {2};
    EXPECT_TRUE(varCInt16->Read(start0.data(), count2, nullptr, nullptr,
                                GDALExtendedDataType::Create(GDT_CInt16),
                                anReadC16));
    EXPECT_EQ(1, anReadC16[0]);
    EXPECT_EQ(-1, anReadC16[1]);

    auto varCFlt = poRG->OpenMDArray("cfloat32");
    ASSERT_NE(nullptr, varCFlt);
    float afReadCFlt[4] = {};
    EXPECT_TRUE(varCFlt->Read(start0.data(), count2, nullptr, nullptr,
                              GDALExtendedDataType::Create(GDT_CFloat32),
                              afReadCFlt));
    EXPECT_FLOAT_EQ(1.5f, afReadCFlt[0]);
    EXPECT_FLOAT_EQ(2.5f, afReadCFlt[1]);

    auto varCDbl = poRG->OpenMDArray("cfloat64");
    ASSERT_NE(nullptr, varCDbl);
    double adfReadCDbl[4] = {};
    EXPECT_TRUE(varCDbl->Read(start0.data(), count2, nullptr, nullptr,
                              GDALExtendedDataType::Create(GDT_CFloat64),
                              adfReadCDbl));
    EXPECT_DOUBLE_EQ(10.5, adfReadCDbl[0]);
    EXPECT_DOUBLE_EQ(20.5, adfReadCDbl[1]);

    auto varCompound = poRG->OpenMDArray("points");
    ASSERT_NE(nullptr, varCompound);
    const auto& dt = varCompound->GetDataType();
    EXPECT_EQ(GEDTC_COMPOUND, dt.GetClass());
    EXPECT_EQ(2, dt.GetComponents().size());
    EXPECT_EQ("x", dt.GetComponents()[0]->GetName());
    EXPECT_EQ("y", dt.GetComponents()[1]->GetName());

    struct Point {
      float x;
      float y;
    };
    Point ptsRead[2] = {};
    EXPECT_TRUE(varCompound->Read(start0.data(), count2, nullptr, nullptr, dt,
                                  ptsRead));
    EXPECT_FLOAT_EQ(1.0f, ptsRead[0].x);
    EXPECT_FLOAT_EQ(2.0f, ptsRead[0].y);
    EXPECT_FLOAT_EQ(3.0f, ptsRead[1].x);
    EXPECT_FLOAT_EQ(4.0f, ptsRead[1].y);
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFMultiDimTest, InquiryOptionsAndVirtualGroups) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "multidim_inquiry.nc")
          .string();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  {
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->CreateMultiDimensional(filename.c_str(), nullptr, nullptr));
    ASSERT_NE(nullptr, poDS);

    auto poRootGroup = poDS->GetRootGroup();
    ASSERT_NE(nullptr, poRootGroup);

    auto dimY = poRootGroup->CreateDimension("y", "", "", 2, nullptr);
    auto dimX = poRootGroup->CreateDimension("x", "", "", 3, nullptr);
    ASSERT_NE(nullptr, dimY);
    ASSERT_NE(nullptr, dimX);

    // Indexing variable x
    auto varX = poRootGroup->CreateMDArray(
        "x", {dimX}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
    ASSERT_NE(nullptr, varX);

    // Variable with standard_name="time"
    auto dimTime = poRootGroup->CreateDimension("time_dim", "", "", 2, nullptr);
    auto varTime = poRootGroup->CreateMDArray(
        "time_var", {dimTime}, GDALExtendedDataType::Create(GDT_Float64),
        nullptr);
    ASSERT_NE(nullptr, varTime);
    auto attStdName = varTime->CreateAttribute(
        "standard_name", {}, GDALExtendedDataType::CreateString(), nullptr);
    ASSERT_NE(nullptr, attStdName);
    EXPECT_TRUE(attStdName->Write("time"));

    // 0-dim scalar variable
    auto varScalar = poRootGroup->CreateMDArray(
        "scalar_var", {}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
    ASSERT_NE(nullptr, varScalar);

    // 2D data array with coordinates and bounds attributes
    auto varData = poRootGroup->CreateMDArray(
        "data_var", {dimY, dimX}, GDALExtendedDataType::Create(GDT_Float32),
        nullptr);
    ASSERT_NE(nullptr, varData);
    auto attCoord = varData->CreateAttribute(
        "coordinates", {}, GDALExtendedDataType::CreateString(), nullptr);
    ASSERT_NE(nullptr, attCoord);
    EXPECT_TRUE(attCoord->Write("coord_y coord_x"));

    auto attBounds = varData->CreateAttribute(
        "bounds", {}, GDALExtendedDataType::CreateString(), nullptr);
    ASSERT_NE(nullptr, attBounds);
    EXPECT_TRUE(attBounds->Write("bounds_var"));

    // Dummy coordinate and bounds variables
    auto varCoordY = poRootGroup->CreateMDArray(
        "coord_y", {dimY}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
    auto varCoordX = poRootGroup->CreateMDArray(
        "coord_x", {dimX}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
    auto varBounds = poRootGroup->CreateMDArray(
        "bounds_var", {dimY}, GDALExtendedDataType::Create(GDT_Float64),
        nullptr);
    ASSERT_NE(nullptr, varCoordY);
    ASSERT_NE(nullptr, varCoordX);
    ASSERT_NE(nullptr, varBounds);

    // Create METADATA / ISO_METADATA subgroup to test JSON metadata lookup
    auto poMeta = poRootGroup->CreateGroup("METADATA", nullptr);
    ASSERT_NE(nullptr, poMeta);
    auto poIsoMeta = poMeta->CreateGroup("ISO_METADATA", nullptr);
    ASSERT_NE(nullptr, poIsoMeta);
    auto attMeta = poIsoMeta->CreateAttribute(
        "title", {}, GDALExtendedDataType::CreateString(), nullptr);
    ASSERT_NE(nullptr, attMeta);
    EXPECT_TRUE(attMeta->Write("Test Product"));
  }

  // Re-open and verify inquiries
  {
    auto poReadDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(filename.c_str(), GDAL_OF_MULTIDIM_RASTER, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    auto poRG = poReadDS->GetRootGroup();
    ASSERT_NE(nullptr, poRG);

    // Default MDArrayNames
    auto namesDef = poRG->GetMDArrayNames();
    EXPECT_FALSE(namesDef.empty());

    // SHOW_ALL=YES
    char** papszShowAll = nullptr;
    papszShowAll = CSLSetNameValue(papszShowAll, "SHOW_ALL", "YES");
    auto namesAll = poRG->GetMDArrayNames(papszShowAll);
    CSLDestroy(papszShowAll);
    EXPECT_GE(namesAll.size(), namesDef.size());

    // SHOW_ZERO_DIM=YES
    char** papszZero = nullptr;
    papszZero = CSLSetNameValue(papszZero, "SHOW_ZERO_DIM", "YES");
    auto namesZero = poRG->GetMDArrayNames(papszZero);
    CSLDestroy(papszZero);
    EXPECT_NE(std::find(namesZero.begin(), namesZero.end(), "scalar_var"),
              namesZero.end());

    // SHOW_COORDINATES=NO
    char** papszNoCoords = nullptr;
    papszNoCoords = CSLSetNameValue(papszNoCoords, "SHOW_COORDINATES", "NO");
    auto namesNoCoords = poRG->GetMDArrayNames(papszNoCoords);
    CSLDestroy(papszNoCoords);
    EXPECT_EQ(std::find(namesNoCoords.begin(), namesNoCoords.end(), "coord_y"),
              namesNoCoords.end());

    // SHOW_BOUNDS=NO
    char** papszNoBounds = nullptr;
    papszNoBounds = CSLSetNameValue(papszNoBounds, "SHOW_BOUNDS", "NO");
    auto namesNoBounds = poRG->GetMDArrayNames(papszNoBounds);
    CSLDestroy(papszNoBounds);
    EXPECT_EQ(
        std::find(namesNoBounds.begin(), namesNoBounds.end(), "bounds_var"),
        namesNoBounds.end());

    // SHOW_INDEXING=NO
    char** papszNoIdx = nullptr;
    papszNoIdx = CSLSetNameValue(papszNoIdx, "SHOW_INDEXING", "NO");
    auto namesNoIdx = poRG->GetMDArrayNames(papszNoIdx);
    CSLDestroy(papszNoIdx);
    EXPECT_EQ(std::find(namesNoIdx.begin(), namesNoIdx.end(), "x"),
              namesNoIdx.end());

    // SHOW_TIME=NO
    char** papszNoTime = nullptr;
    papszNoTime = CSLSetNameValue(papszNoTime, "SHOW_TIME", "NO");
    auto namesNoTime = poRG->GetMDArrayNames(papszNoTime);
    CSLDestroy(papszNoTime);
    EXPECT_EQ(std::find(namesNoTime.begin(), namesNoTime.end(), "time_var"),
              namesNoTime.end());

    // GROUP_BY=SAME_DIMENSION virtual groups are only synthesized for groups
    // without real subgroups. This file has a METADATA subgroup, which the
    // driver hides (because it holds ISO_METADATA), so no group names are
    // reported either way. The virtual-group path itself is exercised in
    // AdvancedCoveragePaths.
    EXPECT_THAT(poRG->GetGroupNames(), ::testing::IsEmpty());
    CPLStringList aosGroupBy;
    aosGroupBy.SetNameValue("GROUP_BY", "SAME_DIMENSION");
    EXPECT_THAT(poRG->GetGroupNames(aosGroupBy.List()), ::testing::IsEmpty());
    EXPECT_EQ(nullptr, poRG->OpenGroup("x", aosGroupBy.List()));
    // The hidden METADATA group is still directly openable.
    EXPECT_NE(nullptr, poRG->OpenGroup("METADATA"));

    // ISO_METADATA attribute exposed as JSON built from the
    // METADATA/ISO_METADATA subgroup attributes.
    auto attIso = poRG->GetAttribute("ISO_METADATA");
    ASSERT_NE(nullptr, attIso);
    EXPECT_EQ(GEDTST_JSON, attIso->GetDataType().GetSubType());
    const char* pszIsoJson = attIso->ReadAsString();
    ASSERT_NE(nullptr, pszIsoJson);
    EXPECT_THAT(pszIsoJson, HasSubstr("\"title\""));
    EXPECT_THAT(pszIsoJson, HasSubstr("Test Product"));
    auto allRootAttrs = poRG->GetAttributes();
    EXPECT_FALSE(allRootAttrs.empty());
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFMultiDimTest, ClassicFormatsAndErrorPaths) {
  const std::string filenameNC =
      (std::filesystem::path(::testing::TempDir()) / "multidim_classic.nc")
          .string();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  // 1. FORMAT=NC (Classic NetCDF-3)
  {
    char** papszOpts = nullptr;
    papszOpts = CSLSetNameValue(papszOpts, "FORMAT", "NC");
    auto poDS = std::unique_ptr<GDALDataset>(poDriver->CreateMultiDimensional(
        filenameNC.c_str(), nullptr, papszOpts));
    CSLDestroy(papszOpts);
    ASSERT_NE(nullptr, poDS);

    auto poRootGroup = poDS->GetRootGroup();
    ASSERT_NE(nullptr, poRootGroup);

    // In classic format, creating a subgroup fails
    EXPECT_EQ(nullptr, poRootGroup->CreateGroup("subgroup", nullptr));

    // Dimension and array
    auto dim = poRootGroup->CreateDimension("d", "", "", 4, nullptr);
    ASSERT_NE(nullptr, dim);
    auto var = poRootGroup->CreateMDArray(
        "var_nc3", {dim}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
    ASSERT_NE(nullptr, var);

    // In NC3, GDT_Byte attribute defaults to NC_BYTE
    auto attByte = poRootGroup->CreateAttribute(
        "b_att", {}, GDALExtendedDataType::Create(GDT_Byte), nullptr);
    ASSERT_NE(nullptr, attByte);
    EXPECT_TRUE(attByte->WriteInt(120));

    // String attribute with NC_CHAR
    char** papszChar = nullptr;
    papszChar = CSLSetNameValue(papszChar, "NC_TYPE", "NC_CHAR");
    auto attChar = poRootGroup->CreateAttribute(
        "c_att", {}, GDALExtendedDataType::CreateString(), papszChar);
    CSLDestroy(papszChar);
    ASSERT_NE(nullptr, attChar);
    EXPECT_TRUE(attChar->Write("classic_text"));
  }

  // 2. Re-open read-only and test read-only error paths
  {
    auto poReadDS =
        std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(GDALOpenEx(
            filenameNC.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);

    auto poRG = poReadDS->GetRootGroup();
    ASSERT_NE(nullptr, poRG);

    // Read-only file rejects mutations:
    EXPECT_EQ(nullptr, poRG->CreateGroup("sub", nullptr));
    EXPECT_EQ(nullptr, poRG->CreateDimension("d2", "", "", 2, nullptr));
    EXPECT_EQ(nullptr,
              poRG->CreateMDArray(
                  "v2", {}, GDALExtendedDataType::Create(GDT_Int32), nullptr));
    EXPECT_EQ(nullptr,
              poRG->CreateAttribute(
                  "a2", {}, GDALExtendedDataType::CreateString(), nullptr));
    EXPECT_FALSE(poRG->DeleteAttribute("b_att"));
    EXPECT_FALSE(poRG->Rename("new_name"));

    auto var = poRG->OpenMDArray("var_nc3");
    ASSERT_NE(nullptr, var);
    EXPECT_FALSE(var->Resize({8}, nullptr));
    EXPECT_FALSE(var->Rename("var_new"));
    EXPECT_EQ(nullptr,
              var->CreateAttribute(
                  "a3", {}, GDALExtendedDataType::CreateString(), nullptr));
    EXPECT_FALSE(var->DeleteAttribute("some_att"));

    auto dims = poRG->GetDimensions();
    ASSERT_FALSE(dims.empty());
    EXPECT_FALSE(dims[0]->Rename("d_new"));

    auto att = poRG->GetAttribute("b_att");
    ASSERT_NE(nullptr, att);
    EXPECT_FALSE(att->Rename("b_new"));
  }

  VSIUnlink(filenameNC.c_str());

  // 3. In-memory dataset (/vsimem/) via nc_open_mem
  const std::string diskFile =
      (std::filesystem::path(::testing::TempDir()) / "multidim_to_mem.nc")
          .string();
  const std::string memFile = "/vsimem/mem_test.nc";
  {
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->CreateMultiDimensional(diskFile.c_str(), nullptr, nullptr));
    ASSERT_NE(nullptr, poDS);

    auto poRootGroup = poDS->GetRootGroup();
    ASSERT_NE(nullptr, poRootGroup);
    auto dim = poRootGroup->CreateDimension("m", "", "", 2, nullptr);
    ASSERT_NE(nullptr, dim);
    auto var = poRootGroup->CreateMDArray(
        "v_mem", {dim}, GDALExtendedDataType::Create(GDT_Int16), nullptr);
    ASSERT_NE(nullptr, var);
    short anVals[2] = {100, 200};
    const std::vector<GUInt64> start0 = {0};
    const size_t count2[1] = {2};
    EXPECT_TRUE(var->Write(start0.data(), count2, nullptr, nullptr,
                           GDALExtendedDataType::Create(GDT_Int16), anVals));
  }

  // Copy to /vsimem/
  EXPECT_EQ(0, CPLCopyFile(memFile.c_str(), diskFile.c_str()));
  VSIUnlink(diskFile.c_str());

  // Open /vsimem/ file read-only with GDAL_OF_MULTIDIM_RASTER (triggers
  // nc_open_mem)
  {
    auto poReadDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(memFile.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poReadDS);
    auto poRG = poReadDS->GetRootGroup();
    ASSERT_NE(nullptr, poRG);
    auto var = poRG->OpenMDArray("v_mem");
    ASSERT_NE(nullptr, var);
    short anRead[2] = {};
    const std::vector<GUInt64> start0 = {0};
    const size_t count2[1] = {2};
    EXPECT_TRUE(var->Read(start0.data(), count2, nullptr, nullptr,
                          GDALExtendedDataType::Create(GDT_Int16), anRead));
    EXPECT_EQ(100, anRead[0]);
    EXPECT_EQ(200, anRead[1]);
  }

  VSIUnlink(memFile.c_str());
}

TEST_F(NetCDFMultiDimTest, Char2DAsStringAndEnumRAT) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "multidim_enum.nc")
          .string();

  // Create directly with NetCDF C API
  int ncid = 0;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_NETCDF4 | NC_CLOBBER, &ncid));

  int dimid_lines = 0;
  int dimid_chars = 0;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "lines", 3, &dimid_lines));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "charlen", 8, &dimid_chars));

  // 2D NC_CHAR variable: (lines, charlen)
  int dims2D[2] = {dimid_lines, dimid_chars};
  int varid_str = 0;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "text_lines", NC_CHAR, 2, dims2D, &varid_str));

  // Enum type
  nc_type enum_type = 0;
  ASSERT_EQ(NC_NOERR, nc_def_enum(ncid, NC_INT, "SeverityEnum", &enum_type));
  int val0 = 10, val1 = 20, val2 = 30;
  ASSERT_EQ(NC_NOERR, nc_insert_enum(ncid, enum_type, "SEV_LOW", &val0));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(ncid, enum_type, "SEV_MED", &val1));
  ASSERT_EQ(NC_NOERR, nc_insert_enum(ncid, enum_type, "SEV_HIGH", &val2));

  int varid_enum = 0;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "severity", enum_type, 1, &dimid_lines,
                                 &varid_enum));

  ASSERT_EQ(NC_NOERR, nc_enddef(ncid));

  // Write text data
  const char szTexts[3][8] = {"apple\0\0", "banana\0", "cherry\0"};
  size_t startStr[2] = {0, 0};
  size_t countStr[2] = {3, 8};
  ASSERT_EQ(NC_NOERR, nc_put_vara_text(ncid, varid_str, startStr, countStr,
                                       &szTexts[0][0]));

  // Write enum data
  int anEnumData[3] = {10, 20, 30};
  size_t start1D[1] = {0};
  size_t count1D[1] = {3};
  ASSERT_EQ(NC_NOERR,
            nc_put_vara(ncid, varid_enum, start1D, count1D, anEnumData));

  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  // Re-open via GDAL MultiDim in UPDATE mode
  {
    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(filename.c_str(), GDAL_OF_MULTIDIM_RASTER | GDAL_OF_UPDATE,
                   nullptr, nullptr, nullptr)));
    ASSERT_NE(nullptr, poDS);

    auto poRG = poDS->GetRootGroup();
    ASSERT_NE(nullptr, poRG);

    // Verify 2D NC_CHAR seen as 1D String MDArray
    auto poVarStr = poRG->OpenMDArray("text_lines");
    ASSERT_NE(nullptr, poVarStr);
    EXPECT_EQ(1, poVarStr->GetDimensionCount());
    EXPECT_EQ(GEDTC_STRING, poVarStr->GetDataType().GetClass());

    char* aszReadStrs[3] = {};
    const std::vector<GUInt64> start0 = {0};
    const size_t count3[1] = {3};
    EXPECT_TRUE(poVarStr->Read(start0.data(), count3, nullptr, nullptr,
                               GDALExtendedDataType::CreateString(),
                               aszReadStrs));
    EXPECT_STREQ("apple", aszReadStrs[0]);
    EXPECT_STREQ("banana", aszReadStrs[1]);
    EXPECT_STREQ("cherry", aszReadStrs[2]);
    CPLFree(aszReadStrs[0]);
    CPLFree(aszReadStrs[1]);
    CPLFree(aszReadStrs[2]);

    // Write updated strings (tests lines 3750-3780)
    const char* aszNewStrs[3] = {"first", "second", "third"};
    EXPECT_TRUE(poVarStr->Write(start0.data(), count3, nullptr, nullptr,
                                GDALExtendedDataType::CreateString(),
                                aszNewStrs));

    // Verify Enum MDArray has RAT
    auto poVarEnum = poRG->OpenMDArray("severity");
    ASSERT_NE(nullptr, poVarEnum);
    const auto& dt = poVarEnum->GetDataType();
    EXPECT_EQ(GEDTC_NUMERIC, dt.GetClass());
    EXPECT_EQ(GDT_Int32, dt.GetNumericDataType());
    auto poRAT = dt.GetRAT();
    ASSERT_NE(nullptr, poRAT);
    EXPECT_EQ(3, poRAT->GetRowCount());

    int anReadEnum[3] = {};
    EXPECT_TRUE(poVarEnum->Read(start0.data(), count3, nullptr, nullptr,
                                GDALExtendedDataType::Create(GDT_Int32),
                                anReadEnum));
    EXPECT_EQ(10, anReadEnum[0]);
    EXPECT_EQ(20, anReadEnum[1]);
    EXPECT_EQ(30, anReadEnum[2]);
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFMultiDimTest, AdvancedCoveragePaths) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "multidim_advanced_cov.nc")
          .string();

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  {
    auto poDS = std::unique_ptr<GDALDataset>(
        poDriver->CreateMultiDimensional(filename.c_str(), nullptr, nullptr));
    ASSERT_NE(nullptr, poDS);

    auto poRootGroup = poDS->GetRootGroup();
    ASSERT_NE(nullptr, poRootGroup);

    // 1. Group error paths
    EXPECT_EQ(nullptr,
              poRootGroup->CreateMDArray(
                  "", {}, GDALExtendedDataType::Create(GDT_Int32), nullptr));
    EXPECT_EQ(nullptr, poRootGroup->CreateDimension("", "", "", 5, nullptr));
    EXPECT_EQ(nullptr, poRootGroup->CreateAttribute(
                           "invalid_2d_att", {2, 2},
                           GDALExtendedDataType::Create(GDT_Int32), nullptr));

    // 2. Foreign GDALDimension auto-creation and size mismatch warning
    auto foreignDim =
        std::make_shared<GDALDimension>("/", "foreign_dim", "", "", 5);
    auto varAutoDim = poRootGroup->CreateMDArray(
        "var_autodim", {foreignDim}, GDALExtendedDataType::Create(GDT_Int32),
        nullptr);
    ASSERT_NE(nullptr, varAutoDim);
    auto foreignDimDiff =
        std::make_shared<GDALDimension>("/", "foreign_dim", "", "", 10);
    auto varAutoDimDiff = poRootGroup->CreateMDArray(
        "var_autodim_diff", {foreignDimDiff},
        GDALExtendedDataType::Create(GDT_Int32), nullptr);
    ASSERT_NE(nullptr, varAutoDimDiff);

    // 3. 1D fixed-length string array
    auto dimFixed = poRootGroup->CreateDimension("d_fixed", "", "", 2, nullptr);
    ASSERT_NE(nullptr, dimFixed);
    auto varFixedStr = poRootGroup->CreateMDArray(
        "fixed_str", {dimFixed}, GDALExtendedDataType::CreateString(10),
        nullptr);
    ASSERT_NE(nullptr, varFixedStr);
    const char* aszFixedStrs[2] = {"alpha", "beta"};
    const GUInt64 start0 = 0;
    const size_t count2 = 2;
    EXPECT_TRUE(varFixedStr->Write(&start0, &count2, nullptr, nullptr,
                                   GDALExtendedDataType::CreateString(),
                                   aszFixedStrs));
    char* aszReadFixed[2] = {};
    EXPECT_TRUE(varFixedStr->Read(&start0, &count2, nullptr, nullptr,
                                  GDALExtendedDataType::CreateString(),
                                  aszReadFixed));
    EXPECT_STREQ("alpha", aszReadFixed[0]);
    EXPECT_STREQ("beta", aszReadFixed[1]);
    CPLFree(aszReadFixed[0]);
    CPLFree(aszReadFixed[1]);

    // 4. Arrays with NC_TYPE=NC_BYTE, NC_INT64, NC_UINT64
    CPLStringList aosByteOpt;
    aosByteOpt.SetNameValue("NC_TYPE", "NC_BYTE");
    auto varNCByte = poRootGroup->CreateMDArray(
        "var_nc_byte", {dimFixed}, GDALExtendedDataType::Create(GDT_Int16),
        aosByteOpt.List());
    ASSERT_NE(nullptr, varNCByte);
    int16_t anByteData[2] = {10, -20};
    EXPECT_TRUE(varNCByte->Write(&start0, &count2, nullptr, nullptr,
                                 GDALExtendedDataType::Create(GDT_Int16),
                                 anByteData));
    int16_t anByteRead[2] = {};
    EXPECT_TRUE(varNCByte->Read(&start0, &count2, nullptr, nullptr,
                                GDALExtendedDataType::Create(GDT_Int16),
                                anByteRead));
    EXPECT_EQ(10, anByteRead[0]);
    EXPECT_EQ(-20, anByteRead[1]);

    CPLStringList aosInt64Opt;
    aosInt64Opt.SetNameValue("NC_TYPE", "NC_INT64");
    auto varNCInt64 = poRootGroup->CreateMDArray(
        "var_nc_int64", {dimFixed}, GDALExtendedDataType::Create(GDT_Float64),
        aosInt64Opt.List());
    ASSERT_NE(nullptr, varNCInt64);
    double adfInt64Data[2] = {1000000000.0, -2000000000.0};
    EXPECT_TRUE(varNCInt64->Write(&start0, &count2, nullptr, nullptr,
                                  GDALExtendedDataType::Create(GDT_Float64),
                                  adfInt64Data));
    double adfInt64Read[2] = {};
    EXPECT_TRUE(varNCInt64->Read(&start0, &count2, nullptr, nullptr,
                                 GDALExtendedDataType::Create(GDT_Float64),
                                 adfInt64Read));
    EXPECT_DOUBLE_EQ(1000000000.0, adfInt64Read[0]);
    EXPECT_DOUBLE_EQ(-2000000000.0, adfInt64Read[1]);

    CPLStringList aosUInt64Opt;
    aosUInt64Opt.SetNameValue("NC_TYPE", "NC_UINT64");
    auto varNCUInt64 = poRootGroup->CreateMDArray(
        "var_nc_uint64", {dimFixed}, GDALExtendedDataType::Create(GDT_Float64),
        aosUInt64Opt.List());
    ASSERT_NE(nullptr, varNCUInt64);
    double adfUInt64Data[2] = {3000000000.0, 4000000000.0};
    EXPECT_TRUE(varNCUInt64->Write(&start0, &count2, nullptr, nullptr,
                                   GDALExtendedDataType::Create(GDT_Float64),
                                   adfUInt64Data));
    double adfUInt64Read[2] = {};
    EXPECT_TRUE(varNCUInt64->Read(&start0, &count2, nullptr, nullptr,
                                  GDALExtendedDataType::Create(GDT_Float64),
                                  adfUInt64Read));
    EXPECT_DOUBLE_EQ(3000000000.0, adfUInt64Read[0]);
    EXPECT_DOUBLE_EQ(4000000000.0, adfUInt64Read[1]);

    // 5. Blocksize + Fletcher32 Checksum + Cache structural info + ZLEVEL
    // warning
    char** papszChunkOpt = nullptr;
    papszChunkOpt = CSLSetNameValue(papszChunkOpt, "BLOCKSIZE", "2");
    papszChunkOpt = CSLSetNameValue(papszChunkOpt, "CHECKSUM", "YES");
    papszChunkOpt = CSLSetNameValue(
        papszChunkOpt, "INCLUDE_CHUNK_CACHE_PARAMETERS_IN_STRUCTURAL_INFO",
        "YES");
    papszChunkOpt = CSLSetNameValue(papszChunkOpt, "COMPRESS", "DEFLATE");
    papszChunkOpt = CSLSetNameValue(papszChunkOpt, "ZLEVEL", "99");
    auto varChunked = poRootGroup->CreateMDArray(
        "var_chunked", {dimFixed}, GDALExtendedDataType::Create(GDT_Int32),
        papszChunkOpt);
    CSLDestroy(papszChunkOpt);
    ASSERT_NE(nullptr, varChunked);
    auto structInfo = varChunked->GetStructuralInfo();
    ASSERT_NE(nullptr, structInfo);
    EXPECT_NE(nullptr, CSLFetchNameValue(structInfo, "PREEMPTION"));
    EXPECT_NE(nullptr, CSLFetchNameValue(structInfo, "CHUNK_SLOTS"));

    // 6. 0-dim scalar string array
    auto varStr0D = poRootGroup->CreateMDArray(
        "str_0d", {}, GDALExtendedDataType::CreateString(), nullptr);
    ASSERT_NE(nullptr, varStr0D);
    const char* pszScalarVal = "scalar_string_val";
    EXPECT_TRUE(varStr0D->Write(nullptr, nullptr, nullptr, nullptr,
                                GDALExtendedDataType::CreateString(),
                                &pszScalarVal));
    char* pszScalarRead = nullptr;
    EXPECT_TRUE(varStr0D->Read(nullptr, nullptr, nullptr, nullptr,
                               GDALExtendedDataType::CreateString(),
                               &pszScalarRead));
    EXPECT_STREQ("scalar_string_val", pszScalarRead);
    CPLFree(pszScalarRead);
    EXPECT_TRUE(varStr0D->AdviseRead(nullptr, nullptr));
    EXPECT_FALSE(varFixedStr->AdviseRead(nullptr, nullptr));

    // Test FILTER option failure
    char** papszFilterOpt = nullptr;
    papszFilterOpt = CSLSetNameValue(papszFilterOpt, "BLOCKSIZE", "2");
    papszFilterOpt = CSLSetNameValue(papszFilterOpt, "FILTER", "99999,1,2");
    EXPECT_EQ(nullptr,
              poRootGroup->CreateMDArray(
                  "var_filter_fail", {dimFixed},
                  GDALExtendedDataType::Create(GDT_Int32), papszFilterOpt));
    CSLDestroy(papszFilterOpt);

    // Test out-of-range chunk cache parameter: PREEMPTION is clamped to
    // [0, 1] and the array is still created.
    CPLStringList aosBadCacheOpt;
    aosBadCacheOpt.SetNameValue("BLOCKSIZE", "2");
    aosBadCacheOpt.SetNameValue("PREEMPTION", "-1.0");
    aosBadCacheOpt.SetNameValue(
        "INCLUDE_CHUNK_CACHE_PARAMETERS_IN_STRUCTURAL_INFO", "YES");
    auto varBadCache = poRootGroup->CreateMDArray(
        "var_bad_cache", {dimFixed}, GDALExtendedDataType::Create(GDT_Int32),
        aosBadCacheOpt.List());
    ASSERT_NE(nullptr, varBadCache);
    auto badCacheInfo = varBadCache->GetStructuralInfo();
    ASSERT_NE(nullptr, badCacheInfo);
    EXPECT_STREQ("0.000000", CSLFetchNameValue(badCacheInfo, "PREEMPTION"));

    // 7. Transposed read on 2D array
    auto dimRow = poRootGroup->CreateDimension("row", "", "", 2, nullptr);
    auto dimCol = poRootGroup->CreateDimension("col", "", "", 3, nullptr);
    auto var2D = poRootGroup->CreateMDArray(
        "var_2d_transpose", {dimRow, dimCol},
        GDALExtendedDataType::Create(GDT_Int32), nullptr);
    ASSERT_NE(nullptr, var2D);
    int an2DData[6] = {1, 2, 3, 4, 5, 6};
    const GUInt64 start2D[2] = {0, 0};
    const size_t count2D[2] = {2, 3};
    EXPECT_TRUE(var2D->Write(start2D, count2D, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Int32),
                             an2DData));
    int anTransposed[6] = {};
    const GInt64 step2D[2] = {1, 1};
    const GPtrDiff_t strideTransposed[2] = {1, 2};
    EXPECT_TRUE(var2D->Read(start2D, count2D, step2D, strideTransposed,
                            GDALExtendedDataType::Create(GDT_Int32),
                            anTransposed));
    EXPECT_EQ(1, anTransposed[0]);
    EXPECT_EQ(2, anTransposed[2]);
    EXPECT_EQ(4, anTransposed[1]);

    // 3D strided write
    auto dimZ = poRootGroup->CreateDimension("z_dim", "", "", 2, nullptr);
    auto var3D = poRootGroup->CreateMDArray(
        "var_3d_strided", {dimZ, dimRow, dimCol},
        GDALExtendedDataType::Create(GDT_Int32), nullptr);
    ASSERT_NE(nullptr, var3D);
    int an3DData[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const GUInt64 start3D[3] = {0, 0, 0};
    const size_t count3D[3] = {2, 2, 3};
    const GInt64 step3D[3] = {1, 1, 1};
    const GPtrDiff_t stride3D[3] = {6, 3, 1};
    EXPECT_TRUE(var3D->Write(start3D, count3D, step3D, stride3D,
                             GDALExtendedDataType::Create(GDT_Int32),
                             an3DData));

    // Multi-double attribute read with step and stride
    auto attMultiDbl = poRootGroup->CreateAttribute(
        "att_multi_dbl", {3}, GDALExtendedDataType::Create(GDT_Float64),
        nullptr);
    ASSERT_NE(nullptr, attMultiDbl);
    double adfMultiDbl[3] = {1.1, 2.2, 3.3};
    const size_t count3 = 3;
    EXPECT_TRUE(attMultiDbl->Write(&start0, &count3, nullptr, nullptr,
                                   GDALExtendedDataType::Create(GDT_Float64),
                                   adfMultiDbl));
    double adfDblRead[2] = {};
    const GInt64 stepDbl = 2;
    const GPtrDiff_t strideDbl = 1;
    EXPECT_TRUE(attMultiDbl->Read(&start0, &count2, &stepDbl, &strideDbl,
                                  GDALExtendedDataType::Create(GDT_Float64),
                                  adfDblRead));
    EXPECT_DOUBLE_EQ(1.1, adfDblRead[0]);
    EXPECT_DOUBLE_EQ(3.3, adfDblRead[1]);

    // 8. Attributes with !m_bPerfectDataTypeMatch (NC_BYTE, NC_INT64,
    // NC_UINT64)
    const size_t count1 = 1;
    CPLStringList aosByteAtt;
    aosByteAtt.SetNameValue("NC_TYPE", "NC_BYTE");
    auto attByte = poRootGroup->CreateAttribute(
        "att_byte", {}, GDALExtendedDataType::Create(GDT_Int16),
        aosByteAtt.List());
    ASSERT_NE(nullptr, attByte);
    int16_t nByteVal = 42;
    EXPECT_TRUE(attByte->Write(&start0, &count1, nullptr, nullptr,
                               GDALExtendedDataType::Create(GDT_Int16),
                               &nByteVal));
    int16_t nByteAttRead = 0;
    EXPECT_TRUE(attByte->Read(&start0, &count1, nullptr, nullptr,
                              GDALExtendedDataType::Create(GDT_Int16),
                              &nByteAttRead));
    EXPECT_EQ(42, nByteAttRead);

    CPLStringList aosInt64Att;
    aosInt64Att.SetNameValue("NC_TYPE", "NC_INT64");
    auto attInt64 = poRootGroup->CreateAttribute(
        "att_int64", {}, GDALExtendedDataType::Create(GDT_Float64),
        aosInt64Att.List());
    ASSERT_NE(nullptr, attInt64);
    double dfInt64Val = 123456789.0;
    EXPECT_TRUE(attInt64->Write(&start0, &count1, nullptr, nullptr,
                                GDALExtendedDataType::Create(GDT_Float64),
                                &dfInt64Val));
    double dfInt64AttRead = 0;
    EXPECT_TRUE(attInt64->Read(&start0, &count1, nullptr, nullptr,
                               GDALExtendedDataType::Create(GDT_Float64),
                               &dfInt64AttRead));
    EXPECT_DOUBLE_EQ(123456789.0, dfInt64AttRead);

    CPLStringList aosUInt64Att;
    aosUInt64Att.SetNameValue("NC_TYPE", "NC_UINT64");
    auto attUInt64 = poRootGroup->CreateAttribute(
        "att_uint64", {}, GDALExtendedDataType::Create(GDT_Float64),
        aosUInt64Att.List());
    ASSERT_NE(nullptr, attUInt64);
    double dfUInt64Val = 987654321.0;
    EXPECT_TRUE(attUInt64->Write(&start0, &count1, nullptr, nullptr,
                                 GDALExtendedDataType::Create(GDT_Float64),
                                 &dfUInt64Val));
    double dfUInt64AttRead = 0;
    EXPECT_TRUE(attUInt64->Read(&start0, &count1, nullptr, nullptr,
                                GDALExtendedDataType::Create(GDT_Float64),
                                &dfUInt64AttRead));
    EXPECT_DOUBLE_EQ(987654321.0, dfUInt64AttRead);

    // 9. Attribute type conversions on Read and Write
    auto attCharNum = poRootGroup->CreateAttribute(
        "att_char_num", {}, GDALExtendedDataType::CreateString(), nullptr);
    ASSERT_NE(nullptr, attCharNum);
    EXPECT_TRUE(attCharNum->Write("42"));
    // NC_CHAR -> numeric conversion is only reachable through the protected
    // IRead() hook (see ProtectedMDArrayAccess).
    int nAttIntRead = 0;
    EXPECT_TRUE(ProtectedMDArrayAccess::ReadScalar(
        *attCharNum, GDALExtendedDataType::Create(GDT_Int32), &nAttIntRead));
    EXPECT_EQ(42, nAttIntRead);

    // Numeric -> NC_CHAR conversion on write goes through the public API.
    int nAttIntWrite = 88;
    EXPECT_TRUE(attCharNum->Write(nullptr, nullptr, nullptr, nullptr,
                                  GDALExtendedDataType::Create(GDT_Int32),
                                  &nAttIntWrite));
    const char* pszCharNum = attCharNum->ReadAsString();
    ASSERT_NE(nullptr, pszCharNum);
    EXPECT_STREQ("88", pszCharNum);

    CPLStringList aosNCStr;
    aosNCStr.SetNameValue("NC_TYPE", "NC_STRING");
    auto attNCStr = poRootGroup->CreateAttribute(
        "att_ncstr_num", {}, GDALExtendedDataType::CreateString(),
        aosNCStr.List());
    ASSERT_NE(nullptr, attNCStr);
    int nNumForStr = 1234;
    EXPECT_TRUE(attNCStr->Write(nullptr, nullptr, nullptr, nullptr,
                                GDALExtendedDataType::Create(GDT_Int32),
                                &nNumForStr));
    char* pszReadNCStr = nullptr;
    EXPECT_TRUE(attNCStr->Read(nullptr, nullptr, nullptr, nullptr,
                               GDALExtendedDataType::CreateString(),
                               &pszReadNCStr));
    EXPECT_STREQ("1234", pszReadNCStr);
    CPLFree(pszReadNCStr);

    auto attMultiStr = poRootGroup->CreateAttribute(
        "att_multistr_num", {2}, GDALExtendedDataType::CreateString(),
        aosNCStr.List());
    ASSERT_NE(nullptr, attMultiStr);
    int anMultiNums[2] = {55, 66};
    const size_t count2_local = 2;
    EXPECT_TRUE(attMultiStr->Write(&start0, &count2_local, nullptr, nullptr,
                                   GDALExtendedDataType::Create(GDT_Int32),
                                   anMultiNums));
    char* aszReadMulti[2] = {};
    EXPECT_TRUE(attMultiStr->Read(&start0, &count2_local, nullptr, nullptr,
                                  GDALExtendedDataType::CreateString(),
                                  aszReadMulti));
    EXPECT_STREQ("55", aszReadMulti[0]);
    EXPECT_STREQ("66", aszReadMulti[1]);
    CPLFree(aszReadMulti[0]);
    CPLFree(aszReadMulti[1]);
    char* aszReadStep[1] = {};
    const GInt64 step2 = 2;
    EXPECT_TRUE(attMultiStr->Read(&start0, &count1, &step2, nullptr,
                                  GDALExtendedDataType::CreateString(),
                                  aszReadStep));
    EXPECT_STREQ("55", aszReadStep[0]);
    CPLFree(aszReadStep[0]);

    // 4D string array write & read (tests general multidim loop)
    auto d1 = poRootGroup->CreateDimension("d1", "", "", 2, nullptr);
    auto d2 = poRootGroup->CreateDimension("d2", "", "", 2, nullptr);
    auto d3 = poRootGroup->CreateDimension("d3", "", "", 2, nullptr);
    auto d4 = poRootGroup->CreateDimension("d4", "", "", 2, nullptr);
    auto var4DStr = poRootGroup->CreateMDArray(
        "var_4d_str", {d1, d2, d3, d4}, GDALExtendedDataType::CreateString(),
        nullptr);
    ASSERT_NE(nullptr, var4DStr);
    const char* asz4DStrings[16] = {"s0",  "s1",  "s2",  "s3", "s4",  "s5",
                                    "s6",  "s7",  "s8",  "s9", "s10", "s11",
                                    "s12", "s13", "s14", "s15"};
    const GUInt64 start4D[4] = {0, 0, 0, 0};
    const size_t count4D[4] = {2, 2, 2, 2};
    EXPECT_TRUE(var4DStr->Write(start4D, count4D, nullptr, nullptr,
                                GDALExtendedDataType::CreateString(),
                                asz4DStrings));
    char* asz4DRead[16] = {};
    EXPECT_TRUE(var4DStr->Read(start4D, count4D, nullptr, nullptr,
                               GDALExtendedDataType::CreateString(),
                               asz4DRead));
    for (int i = 0; i < 16; ++i) {
      EXPECT_STREQ(asz4DStrings[i], asz4DRead[i]) << "index " << i;
      CPLFree(asz4DRead[i]);
    }

    // 10. Nodata handling (missing_value, roundtrip valid string, overflowing
    // string, out-of-range numeric)
    auto varFloat = poRootGroup->CreateMDArray(
        "var_nodata_test", {dimFixed},
        GDALExtendedDataType::Create(GDT_Float32), nullptr);
    ASSERT_NE(nullptr, varFloat);
    auto attMissing = varFloat->CreateAttribute(
        "missing_value", {}, GDALExtendedDataType::Create(GDT_Float32),
        nullptr);
    ASSERT_NE(nullptr, attMissing);
    float fMissing = -9999.0f;
    EXPECT_TRUE(attMissing->Write(fMissing));
    const void* pNoData = varFloat->GetRawNoDataValue();
    ASSERT_NE(nullptr, pNoData);
    EXPECT_FLOAT_EQ(-9999.0f, *static_cast<const float*>(pNoData));
    EXPECT_TRUE(varFloat->SetRawNoDataValue(nullptr));

    // Update missing_value on integer variable
    auto varInt16NoData = poRootGroup->CreateMDArray(
        "var_int16_nodata", {dimFixed}, GDALExtendedDataType::Create(GDT_Int16),
        nullptr);
    ASSERT_NE(nullptr, varInt16NoData);
    auto attMissInt = varInt16NoData->CreateAttribute(
        "missing_value", {}, GDALExtendedDataType::Create(GDT_Int16), nullptr);
    ASSERT_NE(nullptr, attMissInt);
    int16_t nInitMissing = -999;
    EXPECT_TRUE(attMissInt->Write(nInitMissing));
    int16_t nNewMissing = -888;
    // Both missing_value and _FillValue exist, so SetRawNoDataValue returns
    // false (tests lines 3966-3969)
    EXPECT_FALSE(varInt16NoData->SetRawNoDataValue(&nNewMissing));
    EXPECT_TRUE(varInt16NoData->SetScale(2.5, GDT_Float32));
    EXPECT_TRUE(varInt16NoData->SetOffset(10.0, GDT_Float32));
    bool bHasScale = true, bHasOffset = true;
    GDALDataType eScaleType = GDT_Unknown, eOffsetType = GDT_Unknown;
    EXPECT_DOUBLE_EQ(1.0, varFloat->GetScale(&bHasScale, &eScaleType));
    EXPECT_FALSE(bHasScale);
    EXPECT_DOUBLE_EQ(0.0, varFloat->GetOffset(&bHasOffset, &eOffsetType));
    EXPECT_FALSE(bHasOffset);
    auto blkSize = varChunked->GetBlockSize();
    EXPECT_EQ(1, blkSize.size());
    EXPECT_EQ(2, blkSize[0]);

    // Numeric missing_value out of range of variable type
    auto varInt16Bad = poRootGroup->CreateMDArray(
        "var_int16_bad", {dimFixed}, GDALExtendedDataType::Create(GDT_Int16),
        nullptr);
    ASSERT_NE(nullptr, varInt16Bad);
    auto attMissDbl = varInt16Bad->CreateAttribute(
        "missing_value", {}, GDALExtendedDataType::Create(GDT_Float64),
        nullptr);
    ASSERT_NE(nullptr, attMissDbl);
    double dfBadMiss = 100000.0;
    EXPECT_TRUE(attMissDbl->Write(dfBadMiss));
    EXPECT_EQ(nullptr, varInt16Bad->GetRawNoDataValue());

    auto varFloatStrFill = poRootGroup->CreateMDArray(
        "var_str_fill", {dimFixed}, GDALExtendedDataType::Create(GDT_Float32),
        nullptr);
    ASSERT_NE(nullptr, varFloatStrFill);
    auto attFillStr = varFloatStrFill->CreateAttribute(
        "missing_value", {}, GDALExtendedDataType::CreateString(), nullptr);
    ASSERT_NE(nullptr, attFillStr);
    EXPECT_TRUE(attFillStr->Write("123.5"));
    const void* pNoDataStr = varFloatStrFill->GetRawNoDataValue();
    ASSERT_NE(nullptr, pNoDataStr);
    EXPECT_FLOAT_EQ(123.5f, *static_cast<const float*>(pNoDataStr));

    auto varFloatStrOverflow = poRootGroup->CreateMDArray(
        "var_str_overflow", {dimFixed},
        GDALExtendedDataType::Create(GDT_Float32), nullptr);
    ASSERT_NE(nullptr, varFloatStrOverflow);
    auto attFillOverflow = varFloatStrOverflow->CreateAttribute(
        "missing_value", {}, GDALExtendedDataType::CreateString(), nullptr);
    ASSERT_NE(nullptr, attFillOverflow);
    EXPECT_TRUE(attFillOverflow->Write("1e100"));
    EXPECT_EQ(nullptr, varFloatStrOverflow->GetRawNoDataValue());

    // 11. ComputeStatistics with UPDATE_METADATA=YES reaches
    // netCDFVariable::SetStatistics(), which writes the actual_range attribute.
    const float afStatsData[2] = {1.0f, 10.0f};
    EXPECT_TRUE(varFloat->Write(&start0, &count2, nullptr, nullptr,
                                GDALExtendedDataType::Create(GDT_Float32),
                                afStatsData));
    CPLStringList aosStatsOpt;
    aosStatsOpt.SetNameValue("UPDATE_METADATA", "YES");
    double dfStatsMin = 0, dfStatsMax = 0, dfStatsMean = 0, dfStatsStdDev = 0;
    GUInt64 nStatsValid = 0;
    EXPECT_TRUE(varFloat->ComputeStatistics(
        /*bApproxOK=*/false, &dfStatsMin, &dfStatsMax, &dfStatsMean,
        &dfStatsStdDev, &nStatsValid, nullptr, nullptr, aosStatsOpt.List()));
    EXPECT_DOUBLE_EQ(1.0, dfStatsMin);
    EXPECT_DOUBLE_EQ(10.0, dfStatsMax);
    EXPECT_DOUBLE_EQ(5.5, dfStatsMean);
    EXPECT_EQ(2, nStatsValid);
    auto attRange = varFloat->GetAttribute("actual_range");
    ASSERT_NE(nullptr, attRange);
    EXPECT_THAT(attRange->ReadAsDoubleArray(), ElementsAre(1.0, 10.0));

    // 12. Unlimited dimension variables with no data written (triggers dummy
    // fills on destruction)
    CPLStringList aosUnlim;
    aosUnlim.SetNameValue("UNLIMITED", "YES");
    auto dimUnlim1 = poRootGroup->CreateDimension("unlim_dummy1", "", "", 0,
                                                  aosUnlim.List());
    auto dimUnlim2 = poRootGroup->CreateDimension("unlim_dummy2", "", "", 0,
                                                  aosUnlim.List());
    auto dimUnlim3 = poRootGroup->CreateDimension("unlim_dummy3", "", "", 0,
                                                  aosUnlim.List());
    ASSERT_NE(nullptr, dimUnlim1);
    ASSERT_NE(nullptr, dimUnlim2);
    ASSERT_NE(nullptr, dimUnlim3);

    auto varUnlimInt = poRootGroup->CreateMDArray(
        "unlim_int", {dimUnlim1}, GDALExtendedDataType::Create(GDT_Int32),
        nullptr);
    ASSERT_NE(nullptr, varUnlimInt);
    EXPECT_TRUE(varUnlimInt->Resize({3}, nullptr));

    auto varUnlimInt64 = poRootGroup->CreateMDArray(
        "unlim_int64", {dimUnlim2}, GDALExtendedDataType::Create(GDT_Int64),
        nullptr);
    ASSERT_NE(nullptr, varUnlimInt64);
    EXPECT_TRUE(varUnlimInt64->Resize({3}, nullptr));

    auto varUnlimUInt64 = poRootGroup->CreateMDArray(
        "unlim_uint64", {dimUnlim3}, GDALExtendedDataType::Create(GDT_UInt64),
        nullptr);
    ASSERT_NE(nullptr, varUnlimUInt64);
    EXPECT_TRUE(varUnlimUInt64->Resize({3}, nullptr));

    // 13. Virtual group by dimension & ClearStatistics
    poRootGroup->ClearStatistics();

    CPLStringList aosGroupBy;
    aosGroupBy.SetNameValue("GROUP_BY", "SAME_DIMENSION");
    auto groupNames = poRootGroup->GetGroupNames(aosGroupBy.List());
    ASSERT_FALSE(groupNames.empty());
    for (const auto& gname : groupNames) {
      auto virtGroup = poRootGroup->OpenGroup(gname, aosGroupBy.List());
      ASSERT_NE(nullptr, virtGroup) << gname;
      auto arrayNames = virtGroup->GetMDArrayNames();
      EXPECT_FALSE(arrayNames.empty()) << gname;
      for (const auto& aname : arrayNames) {
        auto arr = virtGroup->OpenMDArray(aname);
        EXPECT_NE(nullptr, arr) << gname << "/" << aname;
      }
    }
  }

  VSIUnlink(filename.c_str());
}

TEST_F(NetCDFMultiDimTest, NetCDFCAPIConstructsAndConventions) {
  const std::string filename =
      (std::filesystem::path(::testing::TempDir()) / "multidim_capi_cases.nc")
          .string();

  int ncid = 0;
  ASSERT_EQ(NC_NOERR,
            nc_create(filename.c_str(), NC_NETCDF4 | NC_CLOBBER, &ncid));

  // 1. Root dimension and subgroup variable referencing root dimension
  int dimid_parent = 0;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "parent_dim", 4, &dimid_parent));

  int grpid = 0;
  ASSERT_EQ(NC_NOERR, nc_def_grp(ncid, "subgroup", &grpid));

  int varid_sub = 0;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(grpid, "sub_var", NC_INT, 1, &dimid_parent, &varid_sub));

  // 2. Variables with _Unsigned = "true" and "false"
  int varid_u8 = 0, varid_s8 = 0, varid_u16 = 0, varid_s16 = 0;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "u8_var", NC_BYTE, 1, &dimid_parent, &varid_u8));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, varid_u8, "_Unsigned", 4, "true"));

  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "s8_var", NC_BYTE, 1, &dimid_parent, &varid_s8));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, varid_s8, "_Unsigned", 5, "false"));

  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "u16_var", NC_SHORT, 1, &dimid_parent,
                                 &varid_u16));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, varid_u16, "_Unsigned", 4, "true"));

  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "s16_var", NC_SHORT, 1, &dimid_parent,
                                 &varid_s16));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, varid_s16, "_Unsigned", 5, "false"));

  // 3. Vertical coordinate variables: positive="up" and positive="down"
  int dimid_z1 = 0, dimid_z2 = 0;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "elevation", 2, &dimid_z1));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "depth", 2, &dimid_z2));

  int varid_z1 = 0, varid_z2 = 0;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "elevation", NC_FLOAT, 1, &dimid_z1, &varid_z1));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, varid_z1, "positive", 2, "up"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, varid_z1, "axis", 1, "Z"));

  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "depth", NC_FLOAT, 1, &dimid_z2, &varid_z2));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, varid_z2, "positive", 4, "down"));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, varid_z2, "axis", 1, "Z"));

  // 4. Coordinates attribute with reversed order ("lon lat" where first is
  // longitude)
  int dimid_y = 0, dimid_x = 0;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "y_untyped", 2, &dimid_y));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "x_untyped", 2, &dimid_x));

  int dims_yx[2] = {dimid_y, dimid_x};
  int varid_data = 0;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "grid_data", NC_FLOAT, 2, dims_yx, &varid_data));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, varid_data, "coordinates", 7, "lon lat"));

  int varid_lon = 0, varid_lat = 0;
  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "lon", NC_FLOAT, 1, &dimid_x, &varid_lon));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, varid_lon, "standard_name", 9, "longitude"));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, varid_lon, "units", 12, "degrees_east"));

  ASSERT_EQ(NC_NOERR,
            nc_def_var(ncid, "lat", NC_FLOAT, 1, &dimid_y, &varid_lat));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, varid_lat, "standard_name", 8, "latitude"));
  ASSERT_EQ(NC_NOERR,
            nc_put_att_text(ncid, varid_lat, "units", 13, "degrees_north"));

  // 5. Variable with 2 dimensions but 3 coordinates (coordinates="lat lon
  // time")
  int varid_mismatched = 0;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "mismatched_coords", NC_FLOAT, 2,
                                 dims_yx, &varid_mismatched));
  ASSERT_EQ(NC_NOERR, nc_put_att_text(ncid, varid_mismatched, "coordinates", 12,
                                      "lat lon time"));

  // 6. 1D NC_CHAR variable
  int varid_char1d = 0;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "char1d", NC_CHAR, 1, &dimid_parent,
                                 &varid_char1d));

  // 7. 2D NC_CHAR indexing variable with conflicting second dimension variable
  int dimid_lines = 0, dimid_charlen = 0;
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_text_lines", 3, &dimid_lines));
  ASSERT_EQ(NC_NOERR, nc_def_dim(ncid, "dim_charlen", 8, &dimid_charlen));
  int dims_lines[2] = {dimid_lines, dimid_charlen};
  int varid_text = 0;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "dim_text_lines", NC_CHAR, 2, dims_lines,
                                 &varid_text));
  int varid_dim_charlen = 0;
  ASSERT_EQ(NC_NOERR, nc_def_var(ncid, "dim_charlen", NC_INT, 1, &dimid_charlen,
                                 &varid_dim_charlen));

  ASSERT_EQ(NC_NOERR, nc_close(ncid));

  {
    auto poDS = std::unique_ptr<GDALDataset>(static_cast<GDALDataset*>(
        GDALOpenEx(filename.c_str(), GDAL_OF_MULTIDIM_RASTER, nullptr, nullptr,
                   nullptr)));
    ASSERT_NE(nullptr, poDS);
    auto poRG = poDS->GetRootGroup();
    ASSERT_NE(nullptr, poRG);

    // 1. Subgroup variable referencing parent dim (before dimensions of root
    // are queried/cached):
    auto subgrp = poRG->OpenGroup("subgroup");
    ASSERT_NE(nullptr, subgrp);
    auto subVar = subgrp->OpenMDArray("sub_var");
    ASSERT_NE(nullptr, subVar);
    auto dims = subVar->GetDimensions();
    ASSERT_EQ(1, dims.size());
    EXPECT_EQ("parent_dim", dims[0]->GetName());

    // 1D NC_CHAR variable maps to GDT_Byte
    auto varChar1D = poRG->OpenMDArray("char1d");
    ASSERT_NE(nullptr, varChar1D);
    EXPECT_EQ(GDT_Byte, varChar1D->GetDataType().GetNumericDataType());

    // 2D NC_CHAR indexing variable where second dim has a variable of same name
    std::shared_ptr<GDALDimension> dimTextLines;
    for (const auto& d : poRG->GetDimensions()) {
      if (d->GetName() == "dim_text_lines") {
        dimTextLines = d;
        break;
      }
    }
    ASSERT_NE(nullptr, dimTextLines);
    EXPECT_EQ(nullptr, dimTextLines->GetIndexingVariable());

    // 2. Unsigned attributes:
    auto varU8 = poRG->OpenMDArray("u8_var");
    ASSERT_NE(nullptr, varU8);
    EXPECT_EQ(GDT_Byte, varU8->GetDataType().GetNumericDataType());

    auto varS8 = poRG->OpenMDArray("s8_var");
    ASSERT_NE(nullptr, varS8);
    EXPECT_EQ(GDT_Int8, varS8->GetDataType().GetNumericDataType());

    auto varU16 = poRG->OpenMDArray("u16_var");
    ASSERT_NE(nullptr, varU16);
    EXPECT_EQ(GDT_UInt16, varU16->GetDataType().GetNumericDataType());

    auto varS16 = poRG->OpenMDArray("s16_var");
    ASSERT_NE(nullptr, varS16);
    EXPECT_EQ(GDT_Int16, varS16->GetDataType().GetNumericDataType());

    // 3. Vertical coordinates:
    auto dimsRoot = poRG->GetDimensions();
    for (const auto& d : dimsRoot) {
      if (d->GetName() == "elevation") {
        EXPECT_EQ(GDAL_DIM_TYPE_VERTICAL, d->GetType());
        EXPECT_EQ("UP", d->GetDirection());
      } else if (d->GetName() == "depth") {
        EXPECT_EQ(GDAL_DIM_TYPE_VERTICAL, d->GetType());
        EXPECT_EQ("DOWN", d->GetDirection());
      } else if (d->GetName() == "y_untyped") {
        auto idxVar = d->GetIndexingVariable();
        ASSERT_NE(nullptr, idxVar);
        EXPECT_EQ("lat", idxVar->GetName());
      } else if (d->GetName() == "x_untyped") {
        auto idxVar = d->GetIndexingVariable();
        ASSERT_NE(nullptr, idxVar);
        EXPECT_EQ("lon", idxVar->GetName());
      }
    }
  }

  VSIUnlink(filename.c_str());

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("netCDF");
  ASSERT_NE(nullptr, poDriver);

  // 6. Test untyped dimensions SetSpatialRef warning and CF attribute writing
  {
    const std::string srsFilename =
        (std::filesystem::path(::testing::TempDir()) /
         "multidim_untyped_srs.nc")
            .string();
    {
      auto poDS = std::unique_ptr<GDALDataset>(poDriver->CreateMultiDimensional(
          srsFilename.c_str(), nullptr, nullptr));
      ASSERT_NE(nullptr, poDS);
      auto poRG = poDS->GetRootGroup();
      ASSERT_NE(nullptr, poRG);
      auto dimY = poRG->CreateDimension("dim_y", "", "", 2, nullptr);
      auto dimX = poRG->CreateDimension("dim_x", "", "", 3, nullptr);
      ASSERT_NE(nullptr, dimY);
      ASSERT_NE(nullptr, dimX);
      auto idxY = poRG->CreateMDArray(
          "dim_y", {dimY}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
      auto idxX = poRG->CreateMDArray(
          "dim_x", {dimX}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
      ASSERT_NE(nullptr, idxY);
      ASSERT_NE(nullptr, idxX);
      auto attUnits = idxX->CreateAttribute(
          "units", {}, GDALExtendedDataType::CreateString(), nullptr);
      ASSERT_NE(nullptr, attUnits);
      EXPECT_TRUE(attUnits->Write("meters"));

      auto var = poRG->CreateMDArray("grid", {dimY, dimX},
                                     GDALExtendedDataType::Create(GDT_Float32),
                                     nullptr);
      ASSERT_NE(nullptr, var);
      OGRSpatialReference oSRS;
      oSRS.SetWellKnownGeogCS("WGS84");
      EXPECT_TRUE(var->SetSpatialRef(&oSRS));
    }
    VSIUnlink(srsFilename.c_str());
  }

  // 7. Classic format define mode switching (nc_redef and nc_enddef)
  {
    const std::string classicFilename =
        (std::filesystem::path(::testing::TempDir()) /
         "multidim_classic_redef.nc")
            .string();
    CPLStringList aosOptions;
    aosOptions.SetNameValue("FORMAT", "NC");
    {
      auto poDS = std::unique_ptr<GDALDataset>(poDriver->CreateMultiDimensional(
          classicFilename.c_str(), nullptr, aosOptions.List()));
      ASSERT_NE(nullptr, poDS);
      auto poRG = poDS->GetRootGroup();
      ASSERT_NE(nullptr, poRG);
      auto dim = poRG->CreateDimension("d", "", "", 2, nullptr);
      ASSERT_NE(nullptr, dim);
      auto var = poRG->CreateMDArray(
          "v", {dim}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
      ASSERT_NE(nullptr, var);
      int vals[2] = {10, 20};
      const GUInt64 start0 = 0;
      const size_t count2 = 2;
      EXPECT_TRUE(var->Write(&start0, &count2, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Int32), vals));
      // Creating an attribute after data has been written forces nc_redef().
      auto att = var->CreateAttribute(
          "att", {}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
      ASSERT_NE(nullptr, att);
      int attVal = 99;
      EXPECT_TRUE(att->Write(attVal));
      // Writing data again forces nc_enddef().
      vals[0] = 30;
      EXPECT_TRUE(var->Write(&start0, &count2, nullptr, nullptr,
                             GDALExtendedDataType::Create(GDT_Int32), vals));
      int readVals[2] = {};
      EXPECT_TRUE(var->Read(&start0, &count2, nullptr, nullptr,
                            GDALExtendedDataType::Create(GDT_Int32), readVals));
      EXPECT_THAT(readVals, ElementsAre(30, 20));
    }
    VSIUnlink(classicFilename.c_str());
  }
}

}  // namespace
}  // namespace autotest2
