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
// Tests for GDALMultiDimInfo (apps/gdalmdiminfo_lib.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalmdiminfo_lib.cpp
//   https://gdal.org/programs/gdalmdiminfo.html

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdal_rat.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_float.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

struct MultiDimInfoOptionsDeleter {
  void operator()(GDALMultiDimInfoOptions* opts) const {
    if (opts != nullptr) {
      GDALMultiDimInfoOptionsFree(opts);
    }
  }
};
using MultiDimInfoOptionsUniquePtr =
    std::unique_ptr<GDALMultiDimInfoOptions, MultiDimInfoOptionsDeleter>;

class GdalMultiDimInfoTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }
};

// Custom GDALGroup for mocking non-standard properties like structural info,
// data types with RAT, and duplicate subgroup/array names.
class CustomMockGroup : public GDALGroup {
 public:
  CustomMockGroup(const std::string& parent_name, const std::string& name)
      : GDALGroup(parent_name, name) {}

  static std::shared_ptr<CustomMockGroup> Create(const std::string& parent_name,
                                                 const std::string& name) {
    auto grp = std::make_shared<CustomMockGroup>(parent_name, name);
    grp->SetSelf(grp);
    return grp;
  }

  void SetCustomName(std::string name) { m_osName = std::move(name); }

  void AddAttribute(std::shared_ptr<GDALAttribute> attr) {
    custom_attrs_.push_back(std::move(attr));
  }

  std::vector<std::shared_ptr<GDALAttribute>> GetAttributes(
      CSLConstList = nullptr) const override {
    if (!custom_attrs_.empty()) {
      return custom_attrs_;
    }
    return {};
  }

  void SetStructuralInfo(CSLConstList structural_info) {
    structural_info_.Assign(CSLDuplicate(structural_info),
                            /*bTakeOwnership=*/TRUE);
  }

  CSLConstList GetStructuralInfo() const override {
    return structural_info_.List();
  }

  void AddDataType(const std::shared_ptr<GDALExtendedDataType>& dt) {
    m_apoTypes.push_back(dt);
  }

  void SetGroupNames(std::vector<std::string> names) {
    subgroup_names_ = std::move(names);
  }

  std::vector<std::string> GetGroupNames(
      CSLConstList = nullptr) const override {
    return subgroup_names_;
  }

  void AddSubgroup(const std::string& name, std::shared_ptr<GDALGroup> grp) {
    subgroups_[name] = std::move(grp);
  }

  std::shared_ptr<GDALGroup> OpenGroup(const std::string& name,
                                       CSLConstList = nullptr) const override {
    auto it = subgroups_.find(name);
    if (it != subgroups_.end()) {
      return it->second;
    }
    return nullptr;
  }

  void SetArrayNames(std::vector<std::string> names) {
    array_names_ = std::move(names);
  }

  std::vector<std::string> GetMDArrayNames(
      CSLConstList = nullptr) const override {
    if (bThrowOnGetArrays_) {
      throw std::runtime_error("Simulated array enumeration failure");
    }
    return array_names_;
  }

  void AddArray(const std::string& name, std::shared_ptr<GDALMDArray> arr) {
    arrays_[name] = std::move(arr);
  }

  std::shared_ptr<GDALMDArray> OpenMDArray(
      const std::string& name, CSLConstList = nullptr) const override {
    auto it = arrays_.find(name);
    if (it != arrays_.end()) {
      return it->second;
    }
    return nullptr;
  }

  void AddDimension(std::shared_ptr<GDALDimension> dim) {
    dims_.push_back(std::move(dim));
  }

  std::vector<std::shared_ptr<GDALDimension>> GetDimensions(
      CSLConstList = nullptr) const override {
    return dims_;
  }

  void SetThrowOnGetArrays(bool b) { bThrowOnGetArrays_ = b; }

 private:
  std::vector<std::shared_ptr<GDALDimension>> dims_;
  std::vector<std::shared_ptr<GDALAttribute>> custom_attrs_;
  CPLStringList structural_info_;
  std::vector<std::string> subgroup_names_;
  std::map<std::string, std::shared_ptr<GDALGroup>> subgroups_;
  std::vector<std::string> array_names_;
  std::map<std::string, std::shared_ptr<GDALMDArray>> arrays_;
  bool bThrowOnGetArrays_ = false;
};

// Custom MDArray for testing custom block sizes and custom structural info.
class CustomMockMDArray : public GDALMDArray {
 public:
  CustomMockMDArray(const std::string& parent_name, const std::string& name,
                    std::vector<std::shared_ptr<GDALDimension>> dims,
                    GDALExtendedDataType dt)
      : GDALAbstractMDArray(parent_name, name),
        GDALMDArray(parent_name, name),
        dims_(std::move(dims)),
        dt_(std::move(dt)) {}

  bool IsWritable() const override { return false; }
  const std::string& GetFilename() const override { return filename_; }

  const std::vector<std::shared_ptr<GDALDimension>>& GetDimensions()
      const override {
    return dims_;
  }

  const GDALExtendedDataType& GetDataType() const override { return dt_; }

  void SetBlockSize(std::vector<GUInt64> block_size) {
    block_size_ = std::move(block_size);
  }

  std::vector<GUInt64> GetBlockSize() const override { return block_size_; }

