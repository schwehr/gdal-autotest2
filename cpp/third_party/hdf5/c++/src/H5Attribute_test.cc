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
// Unit tests for third_party/hdf5/c++/src/H5Attribute.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5Attribute.cpp

#include <array>
#include <cstdint>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5AttributeTest, ConstructorsRawIoStringsNamesMetadataAndErrors) {
  H5::Attribute stub;
  EXPECT_EQ(stub.getId(), H5I_INVALID_HID);
  EXPECT_EQ(stub.fromClass(), "Attribute");

  H5::H5File file = CreateInMemoryFile();
  H5::Group grp = file.createGroup("grp");
  H5::DataSpace scalar(H5S_SCALAR);

  H5::Attribute int_attr =
      grp.createAttribute("int_attr_long", H5::PredType::NATIVE_INT32, scalar);
  const int32_t write_val = 42;
  int_attr.write(H5::PredType::NATIVE_INT32, &write_val);
  int32_t read_val = 0;
  int_attr.read(H5::PredType::NATIVE_INT32, &read_val);
  EXPECT_EQ(read_val, 42);

  EXPECT_EQ(int_attr.getSpace().getSimpleExtentType(), H5S_SCALAR);
  EXPECT_EQ(int_attr.getInMemDataSize(), sizeof(int32_t));
  EXPECT_GE(int_attr.getStorageSize(), sizeof(int32_t));

  // Exercise all supported getName overloads.
  EXPECT_EQ(int_attr.getName(), "int_attr_long");
  EXPECT_EQ(int_attr.getName(8), "int_attr");

  H5std_string s_full;
  EXPECT_EQ(int_attr.getName(s_full), 13);
  EXPECT_EQ(s_full, "int_attr_long");

  H5std_string s_trunc;
  EXPECT_EQ(int_attr.getName(s_trunc, 3), 13);
  EXPECT_EQ(s_trunc, "int");

  std::array<char, 32> c_buf{};
  EXPECT_EQ(int_attr.getName(c_buf.data(), c_buf.size()), 13);
  EXPECT_STREQ(c_buf.data(), "int_attr_long");

  // Copy constructor, id constructor, assignment, and self-assignment.
  H5::Attribute copied(int_attr);
  EXPECT_EQ(copied.getId(), int_attr.getId());
  H5::Attribute from_id(int_attr.getId());
  EXPECT_EQ(from_id.getId(), int_attr.getId());
  H5::Attribute assigned;
  assigned = int_attr;
  EXPECT_EQ(assigned.getId(), int_attr.getId());
  H5::Attribute* self_ptr = &assigned;
  assigned = *self_ptr;
  EXPECT_EQ(assigned.getId(), int_attr.getId());
  assigned.close();
  EXPECT_EQ(assigned.getId(), H5I_INVALID_HID);

  // Fixed-length string H5std_string write and read (p_read_fixed_len).
  H5::StrType fixed_str(H5::PredType::C_S1, 10);
  H5::Attribute fstr_attr = grp.createAttribute("fixed_str", fixed_str, scalar);
  const H5std_string fstr_in("hello_fix");
  fstr_attr.write(fixed_str, fstr_in);
  H5std_string fstr_out;
  fstr_attr.read(fixed_str, fstr_out);
  EXPECT_STREQ(fstr_out.c_str(), "hello_fix");

  // Variable-length string H5std_string write and read (p_read_variable_len).
  H5::StrType vlen_str(H5::PredType::C_S1, H5T_VARIABLE);
  H5::Attribute vstr_attr = grp.createAttribute("vlen_str", vlen_str, scalar);
  const H5std_string vstr_in("variable_length_payload");
  vstr_attr.write(vlen_str, vstr_in);
  H5std_string vstr_out;
  vstr_attr.read(vlen_str, vstr_out);
  EXPECT_EQ(vstr_out, "variable_length_payload");

  // Exercise p_setId via PublicSetId.
  PublicSetId<H5::Attribute> sub;
  sub.CallSetId(H5I_INVALID_HID);
  EXPECT_EQ(sub.getId(), H5I_INVALID_HID);

  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(stub.write(H5::PredType::NATIVE_INT32, &write_val),
               H5::AttributeIException);
  EXPECT_THROW(stub.write(H5::DataType(), fstr_in), H5::AttributeIException);
  EXPECT_THROW(stub.write(fixed_str, fstr_in), H5::AttributeIException);
  EXPECT_THROW(stub.read(H5::PredType::NATIVE_INT32, &read_val),
               H5::AttributeIException);
  EXPECT_THROW(stub.read(H5::DataType(), fstr_out), H5::AttributeIException);
  EXPECT_THROW(stub.read(fixed_str, fstr_out), H5::AttributeIException);
  EXPECT_THROW(stub.read(vlen_str, vstr_out), H5::AttributeIException);
  EXPECT_THROW(stub.getInMemDataSize(), H5::AttributeIException);
  EXPECT_THROW(stub.getSpace(), H5::AttributeIException);
  EXPECT_THROW(stub.getName(c_buf.data(), c_buf.size()),
               H5::AttributeIException);
  EXPECT_THROW(stub.getName(), H5::AttributeIException);
  // Pass len=0 on invalid IDs because upstream Attribute::getName(str, len>0)
  // allocates new char[len + 1] without RAII before H5Aget_name throws.
  EXPECT_THROW(stub.getName(static_cast<size_t>(0)), H5::AttributeIException);
  EXPECT_THROW(stub.getName(s_trunc, static_cast<size_t>(0)),
               H5::AttributeIException);
  EXPECT_THROW(stub.getDataType(), H5::DataTypeIException);

  {
    PublicSetId<H5::Attribute> bad_attr;
    bad_attr.CallSetId(scalar.getId());
    EXPECT_THROW(bad_attr.close(), H5::AttributeIException);
    EXPECT_THROW(bad_attr.CallSetId(H5I_INVALID_HID), H5::AttributeIException);
  }  // ~Attribute() catches AttributeIException from H5Aclose(scalar.getId()).
}

}  // namespace
}  // namespace hdf5
