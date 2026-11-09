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
// Unit tests for third_party/hdf5/c++/src/H5PropList.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5PropList.cpp

#include <array>
#include <cstdint>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {

template struct ConstantGetterBypass<ConstantGetterTag<H5::PropList>,
                                     &H5::PropList::getConstant>;

namespace {

TEST(H5PropListTest, DefaultCopyAndIdConstructors) {
  H5::PropList default_ctor;
  EXPECT_EQ(default_ctor.getId(), H5P_DEFAULT);
  EXPECT_EQ(default_ctor.fromClass(), "PropList");

  H5::PropList from_class(H5P_FILE_CREATE);
  PublicSetId<H5::PropList> file_create_cls;
  file_create_cls.CallSetId(H5P_FILE_CREATE);
  absl::Cleanup reset_cls = [&]() { file_create_cls.ResetIdRaw(); };
  EXPECT_TRUE(from_class.isAClass(file_create_cls));
  EXPECT_EQ(file_create_cls.getClassName(), "file create");

  H5::PropList from_list(from_class.getId());
  EXPECT_TRUE(from_list == from_class);

  H5::PropList copy_constructed(from_class);
  EXPECT_EQ(copy_constructed.getId(), from_class.getId());

  H5::PropList non_plist(H5I_INVALID_HID);
  EXPECT_EQ(non_plist.getId(), H5P_DEFAULT);

  H5::DataSpace dataspace(H5S_SCALAR);
  H5::PropList from_dataspace(dataspace.getId());
  EXPECT_EQ(from_dataspace.getId(), H5P_DEFAULT);

  EXPECT_EQ(H5::PropList::DEFAULT.getId(), H5P_DEFAULT);
}

TEST(H5PropListTest, CopyAssignmentAndCloseBehavior) {
  H5::PropList src(H5P_DATASET_CREATE);
  H5::PropList dst;
  dst.copy(src);
  EXPECT_TRUE(dst == src);
  EXPECT_NE(dst.getId(), src.getId());

  // Self-assignment should leave the object valid and intact.
  H5::PropList* self_ptr = &dst;
  dst = *self_ptr;
  EXPECT_TRUE(dst == src);

  H5::PropList assigned(H5P_FILE_CREATE);
  assigned = src;
  EXPECT_TRUE(assigned == src);

  ScopedQuietHdf5Errors quiet;
  src.close();
  EXPECT_THROW(dst.copy(src), H5::PropListIException);
  EXPECT_EQ(dst.getClassName(), "");
  EXPECT_THROW(dst.getNumProps(), H5::PropListIException);
  EXPECT_THROW(dst.getClassParent(), H5::PropListIException);
}

TEST(H5PropListTest, SetIdCloseAndDestructorCatchInvalidType) {
  ScopedQuietHdf5Errors quiet;
  H5::DataSpace space(H5S_SCALAR);

  {
    PublicSetId<H5::PropList> holder;
    holder.CallSetId(space.getId());
    // close() throws before resetting id, leaving space.getId() in holder.
    EXPECT_THROW(holder.close(), H5::PropListIException);
    EXPECT_THROW(holder.CallSetId(H5I_INVALID_HID), H5::PropListIException);

    // Keep the non-proplist ID in holder to cover copy() failure and
    // ~PropList() catching PropListIException cleanly on scope exit.
    H5::PropList other(H5P_FILE_CREATE);
    EXPECT_THROW(other.copy(holder), H5::PropListIException);
  }
}

TEST(H5PropListTest, CustomClassPropertiesOverloadsAndErrors) {
  ScopedQuietHdf5Errors quiet;

  const hid_t custom_cls_id =
      H5Pcreate_class(H5P_DATASET_CREATE, "custom_cls", nullptr, nullptr,
                      nullptr, nullptr, nullptr, nullptr);
  ASSERT_GE(custom_cls_id, 0);

  PublicSetId<H5::PropList> custom_cls;
  custom_cls.CallSetId(custom_cls_id);
  absl::Cleanup cleanup_cls = [&]() {
    custom_cls.ResetIdRaw();
    if (H5Iis_valid(custom_cls_id) > 0) {
      H5Pclose_class(custom_cls_id);
    }
  };

  int32_t init_int = 7;
  ASSERT_GE(H5Pregister2(custom_cls_id, "int_prop", sizeof(init_int), &init_int,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr),
            0);
  std::array<char, 16> init_str = {'h', 'e', 'l', 'l', 'o'};
  ASSERT_GE(H5Pregister2(custom_cls_id, "str_prop", init_str.size(),
                         init_str.data(), nullptr, nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr),
            0);

  EXPECT_EQ(custom_cls.getClassName(), "custom_cls");
  H5::PropList parent = custom_cls.getClassParent();
  PublicSetId<H5::PropList> dset_create_cls;
  dset_create_cls.CallSetId(H5P_DATASET_CREATE);
  absl::Cleanup reset_dset_cls = [&]() { dset_create_cls.ResetIdRaw(); };
  EXPECT_TRUE(parent.isAClass(dset_create_cls));
  EXPECT_GT(custom_cls.getNumProps(), 0u);

  H5::PropList plist(custom_cls_id);
  EXPECT_TRUE(plist.isAClass(custom_cls));
  PublicSetId<H5::PropList> file_create_cls;
  file_create_cls.CallSetId(H5P_FILE_CREATE);
  absl::Cleanup reset_file_cls = [&]() { file_create_cls.ResetIdRaw(); };
  EXPECT_FALSE(plist.isAClass(file_create_cls));

  const hid_t plist_cls_id = plist.getClass();
  EXPECT_GE(plist_cls_id, 0);
  EXPECT_GE(H5Pclose_class(plist_cls_id), 0);

  // H5PropList::getClass() only checks `plist_class == H5P_ROOT`, whereas
  // H5Pget_class returns H5I_INVALID_HID on H5P_DEFAULT and allocates a fresh
  // hid_t on valid lists.
  EXPECT_EQ(H5::PropList::DEFAULT.getClass(), H5I_INVALID_HID);
  EXPECT_THROW(plist.isAClass(H5::PropList()), H5::PropListIException);

  EXPECT_TRUE(plist.propExist("int_prop"));
  EXPECT_TRUE(plist.propExist(H5std_string("str_prop")));
  EXPECT_FALSE(plist.propExist("missing_prop"));
  EXPECT_THROW(H5::PropList().propExist("int_prop"), H5::PropListIException);

  EXPECT_EQ(plist.getPropSize("int_prop"), sizeof(int32_t));
  EXPECT_EQ(plist.getPropSize(H5std_string("str_prop")), init_str.size());
  EXPECT_THROW(plist.getPropSize("missing_prop"), H5::PropListIException);

  // Exercise all const/non-const void* and H5std_string setProperty overloads.
  int32_t v1 = 42;
  plist.setProperty("int_prop", static_cast<const void*>(&v1));
  int32_t out = 0;
  plist.getProperty("int_prop", &out);
  EXPECT_EQ(out, 42);

  int32_t v2 = 99;
  plist.setProperty("int_prop", static_cast<void*>(&v2));
  plist.getProperty(H5std_string("int_prop"), &out);
  EXPECT_EQ(out, 99);

  int32_t v3 = 123;
  plist.setProperty(H5std_string("int_prop"), static_cast<const void*>(&v3));
  plist.getProperty("int_prop", &out);
  EXPECT_EQ(out, 123);

  int32_t v4 = 456;
  plist.setProperty(H5std_string("int_prop"), static_cast<void*>(&v4));
  plist.getProperty(H5std_string("int_prop"), &out);
  EXPECT_EQ(out, 456);

  constexpr auto kStrBuf = std::to_array("alpha_test_1234");
  static_assert(kStrBuf.size() == 16);
  plist.setProperty("str_prop", kStrBuf.data());
  EXPECT_EQ(plist.getProperty("str_prop"), "alpha_test_1234");

  H5std_string s_const("beta");
  plist.setProperty("str_prop", static_cast<const H5std_string&>(s_const));
  EXPECT_EQ(plist.getProperty(H5std_string("str_prop")), "beta");

  H5std_string s_nonconst("gamma");
  plist.setProperty("str_prop", s_nonconst);
  EXPECT_EQ(plist.getProperty("str_prop"), "gamma");

  H5std_string s_key("str_prop");
  plist.setProperty(s_key, static_cast<const H5std_string&>(s_const));
  EXPECT_EQ(plist.getProperty(s_key), "beta");

  H5std_string s_val("delta");
  plist.setProperty(s_key, s_val);
  EXPECT_EQ(plist.getProperty(s_key), "delta");

  // Copy/compare/remove property paths using dynamic list-only properties so
  // H5Pcopy_prop takes the insert path without hitting H5P__del_pclass_cb.
  H5::PropList src_dyn(H5P_DATASET_CREATE);
  H5::PropList dst_dyn(H5P_DATASET_CREATE);
  int32_t dyn_int = 314;
  auto dyn_str = std::to_array("dynval\0");
  static_assert(dyn_str.size() == 8);
  ASSERT_GE(H5Pinsert2(src_dyn.getId(), "dyn_int", sizeof(dyn_int), &dyn_int,
                       nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
            0);
  ASSERT_GE(
      H5Pinsert2(src_dyn.getId(), "dyn_str", dyn_str.size(), dyn_str.data(),
                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
      0);

  EXPECT_FALSE(src_dyn == dst_dyn);
  src_dyn.copyProp(dst_dyn, "dyn_int");
  src_dyn.copyProp(dst_dyn, H5std_string("dyn_str"));
  EXPECT_TRUE(src_dyn == dst_dyn);

  H5::PropList dst_dyn3(H5P_DATASET_CREATE);
  dst_dyn3.copyProp(dst_dyn3, src_dyn, "dyn_int");
  dst_dyn3.copyProp(dst_dyn3, src_dyn, H5std_string("dyn_str"));
  EXPECT_TRUE(src_dyn == dst_dyn3);

  dst_dyn.removeProp("dyn_int");
  dst_dyn.removeProp(H5std_string("dyn_str"));
  EXPECT_FALSE(dst_dyn.propExist("dyn_int"));
  EXPECT_THROW(dst_dyn.removeProp("missing_prop"), H5::PropListIException);
  EXPECT_THROW(src_dyn.copyProp(dst_dyn, "missing_prop"),
               H5::PropListIException);
  EXPECT_THROW(src_dyn.copyProp(dst_dyn, src_dyn, "missing_prop"),
               H5::PropListIException);

  EXPECT_THROW(plist.getProperty("missing_prop", &out), H5::PropListIException);
  EXPECT_THROW(plist.setProperty("missing_prop", &v1), H5::PropListIException);
  EXPECT_THROW(plist.setProperty("missing_prop", static_cast<void*>(&v1)),
               H5::PropListIException);
  EXPECT_THROW(plist.setProperty("missing_prop", "val"),
               H5::PropListIException);
  EXPECT_THROW(H5::PropList().operator==(plist), H5::PropListIException);

  custom_cls.closeClass();
  EXPECT_THROW(plist.closeClass(), H5::PropListIException);
}

TEST(H5PropListTest, GetConstantDuplicateCallThrows) {
  ExpectDuplicateGetConstantThrows<H5::PropList>();
}

}  // namespace
}  // namespace hdf5