  void SetCustomName(std::string name) { m_osName = std::move(name); }

  void AddAttribute(std::shared_ptr<GDALAttribute> attr) {
    custom_attrs_.push_back(std::move(attr));
  }

  std::vector<std::shared_ptr<GDALAttribute>> GetAttributes(
      CSLConstList = nullptr) const override {
    if (!custom_attrs_.empty()) {
      return custom_attrs_;
    }
    return {};
  }

  void SetStructuralInfo(CSLConstList structural_info) {
    structural_info_.Assign(CSLDuplicate(structural_info),
                            /*bTakeOwnership=*/TRUE);
  }

  CSLConstList GetStructuralInfo() const override {
    return structural_info_.List();
  }

 protected:
  bool IRead(const GUInt64*, const size_t*, const GInt64*, const GPtrDiff_t*,
             const GDALExtendedDataType&, void*) const override {
    return true;
  }

 private:
  std::vector<std::shared_ptr<GDALAttribute>> custom_attrs_;
  std::string filename_{};
  std::vector<std::shared_ptr<GDALDimension>> dims_;
  GDALExtendedDataType dt_;
  std::vector<GUInt64> block_size_;
  CPLStringList structural_info_;
};

class DummyBaseDimension : public GDALDimension {
 public:
  DummyBaseDimension(const std::string& parent_name, const std::string& name,
                     const std::string& type, const std::string& direction,
                     GUInt64 size)
      : GDALDimension(parent_name, name, type, direction, size) {}

  bool SetIndexingVariable(
      std::shared_ptr<GDALMDArray> poIndexingVariable) override {
    indexing_var_ = std::move(poIndexingVariable);
    return true;
  }

  std::shared_ptr<GDALMDArray> GetIndexingVariable() const override {
    return indexing_var_;
  }

 private:
  std::shared_ptr<GDALMDArray> indexing_var_;
};

// Custom GDALDataset wrapper for custom root groups.
class CustomDataset : public GDALDataset {
 public:
  explicit CustomDataset(std::shared_ptr<GDALGroup> root,
                         GDALDriver* drv = nullptr)
      : root_group_(std::move(root)) {
    poDriver = drv;
  }

  std::shared_ptr<GDALGroup> GetRootGroup() const override {
    return root_group_;
  }

 private:
  std::shared_ptr<GDALGroup> root_group_;
};

// Test parser usage string retrieval.
TEST_F(GdalMultiDimInfoTest, ParserUsage) {
  const std::string usage = GDALMultiDimInfoAppGetParserUsage();
  EXPECT_FALSE(usage.empty());
  EXPECT_THAT(usage, HasSubstr("Usage: gdalmdiminfo"));
  EXPECT_THAT(usage, HasSubstr("-detailed"));
  EXPECT_THAT(usage, HasSubstr("-nopretty"));
  EXPECT_THAT(usage, HasSubstr("-stats"));
}

