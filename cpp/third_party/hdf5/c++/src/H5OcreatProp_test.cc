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
// Unit tests for third_party/hdf5/c++/src/H5OcreatProp.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5OcreatProp.cpp

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {

template struct ConstantGetterBypass<ConstantGetterTag<H5::ObjCreatPropList>,
                                     &H5::ObjCreatPropList::getConstant>;

namespace {

TEST(H5OcreatPropTest, ConstructorsAndAttrProperties) {
  H5::ObjCreatPropList ocpl;
  EXPECT_EQ(ocpl.fromClass(), "ObjCreatPropList");
  EXPECT_GE(ocpl.getId(), 0);
  EXPECT_GE(H5::ObjCreatPropList::DEFAULT.getId(), 0);

  H5::ObjCreatPropList copied(ocpl);
  EXPECT_EQ(copied.getId(), ocpl.getId());

  H5::ObjCreatPropList from_id(ocpl.getId());
  EXPECT_TRUE(from_id == ocpl);

  ocpl.setAttrPhaseChange(12, 6);
  unsigned max_compact = 0;
  unsigned min_dense = 0;
  ocpl.getAttrPhaseChange(max_compact, min_dense);
  EXPECT_EQ(max_compact, 12u);
  EXPECT_EQ(min_dense, 6u);

  ocpl.setAttrCrtOrder(H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED);
  EXPECT_EQ(
      ocpl.getAttrCrtOrder(),
      static_cast<unsigned>(H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED));

  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(ocpl.setAttrPhaseChange(2, 5), H5::PropListIException);
  EXPECT_THROW(ocpl.setAttrCrtOrder(H5P_CRT_ORDER_INDEXED),
               H5::PropListIException);

  ocpl.close();
  EXPECT_THROW(ocpl.getAttrPhaseChange(max_compact, min_dense),
               H5::PropListIException);
  EXPECT_THROW(ocpl.getAttrCrtOrder(), H5::PropListIException);
}

TEST(H5OcreatPropTest, GetConstantDuplicateCallThrows) {
  ExpectDuplicateGetConstantThrows<H5::ObjCreatPropList>();
}

}  // namespace
}  // namespace hdf5
