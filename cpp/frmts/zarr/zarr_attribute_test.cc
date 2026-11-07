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
// Tests for ZarrAttributeGroup (zarr_attribute.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/zarr_attribute.cpp

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/zarr/zarr_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/zarr/zarr.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_json.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

TEST(ZarrAttributeTest, InitAndSerializeScalarTypes) {
  ZarrAttributeGroup attr_group("/", /*bContainerIsGroup=*/true);

  CPLJSONObject in;
  in.Add("str_attr", "hello world");
  in.Add("int_attr", 42);
  in.Add("int64_attr", static_cast<GInt64>(9876543210123LL));
  in.Add("double_attr", 3.14159);
  in.Add("bool_attr", true);
  in.Add("json_obj_attr", CPLJSONObject());

  attr_group.Init(in, /*bUpdatable=*/true);

  CPLJSONObject out = attr_group.Serialize();
  EXPECT_EQ("hello world", out.GetString("str_attr"));
  EXPECT_EQ(42, out.GetInteger("int_attr"));
  EXPECT_EQ(9876543210123LL, out.GetLong("int64_attr"));
  EXPECT_NEAR(3.14159, out.GetDouble("double_attr"), 1e-5);
  EXPECT_TRUE(out["json_obj_attr"].IsValid());
}

TEST(ZarrAttributeTest, InitAndSerializeArrayTypes) {
  ZarrAttributeGroup attr_group("/", /*bContainerIsGroup=*/true);

  CPLJSONObject in;
  {
    CPLJSONArray arr;
    arr.Add("a");
    arr.Add("b");
    in.Add("str_array", arr);
  }
  {
    CPLJSONArray arr;
    arr.Add(1);
    arr.Add(2);
    in.Add("int_array", arr);
  }
  {
    CPLJSONArray arr;
    arr.Add(static_cast<GInt64>(10000000000LL));
    arr.Add(static_cast<GInt64>(20000000000LL));
    in.Add("int64_array", arr);
  }
  {
    CPLJSONArray arr;
    arr.Add(1.5);
    arr.Add(2.5);
    in.Add("double_array", arr);
  }
  {
    // Mixed array falls back to JSON string
    CPLJSONArray arr;
    arr.Add("str");
    arr.Add(123);
    in.Add("mixed_array", arr);
  }

  attr_group.Init(in, /*bUpdatable=*/true);

  CPLJSONObject out = attr_group.Serialize();
  auto str_arr = out.GetArray("str_array");
  EXPECT_EQ(2, str_arr.Size());
  EXPECT_EQ("a", str_arr[0].ToString());
  EXPECT_EQ("b", str_arr[1].ToString());

  auto int_arr = out.GetArray("int_array");
  EXPECT_EQ(2, int_arr.Size());
  EXPECT_EQ(1, int_arr[0].ToInteger());
  EXPECT_EQ(2, int_arr[1].ToInteger());

  auto int64_arr = out.GetArray("int64_array");
  EXPECT_EQ(2, int64_arr.Size());
  EXPECT_EQ(10000000000LL, int64_arr[0].ToLong());

  auto double_arr = out.GetArray("double_array");
  EXPECT_EQ(2, double_arr.Size());
  EXPECT_DOUBLE_EQ(1.5, double_arr[0].ToDouble());

  EXPECT_TRUE(out["mixed_array"].IsValid());
}

