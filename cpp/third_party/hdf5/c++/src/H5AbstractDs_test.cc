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
// Unit tests for third_party/hdf5/c++/src/H5AbstractDs.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5AbstractDs.cpp

#include <cstddef>
#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5AbstractDsTest, DataSetAndAttributeTypeGetters) {
  H5::H5File file = CreateInMemoryFile();
  H5::DataSpace scalar(H5S_SCALAR);

  const hsize_t array_dim = 3;
  H5::ArrayType array_dt(H5::PredType::NATIVE_INT32, 1, &array_dim);

  struct Pair {
    int32_t a;
    double b;
  };
  H5::CompType comp_dt(sizeof(Pair));
  comp_dt.insertMember("a", offsetof(Pair, a), H5::PredType::NATIVE_INT32);
  comp_dt.insertMember("b", offsetof(Pair, b), H5::PredType::NATIVE_DOUBLE);

  H5::EnumType enum_dt(H5::PredType::NATIVE_INT32);
  int32_t enum_val = 1;
  enum_dt.insert("ONE", &enum_val);

  H5::StrType str_dt(H5::PredType::C_S1, 8);
  H5::VarLenType vlen_dt(H5::PredType::NATIVE_INT32);

  H5::DataSet ds_int =
      file.createDataSet("ds_int", H5::PredType::NATIVE_INT32, scalar);
  H5::DataSet ds_flt =
      file.createDataSet("ds_flt", H5::PredType::NATIVE_DOUBLE, scalar);
  H5::DataSet ds_str = file.createDataSet("ds_str", str_dt, scalar);
  H5::DataSet ds_arr = file.createDataSet("ds_arr", array_dt, scalar);
  H5::DataSet ds_comp = file.createDataSet("ds_comp", comp_dt, scalar);
  H5::DataSet ds_enum = file.createDataSet("ds_enum", enum_dt, scalar);
  H5::DataSet ds_vlen = file.createDataSet("ds_vlen", vlen_dt, scalar);

  EXPECT_EQ(ds_int.getTypeClass(), H5T_INTEGER);
  EXPECT_EQ(ds_int.getDataType().getClass(), H5T_INTEGER);
  EXPECT_EQ(ds_int.getIntType().getClass(), H5T_INTEGER);
  EXPECT_EQ(ds_flt.getFloatType().getClass(), H5T_FLOAT);
  EXPECT_EQ(ds_str.getStrType().getClass(), H5T_STRING);
  EXPECT_EQ(ds_arr.getArrayType().getClass(), H5T_ARRAY);
  EXPECT_EQ(ds_comp.getCompType().getClass(), H5T_COMPOUND);
  EXPECT_EQ(ds_enum.getEnumType().getClass(), H5T_ENUM);
  EXPECT_EQ(ds_vlen.getVarLenType().getClass(), H5T_VLEN);

  // Exercise every AbstractDs getter on Attribute instances too.
  H5::Attribute at_int =
      ds_int.createAttribute("a_int", H5::PredType::NATIVE_INT32, scalar);
  H5::Attribute at_flt =
      ds_int.createAttribute("a_flt", H5::PredType::NATIVE_DOUBLE, scalar);
  H5::Attribute at_str = ds_int.createAttribute("a_str", str_dt, scalar);
  H5::Attribute at_arr = ds_int.createAttribute("a_arr", array_dt, scalar);
  H5::Attribute at_comp = ds_int.createAttribute("a_comp", comp_dt, scalar);
  H5::Attribute at_enum = ds_int.createAttribute("a_enum", enum_dt, scalar);
  H5::Attribute at_vlen = ds_int.createAttribute("a_vlen", vlen_dt, scalar);

  const H5::AbstractDs& abs_at = at_int;
  EXPECT_EQ(abs_at.getTypeClass(), H5T_INTEGER);
  EXPECT_EQ(abs_at.getDataType().getClass(), H5T_INTEGER);
  EXPECT_EQ(at_int.getIntType().getClass(), H5T_INTEGER);
  EXPECT_EQ(at_flt.getFloatType().getClass(), H5T_FLOAT);
  EXPECT_EQ(at_str.getStrType().getClass(), H5T_STRING);
  EXPECT_EQ(at_arr.getArrayType().getClass(), H5T_ARRAY);
  EXPECT_EQ(at_comp.getCompType().getClass(), H5T_COMPOUND);
  EXPECT_EQ(at_enum.getEnumType().getClass(), H5T_ENUM);
  EXPECT_EQ(at_vlen.getVarLenType().getClass(), H5T_VLEN);
}

TEST(H5AbstractDsTest, ClosedDataSetAndAttributeThrowDataTypeIException) {
  ScopedQuietHdf5Errors quiet;

  H5::DataSet closed_ds;
  EXPECT_THROW(closed_ds.getTypeClass(), H5::DataTypeIException);
  EXPECT_THROW(closed_ds.getDataType(), H5::DataTypeIException);
  EXPECT_THROW(closed_ds.getArrayType(), H5::DataTypeIException);
  EXPECT_THROW(closed_ds.getCompType(), H5::DataTypeIException);
  EXPECT_THROW(closed_ds.getEnumType(), H5::DataTypeIException);
  EXPECT_THROW(closed_ds.getIntType(), H5::DataTypeIException);
  EXPECT_THROW(closed_ds.getFloatType(), H5::DataTypeIException);
  EXPECT_THROW(closed_ds.getStrType(), H5::DataTypeIException);
  EXPECT_THROW(closed_ds.getVarLenType(), H5::DataTypeIException);

  H5::Attribute closed_at;
  EXPECT_THROW(closed_at.getTypeClass(), H5::DataTypeIException);
  EXPECT_THROW(closed_at.getDataType(), H5::DataTypeIException);
  EXPECT_THROW(closed_at.getArrayType(), H5::DataTypeIException);
  EXPECT_THROW(closed_at.getCompType(), H5::DataTypeIException);
  EXPECT_THROW(closed_at.getEnumType(), H5::DataTypeIException);
  EXPECT_THROW(closed_at.getIntType(), H5::DataTypeIException);
  EXPECT_THROW(closed_at.getFloatType(), H5::DataTypeIException);
  EXPECT_THROW(closed_at.getStrType(), H5::DataTypeIException);
  EXPECT_THROW(closed_at.getVarLenType(), H5::DataTypeIException);
}

}  // namespace
}  // namespace hdf5
