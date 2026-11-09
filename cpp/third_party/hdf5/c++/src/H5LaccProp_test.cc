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
// Unit tests for third_party/hdf5/c++/src/H5LaccProp.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5LaccProp.cpp

#include <cstddef>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {

template struct ConstantGetterBypass<ConstantGetterTag<H5::LinkAccPropList>,
                                     &H5::LinkAccPropList::getConstant>;

namespace {

TEST(H5LaccPropTest, ConstructorsAndNumLinks) {
  H5::LinkAccPropList lapl;
  EXPECT_EQ(lapl.fromClass(), "LinkAccPropList");
  EXPECT_GE(lapl.getId(), 0);
  EXPECT_GE(H5::LinkAccPropList::DEFAULT.getId(), 0);

  H5::LinkAccPropList copied(lapl);
  EXPECT_EQ(copied.getId(), lapl.getId());

  H5::LinkAccPropList from_id(lapl.getId());
  EXPECT_TRUE(from_id == lapl);

  lapl.setNumLinks(32);
  EXPECT_EQ(lapl.getNumLinks(), 32u);

  ScopedQuietHdf5Errors quiet;
  lapl.close();
  EXPECT_THROW(lapl.setNumLinks(16), H5::PropListIException);
  EXPECT_THROW(lapl.getNumLinks(), H5::PropListIException);
}

TEST(H5LaccPropTest, GetConstantDuplicateCallThrows) {
  ExpectDuplicateGetConstantThrows<H5::LinkAccPropList>();
}

}  // namespace
}  // namespace hdf5