TEST(ZarrAttributeTest, SerializeAdditionalTypes) {
  ZarrAttributeGroup attr_group("/", /*bContainerIsGroup=*/true);

  // Unsigned int scalar
  auto uint_attr = attr_group.CreateAttribute(
      "uint_attr", {}, GDALExtendedDataType::Create(GDT_UInt32), nullptr);
  ASSERT_NE(nullptr, uint_attr);
  const uint32_t uint_val = 12345;
  const GUInt64 start_idx = 0;
  const size_t count = 1;
  EXPECT_TRUE(uint_attr->Write(&start_idx, &count, nullptr, nullptr,
                               uint_attr->GetDataType(), &uint_val));

  // Float scalar
  auto float_attr = attr_group.CreateAttribute(
      "float_attr", {}, GDALExtendedDataType::Create(GDT_Float32), nullptr);
  ASSERT_NE(nullptr, float_attr);
  const float float_val = 2.718f;
  EXPECT_TRUE(float_attr->Write(&start_idx, &count, nullptr, nullptr,
                                float_attr->GetDataType(), &float_val));

  // Unsigned int 1D array
  auto uint_arr = attr_group.CreateAttribute(
      "uint_arr", {2}, GDALExtendedDataType::Create(GDT_UInt32), nullptr);
  ASSERT_NE(nullptr, uint_arr);
  const uint32_t uint_arr_vals[2] = {10, 20};
  const size_t count2 = 2;
  EXPECT_TRUE(uint_arr->Write(&start_idx, &count2, nullptr, nullptr,
                              uint_arr->GetDataType(), uint_arr_vals));

  // Float 1D array
  auto float_arr = attr_group.CreateAttribute(
      "float_arr", {2}, GDALExtendedDataType::Create(GDT_Float32), nullptr);
  ASSERT_NE(nullptr, float_arr);
  const float float_arr_vals[2] = {0.5f, 1.5f};
  EXPECT_TRUE(float_arr->Write(&start_idx, &count2, nullptr, nullptr,
                               float_arr->GetDataType(), float_arr_vals));

  // Null string
  auto null_str_attr = attr_group.CreateAttribute(
      "null_str", {}, GDALExtendedDataType::CreateString(), nullptr);
  ASSERT_NE(nullptr, null_str_attr);

  CPLJSONObject out = attr_group.Serialize();
  EXPECT_EQ(12345, out.GetLong("uint_attr"));
  EXPECT_NEAR(2.718, out.GetDouble("float_attr"), 1e-3);
  EXPECT_EQ(2, out.GetArray("uint_arr").Size());
  EXPECT_EQ(2, out.GetArray("float_arr").Size());
  EXPECT_EQ(CPLJSONObject::Type::Null, out["null_str"].GetType());
}

TEST(ZarrAttributeTest, ModificationAndDeletion) {
  ZarrAttributeGroup attr_group("/", /*bContainerIsGroup=*/true);
  EXPECT_FALSE(attr_group.IsModified());

  auto attr = attr_group.CreateAttribute(
      "attr1", {}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, attr);
  EXPECT_TRUE(attr_group.IsModified());

  attr_group.UnsetModified();
  EXPECT_FALSE(attr_group.IsModified());

  EXPECT_TRUE(attr_group.DeleteAttribute("attr1"));
  EXPECT_TRUE(attr_group.IsModified());
  EXPECT_FALSE(attr_group.DeleteAttribute("attr1"));
}

TEST(ZarrAttributeTest, ParentRenamedAndDeleted) {
  ZarrAttributeGroup group_attr("/", /*bContainerIsGroup=*/true);
  CPLJSONObject obj;
  obj.Add("key", "val");
  group_attr.Init(obj, /*bUpdatable=*/true);

  group_attr.ParentRenamed("/new_parent");
  auto attr = group_attr.GetAttribute("key");
  ASSERT_NE(nullptr, attr);
  EXPECT_EQ("/new_parent/_GLOBAL_/key", attr->GetFullName());

  ZarrAttributeGroup array_attr("/parent_array", /*bContainerIsGroup=*/false);
  array_attr.Init(obj, /*bUpdatable=*/true);
  array_attr.ParentRenamed("/new_array");
  auto arr_attr = array_attr.GetAttribute("key");
  ASSERT_NE(nullptr, arr_attr);
  EXPECT_EQ("/new_array/key", arr_attr->GetFullName());

  array_attr.ParentDeleted();
  group_attr.ParentDeleted();
}

TEST(ZarrAttributeTest, EmptyOrNonObjectInit) {
  ZarrAttributeGroup attr_group("/", /*bContainerIsGroup=*/true);
  CPLJSONObject empty_obj;
  attr_group.Init(empty_obj, /*bUpdatable=*/false);
  EXPECT_TRUE(attr_group.GetAttributes().empty());
}

TEST(ZarrAttributeTest, Serialize2DAttributeWarning) {
  ZarrAttributeGroup attr_group("/", /*bContainerIsGroup=*/true);
  auto attr = attr_group.CreateAttribute(
      "multi_dim", {2, 2}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, attr);

  auto str_2d = attr_group.CreateAttribute(
      "str_2d", {2, 2}, GDALExtendedDataType::CreateString(), nullptr);
  ASSERT_NE(nullptr, str_2d);

  const ::WithQuietHandler quiet;
  CPLErrorReset();
  CPLJSONObject out = attr_group.Serialize();
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_FALSE(out["multi_dim"].IsValid());
  EXPECT_FALSE(out["str_2d"].IsValid());
}

// TODO: b/335317901 - Cover 64-bit integer array element promotion in
// ZarrAttributeGroup::Init (zarr_attribute.cpp).

}  // namespace
}  // namespace autotest2
