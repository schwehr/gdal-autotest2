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
// Tests link creation property list class registration, intermediate-group
// property getters/setters, encoding, and group/dataset/link creation
// behavior (third_party/hdf5/src/H5Plcpl.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Plcpl.c

#include "third_party/hdf5/src/hdf5.h"

#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

constexpr char kIntermediateGroupProp[] = "intermediate_group";

TEST(H5PlcplTest, ClassMetadataAndDefaults) {
  char* class_name = H5Pget_class_name(H5P_LINK_CREATE);
  ASSERT_NE(class_name, nullptr);
  absl::Cleanup free_name = [class_name] { H5free_memory(class_name); };
  EXPECT_STREQ(class_name, "link create");

  ScopedPlist parent(H5Pget_class_parent(H5P_LINK_CREATE));
  ASSERT_TRUE(parent.valid());
  EXPECT_GT(H5Pequal(parent.get(), H5P_STRING_CREATE), 0);

  EXPECT_GT(H5Pexist(H5P_LINK_CREATE_DEFAULT, kIntermediateGroupProp), 0);

  unsigned crt_intmd = 99u;
  ASSERT_GE(
      H5Pget_create_intermediate_group(H5P_LINK_CREATE_DEFAULT, &crt_intmd),
      0);
  EXPECT_EQ(crt_intmd, 0u);

  // Null out-pointer is permitted on H5Pget_create_intermediate_group.
  EXPECT_GE(
      H5Pget_create_intermediate_group(H5P_LINK_CREATE_DEFAULT, nullptr), 0);
}

TEST(H5PlcplTest, SetGetAndNormalizeIntermediateGroupFlag) {
  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());

  unsigned crt_intmd = 99u;
  ASSERT_GE(H5Pget_create_intermediate_group(lcpl.get(), &crt_intmd), 0);
  EXPECT_EQ(crt_intmd, 0u);

  // Any non-zero value normalizes to 1.
  ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 42u), 0);
  ASSERT_GE(H5Pget_create_intermediate_group(lcpl.get(), &crt_intmd), 0);
  EXPECT_EQ(crt_intmd, 1u);

  // Null out-pointer query succeeds on mutable LCPL too.
  EXPECT_GE(H5Pget_create_intermediate_group(lcpl.get(), nullptr), 0);

  ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 0u), 0);
  ASSERT_GE(H5Pget_create_intermediate_group(lcpl.get(), &crt_intmd), 0);
  EXPECT_EQ(crt_intmd, 0u);

  ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 1u), 0);
  ASSERT_GE(H5Pget_create_intermediate_group(lcpl.get(), &crt_intmd), 0);
  EXPECT_EQ(crt_intmd, 1u);
}

TEST(H5PlcplTest, SetAndGetRejectInvalidLists) {
  ScopedPlist lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(lapl.valid());
  ScopedPlist lcpl_removed(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl_removed.valid());
  ASSERT_GE(H5Premove(lcpl_removed.get(), kIntermediateGroupProp), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_create_intermediate_group(H5I_INVALID_HID, 1u), 0);
  EXPECT_LT(H5Pset_create_intermediate_group(H5P_LINK_CREATE_DEFAULT, 1u), 0);
  EXPECT_LT(H5Pset_create_intermediate_group(lapl.get(), 1u), 0);
  EXPECT_LT(H5Pset_create_intermediate_group(lcpl_removed.get(), 1u), 0);

  unsigned val = 0;
  EXPECT_LT(H5Pget_create_intermediate_group(H5I_INVALID_HID, &val), 0);
  EXPECT_LT(H5Pget_create_intermediate_group(lapl.get(), &val), 0);
  EXPECT_LT(H5Pget_create_intermediate_group(lcpl_removed.get(), &val), 0);
}

TEST(H5PlcplTest, CopyEqualAndEncodeDecodeRoundTrip) {
  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());
  ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 1u), 0);

  ScopedPlist copied(H5Pcopy(lcpl.get()));
  ASSERT_TRUE(copied.valid());
  EXPECT_GT(H5Pequal(lcpl.get(), copied.get()), 0);

  size_t encoded_size = 0;
  ASSERT_GE(H5Pencode2(lcpl.get(), nullptr, &encoded_size, H5P_DEFAULT), 0);
  ASSERT_GT(encoded_size, 0u);
  std::vector<uint8_t> encoded(encoded_size, 0);
  ASSERT_GE(H5Pencode2(lcpl.get(), encoded.data(), &encoded_size, H5P_DEFAULT),
            0);

  ScopedPlist decoded(H5Pdecode(encoded.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pisa_class(decoded.get(), H5P_LINK_CREATE), 0);
  EXPECT_GT(H5Pequal(lcpl.get(), decoded.get()), 0);

  unsigned crt_intmd = 0;
  ASSERT_GE(H5Pget_create_intermediate_group(decoded.get(), &crt_intmd), 0);
  EXPECT_EQ(crt_intmd, 1u);
}

TEST(H5PlcplTest, CreatesMissingIntermediateGroupsOnGroupsDatasetsAndLinks) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedPlist lcpl_off(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl_off.valid());

  // 1. Without intermediate group creation, missing parent groups fail.
  {
    ScopedErrorSilence silence;
    ScopedGroup missing(H5Gcreate2(file.get(), "/missing/sub/grp",
                                   lcpl_off.get(), H5P_DEFAULT, H5P_DEFAULT));
    EXPECT_EQ(missing.get(), H5I_INVALID_HID);
  }

  // 2. Enabling intermediate groups allows nested group, dataset, soft link,
  //    and hard link creation without pre-creating intermediate parents.
  ScopedPlist lcpl_on(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl_on.valid());
  ASSERT_GE(H5Pset_create_intermediate_group(lcpl_on.get(), 7u), 0);

  ScopedGroup grp(H5Gcreate2(file.get(), "/alpha/beta/gamma", lcpl_on.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "/delta/epsilon/val",
                                H5T_NATIVE_INT32, space.get(), lcpl_on.get(),
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  ASSERT_GE(H5Lcreate_soft("/delta/epsilon/val", file.get(),
                           "/links/soft/to_val", lcpl_on.get(), H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_hard(file.get(), "/delta/epsilon/val", file.get(),
                           "/links/hard/to_val", lcpl_on.get(), H5P_DEFAULT),
            0);

  EXPECT_GT(H5Lexists(file.get(), "/alpha", H5P_DEFAULT), 0);
  EXPECT_GT(H5Lexists(file.get(), "/alpha/beta", H5P_DEFAULT), 0);
  EXPECT_GT(H5Lexists(file.get(), "/links/soft/to_val", H5P_DEFAULT), 0);
  EXPECT_GT(H5Lexists(file.get(), "/links/hard/to_val", H5P_DEFAULT), 0);
}

TEST(H5PlcplTest, ReinitExercisesClassRegistrationCallback) {
  ASSERT_GE(H5close(), 0);
  ASSERT_GE(H5open(), 0);

  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());
  EXPECT_GT(H5Pexist(lcpl.get(), kIntermediateGroupProp), 0);
}

}  // namespace
}  // namespace hdf5
