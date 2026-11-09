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
// Unit tests for third_party/hdf5/c++/src/H5ArrayType.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5ArrayType.cpp

#include <array>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5ArrayTypeTest, ConstructorsAssignmentDimsDecodeAndErrors) {
  H5::ArrayType stub;
  EXPECT_EQ(stub.getId(), H5I_INVALID_HID);
  EXPECT_EQ(stub.fromClass(), "ArrayType");

  const std::array<hsize_t, 2> dims = {2, 3};
  H5::ArrayType arr(H5::PredType::NATIVE_INT32, 2, dims.data());
  EXPECT_EQ(arr.getClass(), H5T_ARRAY);
  EXPECT_EQ(arr.getArrayNDims(), 2);

  std::array<hsize_t, 2> got_dims = {0, 0};
  EXPECT_EQ(arr.getArrayDims(got_dims.data()), 2);
  EXPECT_EQ(got_dims[0], 2u);
  EXPECT_EQ(got_dims[1], 3u);

  H5::ArrayType copied(arr);
  EXPECT_TRUE(copied == arr);
  H5::ArrayType from_id(arr.getId());
  EXPECT_EQ(from_id.getId(), arr.getId());

  H5::ArrayType assigned;
  assigned = arr;
  EXPECT_TRUE(assigned == arr);
  H5::ArrayType* self_ptr = &assigned;
  assigned = *self_ptr;
  EXPECT_TRUE(assigned == arr);

  H5::H5File file = CreateInMemoryFile();
  arr.commit(file, "committed_arr");
  H5::ArrayType opened_c(file, "committed_arr");
  EXPECT_EQ(opened_c.getArrayNDims(), 2);
  H5::ArrayType opened_s(file, H5std_string("committed_arr"));
  EXPECT_EQ(opened_s.getArrayNDims(), 2);

  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(opened_s.decode(), H5::DataTypeIException);

  arr.encode();
  std::unique_ptr<H5::DataType> decoded(arr.decode());
  ASSERT_NE(decoded.get(), nullptr);
  EXPECT_EQ(decoded->fromClass(), "ArrayType");
  EXPECT_TRUE(*decoded == arr);

  EXPECT_THROW(H5::ArrayType(H5::PredType::NATIVE_INT32, -1, dims.data()),
               H5::DataTypeIException);
  EXPECT_THROW(
      H5::ArrayType(H5::PredType::NATIVE_INT32, H5S_MAX_RANK + 1, dims.data()),
      H5::DataTypeIException);
  EXPECT_THROW(H5::ArrayType(H5::DataType(), 2, dims.data()),
               H5::DataTypeIException);
  EXPECT_THROW(H5::ArrayType(file, "missing_arr"), H5::DataTypeIException);
  EXPECT_THROW(stub.getArrayNDims(), H5::DataTypeIException);
  EXPECT_THROW(stub.getArrayDims(got_dims.data()), H5::DataTypeIException);

  // Locking lhs makes setId() throw inside operator=.
  H5::ArrayType locked_arr(H5::PredType::NATIVE_INT32, 2, dims.data());
  locked_arr.lock();
  EXPECT_THROW(locked_arr = arr, H5::DataTypeIException);
}

}  // namespace
}  // namespace hdf5
