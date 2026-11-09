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
// Unit tests for third_party/hdf5/c++/src/H5StrType.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5StrType.cpp

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5StrTypeTest, ConstructorsPropertiesNamedTypesDecodeAndErrors) {
  H5::StrType stub;
  EXPECT_EQ(stub.getId(), H5I_INVALID_HID);
  EXPECT_EQ(stub.fromClass(), "StrType");

  H5::StrType from_pred(H5::PredType::C_S1);
  EXPECT_EQ(from_pred.getClass(), H5T_STRING);
  EXPECT_EQ(from_pred.getSize(), 1u);

  H5::StrType from_pred_len(H5::PredType::C_S1, 16);
  EXPECT_EQ(from_pred_len.getSize(), 16u);

  H5::StrType from_dummy_len(0, 24);
  EXPECT_EQ(from_dummy_len.getSize(), 24u);

  H5::StrType copied(from_dummy_len);
  EXPECT_TRUE(copied == from_dummy_len);

  H5::StrType from_id(from_dummy_len.getId());
  EXPECT_EQ(from_id.getId(), from_dummy_len.getId());

  from_dummy_len.setCset(H5T_CSET_UTF8);
  EXPECT_EQ(from_dummy_len.getCset(), H5T_CSET_UTF8);
  from_dummy_len.setCset(H5T_CSET_ASCII);
  EXPECT_EQ(from_dummy_len.getCset(), H5T_CSET_ASCII);

  from_dummy_len.setStrpad(H5T_STR_NULLPAD);
  EXPECT_EQ(from_dummy_len.getStrpad(), H5T_STR_NULLPAD);
  from_dummy_len.setStrpad(H5T_STR_SPACEPAD);
  EXPECT_EQ(from_dummy_len.getStrpad(), H5T_STR_SPACEPAD);
  from_dummy_len.setStrpad(H5T_STR_NULLTERM);
  EXPECT_EQ(from_dummy_len.getStrpad(), H5T_STR_NULLTERM);

  H5::H5File file = CreateInMemoryFile();
  const hsize_t dim = 1;
  H5::DataSpace space(1, &dim);
  H5::DataSet dset = file.createDataSet("strs", from_dummy_len, space);
  H5::StrType from_ds(dset);
  EXPECT_EQ(from_ds.getSize(), 24u);

  from_dummy_len.commit(file, "committed_str");
  H5::StrType opened_c(file, "committed_str");
  EXPECT_EQ(opened_c.getSize(), 24u);
  H5::StrType opened_s(file, H5std_string("committed_str"));
  EXPECT_EQ(opened_s.getSize(), 24u);

  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(opened_s.decode(), H5::DataTypeIException);

  from_dummy_len.encode();
  std::unique_ptr<H5::DataType> decoded(from_dummy_len.decode());
  ASSERT_NE(decoded.get(), nullptr);
  EXPECT_EQ(decoded->fromClass(), "StrType");
  EXPECT_TRUE(*decoded == from_dummy_len);

  EXPECT_THROW((H5::StrType(H5::DataSet())), H5::DataSetIException);
  EXPECT_THROW(H5::StrType(file, "missing_str"), H5::DataTypeIException);
  EXPECT_THROW(stub.getCset(), H5::DataTypeIException);
  EXPECT_THROW(from_dummy_len.setCset(H5T_CSET_ERROR), H5::DataTypeIException);
  EXPECT_THROW(stub.getStrpad(), H5::DataTypeIException);
  EXPECT_THROW(from_dummy_len.setStrpad(H5T_STR_ERROR), H5::DataTypeIException);
}

}  // namespace
}  // namespace hdf5
