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
// Unit tests for third_party/hdf5/c++/src/H5IdComponent.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5IdComponent.cpp

#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

// Minimal concrete subclass to exercise IdComponent::fromClass(),
// IdComponent::inMemFunc(), and protected helpers directly.
class BareIdComponent : public H5::IdComponent {
 public:
  explicit BareIdComponent(hid_t id) : id_(id) {}
  hid_t getId() const override { return id_; }
  void close() override { id_ = H5I_INVALID_HID; }

  std::string CallGetFileName() const { return p_get_file_name(); }
  static bool CallValidId(hid_t id) { return p_valid_id(id); }

 protected:
  void p_setId(hid_t new_id) override { id_ = new_id; }

 private:
  hid_t id_;
};

// Subclass whose p_setId throws to exercise IdComponent::operator='s catch
// handler cleanly without corrupting live HDF5 handles.
class ThrowingIdComponent : public H5::IdComponent {
 public:
  hid_t getId() const override { return H5I_INVALID_HID; }
  void close() override {}

 protected:
  void p_setId(hid_t /*new_id*/) override {
    throw H5::IdComponentException("ThrowingIdComponent::p_setId",
                                   "injected failure");
  }
};

TEST(H5IdComponentTest, ReferenceCountAndSetIdAndAssignment) {
  H5::DataSpace space_a(H5S_SCALAR);
  H5::DataSpace space_b(H5S_SCALAR);

  EXPECT_EQ(1, space_a.getCounter());
  space_a.incRefCount();
  EXPECT_EQ(2, space_a.getCounter());
  space_a.decRefCount();
  EXPECT_EQ(1, space_a.getCounter());

  const hid_t id_b = space_b.getId();
  space_a.incRefCount(id_b);
  EXPECT_EQ(2, space_a.getCounter(id_b));
  space_a.decRefCount(id_b);
  EXPECT_EQ(1, space_a.getCounter(id_b));

  // IdComponent::operator= updates ID via setId() and respects self-assign.
  H5::IdComponent* base_a = &space_a;
  *base_a = space_b;
  EXPECT_EQ(id_b, space_a.getId());
  EXPECT_EQ(2, space_b.getCounter());
  *base_a = *base_a;
  EXPECT_EQ(id_b, space_a.getId());

  H5::DataSpace space_c(H5S_SCALAR);
  const hid_t id_c = space_c.getId();
  space_a.setId(id_c);
  EXPECT_EQ(id_c, space_a.getId());
  EXPECT_EQ(2, space_c.getCounter());
  EXPECT_EQ(1, space_b.getCounter());

  // Invalid IDs return 0 from getCounter() and ignore inc/decRefCount.
  EXPECT_EQ(0, space_a.getCounter(H5I_INVALID_HID));
  space_a.incRefCount(H5I_INVALID_HID);
  space_a.decRefCount(H5I_INVALID_HID);
}

TEST(H5IdComponentTest, TypeQueriesAndValidity) {
  ScopedQuietHdf5Errors quiet;

  H5::H5File file = CreateInMemoryFile("idcomp.h5");
  H5::Group group = file.createGroup("grp");
  H5::DataSpace space(H5S_SCALAR);

  EXPECT_EQ(H5I_FILE, H5::IdComponent::getHDFObjType(file.getId()));
  EXPECT_EQ(H5I_GROUP, group.getHDFObjType());
  EXPECT_EQ(H5I_DATASPACE, space.getHDFObjType());
  EXPECT_EQ(H5I_BADID, H5::IdComponent::getHDFObjType(H5I_INVALID_HID));
  EXPECT_EQ(H5I_BADID, H5::IdComponent::getHDFObjType(0));

  EXPECT_TRUE(H5::IdComponent::isValid(space.getId()));
  EXPECT_FALSE(H5::IdComponent::isValid(H5I_INVALID_HID));
  EXPECT_FALSE(H5::IdComponent::isValid(static_cast<hid_t>(1LL << 50)));

  // H5Itype_exists and H5Inmembers apply only to user-registered ID types;
  // passing library types such as H5I_DATASPACE fails with BADGROUP.
  H5I_type_t custom_type = H5Iregister_type2(0, nullptr);
  ASSERT_NE(H5I_BADID, custom_type);
  absl::Cleanup destroy_custom_type = [&custom_type]() {
    if (custom_type != H5I_BADID && H5Itype_exists(custom_type) > 0) {
      H5Idestroy_type(custom_type);
    }
  };

  EXPECT_TRUE(H5::IdComponent::typeExists(custom_type));
  EXPECT_EQ(0u, H5::IdComponent::getNumMembers(custom_type));

  int dummy_object = 42;
  const hid_t custom_id = H5Iregister(custom_type, &dummy_object);
  ASSERT_NE(H5I_INVALID_HID, custom_id);
  EXPECT_EQ(1u, H5::IdComponent::getNumMembers(custom_type));
  EXPECT_NE(nullptr, H5Iremove_verify(custom_id, custom_type));
  EXPECT_EQ(0u, H5::IdComponent::getNumMembers(custom_type));

  ASSERT_GE(H5Idestroy_type(custom_type), 0);
  EXPECT_FALSE(H5::IdComponent::typeExists(custom_type));
  EXPECT_THROW(H5::IdComponent::getNumMembers(custom_type),
               H5::IdComponentException);
  custom_type = H5I_BADID;

  EXPECT_THROW(H5::IdComponent::typeExists(H5I_DATASPACE),
               H5::IdComponentException);
  EXPECT_THROW(H5::IdComponent::getNumMembers(H5I_DATASPACE),
               H5::IdComponentException);
}

TEST(H5IdComponentTest, BaseHelpersAndErrorBranches) {
  ScopedQuietHdf5Errors quiet;

  H5::H5File file = CreateInMemoryFile("idcomp_name.h5");
  H5::Group group = file.createGroup("sub");
  EXPECT_THAT(group.getFileName(), HasSubstr("idcomp_name.h5"));

  BareIdComponent bare(H5I_INVALID_HID);
  EXPECT_EQ("IdComponent", bare.fromClass());
  EXPECT_EQ("IdComponent::helper", bare.inMemFunc("helper"));
  EXPECT_FALSE(BareIdComponent::CallValidId(H5I_INVALID_HID));
  EXPECT_THROW(bare.CallGetFileName(), H5::IdComponentException);

  H5::DataSpace rhs_space(H5S_SCALAR);
  ThrowingIdComponent throwing_lhs;
  H5::IdComponent* base_lhs = &throwing_lhs;
  EXPECT_THROW(*base_lhs = rhs_space, H5::FileIException);

  H5::DataSpace invalid_space(H5S_SCALAR);
  invalid_space.close();
  EXPECT_EQ(H5I_INVALID_HID, invalid_space.getId());

  EXPECT_THROW(H5::IdComponent::getNumMembers(H5I_BADID),
               H5::IdComponentException);
}

}  // namespace
}  // namespace hdf5
