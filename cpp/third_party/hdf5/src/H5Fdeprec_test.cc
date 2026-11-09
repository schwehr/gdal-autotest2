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
// Tests deprecated H5F API functions H5Fget_info1, H5Fis_hdf5, and
// H5Fset_latest_format (third_party/hdf5/src/H5Fdeprec.c).
//
// Note: google3 builds hdf5 with deprecated symbols enabled. H5Fis_hdf5 uses
// H5P_FILE_ACCESS_DEFAULT (sec2 VFD), so file-signature tests use TempFile.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Fdeprec.c

#include "third_party/hdf5/src/hdf5.h"

#include <cstdint>
#include <fstream>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

namespace hdf5 {
namespace {

TEST(H5FdeprecTest, GetInfo1RejectsNullStructAndInvalidIds) {
  ScopedFile file = CreateInMemoryFile("deprec_invalid.h5");
  ASSERT_TRUE(file.valid());

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());

  // Uncommitted transient datatype has type H5I_DATATYPE but no VOL object.
  ScopedType transient_type(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(transient_type.valid());

  ScopedErrorSilence silence;
  H5F_info1_t info{};
  EXPECT_LT(H5Fget_info1(file.get(), nullptr), 0);
  EXPECT_LT(H5Fget_info1(H5I_INVALID_HID, &info), 0);
  EXPECT_LT(H5Fget_info1(space.get(), &info), 0);
  EXPECT_LT(H5Fget_info1(fapl.get(), &info), 0);
  EXPECT_LT(H5Fget_info1(transient_type.get(), &info), 0);
}

TEST(H5FdeprecTest, GetInfo1SucceedsAcrossAllFileObjectTypes) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  // Configure a Shared Object Header Message index to populate both
  // superblock extension and SOHM fields in H5F_info1_t.
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
  ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_ATTR_FLAG, 1),
            0);

  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST, H5F_LIBVER_LATEST),
      0);

  ScopedFile file(
      H5Fcreate("deprec_info1.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(H5Gcreate2(file.get(), "grp", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedType committed_type(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(committed_type.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "dtype", committed_type.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(grp.get(), "dset", H5T_NATIVE_INT32,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  constexpr int32_t kVal = 42;
  ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, &kVal),
            0);

  ScopedAttr attr1(H5Acreate2(dset.get(), "a1", H5T_NATIVE_INT32, space.get(),
                              H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr1.valid());
  ASSERT_GE(H5Awrite(attr1.get(), H5T_NATIVE_INT32, &kVal), 0);

  ScopedAttr attr2(H5Acreate2(grp.get(), "a2", H5T_NATIVE_INT32, space.get(),
                              H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr2.valid());
  ASSERT_GE(H5Awrite(attr2.get(), H5T_NATIVE_INT32, &kVal), 0);

  H5F_info1_t file_info{};
  ASSERT_GE(H5Fget_info1(file.get(), &file_info), 0);
  EXPECT_GT(file_info.super_ext_size, 0u);
  EXPECT_GT(file_info.sohm.hdr_size, 0u);

  const hid_t ids[] = {grp.get(), committed_type.get(), dset.get(),
                       attr1.get()};
  for (hid_t id : ids) {
    H5F_info1_t obj_info{};
    ASSERT_GE(H5Fget_info1(id, &obj_info), 0);
    EXPECT_EQ(obj_info.super_ext_size, file_info.super_ext_size);
    EXPECT_EQ(obj_info.sohm.hdr_size, file_info.sohm.hdr_size);
    EXPECT_EQ(obj_info.sohm.msgs_info.index_size,
              file_info.sohm.msgs_info.index_size);
    EXPECT_EQ(obj_info.sohm.msgs_info.heap_size,
              file_info.sohm.msgs_info.heap_size);
  }
}

TEST(H5FdeprecTest, IsHdf5ValidatesPathsAndSignatures) {
  TempFile hdf5_file("valid_deprec.h5");
  {
    ScopedFile file(H5Fcreate(hdf5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
  }
  EXPECT_GT(H5Fis_hdf5(hdf5_file.c_str()), 0);

  TempFile text_file("plain_text.txt");
  {
    std::ofstream out(text_file.path(), std::ios::binary);
    out << "not an hdf5 signature\n";
    ASSERT_TRUE(out.good());
  }
  EXPECT_EQ(H5Fis_hdf5(text_file.c_str()), 0);

  TempFile missing_file("does_not_exist.h5");
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Fis_hdf5(nullptr), 0);
    EXPECT_LT(H5Fis_hdf5(""), 0);
    EXPECT_LT(H5Fis_hdf5(missing_file.c_str()), 0);
  }
}

TEST(H5FdeprecTest, SetLatestFormatValidationAndToggling) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST),
      0);

  ScopedFile file(H5Fcreate("deprec_latest.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(H5Gcreate2(file.get(), "not_a_file", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Fset_latest_format(H5I_INVALID_HID, true), 0);
    EXPECT_LT(H5Fset_latest_format(grp.get(), false), 0);
  }

  auto check_bounds = [&](H5F_libver_t expected_low,
                          H5F_libver_t expected_high) {
    ScopedPlist got_fapl(H5Fget_access_plist(file.get()));
    ASSERT_TRUE(got_fapl.valid());
    H5F_libver_t low = H5F_LIBVER_ERROR;
    H5F_libver_t high = H5F_LIBVER_ERROR;
    ASSERT_GE(H5Pget_libver_bounds(got_fapl.get(), &low, &high), 0);
    EXPECT_EQ(low, expected_low);
    EXPECT_EQ(high, expected_high);
  };

  check_bounds(H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);

  ASSERT_GE(H5Fset_latest_format(file.get(), true), 0);
  check_bounds(H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);

  // Repeating the same setting exercises the no-op check.
  ASSERT_GE(H5Fset_latest_format(file.get(), true), 0);
  check_bounds(H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);

  ASSERT_GE(H5Fset_latest_format(file.get(), false), 0);
  check_bounds(H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
}

}  // namespace
}  // namespace hdf5
