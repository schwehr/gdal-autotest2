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
// Unit tests for third_party/hdf5/c++/src/H5EnumType.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5EnumType.cpp

#include <cstdint>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5EnumTypeTest, ConstructorsMembersLookupDecodeAndErrors) {
  H5::EnumType stub;
  EXPECT_EQ(stub.getId(), H5I_INVALID_HID);
  EXPECT_EQ(stub.fromClass(), "EnumType");

  H5::EnumType by_size(sizeof(int32_t));
  EXPECT_EQ(by_size.getClass(), H5T_ENUM);
  EXPECT_EQ(by_size.getSize(), sizeof(int32_t));

  H5::IntType base_int(H5::PredType::NATIVE_INT32);
  H5::EnumType enum_dt(base_int);

  H5::EnumType copied(enum_dt);
  EXPECT_TRUE(copied == enum_dt);
  H5::EnumType from_id(enum_dt.getId());
  EXPECT_EQ(from_id.getId(), enum_dt.getId());

  int32_t red_val = 10;
  int32_t green_val = 20;
  enum_dt.insert("RED", &red_val);
  enum_dt.insert(H5std_string("GREEN"), &green_val);

  EXPECT_EQ(enum_dt.getNmembers(), 2);
  EXPECT_EQ(enum_dt.getMemberIndex("RED"), 0);
  EXPECT_EQ(enum_dt.getMemberIndex(H5std_string("GREEN")), 1);

  int32_t out_val = -1;
  enum_dt.getMemberValue(1, &out_val);
  EXPECT_EQ(out_val, 20);

  EXPECT_EQ(enum_dt.nameOf(&red_val, 16), "RED");
  out_val = -1;
  enum_dt.valueOf("RED", &out_val);
  EXPECT_EQ(out_val, 10);
  out_val = -1;
  enum_dt.valueOf(H5std_string("GREEN"), &out_val);
  EXPECT_EQ(out_val, 20);

  H5::H5File file = CreateInMemoryFile();
  const hsize_t dim = 1;
  H5::DataSpace space(1, &dim);
  H5::DataSet dset = file.createDataSet("enums", enum_dt, space);
  H5::EnumType from_ds(dset);
  EXPECT_EQ(from_ds.getNmembers(), 2);

  enum_dt.commit(file, "committed_enum");
  H5::EnumType opened_c(file, "committed_enum");
  EXPECT_EQ(opened_c.getNmembers(), 2);
  H5::EnumType opened_s(file, H5std_string("committed_enum"));
  EXPECT_EQ(opened_s.getNmembers(), 2);

  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(opened_s.decode(), H5::DataTypeIException);

  enum_dt.encode();
  std::unique_ptr<H5::DataType> decoded(enum_dt.decode());
  ASSERT_NE(decoded.get(), nullptr);
  EXPECT_EQ(decoded->fromClass(), "EnumType");
  EXPECT_TRUE(*decoded == enum_dt);

  EXPECT_THROW((H5::EnumType(H5::DataSet())), H5::DataSetIException);
  // Upstream H5EnumType.cpp throws DataSetIException on H5Tenum_create failure.
  EXPECT_THROW((H5::EnumType(H5::IntType())), H5::DataSetIException);
  EXPECT_THROW(H5::EnumType(file, "missing_enum"), H5::DataTypeIException);
  EXPECT_THROW(stub.getNmembers(), H5::DataTypeIException);
  EXPECT_THROW(enum_dt.getMemberIndex("BLUE"), H5::DataTypeIException);
  EXPECT_THROW(enum_dt.getMemberValue(99, &out_val), H5::DataTypeIException);
  EXPECT_THROW(stub.insert("BAD", &red_val), H5::DataTypeIException);

  int32_t missing_val = 999;
  EXPECT_THROW(enum_dt.nameOf(&missing_val, 16), H5::DataTypeIException);
  EXPECT_THROW(enum_dt.valueOf("BLUE", &out_val), H5::DataTypeIException);
}

}  // namespace
}  // namespace hdf5