// Test options creation, flag parsing, and memory freeing.
TEST_F(GdalMultiDimInfoTest, OptionsManagement) {
  // 1. Default options.
  MultiDimInfoOptionsUniquePtr options(
      GDALMultiDimInfoOptionsNew(nullptr, nullptr));
  ASSERT_NE(nullptr, options);

  // 2. All valid flags.
  std::vector<std::string> args = {
      "-detailed",  "-nopretty",    "-stats",  "-array",
      "my_array",   "-limit",       "10",      "-arrayoption",
      "FILTER=YES", "-arrayoption", "EXTRA=1", "-stdout",
  };
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  options.reset(GDALMultiDimInfoOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  // 3. Options for binary.
  GDALMultiDimInfoOptionsForBinary options_for_binary;
  std::vector<std::string> bin_args = {
      "-oo", "NAME=VALUE", "-if", "MEM", "dummy_dataset_name",
  };
  std::vector<char*> bin_argv;
  bin_argv.reserve(bin_args.size() + 1);
  for (auto& arg : bin_args) {
    bin_argv.push_back(arg.data());
  }
  bin_argv.push_back(nullptr);

  options.reset(
      GDALMultiDimInfoOptionsNew(bin_argv.data(), &options_for_binary));
  ASSERT_NE(nullptr, options);
  EXPECT_EQ(1, options_for_binary.aosOpenOptions.size());
  EXPECT_EQ(1, options_for_binary.aosAllowInputDrivers.size());
  EXPECT_EQ("dummy_dataset_name", options_for_binary.osFilename);

  // 4. Invalid arguments return nullptr.
  std::vector<std::string> invalid_args = {"-unsupported_option"};
  std::vector<char*> invalid_argv = {invalid_args[0].data(), nullptr};
  WithQuietHandler quiet;
  CPLErrorReset();
  options.reset(GDALMultiDimInfoOptionsNew(invalid_argv.data(), nullptr));
  EXPECT_EQ(nullptr, options);
  EXPECT_NE(CE_None, CPLGetLastErrorType());
}

// Test null dataset handling and non-multidimensional dataset handling.
TEST_F(GdalMultiDimInfoTest, NullDatasetAndNonMultidimDataset) {
  // 1. Null dataset.
  EXPECT_EQ(nullptr, GDALMultiDimInfo(nullptr, nullptr));

  // 2. Non-multidimensional dataset (MEM dataset without multidim support).
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr classic_ds(
      mem_drv->Create("", 1, 1, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, classic_ds);
  EXPECT_EQ(nullptr, classic_ds->GetRootGroup());

  EXPECT_EQ(nullptr, GDALMultiDimInfo(classic_ds.get(), nullptr));
}

// Test basic multidimensional dataset info output in pretty and nopretty mode.
TEST_F(GdalMultiDimInfoTest, BasicGroupAndArrayInfo) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  GDALDatasetUniquePtr ds(
      mem_drv->CreateMultiDimensional("mem_ds", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  // Add group attributes: single string, int, double.
  auto attr_str =
      root->CreateAttribute("title", {}, GDALExtendedDataType::CreateString());
  ASSERT_NE(nullptr, attr_str);
  EXPECT_TRUE(attr_str->Write("Test Dataset"));

  auto attr_int = root->CreateAttribute(
      "version", {}, GDALExtendedDataType::Create(GDT_Int32));
  ASSERT_NE(nullptr, attr_int);
  EXPECT_TRUE(attr_int->Write(42));

  // Add dimensions.
  auto dim_x = root->CreateDimension("x", "HORIZONTAL_X", "EAST", 4);
  ASSERT_NE(nullptr, dim_x);
  auto dim_y = root->CreateDimension("y", "HORIZONTAL_Y", "NORTH", 3);
  ASSERT_NE(nullptr, dim_y);

  // Add array.
  const auto dt_f32 = GDALExtendedDataType::Create(GDT_Float32);
  auto arr = root->CreateMDArray("temperature", {dim_y, dim_x}, dt_f32);
  ASSERT_NE(nullptr, arr);
  arr->SetUnit("K");
  arr->SetOffset(273.15);
  arr->SetScale(0.1);
  const float nodata = -999.0f;
  EXPECT_TRUE(arr->SetNoDataValue(nodata));

  auto arr_attr = arr->CreateAttribute("long_name", {},
                                       GDALExtendedDataType::CreateString());
  ASSERT_NE(nullptr, arr_attr);
  EXPECT_TRUE(arr_attr->Write("Air Temperature"));

  // 1. Default info output (pretty).
  char* pszInfo = GDALMultiDimInfo(ds.get(), nullptr);
  ASSERT_NE(nullptr, pszInfo);
  std::unique_ptr<char, decltype(&VSIFree)> info_holder(pszInfo, VSIFree);
  const std::string info_str(pszInfo);

  EXPECT_THAT(info_str, HasSubstr("\"type\": \"group\""));
  EXPECT_THAT(info_str, HasSubstr("\"driver\": \"MEM\""));
  EXPECT_THAT(info_str, HasSubstr("\"name\": \"/\""));
  EXPECT_THAT(info_str, HasSubstr("\"title\": \"Test Dataset\""));
  EXPECT_THAT(info_str, HasSubstr("\"version\": 42"));
  EXPECT_THAT(info_str, HasSubstr("\"temperature\""));
  EXPECT_THAT(info_str, HasSubstr("\"unit\": \"K\""));
  EXPECT_THAT(info_str, HasSubstr("\"offset\": 273.1"));
  EXPECT_THAT(info_str, HasSubstr("\"scale\": 0.1"));
  EXPECT_THAT(info_str, HasSubstr("\"nodata_value\": -999"));
  EXPECT_THAT(info_str, HasSubstr("\n"));  // Pretty formatted.

  // 2. No-pretty formatting.
  std::vector<std::string> args = {"-nopretty"};
  std::vector<char*> argv = {args[0].data(), nullptr};
  MultiDimInfoOptionsUniquePtr options(
      GDALMultiDimInfoOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  char* pszNoPretty = GDALMultiDimInfo(ds.get(), options.get());
  ASSERT_NE(nullptr, pszNoPretty);
  std::unique_ptr<char, decltype(&VSIFree)> nopretty_holder(pszNoPretty,
                                                            VSIFree);
  const std::string nopretty_str(pszNoPretty);
  EXPECT_THAT(nopretty_str, HasSubstr("\"type\":\"group\""));
  EXPECT_EQ(std::string::npos, nopretty_str.find('\n'));

  // 3. Stdout output.
  std::vector<std::string> stdout_args = {"-stdout"};
  std::vector<char*> stdout_argv = {stdout_args[0].data(), nullptr};
  options.reset(GDALMultiDimInfoOptionsNew(stdout_argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  char* pszStdout = GDALMultiDimInfo(ds.get(), options.get());
  ASSERT_NE(nullptr, pszStdout);
  std::unique_ptr<char, decltype(&VSIFree)> stdout_holder(pszStdout, VSIFree);
  EXPECT_STREQ("ok", pszStdout);
}

// Test detailed mode displaying array values across numeric and complex types.
TEST_F(GdalMultiDimInfoTest, DetailedValuesNumericAndComplex) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  GDALDatasetUniquePtr ds(
      mem_drv->CreateMultiDimensional("detailed_ds", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto dim2 = root->CreateDimension("dim2", "", "", 2);
  ASSERT_NE(nullptr, dim2);

  // Helper lambda to create a 1D array of length 2 and write values.
  auto create_arr = [&](const char* name, GDALDataType type, const void* data) {
    auto dt = GDALExtendedDataType::Create(type);
    auto arr = root->CreateMDArray(name, {dim2}, dt);
    const std::vector<GUInt64> start = {0};
    const std::vector<size_t> count = {2};
    EXPECT_TRUE(
        arr->Write(start.data(), count.data(), nullptr, nullptr, dt, data));
  };

  const std::vector<GByte> u8_vals = {10, 20};
  create_arr("u8", GDT_Byte, u8_vals.data());

  const std::vector<GInt8> i8_vals = {-5, 5};
  create_arr("i8", GDT_Int8, i8_vals.data());

  const std::vector<GInt16> i16_vals = {-300, 300};
  create_arr("i16", GDT_Int16, i16_vals.data());

  const std::vector<GUInt16> u16_vals = {1000, 2000};
  create_arr("u16", GDT_UInt16, u16_vals.data());

  const std::vector<GInt32> i32_vals = {-100000, 100000};
  create_arr("i32", GDT_Int32, i32_vals.data());

  const std::vector<GUInt32> u32_vals = {100000, 200000};
  create_arr("u32", GDT_UInt32, u32_vals.data());

  const std::vector<std::int64_t> i64_vals = {-10000000000LL, 10000000000LL};
  create_arr("i64", GDT_Int64, i64_vals.data());

  const std::vector<std::uint64_t> u64_vals = {10000000000ULL, 20000000000ULL};
  create_arr("u64", GDT_UInt64, u64_vals.data());

  const std::vector<float> f32_vals = {1.5f, 2.5f};
  create_arr("f32", GDT_Float32, f32_vals.data());

  const std::vector<double> f64_vals = {3.14159, 2.71828};
  create_arr("f64", GDT_Float64, f64_vals.data());

  // Complex types.
  const std::vector<GInt16> ci16_vals = {1, 2, 3, 4};
  create_arr("ci16", GDT_CInt16, ci16_vals.data());

  const std::vector<GInt32> ci32_vals = {10, 20, 30, 40};
  create_arr("ci32", GDT_CInt32, ci32_vals.data());

  const std::vector<float> cf32_vals = {1.0f, -1.0f, 2.0f, -2.0f};
  create_arr("cf32", GDT_CFloat32, cf32_vals.data());

  const std::vector<double> cf64_vals = {5.0, 6.0, 7.0, 8.0};
  create_arr("cf64", GDT_CFloat64, cf64_vals.data());

  const std::vector<GFloat16> f16_vals = {GFloat16(1.0f), GFloat16(2.0f)};
  create_arr("f16", GDT_Float16, f16_vals.data());

  const std::vector<GFloat16> cf16_vals = {GFloat16(1.0f), GFloat16(2.0f),
                                           GFloat16(3.0f), GFloat16(4.0f)};
  create_arr("cf16", GDT_CFloat16, cf16_vals.data());

  // Compound array values.
  std::vector<std::unique_ptr<GDALEDTComponent>> comps;
  comps.push_back(std::make_unique<GDALEDTComponent>(
      "id", 0, GDALExtendedDataType::Create(GDT_Int32)));
  auto dt_comp =
      GDALExtendedDataType::Create("record", sizeof(int), std::move(comps));
  auto arr_comp = root->CreateMDArray("arr_comp", {dim2}, dt_comp);
  ASSERT_NE(nullptr, arr_comp);
  int comp_vals[2] = {101, 102};
  const std::vector<GUInt64> start_comp = {0};
  const std::vector<size_t> count_comp = {2};
  EXPECT_TRUE(arr_comp->Write(start_comp.data(), count_comp.data(), nullptr,
                              nullptr, dt_comp, comp_vals));

  // String array.
  auto dt_str = GDALExtendedDataType::CreateString();
  auto arr_str = root->CreateMDArray("str_arr", {dim2}, dt_str);
  const char* str_vals[2] = {"hello", "world"};
  const std::vector<GUInt64> start = {0};
  const std::vector<size_t> count = {2};
  EXPECT_TRUE(arr_str->Write(start.data(), count.data(), nullptr, nullptr,
                             dt_str, str_vals));

  // 0-dimensional scalar array.
  auto scalar_arr = root->CreateMDArray(
      "scalar", {}, GDALExtendedDataType::Create(GDT_Int32));
  ASSERT_NE(nullptr, scalar_arr);
  int scalar_val = 999;
  EXPECT_TRUE(scalar_arr->Write(nullptr, nullptr, nullptr, nullptr,
                                scalar_arr->GetDataType(), &scalar_val));

  // Run with -detailed.
  std::vector<std::string> args = {"-detailed"};
  std::vector<char*> argv = {args[0].data(), nullptr};
  MultiDimInfoOptionsUniquePtr options(
      GDALMultiDimInfoOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  char* pszInfo = GDALMultiDimInfo(ds.get(), options.get());
  ASSERT_NE(nullptr, pszInfo);
  std::unique_ptr<char, decltype(&VSIFree)> info_holder(pszInfo, VSIFree);
  const std::string info_str(pszInfo);

  EXPECT_THAT(info_str, HasSubstr("\"values\": ["));
  EXPECT_THAT(info_str, HasSubstr("\"arr_comp\""));
  EXPECT_THAT(info_str, HasSubstr("\"id\": 101"));
  EXPECT_THAT(info_str, HasSubstr("\"u8\""));
  EXPECT_THAT(info_str, HasSubstr("10"));
  EXPECT_THAT(info_str, HasSubstr("\"i8\""));
  EXPECT_THAT(info_str, HasSubstr("-5"));
  EXPECT_THAT(info_str, HasSubstr("\"ci16\""));
  EXPECT_THAT(info_str, HasSubstr("\"real\": 1"));
  EXPECT_THAT(info_str, HasSubstr("\"imag\": 2"));
  EXPECT_THAT(info_str, HasSubstr("\"str_arr\""));
  EXPECT_THAT(info_str, HasSubstr("\"hello\""));
  EXPECT_THAT(info_str, HasSubstr("\"world\""));
  EXPECT_THAT(info_str, HasSubstr("\"scalar\""));
  EXPECT_THAT(info_str, HasSubstr("999"));
}

// Test value truncation with -limit option in 1D and multi-dimensional arrays.
TEST_F(GdalMultiDimInfoTest, LimitValuesInDimensions) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  GDALDatasetUniquePtr ds(
      mem_drv->CreateMultiDimensional("limit_ds", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  // 1D array of size 10.
  auto dim_10 = root->CreateDimension("dim_10", "", "", 10);
  const auto dt_i32 = GDALExtendedDataType::Create(GDT_Int32);
  auto arr_1d = root->CreateMDArray("arr_1d", {dim_10}, dt_i32);
  std::vector<int> vals_1d(10);
  for (int i = 0; i < 10; ++i) vals_1d[i] = i * 10;
  const std::vector<GUInt64> start1 = {0};
  const std::vector<size_t> count1 = {10};
  EXPECT_TRUE(arr_1d->Write(start1.data(), count1.data(), nullptr, nullptr,
                            dt_i32, vals_1d.data()));

  // 2D array of size 6x6.
  auto dim_6a = root->CreateDimension("dim_6a", "", "", 6);
  auto dim_6b = root->CreateDimension("dim_6b", "", "", 6);
  auto arr_2d = root->CreateMDArray("arr_2d", {dim_6a, dim_6b}, dt_i32);
  std::vector<int> vals_2d(36, 1);
  const std::vector<GUInt64> start2 = {0, 0};
  const std::vector<size_t> count2 = {6, 6};
  EXPECT_TRUE(arr_2d->Write(start2.data(), count2.data(), nullptr, nullptr,
                            dt_i32, vals_2d.data()));

  // Query with -detailed -limit 4.
  std::vector<std::string> args = {"-detailed", "-limit", "4"};
  std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                             nullptr};
  MultiDimInfoOptionsUniquePtr options(
      GDALMultiDimInfoOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  char* pszInfo = GDALMultiDimInfo(ds.get(), options.get());
  ASSERT_NE(nullptr, pszInfo);
  std::unique_ptr<char, decltype(&VSIFree)> info_holder(pszInfo, VSIFree);
  const std::string info_str(pszInfo);

  EXPECT_THAT(info_str, HasSubstr("\"[...]\""));
  EXPECT_THAT(info_str, HasSubstr("\"arr_1d\""));
  EXPECT_THAT(info_str, HasSubstr("\"arr_2d\""));
}

// Test -stats computation and reporting.
TEST_F(GdalMultiDimInfoTest, ArrayStatistics) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  GDALDatasetUniquePtr ds(
      mem_drv->CreateMultiDimensional("stats_ds", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto dim = root->CreateDimension("dim", "", "", 4);
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto arr = root->CreateMDArray("stats_arr", {dim}, dt_f64);
  const std::vector<double> vals = {10.0, 20.0, 30.0, 40.0};
  const std::vector<GUInt64> start = {0};
  const std::vector<size_t> count = {4};
  EXPECT_TRUE(arr->Write(start.data(), count.data(), nullptr, nullptr, dt_f64,
                         vals.data()));

  std::vector<std::string> args = {"-stats"};
  std::vector<char*> argv = {args[0].data(), nullptr};
  MultiDimInfoOptionsUniquePtr options(
      GDALMultiDimInfoOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  char* pszInfo = GDALMultiDimInfo(ds.get(), options.get());
  ASSERT_NE(nullptr, pszInfo);
  std::unique_ptr<char, decltype(&VSIFree)> info_holder(pszInfo, VSIFree);
  const std::string info_str(pszInfo);

  EXPECT_THAT(info_str, HasSubstr("\"statistics\": {"));
  EXPECT_THAT(info_str, HasSubstr("\"min\": 10"));
  EXPECT_THAT(info_str, HasSubstr("\"max\": 40"));
  EXPECT_THAT(info_str, HasSubstr("\"mean\": 25"));
  EXPECT_THAT(info_str, HasSubstr("\"valid_sample_count\": 4"));
}

// Test attributes: JSON subtype, string array, numeric array, compound, and
// non-unique names.
TEST_F(GdalMultiDimInfoTest, ComplexAttributesAndSubtypes) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  GDALDatasetUniquePtr ds(
      mem_drv->CreateMultiDimensional("attr_ds", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  // 1. JSON valid attribute.
  const auto dt_json = GDALExtendedDataType::CreateString(0, GEDTST_JSON);
  auto attr_json = root->CreateAttribute("json_attr", {}, dt_json);
  ASSERT_NE(nullptr, attr_json);
  EXPECT_TRUE(attr_json->Write(
      R"({"key": "val", "count": 12, "pi": 3.14, "flag": true, "empty": null, "list": [1, 2]})"));

  // 2. JSON invalid attribute (falls back to string output).
  auto attr_invalid_json = root->CreateAttribute("invalid_json", {}, dt_json);
  ASSERT_NE(nullptr, attr_invalid_json);
  EXPECT_TRUE(attr_invalid_json->Write("{not-valid-json"));

  // 3. String array attribute.
  auto attr_str_arr = root->CreateAttribute(
      "str_arr", {3}, GDALExtendedDataType::CreateString());
  ASSERT_NE(nullptr, attr_str_arr);
  const char* str_array[4] = {"first", "second", "third", nullptr};
  EXPECT_TRUE(attr_str_arr->Write(str_array));

  // 4. Numeric array attribute.
  auto attr_num_arr = root->CreateAttribute(
      "num_arr", {3}, GDALExtendedDataType::Create(GDT_Int32));
  ASSERT_NE(nullptr, attr_num_arr);
  const std::vector<int> num_array = {100, 200, 300};
  EXPECT_TRUE(attr_num_arr->Write(num_array.data(), num_array.size()));

  // 5. Compound attribute.
  std::vector<std::unique_ptr<GDALEDTComponent>> comps;
  comps.push_back(std::make_unique<GDALEDTComponent>(
      "id", 0, GDALExtendedDataType::Create(GDT_Int32)));
  comps.push_back(std::make_unique<GDALEDTComponent>(
      "weight", sizeof(int), GDALExtendedDataType::Create(GDT_Float32)));
  const size_t comp_size = sizeof(int) + sizeof(float);
  const auto dt_compound =
      GDALExtendedDataType::Create("record", comp_size, std::move(comps));

  auto attr_compound = root->CreateAttribute("comp_attr", {}, dt_compound);
  ASSERT_NE(nullptr, attr_compound);
  struct Record {
    int id;
    float weight;
  } rec = {7, 45.5f};
  EXPECT_TRUE(attr_compound->Write(&rec, sizeof(Record)));

  // Array of compound attributes (2 elements).
  auto attr_comp_arr = root->CreateAttribute("comp_arr", {2}, dt_compound);
  ASSERT_NE(nullptr, attr_comp_arr);
  Record recs[2] = {{1, 10.0f}, {2, 20.0f}};
  EXPECT_TRUE(attr_comp_arr->Write(recs, sizeof(recs)));

  // Detailed mode for attributes checks datatype, subtype and value keys.
  std::vector<std::string> args = {"-detailed"};
  std::vector<char*> argv = {args[0].data(), nullptr};
  MultiDimInfoOptionsUniquePtr options(
      GDALMultiDimInfoOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  char* pszInfo = GDALMultiDimInfo(ds.get(), options.get());
  ASSERT_NE(nullptr, pszInfo);
  std::unique_ptr<char, decltype(&VSIFree)> info_holder(pszInfo, VSIFree);
  const std::string info_str(pszInfo);

  EXPECT_THAT(info_str, HasSubstr("\"json_attr\""));
  EXPECT_THAT(info_str, HasSubstr("\"subtype\": \"JSON\""));
  EXPECT_THAT(info_str, HasSubstr("\"key\": \"val\""));
  EXPECT_THAT(info_str, HasSubstr("\"invalid_json\""));
  EXPECT_THAT(info_str, HasSubstr("\"{not-valid-json\""));
  EXPECT_THAT(info_str, HasSubstr("\"first\""));
  EXPECT_THAT(info_str, HasSubstr("\"second\""));
  EXPECT_THAT(info_str, HasSubstr("\"num_arr\""));
  EXPECT_THAT(info_str, HasSubstr("100"));
  EXPECT_THAT(info_str, HasSubstr("\"comp_attr\""));
  EXPECT_THAT(info_str, HasSubstr("\"comp_arr\""));
  EXPECT_THAT(info_str, HasSubstr("\"id\": 7"));
}

// Test spatial reference, indexing variables, and dimensions reuse.
TEST_F(GdalMultiDimInfoTest, SpatialRefAndIndexingVariable) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  GDALDatasetUniquePtr ds(
      mem_drv->CreateMultiDimensional("srs_ds", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto dim_x = root->CreateDimension("x", "HORIZONTAL_X", "EAST", 2);
  ASSERT_NE(nullptr, dim_x);
  auto dim_y = root->CreateDimension("y", "HORIZONTAL_Y", "NORTH", 2);
  ASSERT_NE(nullptr, dim_y);

  // Set indexing variable.
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto arr_x = root->CreateMDArray("x", {dim_x}, dt_f64);
  ASSERT_NE(nullptr, arr_x);
  EXPECT_TRUE(dim_x->SetIndexingVariable(arr_x));

  auto arr_main = root->CreateMDArray("var", {dim_y, dim_x}, dt_f64);
  ASSERT_NE(nullptr, arr_main);

  // Set SRS EPSG:4326.
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  EXPECT_TRUE(arr_main->SetSpatialRef(&srs));

  char* pszInfo = GDALMultiDimInfo(ds.get(), nullptr);
  ASSERT_NE(nullptr, pszInfo);
  std::unique_ptr<char, decltype(&VSIFree)> info_holder(pszInfo, VSIFree);
  const std::string info_str(pszInfo);

  EXPECT_THAT(info_str, HasSubstr("\"srs\": {"));
  EXPECT_THAT(info_str, HasSubstr("\"wkt\":"));
  EXPECT_THAT(info_str, HasSubstr("\"data_axis_to_srs_axis_mapping\""));
  EXPECT_THAT(info_str, HasSubstr("\"indexing_variable\": {"));
  // dim_x is referenced by arr_x and arr_main; second dump uses full_name
  // string.
  EXPECT_THAT(info_str, HasSubstr("\"/x\""));
}

// Test single array inspection via -array flag, including sub-groups and
// errors.
TEST_F(GdalMultiDimInfoTest, SpecificArrayInspection) {
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);

  GDALDatasetUniquePtr ds(
      mem_drv->CreateMultiDimensional("subgroup_ds", nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto root = ds->GetRootGroup();
  ASSERT_NE(nullptr, root);

  auto sub1 = root->CreateGroup("sub1");
  ASSERT_NE(nullptr, sub1);
  auto sub2 = sub1->CreateGroup("sub2");
  ASSERT_NE(nullptr, sub2);

  auto dim = sub2->CreateDimension("d", "", "", 2);
  auto arr = sub2->CreateMDArray("nested_arr", {dim},
                                 GDALExtendedDataType::Create(GDT_Int32));
  ASSERT_NE(nullptr, arr);

  // 1. Successful lookup of nested array.
  std::vector<std::string> args = {"-array", "sub1/sub2/nested_arr"};
  std::vector<char*> argv = {args[0].data(), args[1].data(), nullptr};
  MultiDimInfoOptionsUniquePtr options(
      GDALMultiDimInfoOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  char* pszInfo = GDALMultiDimInfo(ds.get(), options.get());
  ASSERT_NE(nullptr, pszInfo);
  std::unique_ptr<char, decltype(&VSIFree)> info_holder(pszInfo, VSIFree);
  const std::string info_str(pszInfo);

  EXPECT_THAT(info_str, HasSubstr("\"name\": \"nested_arr\""));
  EXPECT_THAT(info_str, HasSubstr("\"type\": \"array\""));

  // 2. Non-existent group error.
  std::vector<std::string> bad_grp_args = {"-array", "bad_group/nested_arr"};
  std::vector<char*> bad_grp_argv = {bad_grp_args[0].data(),
                                     bad_grp_args[1].data(), nullptr};
  options.reset(GDALMultiDimInfoOptionsNew(bad_grp_argv.data(), nullptr));
  ASSERT_NE(nullptr, options);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMultiDimInfo(ds.get(), options.get()));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot find group"));
  }

  // 3. Non-existent array error.
  std::vector<std::string> bad_arr_args = {"-array", "sub1/sub2/bad_arr"};
  std::vector<char*> bad_arr_argv = {bad_arr_args[0].data(),
                                     bad_arr_args[1].data(), nullptr};
  options.reset(GDALMultiDimInfoOptionsNew(bad_arr_argv.data(), nullptr));
  ASSERT_NE(nullptr, options);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMultiDimInfo(ds.get(), options.get()));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot find array"));
  }
}

// Test custom group with RAT (Raster Attribute Table), structural info,
// custom block size, duplicate subgroup/array names, and exception handling.
TEST_F(GdalMultiDimInfoTest, MockGroupAdvancedFeatures) {
  auto root = CustomMockGroup::Create("", "not_root");
  root->SetCustomName("not_root");
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  CustomDataset custom_ds(root, mem_drv);

  // Add duplicate attributes to exercise non-unique attribute names branch.
  GDALDatasetUniquePtr mem_ds(
      mem_drv->CreateMultiDimensional("tmp_mem", nullptr, nullptr));
  ASSERT_NE(nullptr, mem_ds);
  auto tmp_root = mem_ds->GetRootGroup();
  auto attr_dup = tmp_root->CreateAttribute(
      "dup_attr", {}, GDALExtendedDataType::CreateString());
  ASSERT_NE(nullptr, attr_dup);
  EXPECT_TRUE(attr_dup->Write("dup_val"));
  root->AddAttribute(attr_dup);
  root->AddAttribute(attr_dup);

  // 1. Structural info with key-value and non-key-value items.
  CPLStringList group_struct_info;
  group_struct_info.AddString("NC_FORMAT=NETCDF4");
  group_struct_info.AddString("BARE_METADATA_ITEM");
  root->SetStructuralInfo(group_struct_info.List());

  // 2. Data types: String, RAT with all column types, and Compound.
  root->AddDataType(std::make_shared<GDALExtendedDataType>(
      GDALExtendedDataType::CreateString()));

  // Create RAT with Integer, Real, String, Boolean, DateTime, WKBGeometry.
  auto rat = std::make_unique<GDALDefaultRasterAttributeTable>();
  rat->CreateColumn("int_col", GFT_Integer, GFU_Generic);
  rat->CreateColumn("real_col", GFT_Real, GFU_Generic);
  rat->CreateColumn("str_col", GFT_String, GFU_Generic);
  rat->CreateColumn("bool_col", GFT_Boolean, GFU_Generic);
  rat->CreateColumn("date_col", GFT_DateTime, GFU_Generic);
  rat->CreateColumn("geom_col", GFT_WKBGeometry, GFU_Generic);

  rat->SetValue(0, 0, 10);
  rat->SetValue(0, 1, 20.5);
  rat->SetValue(0, 2, "hello");
  rat->SetValue(0, 3, true);
  GDALRATDateTime dt_val;
  dt_val.nYear = 2026;
  dt_val.nMonth = 11;
  dt_val.nDay = 5;
  dt_val.nHour = 12;
  dt_val.nMinute = 0;
  dt_val.fSecond = 0.0f;
  dt_val.bIsValid = true;
  rat->SetValue(0, 4, dt_val);
  // Valid WKB Point(0, 0).
  const unsigned char wkb_point[] = {
      0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  rat->SetValue(0, 5, wkb_point, sizeof(wkb_point));

  // Second row with empty WKB geometry.
  rat->SetValue(1, 0, 20);
  rat->SetValue(1, 1, 30.5);
  rat->SetValue(1, 2, "world");
  rat->SetValue(1, 3, false);
  rat->SetValue(1, 4, dt_val);
  rat->SetValue(1, 5, nullptr, 0);

  const auto dt_rat =
      GDALExtendedDataType::Create("enum_rat", GDT_Int32, std::move(rat));
  root->AddDataType(std::make_shared<GDALExtendedDataType>(dt_rat));

  // Compound data type.
  std::vector<std::unique_ptr<GDALEDTComponent>> comps;
  comps.push_back(std::make_unique<GDALEDTComponent>(
      "sub_int", 0, GDALExtendedDataType::Create(GDT_Int32)));
  const auto dt_comp =
      GDALExtendedDataType::Create("my_struct", sizeof(int), std::move(comps));
  root->AddDataType(std::make_shared<GDALExtendedDataType>(dt_comp));

  // 3. Custom MDArray with non-null block size and structural info.
  auto custom_arr = std::make_shared<CustomMockMDArray>(
      "/", "custom_arr", std::vector<std::shared_ptr<GDALDimension>>{},
      GDALExtendedDataType::Create(GDT_Int32));
  custom_arr->SetBlockSize({64, 64});
  CPLStringList arr_struct_info;
  arr_struct_info.AddString("STORAGE=CHUNKED");
  custom_arr->SetStructuralInfo(arr_struct_info.List());

  // Set duplicate array names to exercise duplicate suppression in DumpArrays.
  root->SetArrayNames({"custom_arr", "custom_arr"});
  root->AddArray("custom_arr", custom_arr);
  root->AddArray("/custom_arr", custom_arr);

  // Add dimension with indexing variable where OpenMDArray succeeds.
  auto dim_idx = std::make_shared<DummyBaseDimension>("/", "d_idx", "", "", 2);
  dim_idx->SetIndexingVariable(custom_arr);
  root->AddDimension(dim_idx);

  // Set duplicate subgroup names to exercise non-unique subgroup names branch.
  auto sub = CustomMockGroup::Create("/", "sub");
  root->SetGroupNames({"sub", "sub"});
  root->AddSubgroup("sub", sub);

  char* pszInfo = GDALMultiDimInfo(&custom_ds, nullptr);
  ASSERT_NE(nullptr, pszInfo);
  std::unique_ptr<char, decltype(&VSIFree)> info_holder(pszInfo, VSIFree);
  const std::string info_str(pszInfo);

  EXPECT_THAT(info_str, HasSubstr("\"name\": \"not_root\""));
  EXPECT_THAT(info_str, HasSubstr("\"full_name\": \"/\""));
  EXPECT_THAT(info_str, HasSubstr("\"indexing_variable\": \"/custom_arr\""));
  EXPECT_THAT(info_str, HasSubstr("\"NC_FORMAT\": \"NETCDF4\""));
  EXPECT_THAT(info_str, HasSubstr("\"metadata_1\": \"BARE_METADATA_ITEM\""));
  EXPECT_THAT(info_str, HasSubstr("\"enum_rat\""));
  EXPECT_THAT(info_str, HasSubstr("\"attribute_table\""));
  EXPECT_THAT(info_str, HasSubstr("\"int_col\": 10"));
  EXPECT_THAT(info_str, HasSubstr("\"real_col\": 20.5"));
  EXPECT_THAT(info_str, HasSubstr("\"bool_col\": true"));
  EXPECT_THAT(info_str, HasSubstr("\"POINT (0 0)\""));
  EXPECT_THAT(info_str,
              HasSubstr("\"block_size\": [\n        64,\n        64\n      ]"));
  EXPECT_THAT(info_str, HasSubstr("\"STORAGE\": \"CHUNKED\""));

  // 4. Exception handling in GDALMultiDimInfo.
  root->SetThrowOnGetArrays(true);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMultiDimInfo(&custom_ds, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Simulated array enumeration failure"));
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdalmdiminfo_lib.cpp.
