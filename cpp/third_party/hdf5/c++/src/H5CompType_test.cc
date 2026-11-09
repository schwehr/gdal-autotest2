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
// Unit tests for third_party/hdf5/c++/src/H5CompType.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5CompType.cpp

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

struct NestedRecord {
  int32_t inner_id;
};

struct OpaqueFour {
  std::array<uint8_t, 4> bytes;
};

struct FullRecord {
  int32_t i;
  double f;  // Leaves natural 4-byte alignment padding after |i| on LP64.
  std::array<char, 8> s;
  int32_t e;
  std::array<int32_t, 3> arr;
  NestedRecord nested;
  hvl_t vlen;
  OpaqueFour opq;
};

TEST(H5CompTypeTest, ConstructorsMembersPackDecodeAndErrors) {
  H5::CompType stub;
  EXPECT_EQ(stub.getId(), H5I_INVALID_HID);
  EXPECT_EQ(stub.fromClass(), "CompType");

  H5::CompType comp(sizeof(FullRecord));
  EXPECT_EQ(comp.getClass(), H5T_COMPOUND);

  H5::CompType copied(comp);
  EXPECT_TRUE(copied == comp);
  H5::CompType from_id(comp.getId());
  EXPECT_EQ(from_id.getId(), comp.getId());

  H5::EnumType enum_member(sizeof(int32_t));
  int32_t zero = 0;
  enum_member.insert("ZERO", &zero);

  constexpr hsize_t kArrDim = 3;
  H5::ArrayType array_member(H5::PredType::NATIVE_INT32, 1, &kArrDim);

  H5::CompType nested_member(sizeof(NestedRecord));
  nested_member.insertMember("inner_id", offsetof(NestedRecord, inner_id),
                             H5::PredType::NATIVE_INT32);

  H5::VarLenType vlen_member(H5::PredType::NATIVE_INT32);
  H5::StrType str_member(H5::PredType::C_S1, 8);
  H5::DataType opaque_member(H5T_OPAQUE, 4);
  opaque_member.setTag("four");

  comp.insertMember("i", offsetof(FullRecord, i), H5::PredType::NATIVE_INT32);
  comp.insertMember("f", offsetof(FullRecord, f), H5::PredType::NATIVE_DOUBLE);
  comp.insertMember("s", offsetof(FullRecord, s), str_member);
  comp.insertMember("e", offsetof(FullRecord, e), enum_member);
  comp.insertMember("arr", offsetof(FullRecord, arr), array_member);
  comp.insertMember("nested", offsetof(FullRecord, nested), nested_member);
  comp.insertMember("vlen", offsetof(FullRecord, vlen), vlen_member);
  comp.insertMember("opq", offsetof(FullRecord, opq), opaque_member);

  EXPECT_EQ(comp.getNmembers(), 8);
  EXPECT_EQ(comp.getMemberName(0), "i");
  EXPECT_EQ(comp.getMemberIndex("f"), 1);
  EXPECT_EQ(comp.getMemberIndex(H5std_string("vlen")), 6);
  EXPECT_EQ(comp.getMemberOffset(0), offsetof(FullRecord, i));
  EXPECT_EQ(comp.getMemberOffset(1), offsetof(FullRecord, f));

  EXPECT_EQ(comp.getMemberClass(0), H5T_INTEGER);
  EXPECT_EQ(comp.getMemberClass(1), H5T_FLOAT);
  EXPECT_EQ(comp.getMemberClass(2), H5T_STRING);
  EXPECT_EQ(comp.getMemberClass(3), H5T_ENUM);
  EXPECT_EQ(comp.getMemberClass(4), H5T_ARRAY);
  EXPECT_EQ(comp.getMemberClass(5), H5T_COMPOUND);
  EXPECT_EQ(comp.getMemberClass(6), H5T_VLEN);
  EXPECT_EQ(comp.getMemberClass(7), H5T_OPAQUE);

  EXPECT_EQ(comp.getMemberIntType(0).getClass(), H5T_INTEGER);
  EXPECT_EQ(comp.getMemberFloatType(1).getClass(), H5T_FLOAT);
  EXPECT_EQ(comp.getMemberStrType(2).getSize(), 8u);
  EXPECT_EQ(comp.getMemberEnumType(3).getNmembers(), 1);
  EXPECT_EQ(comp.getMemberArrayType(4).getArrayNDims(), 1);
  EXPECT_EQ(comp.getMemberCompType(5).getNmembers(), 1);
  EXPECT_EQ(comp.getMemberVarLenType(6).getClass(), H5T_VLEN);
  EXPECT_EQ(comp.getMemberDataType(7).getClass(), H5T_OPAQUE);

  H5::H5File file = CreateInMemoryFile();
  constexpr hsize_t kDim = 1;
  H5::DataSpace space(1, &kDim);
  H5::DataSet dset = file.createDataSet("comp_ds", comp, space);
  H5::CompType from_ds(dset);
  EXPECT_EQ(from_ds.getNmembers(), 8);

  comp.commit(file, "committed_comp");
  H5::CompType opened_c(file, "committed_comp");
  EXPECT_EQ(opened_c.getNmembers(), 8);
  H5::CompType opened_s(file, H5std_string("committed_comp"));
  EXPECT_EQ(opened_s.getNmembers(), 8);

  // Pack removes struct padding and setSize restores explicit sizing.
  comp.pack();
  const size_t packed_size = comp.getSize();
  EXPECT_LT(packed_size, sizeof(FullRecord));
  comp.setSize(packed_size + 4);
  EXPECT_EQ(comp.getSize(), packed_size + 4);

  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(opened_s.decode(), H5::DataTypeIException);

  comp.encode();
  std::unique_ptr<H5::DataType> decoded(comp.decode());
  ASSERT_NE(decoded.get(), nullptr);
  EXPECT_EQ(decoded->fromClass(), "CompType");
  EXPECT_TRUE(*decoded == comp);

  EXPECT_THROW((H5::CompType(H5::DataSet())), H5::DataSetIException);
  EXPECT_THROW(H5::CompType(file, "missing_comp"), H5::DataTypeIException);
  EXPECT_THROW(stub.getNmembers(), H5::DataTypeIException);
  EXPECT_THROW(comp.getMemberName(99), H5::DataTypeIException);
  EXPECT_THROW(comp.getMemberIndex("missing_member"), H5::DataTypeIException);
  EXPECT_THROW(comp.getMemberClass(99), H5::DataTypeIException);
  EXPECT_THROW(comp.getMemberDataType(99), H5::DataTypeIException);
  EXPECT_THROW(comp.getMemberArrayType(99), H5::DataTypeIException);
  EXPECT_THROW(comp.getMemberCompType(99), H5::DataTypeIException);
  EXPECT_THROW(comp.getMemberEnumType(99), H5::DataTypeIException);
  EXPECT_THROW(comp.getMemberIntType(99), H5::DataTypeIException);
  EXPECT_THROW(comp.getMemberFloatType(99), H5::DataTypeIException);
  EXPECT_THROW(comp.getMemberStrType(99), H5::DataTypeIException);
  EXPECT_THROW(comp.getMemberVarLenType(99), H5::DataTypeIException);
  EXPECT_THROW(stub.insertMember("bad", 0, H5::PredType::NATIVE_INT),
               H5::DataTypeIException);
  EXPECT_THROW(stub.pack(), H5::DataTypeIException);
  EXPECT_THROW(stub.setSize(8), H5::DataTypeIException);
}

}  // namespace
}  // namespace hdf5
