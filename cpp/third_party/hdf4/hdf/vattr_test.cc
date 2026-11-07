// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/hproto.h"
#include "third_party/hdf4/hdf/src/vg.h"

namespace hdf4 {
namespace {

using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::Gt;
using ::testing::Ne;
using ::testing::StrEq;

constexpr int32_t kFakeVsid = (4 << 28) | 0x1234;
constexpr int32_t kFakeVgid = (3 << 28) | 0x1234;

std::string GetTempFilePath(std::string_view filename) {
  return (std::filesystem::path(::testing::TempDir()) / filename).string();
}

TEST(VattrTest, VdataFieldAndVdataAttributes) {
  const std::string path = GetTempFilePath("vattr_vs_basic.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  // 1. Create file and write a vdata with fields and attributes
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    const int32_t vsid = VSattach(fid, -1, "w");
    ASSERT_THAT(vsid, Ne(FAIL));
    absl::Cleanup detach_vs = [vsid] {
      EXPECT_THAT(VSdetach(vsid), Eq(SUCCEED));
    };

    EXPECT_THAT(VSsetname(vsid, "TestVdata"), Eq(SUCCEED));
    EXPECT_THAT(VSsetclass(vsid, "TestClass"), Eq(SUCCEED));

    // Define two fields: FieldX (INT32), FieldY (FLOAT32 order 2)
    EXPECT_THAT(VSfdefine(vsid, "FieldX", DFNT_INT32, 1), Eq(SUCCEED));
    EXPECT_THAT(VSfdefine(vsid, "FieldY", DFNT_FLOAT32, 2), Eq(SUCCEED));
    EXPECT_THAT(VSsetfields(vsid, "FieldX,FieldY"), Eq(SUCCEED));

    // Test VSfindex
    int32_t fidx_x = -1, fidx_y = -1, fidx_bad = -1;
    EXPECT_THAT(VSfindex(vsid, "FieldX", &fidx_x), Eq(SUCCEED));
    EXPECT_THAT(fidx_x, Eq(0));
    EXPECT_THAT(VSfindex(vsid, "FieldY", &fidx_y), Eq(SUCCEED));
    EXPECT_THAT(fidx_y, Eq(1));
    EXPECT_THAT(VSfindex(vsid, "NonExistent", &fidx_bad), Eq(FAIL));

    // Initially 0 attributes
    EXPECT_THAT(VSnattrs(vsid), Eq(0));
    EXPECT_THAT(VSfnattrs(vsid, _HDF_VDATA), Eq(0));
    EXPECT_THAT(VSfnattrs(vsid, 0), Eq(0));
    EXPECT_THAT(VSfnattrs(vsid, 1), Eq(0));

    // Set attribute on entire vdata (_HDF_VDATA)
    const int32_t vdata_attr_val = 1001;
    EXPECT_THAT(VSsetattr(vsid, _HDF_VDATA, "VdataAttr1", DFNT_INT32, 1,
                          &vdata_attr_val),
                Eq(SUCCEED));

    // Set second attribute on entire vdata
    const char vdata_str_attr[] = "VdataComment";
    EXPECT_THAT(VSsetattr(vsid, _HDF_VDATA, "Comment", DFNT_CHAR,
                          sizeof(vdata_str_attr), vdata_str_attr),
                Eq(SUCCEED));

    // Set attribute on FieldX (index 0)
    const float fieldx_cal[2] = {1.5f, 2.5f};
    EXPECT_THAT(VSsetattr(vsid, 0, "CalibX", DFNT_FLOAT32, 2, fieldx_cal),
                Eq(SUCCEED));

    // Set attribute on FieldY (index 1)
    const int16_t fieldy_tag = 42;
    EXPECT_THAT(VSsetattr(vsid, 1, "TagY", DFNT_INT16, 1, &fieldy_tag),
                Eq(SUCCEED));

    // Verify attribute counts: total 4, vdata has 2, field 0 has 1, field 1 has
    // 1
    EXPECT_THAT(VSnattrs(vsid), Eq(4));
    EXPECT_THAT(VSfnattrs(vsid, _HDF_VDATA), Eq(2));
    EXPECT_THAT(VSfnattrs(vsid, 0), Eq(1));
    EXPECT_THAT(VSfnattrs(vsid, 1), Eq(1));

    // Overwrite existing attribute with SAME type and order -> succeeds
    const int32_t new_vdata_attr_val = 2002;
    EXPECT_THAT(VSsetattr(vsid, _HDF_VDATA, "VdataAttr1", DFNT_INT32, 1,
                          &new_vdata_attr_val),
                Eq(SUCCEED));

    // Overwrite existing attribute on field 0 with SAME type and order ->
    // succeeds
    const float new_fieldx_cal[2] = {3.5f, 4.5f};
    EXPECT_THAT(VSsetattr(vsid, 0, "CalibX", DFNT_FLOAT32, 2, new_fieldx_cal),
                Eq(SUCCEED));

    // Overwrite with DIFFERENT type -> fails
    const int16_t wrong_type_val = 99;
    EXPECT_THAT(VSsetattr(vsid, _HDF_VDATA, "VdataAttr1", DFNT_INT16, 1,
                          &wrong_type_val),
                Eq(FAIL));

    // Overwrite with DIFFERENT count -> fails
    const int32_t wrong_count_vals[2] = {1, 2};
    EXPECT_THAT(VSsetattr(vsid, _HDF_VDATA, "VdataAttr1", DFNT_INT32, 2,
                          wrong_count_vals),
                Eq(FAIL));

    // Test VSfindattr
    EXPECT_THAT(VSfindattr(vsid, _HDF_VDATA, "VdataAttr1"), Eq(0));
    EXPECT_THAT(VSfindattr(vsid, _HDF_VDATA, "Comment"), Eq(1));
    EXPECT_THAT(VSfindattr(vsid, _HDF_VDATA, "NonExistent"), Eq(FAIL));
    EXPECT_THAT(VSfindattr(vsid, 0, "CalibX"), Eq(0));
    EXPECT_THAT(VSfindattr(vsid, 0, "NonExistent"), Eq(FAIL));
    EXPECT_THAT(VSfindattr(vsid, 1, "TagY"), Eq(0));

    // Test VSattrinfo on vdata attribute 0
    char name_buf[64] = {0};
    int32_t dt = 0, count = 0, sz = 0;
    EXPECT_THAT(VSattrinfo(vsid, _HDF_VDATA, 0, name_buf, &dt, &count, &sz),
                Eq(SUCCEED));
    EXPECT_THAT(std::string_view(name_buf), StrEq("VdataAttr1"));
    EXPECT_THAT(dt, Eq(DFNT_INT32));
    EXPECT_THAT(count, Eq(1));
    EXPECT_THAT(sz, Eq(sizeof(int32_t)));

    // Test VSattrinfo with nullptr arguments
    EXPECT_THAT(
        VSattrinfo(vsid, _HDF_VDATA, 0, nullptr, nullptr, nullptr, nullptr),
        Eq(SUCCEED));
    EXPECT_THAT(
        VSattrinfo(vsid, _HDF_VDATA, 0, name_buf, nullptr, nullptr, nullptr),
        Eq(SUCCEED));
    EXPECT_THAT(VSattrinfo(vsid, _HDF_VDATA, 0, nullptr, &dt, nullptr, nullptr),
                Eq(SUCCEED));
    EXPECT_THAT(
        VSattrinfo(vsid, _HDF_VDATA, 0, nullptr, nullptr, &count, nullptr),
        Eq(SUCCEED));
    EXPECT_THAT(VSattrinfo(vsid, _HDF_VDATA, 0, nullptr, nullptr, nullptr, &sz),
                Eq(SUCCEED));

    // Test VSattrinfo on field attribute
    EXPECT_THAT(VSattrinfo(vsid, 0, 0, name_buf, &dt, &count, &sz),
                Eq(SUCCEED));
    EXPECT_THAT(std::string_view(name_buf), StrEq("CalibX"));
    EXPECT_THAT(dt, Eq(DFNT_FLOAT32));
    EXPECT_THAT(count, Eq(2));
    EXPECT_THAT(sz, Eq(2 * sizeof(float)));

    // Test VSgetattr on vdata attribute
    int32_t read_val = 0;
    EXPECT_THAT(VSgetattr(vsid, _HDF_VDATA, 0, &read_val), Eq(SUCCEED));
    EXPECT_THAT(read_val, Eq(new_vdata_attr_val));

    // Test VSgetattr on field attribute
    float read_cal[2] = {0.0f, 0.0f};
    EXPECT_THAT(VSgetattr(vsid, 0, 0, read_cal), Eq(SUCCEED));
    EXPECT_THAT(read_cal[0], Eq(new_fieldx_cal[0]));
    EXPECT_THAT(read_cal[1], Eq(new_fieldx_cal[1]));

    // Test VSisattr on regular vdata (should return FALSE = 0)
    EXPECT_THAT(VSisattr(vsid), Eq(0));

    // Write one record to make the vdata valid on disk
    struct {
      int32_t x;
      float y[2];
    } rec = {10, {1.1f, 2.2f}};
    EXPECT_THAT(VSwrite(vsid, reinterpret_cast<const uint8_t*>(&rec), 1,
                        FULL_INTERLACE),
                Eq(1));
  }

  // 2. Re-open in read mode and verify persistence
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    int32_t vs_ref = -1;
    EXPECT_THAT(VSlone(fid, &vs_ref, 1), Gt(0));
    ASSERT_THAT(vs_ref, Gt(0));

    const int32_t vsid = VSattach(fid, vs_ref, "r");
    ASSERT_THAT(vsid, Ne(FAIL));

    // Setting attribute on read-only vdata fails
    int32_t dummy = 0;
    EXPECT_THAT(VSsetattr(vsid, _HDF_VDATA, "FailAttr", DFNT_INT32, 1, &dummy),
                Eq(FAIL));

    EXPECT_THAT(VSnattrs(vsid), Eq(4));
    EXPECT_THAT(VSfnattrs(vsid, _HDF_VDATA), Eq(2));
    EXPECT_THAT(VSfnattrs(vsid, 0), Eq(1));
    EXPECT_THAT(VSfnattrs(vsid, 1), Eq(1));

    int32_t val = 0;
    EXPECT_THAT(VSgetattr(vsid, _HDF_VDATA, 0, &val), Eq(SUCCEED));
    EXPECT_THAT(val, Eq(2002));

    float cal[2] = {0.0f, 0.0f};
    EXPECT_THAT(VSgetattr(vsid, 0, 0, cal), Eq(SUCCEED));
    EXPECT_THAT(cal[0], Eq(3.5f));
    EXPECT_THAT(cal[1], Eq(4.5f));

    EXPECT_THAT(VSdetach(vsid), Eq(SUCCEED));

    // Test VSisattr on an actual attribute vdata
    // Find the attribute vdata ref from VSfindattr / internal ref
    // We can query lone vdatas in the file: attribute vdatas have class
    // _HDF_ATTRIBUTE
    int32_t all_vs_refs[10] = {0};
    const int32_t n_vdatas = VSlone(fid, all_vs_refs, 10);
    EXPECT_THAT(n_vdatas, Gt(0));

    // Attach to the attribute vdata and verify VSisattr returns 1
    bool found_attr_vdata = false;
    for (int32_t i = 0; i < n_vdatas; ++i) {
      const int32_t test_vsid = VSattach(fid, all_vs_refs[i], "r");
      if (test_vsid != FAIL) {
        if (VSisattr(test_vsid) == 1) {
          found_attr_vdata = true;
        }
        EXPECT_THAT(VSdetach(test_vsid), Eq(SUCCEED));
      }
    }
    EXPECT_TRUE(found_attr_vdata);
  }
}

TEST(VattrTest, VdataAttributeErrorBranches) {
  const std::string path = GetTempFilePath("vattr_vs_errors.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
  absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

  const int32_t vsid = VSattach(fid, -1, "w");
  ASSERT_THAT(vsid, Ne(FAIL));
  absl::Cleanup detach_vs = [vsid] {
    EXPECT_THAT(VSdetach(vsid), Eq(SUCCEED));
  };

  EXPECT_THAT(VSfdefine(vsid, "FieldA", DFNT_INT32, 1), Eq(SUCCEED));
  EXPECT_THAT(VSsetfields(vsid, "FieldA"), Eq(SUCCEED));

  // Invalid IDs for VSfindex
  int32_t fidx = -1;
  EXPECT_THAT(VSfindex(FAIL, "FieldA", &fidx), Eq(FAIL));
  EXPECT_THAT(VSfindex(kFakeVsid, "FieldA", &fidx), Eq(FAIL));

  // Invalid arguments for VSsetattr
  int32_t dummy = 123;
  EXPECT_THAT(VSsetattr(FAIL, _HDF_VDATA, "Attr", DFNT_INT32, 1, &dummy),
              Eq(FAIL));
  EXPECT_THAT(VSsetattr(kFakeVsid, _HDF_VDATA, "Attr", DFNT_INT32, 1, &dummy),
              Eq(FAIL));
  EXPECT_THAT(VSsetattr(vsid, _HDF_VDATA, nullptr, DFNT_INT32, 1, &dummy),
              Eq(FAIL));
  EXPECT_THAT(VSsetattr(vsid, 999, "Attr", DFNT_INT32, 1, &dummy),
              Eq(FAIL));  // Bad findex
  EXPECT_THAT(VSsetattr(vsid, -5, "Attr", DFNT_INT32, 1, &dummy),
              Eq(FAIL));  // Bad findex

  // Invalid arguments for VSnattrs
  EXPECT_THAT(VSnattrs(FAIL), Eq(FAIL));
  EXPECT_THAT(VSnattrs(kFakeVsid), Eq(FAIL));

  // Invalid arguments for VSfnattrs
  EXPECT_THAT(VSfnattrs(FAIL, _HDF_VDATA), Eq(FAIL));
  EXPECT_THAT(VSfnattrs(kFakeVsid, _HDF_VDATA), Eq(FAIL));
  EXPECT_THAT(VSfnattrs(vsid, 999), Eq(FAIL));

  // When no attributes exist:
  EXPECT_THAT(VSfindattr(vsid, _HDF_VDATA, "Attr"), Eq(FAIL));
  char name[64];
  int32_t dt = 0, count = 0, sz = 0;
  EXPECT_THAT(VSattrinfo(vsid, _HDF_VDATA, 0, name, &dt, &count, &sz),
              Eq(FAIL));
  EXPECT_THAT(VSgetattr(vsid, _HDF_VDATA, 0, &dummy), Eq(FAIL));

  // Invalid arguments for VSfindattr
  EXPECT_THAT(VSfindattr(FAIL, _HDF_VDATA, "Attr"), Eq(FAIL));
  EXPECT_THAT(VSfindattr(kFakeVsid, _HDF_VDATA, "Attr"), Eq(FAIL));
  EXPECT_THAT(VSfindattr(vsid, _HDF_VDATA, nullptr), Eq(FAIL));
  EXPECT_THAT(VSfindattr(vsid, 999, "Attr"), Eq(FAIL));

  // Now set one attribute to test out of bounds indices
  EXPECT_THAT(VSsetattr(vsid, _HDF_VDATA, "Attr1", DFNT_INT32, 1, &dummy),
              Eq(SUCCEED));

  // VSattrinfo errors
  EXPECT_THAT(VSattrinfo(FAIL, _HDF_VDATA, 0, name, &dt, &count, &sz),
              Eq(FAIL));
  EXPECT_THAT(VSattrinfo(kFakeVsid, _HDF_VDATA, 0, name, &dt, &count, &sz),
              Eq(FAIL));
  EXPECT_THAT(VSattrinfo(vsid, 999, 0, name, &dt, &count, &sz), Eq(FAIL));
  EXPECT_THAT(VSattrinfo(vsid, _HDF_VDATA, -1, name, &dt, &count, &sz),
              Eq(FAIL));
  EXPECT_THAT(VSattrinfo(vsid, _HDF_VDATA, 5, name, &dt, &count, &sz),
              Eq(FAIL));
  EXPECT_THAT(VSattrinfo(vsid, 0, 0, name, &dt, &count, &sz),
              Eq(FAIL));  // field 0 has no attrs

  // VSgetattr errors
  EXPECT_THAT(VSgetattr(FAIL, _HDF_VDATA, 0, &dummy), Eq(FAIL));
  EXPECT_THAT(VSgetattr(kFakeVsid, _HDF_VDATA, 0, &dummy), Eq(FAIL));
  EXPECT_THAT(VSgetattr(vsid, 999, 0, &dummy), Eq(FAIL));
  EXPECT_THAT(VSgetattr(vsid, _HDF_VDATA, -1, &dummy), Eq(FAIL));
  EXPECT_THAT(VSgetattr(vsid, _HDF_VDATA, 5, &dummy), Eq(FAIL));
  EXPECT_THAT(VSgetattr(vsid, 0, 0, &dummy), Eq(FAIL));

  // VSisattr errors
  EXPECT_THAT(VSisattr(FAIL), Eq(0));
  EXPECT_THAT(VSisattr(kFakeVsid), Eq(0));

  // Set an attribute on FieldA (index 0)
  EXPECT_THAT(VSsetattr(vsid, 0, "FieldAttr1", DFNT_INT32, 1, &dummy),
              Eq(SUCCEED));
  // Attribute index 1 on field 0 does not exist
  EXPECT_THAT(VSattrinfo(vsid, 0, 1, name, &dt, &count, &sz), Eq(FAIL));
  EXPECT_THAT(VSgetattr(vsid, 0, 1, &dummy), Eq(FAIL));
}

TEST(VattrTest, VgroupAttributesAndVersions) {
  const std::string path = GetTempFilePath("vattr_vg_basic.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  // 1. Create file and write a vgroup with attributes
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    const int32_t vgid = Vattach(fid, -1, "w");
    ASSERT_THAT(vgid, Ne(FAIL));
    absl::Cleanup detach_vg = [vgid] {
      EXPECT_THAT(Vdetach(vgid), Eq(SUCCEED));
    };

    EXPECT_THAT(Vsetname(vgid, "TestVgroup"), Eq(SUCCEED));
    EXPECT_THAT(Vsetclass(vgid, "TestGroupClass"), Eq(SUCCEED));

    // Initially version is 3 (VSET_VERSION) and 0 attributes
    EXPECT_THAT(Vgetversion(vgid), Eq(VSET_VERSION));
    EXPECT_THAT(Vnattrs(vgid), Eq(0));

    // Set first attribute (int32)
    const int32_t val1 = 555;
    EXPECT_THAT(Vsetattr(vgid, "GroupAttr1", DFNT_INT32, 1, &val1),
                Eq(SUCCEED));

    // After setting attribute, version changes to 4 (VSET_NEW_VERSION)
    EXPECT_THAT(Vgetversion(vgid), Eq(VSET_NEW_VERSION));
    EXPECT_THAT(Vnattrs(vgid), Eq(1));

    // Set second attribute (string)
    const char val2[] = "GroupComment";
    EXPECT_THAT(Vsetattr(vgid, "Comment", DFNT_CHAR, sizeof(val2), val2),
                Eq(SUCCEED));
    EXPECT_THAT(Vnattrs(vgid), Eq(2));

    // Overwrite existing attribute with SAME type and order -> succeeds
    const int32_t new_val1 = 777;
    EXPECT_THAT(Vsetattr(vgid, "GroupAttr1", DFNT_INT32, 1, &new_val1),
                Eq(SUCCEED));

    // Overwrite with DIFFERENT type -> fails
    const int16_t wrong_type = 99;
    EXPECT_THAT(Vsetattr(vgid, "GroupAttr1", DFNT_INT16, 1, &wrong_type),
                Eq(FAIL));

    // Overwrite with DIFFERENT order/count -> fails
    const int32_t wrong_order[2] = {1, 2};
    EXPECT_THAT(Vsetattr(vgid, "GroupAttr1", DFNT_INT32, 2, wrong_order),
                Eq(FAIL));

    // Test Vfindattr
    EXPECT_THAT(Vfindattr(vgid, "GroupAttr1"), Eq(0));
    EXPECT_THAT(Vfindattr(vgid, "Comment"), Eq(1));
    EXPECT_THAT(Vfindattr(vgid, "NonExistent"), Eq(FAIL));

    // Test Vattrinfo
    char name[64] = {0};
    int32_t dt = 0, count = 0, sz = 0;
    EXPECT_THAT(Vattrinfo(vgid, 0, name, &dt, &count, &sz), Eq(SUCCEED));
    EXPECT_THAT(std::string_view(name), StrEq("GroupAttr1"));
    EXPECT_THAT(dt, Eq(DFNT_INT32));
    EXPECT_THAT(count, Eq(1));
    EXPECT_THAT(sz, Eq(sizeof(int32_t)));

    // Test Vattrinfo with nullptr arguments
    EXPECT_THAT(Vattrinfo(vgid, 0, nullptr, nullptr, nullptr, nullptr),
                Eq(SUCCEED));
    EXPECT_THAT(Vattrinfo(vgid, 0, name, nullptr, nullptr, nullptr),
                Eq(SUCCEED));
    EXPECT_THAT(Vattrinfo(vgid, 0, nullptr, &dt, nullptr, nullptr),
                Eq(SUCCEED));
    EXPECT_THAT(Vattrinfo(vgid, 0, nullptr, nullptr, &count, nullptr),
                Eq(SUCCEED));
    EXPECT_THAT(Vattrinfo(vgid, 0, nullptr, nullptr, nullptr, &sz),
                Eq(SUCCEED));

    // Test Vgetattr
    int32_t read_val = 0;
    EXPECT_THAT(Vgetattr(vgid, 0, &read_val), Eq(SUCCEED));
    EXPECT_THAT(read_val, Eq(new_val1));

    char read_comment[64] = {0};
    EXPECT_THAT(Vgetattr(vgid, 1, read_comment), Eq(SUCCEED));
    EXPECT_THAT(std::string_view(read_comment), StrEq(val2));

    // Test Vnoldattrs, Vnattrs2, Vattrinfo2, Vgetattr2 on Vgroup with 0 old
    // attrs
    EXPECT_THAT(Vnoldattrs(vgid), Eq(0));
    EXPECT_THAT(Vnattrs2(vgid), Eq(2));
    EXPECT_THAT(Vattrinfo2(vgid, 0, name, &dt, &count, &sz, nullptr, nullptr),
                Eq(SUCCEED));
    int32_t read_val2 = 0;
    EXPECT_THAT(Vgetattr2(vgid, 0, &read_val2), Eq(SUCCEED));
    EXPECT_THAT(read_val2, Eq(new_val1));
    char read_comment2[64] = {0};
    EXPECT_THAT(Vgetattr2(vgid, 1, read_comment2), Eq(SUCCEED));
    EXPECT_THAT(std::string_view(read_comment2), StrEq(val2));
  }

  // 2. Re-open in read mode and verify
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    int32_t vg_ref = -1;
    EXPECT_THAT(Vlone(fid, &vg_ref, 1), Gt(0));
    ASSERT_THAT(vg_ref, Gt(0));

    const int32_t vgid = Vattach(fid, vg_ref, "r");
    ASSERT_THAT(vgid, Ne(FAIL));
    absl::Cleanup detach_vg = [vgid] {
      EXPECT_THAT(Vdetach(vgid), Eq(SUCCEED));
    };

    // Cannot set attribute in read mode
    int32_t dummy = 0;
    EXPECT_THAT(Vsetattr(vgid, "AttrFail", DFNT_INT32, 1, &dummy), Eq(FAIL));

    EXPECT_THAT(Vgetversion(vgid), Eq(VSET_NEW_VERSION));
    EXPECT_THAT(Vnattrs(vgid), Eq(2));

    int32_t read_val = 0;
    EXPECT_THAT(Vgetattr(vgid, 0, &read_val), Eq(SUCCEED));
    EXPECT_THAT(read_val, Eq(777));
  }
}

TEST(VattrTest, VgroupAttributeErrorBranches) {
  const std::string path = GetTempFilePath("vattr_vg_errors.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
  absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

  const int32_t vgid = Vattach(fid, -1, "w");
  ASSERT_THAT(vgid, Ne(FAIL));
  absl::Cleanup detach_vg = [vgid] { EXPECT_THAT(Vdetach(vgid), Eq(SUCCEED)); };

  // Vsetattr error checks
  int32_t dummy = 123;
  EXPECT_THAT(Vsetattr(FAIL, "Attr", DFNT_INT32, 1, &dummy), Eq(FAIL));
  EXPECT_THAT(Vsetattr(kFakeVgid, "Attr", DFNT_INT32, 1, &dummy), Eq(FAIL));
  EXPECT_THAT(Vsetattr(vgid, nullptr, DFNT_INT32, 1, &dummy), Eq(FAIL));

  // Vgetversion errors
  EXPECT_THAT(Vgetversion(FAIL), Eq(FAIL));
  EXPECT_THAT(Vgetversion(kFakeVgid), Eq(FAIL));

  // Vnattrs errors
  EXPECT_THAT(Vnattrs(FAIL), Eq(FAIL));
  EXPECT_THAT(Vnattrs(kFakeVgid), Eq(FAIL));

  // Vfindattr when nattrs == 0
  EXPECT_THAT(Vfindattr(vgid, "Attr"), Eq(FAIL));
  EXPECT_THAT(Vfindattr(FAIL, "Attr"), Eq(FAIL));
  EXPECT_THAT(Vfindattr(kFakeVgid, "Attr"), Eq(FAIL));
  EXPECT_THAT(Vfindattr(vgid, nullptr), Eq(FAIL));

  // Vattrinfo when nattrs == 0
  char name[64];
  int32_t dt = 0, count = 0, sz = 0;
  EXPECT_THAT(Vattrinfo(vgid, 0, name, &dt, &count, &sz), Eq(FAIL));
  EXPECT_THAT(Vattrinfo(FAIL, 0, name, &dt, &count, &sz), Eq(FAIL));
  EXPECT_THAT(Vattrinfo(kFakeVgid, 0, name, &dt, &count, &sz), Eq(FAIL));

  // Vgetattr when nattrs == 0
  EXPECT_THAT(Vgetattr(vgid, 0, &dummy), Eq(FAIL));
  EXPECT_THAT(Vgetattr(FAIL, 0, &dummy), Eq(FAIL));
  EXPECT_THAT(Vgetattr(kFakeVgid, 0, &dummy), Eq(FAIL));

  // Vnoldattrs, Vnattrs2, Vattrinfo2, Vgetattr2 when nattrs == 0
  EXPECT_THAT(Vnoldattrs(vgid), Eq(0));
  EXPECT_THAT(Vnattrs2(vgid), Eq(0));
  EXPECT_THAT(Vattrinfo2(vgid, 0, name, &dt, &count, &sz, nullptr, nullptr),
              Eq(FAIL));
  EXPECT_THAT(Vgetattr2(vgid, 0, &dummy), Eq(FAIL));

  // Set one attribute to test out of bounds indices
  EXPECT_THAT(Vsetattr(vgid, "Attr1", DFNT_INT32, 1, &dummy), Eq(SUCCEED));

  // Note: Upstream HDF4 bug in Vattrinfo and Vgetattr: they check
  // `if (vg->nattrs <= attrindex)` but omit `attrindex < 0`, so passing
  // negative index results in heap-buffer-overflow reading alist[negative].
  EXPECT_THAT(Vattrinfo(vgid, 5, name, &dt, &count, &sz), Eq(FAIL));
  EXPECT_THAT(Vgetattr(vgid, 5, &dummy), Eq(FAIL));

  // Overwriting with wrong type/count fails
  int16_t bad_type = 99;
  EXPECT_THAT(Vsetattr(vgid, "Attr1", DFNT_INT16, 1, &bad_type), Eq(FAIL));
  int32_t bad_count[2] = {1, 2};
  EXPECT_THAT(Vsetattr(vgid, "Attr1", DFNT_INT32, 2, bad_count), Eq(FAIL));

  // Add a second attribute so Vfindattr has to loop
  EXPECT_THAT(Vsetattr(vgid, "Attr2", DFNT_INT32, 1, &dummy), Eq(SUCCEED));
  EXPECT_THAT(Vfindattr(vgid, "Attr2"), Eq(1));
  EXPECT_THAT(Vfindattr(vgid, "NonExistent"), Eq(FAIL));
}

TEST(VattrTest, OldStyleAttributesAndVattrinfo2) {
  const std::string path = GetTempFilePath("vattr_old_style.hdf");
  absl::Cleanup remove_temp = [&path] { std::filesystem::remove(path); };

  // 1. Create file with both old-style attributes (simulated via VHstoredatam +
  // Vaddtagref) and new-style attributes (created via Vsetattr).
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    ASSERT_THAT(Vstart(fid), Eq(SUCCEED));
    absl::Cleanup end_v = [fid] { EXPECT_THAT(Vend(fid), Eq(SUCCEED)); };

    const int32_t vgid = Vattach(fid, -1, "w");
    ASSERT_THAT(vgid, Ne(FAIL));
    absl::Cleanup detach_vg = [vgid] {
      EXPECT_THAT(Vdetach(vgid), Eq(SUCCEED));
    };

    EXPECT_THAT(Vsetname(vgid, "OldStyleVg"), Eq(SUCCEED));

    // Initially 0 old and 0 new attrs
    EXPECT_THAT(Vnoldattrs(vgid), Eq(0));
    EXPECT_THAT(Vnattrs(vgid), Eq(0));
    EXPECT_THAT(Vnattrs2(vgid), Eq(0));

    // Add old-style attribute 1: int32 value
    const int32_t old_val1 = 1234;
    const int32_t vs1_ref = VHstoredatam(
        fid, ATTR_FIELD_NAME, reinterpret_cast<const uint8_t*>(&old_val1), 1,
        DFNT_INT32, "OldAttr1", _HDF_ATTRIBUTE, 1);
    ASSERT_THAT(vs1_ref, Gt(0));
    EXPECT_THAT(Vaddtagref(vgid, DFTAG_VH, vs1_ref), Gt(0));

    // Add old-style attribute 2: float array
    const float old_val2[3] = {1.1f, 2.2f, 3.3f};
    const int32_t vs2_ref = VHstoredatam(
        fid, ATTR_FIELD_NAME, reinterpret_cast<const uint8_t*>(old_val2), 1,
        DFNT_FLOAT32, "OldAttr2", _HDF_ATTRIBUTE, 3);
    ASSERT_THAT(vs2_ref, Gt(0));
    EXPECT_THAT(Vaddtagref(vgid, DFTAG_VH, vs2_ref), Gt(0));

    // Add new-style attributes using Vsetattr (before Vnoldattrs, to avoid
    // orphaning vg->old_alist)
    const int32_t new_val1 = 9999;
    EXPECT_THAT(Vsetattr(vgid, "NewAttr1", DFNT_INT32, 1, &new_val1),
                Eq(SUCCEED));

    const char new_str[] = "NewStyleValue";
    EXPECT_THAT(Vsetattr(vgid, "NewAttr2", DFNT_CHAR, sizeof(new_str), new_str),
                Eq(SUCCEED));

    // Now query Vnoldattrs: should find 2 old attributes
    EXPECT_THAT(Vnoldattrs(vgid), Eq(2));

    // Calling Vnoldattrs again tests the branch where the list is current
    // (lines 1029-1032)
    EXPECT_THAT(Vnoldattrs(vgid), Eq(2));

    // Add a 3rd old-style attribute to test the branch where the list is
    // outdated (lines 1035-1046)
    const int16_t old_val3 = 42;
    const int32_t vs3_ref = VHstoredatam(
        fid, ATTR_FIELD_NAME, reinterpret_cast<const uint8_t*>(&old_val3), 1,
        DFNT_INT16, "OldAttr3", _HDF_ATTRIBUTE, 1);
    ASSERT_THAT(vs3_ref, Gt(0));
    EXPECT_THAT(Vaddtagref(vgid, DFTAG_VH, vs3_ref), Gt(0));
    EXPECT_THAT(Vnoldattrs(vgid), Eq(3));

    // Verify counts:
    // Vnoldattrs = 3
    // Vnattrs = 2
    // Vnattrs2 = 3 + 2 = 5
    EXPECT_THAT(Vnoldattrs(vgid), Eq(3));
    EXPECT_THAT(Vnattrs(vgid), Eq(2));
    EXPECT_THAT(Vnattrs2(vgid), Eq(5));

    // Test Vattrinfo2 across all 5 attributes:
    // Old-style attributes come first (indices 0, 1, 2)
    // New-style attributes follow (indices 3, 4)
    char name[64] = {0};
    int32_t dt = 0, count = 0, sz = 0, nflds = 0;
    uint16_t refnum = 0;

    // Index 0: OldAttr1
    EXPECT_THAT(Vattrinfo2(vgid, 0, name, &dt, &count, &sz, &nflds, &refnum),
                Eq(SUCCEED));
    EXPECT_THAT(dt, Eq(DFNT_INT32));
    EXPECT_THAT(count, Eq(1));
    EXPECT_THAT(nflds, Eq(1));
    EXPECT_THAT(refnum, Gt(0));

    // Index 1: OldAttr2
    EXPECT_THAT(Vattrinfo2(vgid, 1, name, &dt, &count, &sz, &nflds, &refnum),
                Eq(SUCCEED));
    EXPECT_THAT(dt, Eq(DFNT_FLOAT32));
    EXPECT_THAT(count, Eq(3));

    // Index 2: OldAttr3
    EXPECT_THAT(Vattrinfo2(vgid, 2, name, &dt, &count, &sz, &nflds, &refnum),
                Eq(SUCCEED));
    EXPECT_THAT(dt, Eq(DFNT_INT16));
    EXPECT_THAT(count, Eq(1));

    // Index 3: NewAttr1
    EXPECT_THAT(Vattrinfo2(vgid, 3, name, &dt, &count, &sz, &nflds, &refnum),
                Eq(SUCCEED));
    EXPECT_THAT(std::string_view(name), StrEq("NewAttr1"));
    EXPECT_THAT(dt, Eq(DFNT_INT32));
    EXPECT_THAT(count, Eq(1));

    // Index 4: NewAttr2
    EXPECT_THAT(Vattrinfo2(vgid, 4, name, &dt, &count, &sz, &nflds, &refnum),
                Eq(SUCCEED));
    EXPECT_THAT(std::string_view(name), StrEq("NewAttr2"));
    EXPECT_THAT(dt, Eq(DFNT_CHAR));
    EXPECT_THAT(count, Eq(sizeof(new_str)));

    // Vattrinfo2 with nullptr args
    EXPECT_THAT(Vattrinfo2(vgid, 0, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr),
                Eq(SUCCEED));
    EXPECT_THAT(
        Vattrinfo2(vgid, 0, name, nullptr, nullptr, nullptr, nullptr, nullptr),
        Eq(SUCCEED));
    EXPECT_THAT(
        Vattrinfo2(vgid, 0, nullptr, &dt, nullptr, nullptr, nullptr, nullptr),
        Eq(SUCCEED));
    EXPECT_THAT(Vattrinfo2(vgid, 0, nullptr, nullptr, &count, nullptr, nullptr,
                           nullptr),
                Eq(SUCCEED));
    EXPECT_THAT(
        Vattrinfo2(vgid, 0, nullptr, nullptr, nullptr, &sz, nullptr, nullptr),
        Eq(SUCCEED));
    EXPECT_THAT(Vattrinfo2(vgid, 0, nullptr, nullptr, nullptr, nullptr, &nflds,
                           nullptr),
                Eq(SUCCEED));
    EXPECT_THAT(Vattrinfo2(vgid, 0, nullptr, nullptr, nullptr, nullptr, nullptr,
                           &refnum),
                Eq(SUCCEED));

    // Test Vgetattr2 across all 5 attributes:
    int32_t read_old1 = 0;
    EXPECT_THAT(Vgetattr2(vgid, 0, &read_old1), Eq(SUCCEED));
    EXPECT_THAT(read_old1, Eq(old_val1));

    float read_old2[3] = {0.0f, 0.0f, 0.0f};
    EXPECT_THAT(Vgetattr2(vgid, 1, read_old2), Eq(SUCCEED));
    EXPECT_THAT(read_old2[0], Eq(old_val2[0]));
    EXPECT_THAT(read_old2[1], Eq(old_val2[1]));
    EXPECT_THAT(read_old2[2], Eq(old_val2[2]));

    int16_t read_old3 = 0;
    EXPECT_THAT(Vgetattr2(vgid, 2, &read_old3), Eq(SUCCEED));
    EXPECT_THAT(read_old3, Eq(old_val3));

    int32_t read_new1 = 0;
    EXPECT_THAT(Vgetattr2(vgid, 3, &read_new1), Eq(SUCCEED));
    EXPECT_THAT(read_new1, Eq(new_val1));

    char read_new2[64] = {0};
    EXPECT_THAT(Vgetattr2(vgid, 4, read_new2), Eq(SUCCEED));
    EXPECT_THAT(std::string_view(read_new2), StrEq(new_str));

    // Error cases for Vnoldattrs, Vnattrs2, Vattrinfo2, Vgetattr2
    EXPECT_THAT(Vnoldattrs(FAIL), Eq(FAIL));
    EXPECT_THAT(Vnoldattrs(kFakeVgid), Eq(0));

    EXPECT_THAT(Vnattrs2(FAIL), Eq(FAIL));
    EXPECT_THAT(Vnattrs2(kFakeVgid), Eq(FAIL));

    EXPECT_THAT(Vattrinfo2(FAIL, 0, name, &dt, &count, &sz, &nflds, &refnum),
                Eq(FAIL));
    EXPECT_THAT(
        Vattrinfo2(kFakeVgid, 0, name, &dt, &count, &sz, &nflds, &refnum),
        Eq(FAIL));
    EXPECT_THAT(Vattrinfo2(vgid, -1, name, &dt, &count, &sz, &nflds, &refnum),
                Eq(FAIL));
    EXPECT_THAT(Vattrinfo2(vgid, 10, name, &dt, &count, &sz, &nflds, &refnum),
                Eq(FAIL));

    EXPECT_THAT(Vgetattr2(FAIL, 0, &read_old1), Eq(FAIL));
    EXPECT_THAT(Vgetattr2(kFakeVgid, 0, &read_old1), Eq(FAIL));
    EXPECT_THAT(Vgetattr2(vgid, -1, &read_old1), Eq(FAIL));
    EXPECT_THAT(Vgetattr2(vgid, 10, &read_old1), Eq(FAIL));
  }
}

}  // namespace
}  // namespace hdf4
