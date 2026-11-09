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
// Unit tests for third_party/hdf5/c++/src/H5LcreatProp.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5LcreatProp.cpp

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {

template struct ConstantGetterBypass<ConstantGetterTag<H5::LinkCreatPropList>,
                                     &H5::LinkCreatPropList::getConstant>;

namespace {

TEST(H5LcreatPropTest, ConstructorsAndProperties) {
  H5::LinkCreatPropList lcpl;
  EXPECT_EQ(lcpl.fromClass(), "LinkCreatPropList");
  EXPECT_GE(lcpl.getId(), 0);
  EXPECT_GE(H5::LinkCreatPropList::DEFAULT.getId(), 0);

  H5::LinkCreatPropList copied(lcpl);
  EXPECT_EQ(copied.getId(), lcpl.getId());

  H5::LinkCreatPropList from_id(lcpl.getId());
  EXPECT_TRUE(from_id == lcpl);

  lcpl.setCreateIntermediateGroup(true);
  EXPECT_TRUE(lcpl.getCreateIntermediateGroup());
  lcpl.setCreateIntermediateGroup(false);
  EXPECT_FALSE(lcpl.getCreateIntermediateGroup());

  lcpl.setCharEncoding(H5T_CSET_UTF8);
  EXPECT_EQ(lcpl.getCharEncoding(), H5T_CSET_UTF8);
  lcpl.setCharEncoding(H5T_CSET_ASCII);
  EXPECT_EQ(lcpl.getCharEncoding(), H5T_CSET_ASCII);

  ScopedQuietHdf5Errors quiet;
  lcpl.close();
  EXPECT_THROW(lcpl.setCreateIntermediateGroup(true), H5::PropListIException);
  EXPECT_THROW(lcpl.getCreateIntermediateGroup(), H5::PropListIException);
  EXPECT_THROW(lcpl.setCharEncoding(H5T_CSET_UTF8), H5::PropListIException);
  EXPECT_THROW(lcpl.getCharEncoding(), H5::PropListIException);
}

TEST(H5LcreatPropTest, GetConstantDuplicateCallThrows) {
  ExpectDuplicateGetConstantThrows<H5::LinkCreatPropList>();
}

}  // namespace
}  // namespace hdf5
