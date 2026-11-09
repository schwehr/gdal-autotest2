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
// Unit tests for third_party/hdf5/c++/src/H5File.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5File.cpp

#include <array>
#include <cstdint>
#include <fstream>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5FileTest, DiskConstructorsIsHdf5OpenReopenQueriesAndCorruptInput) {
  H5::H5File stub;
  EXPECT_EQ(stub.getId(), H5I_INVALID_HID);
  EXPECT_EQ(stub.getLocId(), H5I_INVALID_HID);
  EXPECT_EQ(stub.fromClass(), "H5File");

  TempFile disk_a("_a.h5");
  TempFile disk_b("_b.h5");

  {
    H5::H5File created_c(disk_a.c_str(), H5F_ACC_TRUNC,
                         H5::FileCreatPropList::DEFAULT,
                         H5::FileAccPropList::DEFAULT);
    EXPECT_EQ(created_c.getLocId(), created_c.getId());
    created_c.createGroup("g");
  }

  // Test static isHdf5 / isAccessible (both const char* and H5std_string).
  EXPECT_TRUE(H5::H5File::isHdf5(disk_a.c_str()));
  EXPECT_TRUE(H5::H5File::isHdf5(disk_a.string()));
  EXPECT_TRUE(H5::H5File::isAccessible(disk_a.c_str()));
  EXPECT_TRUE(H5::H5File::isAccessible(disk_a.string()));

  // Open existing file via all 3-arg and openFile overloads.
  {
    H5::H5File opened_3arg_c(disk_a.c_str(), H5F_ACC_RDONLY,
                             H5::FileAccPropList::DEFAULT);
    EXPECT_TRUE(opened_3arg_c.nameExists("g"));
  }
  {
    H5::H5File opened_3arg_s(disk_a.string(), H5F_ACC_RDONLY,
                             H5::FileAccPropList::DEFAULT);
    EXPECT_TRUE(opened_3arg_s.nameExists("g"));
  }
  {
    H5::H5File created_s(disk_b.string(), H5F_ACC_TRUNC,
                         H5::FileCreatPropList::DEFAULT,
                         H5::FileAccPropList::DEFAULT);
    created_s.createGroup("g2");
  }

  H5::H5File reopened;
  reopened.openFile(disk_a.c_str(), H5F_ACC_RDWR);
  EXPECT_TRUE(reopened.nameExists("g"));
  reopened.openFile(disk_b.string(), H5F_ACC_RDONLY);
  EXPECT_TRUE(reopened.nameExists("g2"));

  // VFD handle, file size, info, free space, file number, obj count/ids.
  void* vfd_handle1 = nullptr;
  void* vfd_handle2 = nullptr;
  reopened.getVFDHandle(&vfd_handle1);
  reopened.getVFDHandle(H5::FileAccPropList::DEFAULT, &vfd_handle2);
  EXPECT_NE(vfd_handle1, nullptr);
  EXPECT_NE(vfd_handle2, nullptr);

  H5F_info2_t finfo{};
  reopened.getFileInfo(finfo);
  EXPECT_GT(reopened.getFileSize(), 0u);
  EXPECT_GE(reopened.getFreeSpace(), 0);
  EXPECT_GT(reopened.getFileNum(), 0ul);

  EXPECT_NE(reopened.getCreatePlist().getId(), H5I_INVALID_HID);
  EXPECT_NE(reopened.getAccessPlist().getId(), H5I_INVALID_HID);

  H5::Group opened_grp = reopened.openGroup("g2");
  EXPECT_GE(reopened.getObjCount(H5F_OBJ_ALL), 2);
  std::array<hid_t, 4> obj_ids{};
  reopened.getObjIDs(H5F_OBJ_ALL, obj_ids.size(), obj_ids.data());
  // Note: H5Fget_obj_ids does not increment refcounts; do not close obj_ids.
  EXPECT_GE(obj_ids[0], 0);
  opened_grp.close();

  // Copy constructor and existing-id constructor.
  H5::H5File copied(reopened);
  EXPECT_EQ(copied.getId(), reopened.getId());
  H5::H5File from_id(reopened.getId());
  EXPECT_EQ(from_id.getId(), reopened.getId());

  PublicSetId<H5::H5File> sub;
  sub.CallSetId(H5I_INVALID_HID);
  EXPECT_EQ(sub.getId(), H5I_INVALID_HID);

  // Truncated/corrupt file header must return false for isHdf5/isAccessible
  // and fail to open as HDF5.
  TempFile corrupt("_corrupt.h5");
  {
    std::ofstream ofs(corrupt.string(), std::ios::binary);
    constexpr std::array<uint8_t, 8> kBadHeader = {0x01, 0x02, 0x03, 0x04,
                                                   0xDE, 0xAD, 0xBE, 0xEF};
    ofs.write(reinterpret_cast<const char*>(kBadHeader.data()),
              kBadHeader.size());
  }
  EXPECT_FALSE(H5::H5File::isHdf5(corrupt.c_str()));
  EXPECT_FALSE(H5::H5File::isAccessible(corrupt.c_str()));

  // Non-existent path and invalid/closed state error paths.
  TempFile missing("_nonexistent.h5");
  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(H5::H5File::isHdf5(missing.c_str()), H5::FileIException);
  EXPECT_THROW(H5::H5File::isHdf5(missing.string()), H5::FileIException);
  EXPECT_THROW(H5::H5File::isAccessible(missing.c_str()), H5::FileIException);
  EXPECT_THROW(H5::H5File::isAccessible(missing.string()), H5::FileIException);
  EXPECT_THROW(H5::H5File(missing.c_str(), H5F_ACC_RDONLY), H5::FileIException);
  EXPECT_THROW(H5::H5File(missing.string(), H5F_ACC_RDONLY,
                          H5::FileAccPropList::DEFAULT),
               H5::FileIException);
  EXPECT_THROW(H5::H5File(corrupt.c_str(), H5F_ACC_RDONLY), H5::FileIException);
  EXPECT_THROW(H5::H5File("", H5F_ACC_TRUNC), H5::FileIException);
  EXPECT_THROW(
      H5::H5File(H5std_string(""), H5F_ACC_TRUNC,
                 H5::FileCreatPropList::DEFAULT, H5::FileAccPropList::DEFAULT),
      H5::FileIException);
  EXPECT_THROW(
      H5::H5File(missing.c_str(), H5F_ACC_RDONLY, H5::FileAccPropList::DEFAULT),
      H5::FileIException);
  H5::DataSpace non_file_space(H5S_SCALAR);
  {
    PublicSetId<H5::H5File> bad_file;
    bad_file.CallSetId(non_file_space.getId());
    EXPECT_THROW(bad_file.close(), H5::FileIException);
    EXPECT_THROW(bad_file.openFile(disk_a.c_str(), H5F_ACC_RDONLY),
                 H5::FileIException);
    EXPECT_THROW(bad_file.reOpen(), H5::FileIException);
    EXPECT_THROW(bad_file.CallSetId(H5I_INVALID_HID), H5::FileIException);
  }  // ~H5File() catches FileIException from H5Fclose(non_file_space.getId()).
  EXPECT_THROW(stub.openFile(missing.c_str(), H5F_ACC_RDONLY),
               H5::FileIException);
  // Upstream H5File::reOpen closes id before calling H5Freopen(id), so both
  // reOpen() and reopen() always throw FileIException.
  EXPECT_THROW(reopened.reOpen(), H5::FileIException);
  EXPECT_THROW(copied.reopen(), H5::FileIException);

  EXPECT_THROW(stub.getCreatePlist(), H5::FileIException);
  EXPECT_THROW(stub.getAccessPlist(), H5::FileIException);
  EXPECT_THROW(stub.getFileInfo(finfo), H5::FileIException);
  EXPECT_THROW(stub.getFreeSpace(), H5::FileIException);
  EXPECT_THROW(stub.getObjCount(), H5::FileIException);
  EXPECT_THROW(stub.getObjIDs(H5F_OBJ_ALL, obj_ids.size(), obj_ids.data()),
               H5::FileIException);
  EXPECT_THROW(stub.getVFDHandle(&vfd_handle1), H5::FileIException);
  EXPECT_THROW(stub.getVFDHandle(H5::FileAccPropList::DEFAULT, &vfd_handle1),
               H5::FileIException);
  EXPECT_THROW(stub.getFileSize(), H5::FileIException);
  EXPECT_THROW(stub.getFileNum(), H5::FileIException);
  EXPECT_THROW(stub.throwException("testFn", "msg"), H5::FileIException);
}

}  // namespace
}  // namespace hdf5
