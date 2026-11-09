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
// Unit tests for third_party/hdf5/c++/src/H5CommonFG.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5CommonFG.cpp

#include <cstddef>
#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

void CommitNamedTypes(const H5::H5Location& loc) {
  H5::DataType opaque_dt(H5T_OPAQUE, 4);
  opaque_dt.setTag("raw4");
  opaque_dt.commit(loc, "t_generic");

  const hsize_t dim = 2;
  H5::ArrayType array_dt(H5::PredType::NATIVE_INT32, 1, &dim);
  array_dt.commit(loc, "t_array");

  struct Pair {
    int32_t x;
  };
  H5::CompType comp_dt(sizeof(Pair));
  comp_dt.insertMember("x", offsetof(Pair, x), H5::PredType::NATIVE_INT32);
  comp_dt.commit(loc, "t_comp");

  H5::EnumType enum_dt(H5::PredType::NATIVE_INT32);
  int32_t val = 7;
  enum_dt.insert("SEVEN", &val);
  enum_dt.commit(loc, "t_enum");

  H5::IntType int_dt(H5::PredType::NATIVE_INT32);
  int_dt.commit(loc, "t_int");

  H5::FloatType float_dt(H5::PredType::NATIVE_DOUBLE);
  float_dt.commit(loc, "t_float");

  H5::StrType str_dt(H5::PredType::C_S1, 16);
  str_dt.commit(loc, "t_str");

  H5::VarLenType vlen_dt(H5::PredType::NATIVE_INT32);
  vlen_dt.commit(loc, "t_vlen");
}

void VerifyOpenTypesOnLocation(const H5::CommonFG& fg) {
  EXPECT_EQ(fg.openDataType("t_generic").getClass(), H5T_OPAQUE);
  EXPECT_EQ(fg.openDataType(H5std_string("t_generic")).getClass(), H5T_OPAQUE);

  EXPECT_EQ(fg.openArrayType("t_array").getClass(), H5T_ARRAY);
  EXPECT_EQ(fg.openArrayType(H5std_string("t_array")).getClass(), H5T_ARRAY);

  EXPECT_EQ(fg.openCompType("t_comp").getClass(), H5T_COMPOUND);
  EXPECT_EQ(fg.openCompType(H5std_string("t_comp")).getClass(), H5T_COMPOUND);

  EXPECT_EQ(fg.openEnumType("t_enum").getClass(), H5T_ENUM);
  EXPECT_EQ(fg.openEnumType(H5std_string("t_enum")).getClass(), H5T_ENUM);

  EXPECT_EQ(fg.openIntType("t_int").getClass(), H5T_INTEGER);
  EXPECT_EQ(fg.openIntType(H5std_string("t_int")).getClass(), H5T_INTEGER);

  EXPECT_EQ(fg.openFloatType("t_float").getClass(), H5T_FLOAT);
  EXPECT_EQ(fg.openFloatType(H5std_string("t_float")).getClass(), H5T_FLOAT);

  EXPECT_EQ(fg.openStrType("t_str").getClass(), H5T_STRING);
  EXPECT_EQ(fg.openStrType(H5std_string("t_str")).getClass(), H5T_STRING);

  EXPECT_EQ(fg.openVarLenType("t_vlen").getClass(), H5T_VLEN);
  EXPECT_EQ(fg.openVarLenType(H5std_string("t_vlen")).getClass(), H5T_VLEN);
}

TEST(H5CommonFGTest, OpenAllNamedDatatypeOverloadsAndMissingErrors) {
  H5::H5File file = CreateInMemoryFile();
  H5::Group grp = file.createGroup("sub");

  CommitNamedTypes(file);
  CommitNamedTypes(grp);

  // Also create/open a DataSet to exercise friend f_DataSet_setId defined in
  // H5CommonFG.cpp.
  H5::DataSpace scalar(H5S_SCALAR);
  H5::DataSet ds =
      file.createDataSet("dummy_ds", H5::PredType::NATIVE_INT32, scalar);
  EXPECT_NE(ds.getId(), H5I_INVALID_HID);

  VerifyOpenTypesOnLocation(file);
  VerifyOpenTypesOnLocation(grp);

  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(file.openDataType("missing"), H5::FileIException);
  EXPECT_THROW(file.openArrayType("missing"), H5::FileIException);
  EXPECT_THROW(file.openCompType("missing"), H5::FileIException);
  EXPECT_THROW(file.openEnumType("missing"), H5::FileIException);
  EXPECT_THROW(file.openIntType("missing"), H5::FileIException);
  EXPECT_THROW(file.openFloatType("missing"), H5::FileIException);
  EXPECT_THROW(file.openStrType("missing"), H5::FileIException);
  EXPECT_THROW(file.openVarLenType("missing"), H5::FileIException);

  EXPECT_THROW(grp.openDataType("missing"), H5::GroupIException);
  EXPECT_THROW(grp.openArrayType("missing"), H5::GroupIException);
  EXPECT_THROW(grp.openCompType("missing"), H5::GroupIException);
  EXPECT_THROW(grp.openEnumType("missing"), H5::GroupIException);
  EXPECT_THROW(grp.openIntType("missing"), H5::GroupIException);
  EXPECT_THROW(grp.openFloatType("missing"), H5::GroupIException);
  EXPECT_THROW(grp.openStrType("missing"), H5::GroupIException);
  EXPECT_THROW(grp.openVarLenType("missing"), H5::GroupIException);
}

}  // namespace
}  // namespace hdf5
