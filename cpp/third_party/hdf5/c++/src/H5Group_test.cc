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
// Unit tests for third_party/hdf5/c++/src/H5Group.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5Group.cpp

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5GroupTest, ConstructorsAssignmentObjIdDereferenceAndErrors) {
  H5::Group stub;
  EXPECT_EQ(stub.getId(), H5I_INVALID_HID);
  EXPECT_EQ(stub.getLocId(), H5I_INVALID_HID);
  EXPECT_EQ(stub.fromClass(), "Group");

  H5::H5File file = CreateInMemoryFile();
  H5::Group grp = file.createGroup("g1");
  H5::DataSpace scalar(H5S_SCALAR);
  H5::DataSet child =
      grp.createDataSet("child_ds", H5::PredType::NATIVE_INT32, scalar);
  EXPECT_EQ(grp.getNumObjs(), 1u);
  EXPECT_EQ(grp.getLocId(), grp.getId());

  H5::Group copied(grp);
  EXPECT_EQ(copied.getId(), grp.getId());

  H5::Group from_id(grp.getId());
  EXPECT_EQ(from_id.getId(), grp.getId());

  H5::Group assigned;
  assigned = grp;
  EXPECT_EQ(assigned.getId(), grp.getId());
  H5::Group* self_ptr = &assigned;
  assigned = *self_ptr;
  EXPECT_EQ(assigned.getId(), grp.getId());

  hobj_ref_t ref{};
  file.reference(&ref, "/g1", H5R_OBJECT);
  H5::Group deref(file, &ref, H5R_OBJECT);
  EXPECT_NE(deref.getId(), H5I_INVALID_HID);
  EXPECT_EQ(deref.getNumObjs(), 1u);

  const hid_t obj_id_c = grp.getObjId("child_ds");
  EXPECT_GE(obj_id_c, 0);
  grp.closeObjId(obj_id_c);

  const hid_t obj_id_s = grp.getObjId(H5std_string("child_ds"));
  EXPECT_GE(obj_id_s, 0);
  grp.closeObjId(obj_id_s);

  deref.close();
  EXPECT_EQ(deref.getId(), H5I_INVALID_HID);

  // Use PublicSetId to verify p_setId overwrites without failing when valid.
  PublicSetId<H5::Group> sub;
  const hid_t reopened = grp.getObjId(".");
  sub.CallSetId(reopened);
  EXPECT_EQ(sub.getId(), reopened);
  sub.CallSetId(H5I_INVALID_HID);
  EXPECT_EQ(sub.getId(), H5I_INVALID_HID);

  ScopedQuietHdf5Errors quiet;
  {
    PublicSetId<H5::Group> bad_grp;
    bad_grp.CallSetId(scalar.getId());
    EXPECT_THROW(bad_grp.close(), H5::GroupIException);
    EXPECT_THROW(bad_grp.CallSetId(H5I_INVALID_HID), H5::GroupIException);
  }  // ~Group() catches GroupIException from H5Gclose(scalar.getId()).
  EXPECT_THROW(stub.getNumObjs(), H5::GroupIException);
  EXPECT_THROW(grp.getObjId("missing_obj"), H5::GroupIException);
  EXPECT_THROW(grp.closeObjId(H5I_INVALID_HID), H5::GroupIException);
  EXPECT_THROW((H5::Group(file, nullptr, H5R_OBJECT)), H5::ReferenceException);
  EXPECT_THROW(stub.throwException("testFn", "failed"), H5::GroupIException);
}

}  // namespace
}  // namespace hdf5
