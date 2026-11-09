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
// Unit tests for third_party/hdf5/c++/src/H5IntType.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5IntType.cpp

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5IntTypeTest, ConstructorsNamedTypesDecodeSignAndErrors) {
  H5::IntType stub;
  EXPECT_EQ(stub.getId(), H5I_INVALID_HID);
  EXPECT_EQ(stub.fromClass(), "IntType");

  H5::IntType from_pred(H5::PredType::STD_I32LE);
  EXPECT_EQ(from_pred.getSign(), H5T_SGN_2);

  H5::IntType copied(from_pred);
  EXPECT_TRUE(copied == from_pred);

  H5::IntType from_id(from_pred.getId());
  EXPECT_EQ(from_id.getId(), from_pred.getId());

  from_pred.setSign(H5T_SGN_NONE);
  EXPECT_EQ(from_pred.getSign(), H5T_SGN_NONE);

  H5::H5File file = CreateInMemoryFile();
  const hsize_t dim = 2;
  H5::DataSpace space(1, &dim);
  H5::DataSet dset =
      file.createDataSet("ints", H5::PredType::NATIVE_INT, space);
  H5::IntType from_ds(dset);
  EXPECT_EQ(from_ds.getClass(), H5T_INTEGER);

  from_pred.commit(file, "committed_u32");
  H5::IntType opened_c(file, "committed_u32");
  EXPECT_EQ(opened_c.getSign(), H5T_SGN_NONE);

  H5::IntType opened_s(file, H5std_string("committed_u32"));
  EXPECT_EQ(opened_s.getSign(), H5T_SGN_NONE);

  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(opened_s.decode(), H5::DataTypeIException);

  from_pred.encode();
  std::unique_ptr<H5::DataType> decoded(from_pred.decode());
  ASSERT_NE(decoded.get(), nullptr);
  EXPECT_EQ(decoded->fromClass(), "IntType");
  EXPECT_TRUE(*decoded == from_pred);

  EXPECT_THROW((H5::IntType(H5::DataSet())), H5::DataSetIException);
  EXPECT_THROW(H5::IntType(file, "missing_int"), H5::DataTypeIException);
  EXPECT_THROW(stub.getSign(), H5::DataTypeIException);
  EXPECT_THROW(from_pred.setSign(H5T_SGN_ERROR), H5::DataTypeIException);
}

}  // namespace
}  // namespace hdf5
