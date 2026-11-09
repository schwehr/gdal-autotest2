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
// Tests deprecated HDF5 1.6 attribute APIs (H5Acreate1, H5Aopen_name,
// H5Aopen_idx, H5Aget_num_attrs, H5Aiterate1) in
// third_party/hdf5/src/H5Adeprec.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Adeprec.c

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

std::string ReadAttrName(hid_t attr_id) {
  const ssize_t len = H5Aget_name(attr_id, 0, nullptr);
  if (len < 0) return "";
  std::string name(static_cast<size_t>(len), '\0');
  if (H5Aget_name(attr_id, name.size() + 1, name.data()) < 0) {
    return "";
  }
  return name;
}

TEST(H5AdeprecTest, CreateOpenAndReadAcrossObjectTypesAndAcpl) {
  // 1. Create an in-memory file and attach attributes via H5Acreate1 on the
  //    root group (via file ID), a subgroup, a dataset, and a committed type.
  ScopedFile file = CreateInMemoryFile("deprec_objects.h5");
  ASSERT_TRUE(file.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  ScopedGroup grp(
      H5Gcreate2(file, "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedDataset dset(H5Dcreate2(file, "dset", H5T_NATIVE_INT32, scalar,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  ScopedType committed(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(committed.valid());
  ASSERT_GE(H5Tcommit2(file, "ctype", committed, H5P_DEFAULT, H5P_DEFAULT,
                       H5P_DEFAULT),
            0);

  // Use a non-default ACPL (UTF-8 character encoding) to exercise the
  // non-H5P_DEFAULT acpl_id branch in H5Acreate1.
  ScopedPlist acpl(H5Pcreate(H5P_ATTRIBUTE_CREATE));
  ASSERT_TRUE(acpl.valid());
  ASSERT_GE(H5Pset_char_encoding(acpl, H5T_CSET_UTF8), 0);

  constexpr int32_t kRootVal = 11;
  constexpr int32_t kGrpVal = 22;
  constexpr int32_t kDsetVal = 33;
  constexpr int32_t kTypeVal = 44;

  {
    ScopedAttr a_file(
        H5Acreate1(file, "file_attr", H5T_NATIVE_INT32, scalar, H5P_DEFAULT));
    ASSERT_TRUE(a_file.valid());
    ASSERT_GE(H5Awrite(a_file, H5T_NATIVE_INT32, &kRootVal), 0);

    ScopedAttr a_grp(
        H5Acreate1(grp, "grp_attr_utf8", H5T_NATIVE_INT32, scalar, acpl));
    ASSERT_TRUE(a_grp.valid());
    ASSERT_GE(H5Awrite(a_grp, H5T_NATIVE_INT32, &kGrpVal), 0);

    ScopedAttr a_dset(
        H5Acreate1(dset, "dset_attr", H5T_NATIVE_INT32, scalar, H5P_DEFAULT));
    ASSERT_TRUE(a_dset.valid());
    ASSERT_GE(H5Awrite(a_dset, H5T_NATIVE_INT32, &kDsetVal), 0);

    ScopedAttr a_type(H5Acreate1(committed, "type_attr", H5T_NATIVE_INT32,
                                 scalar, H5P_DEFAULT));
    ASSERT_TRUE(a_type.valid());
    ASSERT_GE(H5Awrite(a_type, H5T_NATIVE_INT32, &kTypeVal), 0);
  }

  // 2. Re-open each attribute with H5Aopen_name and verify written values.
  auto verify_open_name = [](hid_t loc, const char* name, int32_t expected) {
    ScopedAttr a(H5Aopen_name(loc, name));
    ASSERT_TRUE(a.valid()) << name;
    int32_t got = -1;
    ASSERT_GE(H5Aread(a, H5T_NATIVE_INT32, &got), 0);
    EXPECT_EQ(got, expected);
  };
  verify_open_name(file, "file_attr", kRootVal);
  verify_open_name(grp, "grp_attr_utf8", kGrpVal);
  verify_open_name(dset, "dset_attr", kDsetVal);
  verify_open_name(committed, "type_attr", kTypeVal);
}

TEST(H5AdeprecTest, OpenByIdxNumAttrsAndIterateWithResume) {
  ScopedFile file = CreateInMemoryFile("deprec_iter.h5");
  ASSERT_TRUE(file.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  // Track creation order so H5Aopen_idx / H5Aiterate1 match insertion sequence.
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_attr_creation_order(
                gcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);
  ASSERT_GE(H5Pset_attr_phase_change(gcpl, /*max_compact=*/2, /*min_dense=*/1),
            0);

  ScopedGroup grp(H5Gcreate2(file, "g", H5P_DEFAULT, gcpl, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  // Empty object initially reports 0 attributes.
  EXPECT_EQ(H5Aget_num_attrs(grp), 0);

  // Create 4 attributes to force dense storage transition.
  constexpr std::array<const char*, 4> kNames = {"alpha", "beta", "gamma",
                                                 "delta"};
  for (int32_t i = 0; i < static_cast<int32_t>(kNames.size()); ++i) {
    ScopedAttr a(
        H5Acreate1(grp, kNames[i], H5T_NATIVE_INT32, scalar, H5P_DEFAULT));
    ASSERT_TRUE(a.valid());
    const int32_t val = 100 + i;
    ASSERT_GE(H5Awrite(a, H5T_NATIVE_INT32, &val), 0);
  }

  EXPECT_EQ(H5Aget_num_attrs(grp), 4);

  // Open each attribute by 0-based creation-order index via H5Aopen_idx.
  for (unsigned i = 0; i < kNames.size(); ++i) {
    ScopedAttr a(H5Aopen_idx(grp, i));
    ASSERT_TRUE(a.valid());
    EXPECT_EQ(ReadAttrName(a), kNames[i]);
    int32_t val = -1;
    ASSERT_GE(H5Aread(a, H5T_NATIVE_INT32, &val), 0);
    EXPECT_EQ(val, 100 + static_cast<int32_t>(i));
  }

  // Also test H5Aopen_name on dense storage.
  {
    ScopedAttr a_dense(H5Aopen_name(grp, "delta"));
    ASSERT_TRUE(a_dense.valid());
    int32_t val = -1;
    ASSERT_GE(H5Aread(a_dense, H5T_NATIVE_INT32, &val), 0);
    EXPECT_EQ(val, 103);
  }

  // Iterate all attributes with attr_num == nullptr.
  struct CollectState {
    std::vector<std::string> names;
    int stop_after = -1;
    herr_t stop_ret = 0;
  };
  auto collect_cb = [](hid_t loc_id, const char* attr_name,
                       void* op_data) -> herr_t {
    auto* st = static_cast<CollectState*>(op_data);
    EXPECT_NE(loc_id, H5I_INVALID_HID);
    st->names.emplace_back(attr_name);
    if (st->stop_after >= 0 &&
        static_cast<int>(st->names.size()) >= st->stop_after) {
      return st->stop_ret;
    }
    return 0;
  };

  CollectState all_state;
  EXPECT_EQ(H5Aiterate1(grp, nullptr, collect_cb, &all_state), 0);
  EXPECT_THAT(all_state.names, ElementsAre("alpha", "beta", "gamma", "delta"));

  // Short-circuit positive return at the 2nd attribute, then resume from the
  // updated *attr_num index.
  CollectState part1;
  part1.stop_after = 2;
  part1.stop_ret = 42;
  unsigned idx = 0;
  EXPECT_EQ(H5Aiterate1(grp, &idx, collect_cb, &part1), 42);
  EXPECT_THAT(part1.names, ElementsAre("alpha", "beta"));
  EXPECT_EQ(idx, 2u);

  CollectState part2;
  EXPECT_EQ(H5Aiterate1(grp, &idx, collect_cb, &part2), 0);
  EXPECT_THAT(part2.names, ElementsAre("gamma", "delta"));
  EXPECT_EQ(idx, 4u);

  // Propagate negative callback failure return through H5Aiterate1.
  CollectState err_state;
  err_state.stop_after = 1;
  err_state.stop_ret = -7;
  idx = 0;
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Aiterate1(grp, &idx, collect_cb, &err_state), -7);
  }
  EXPECT_THAT(err_state.names, ElementsAre("alpha"));
}

TEST(H5AdeprecTest, ArgumentAndRuntimeErrorPathsFailCleanly) {
  ScopedFile file = CreateInMemoryFile("deprec_errors.h5");
  ASSERT_TRUE(file.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  ScopedAttr attr(
      H5Acreate1(file, "existing", H5T_NATIVE_INT32, scalar, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());

  ScopedErrorSilence silence;

  // H5Acreate1 invalid arguments and duplicate creation failure.
  EXPECT_EQ(H5Acreate1(attr, "nested", H5T_NATIVE_INT32, scalar, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Acreate1(file, nullptr, H5T_NATIVE_INT32, scalar, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Acreate1(file, "", H5T_NATIVE_INT32, scalar, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Acreate1(H5I_INVALID_HID, "bad_loc", H5T_NATIVE_INT32, scalar,
                       H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(
      H5Acreate1(scalar, "space_loc", H5T_NATIVE_INT32, scalar, H5P_DEFAULT),
      H5I_INVALID_HID);
  EXPECT_EQ(H5Acreate1(file, "bad_type", H5I_INVALID_HID, scalar, H5P_DEFAULT),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Acreate1(file, "existing", H5T_NATIVE_INT32, scalar, H5P_DEFAULT),
            H5I_INVALID_HID);

  // H5Aopen_name invalid arguments and missing attribute.
  EXPECT_EQ(H5Aopen_name(attr, "existing"), H5I_INVALID_HID);
  EXPECT_EQ(H5Aopen_name(file, nullptr), H5I_INVALID_HID);
  EXPECT_EQ(H5Aopen_name(file, ""), H5I_INVALID_HID);
  EXPECT_EQ(H5Aopen_name(H5I_INVALID_HID, "existing"), H5I_INVALID_HID);
  EXPECT_EQ(H5Aopen_name(file, "does_not_exist"), H5I_INVALID_HID);

  // H5Aopen_idx invalid arguments and out-of-range index.
  EXPECT_EQ(H5Aopen_idx(attr, 0), H5I_INVALID_HID);
  EXPECT_EQ(H5Aopen_idx(H5I_INVALID_HID, 0), H5I_INVALID_HID);
  EXPECT_EQ(H5Aopen_idx(file, 99), H5I_INVALID_HID);

  // H5Aget_num_attrs rejects invalid/non-object IDs, while an attribute ID
  // resolves to the underlying parent object's attribute count.
  EXPECT_LT(H5Aget_num_attrs(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Aget_num_attrs(scalar), 0);
  EXPECT_EQ(H5Aget_num_attrs(attr), 1);

  // H5Aiterate1 rejects attribute IDs, invalid IDs, and out-of-range start idx.
  auto noop_cb = [](hid_t, const char*, void*) -> herr_t { return 0; };
  EXPECT_LT(H5Aiterate1(attr, nullptr, noop_cb, nullptr), 0);
  EXPECT_LT(H5Aiterate1(H5I_INVALID_HID, nullptr, noop_cb, nullptr), 0);
  unsigned bad_idx = 5;
  EXPECT_LT(H5Aiterate1(file, &bad_idx, noop_cb, nullptr), 0);
}

}  // namespace
}  // namespace hdf5
