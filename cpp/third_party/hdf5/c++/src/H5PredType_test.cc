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
// Unit tests for third_party/hdf5/c++/src/H5PredType.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5PredType.cpp

#include <array>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {

template struct ConstantGetterBypass<ConstantGetterTag<H5::PredType>,
                                     &H5::PredType::getPredTypes>;

namespace {

class PredTypeSubclass : public H5::PredType {
 public:
  PredTypeSubclass() = default;
  explicit PredTypeSubclass(hid_t predtype_id) : H5::PredType(predtype_id) {}
};

TEST(H5PredTypeTest, ConstructorsAssignmentAndDummyOperations) {
  PredTypeSubclass stub;
  EXPECT_EQ(stub.getId(), H5I_INVALID_HID);
  EXPECT_EQ(stub.fromClass(), "PredType");

  const int ref_before = H5Iget_ref(H5T_STD_I32LE);
  PredTypeSubclass from_hid(H5T_STD_I32LE);
  EXPECT_NE(from_hid.getId(), H5I_INVALID_HID);
  EXPECT_EQ(from_hid.getClass(), H5T_INTEGER);
  EXPECT_EQ(from_hid.getSize(), 4u);
  // Upstream PredType::PredType(hid_t) (H5PredType.cpp:42-45) increments
  // predtype_id in AtomType(predtype_id) before overwriting id =
  // H5Tcopy(predtype_id). Verify and balance the extra reference.
  EXPECT_EQ(H5Iget_ref(H5T_STD_I32LE), ref_before + 1);
  from_hid.decRefCount(H5T_STD_I32LE);

  H5::PredType copy(H5::PredType::NATIVE_INT32);
  EXPECT_EQ(copy.fromClass(), "PredType");
  EXPECT_TRUE(copy == H5::PredType::NATIVE_INT32);

  H5::PredType assigned(H5::PredType::IEEE_F64LE);
  assigned = copy;
  EXPECT_TRUE(assigned == copy);

  // Self-assignment must be a no-op.
  H5::PredType* self_ptr = &assigned;
  assigned = *self_ptr;
  EXPECT_TRUE(assigned == copy);

  // Dummy commit/committed methods always throw DataTypeIException.
  H5::H5File file = CreateInMemoryFile();
  EXPECT_THROW(copy.commit(file, "predefined_cstr"), H5::DataTypeIException);
  EXPECT_THROW(copy.commit(file, H5std_string("predefined_str")),
               H5::DataTypeIException);
  EXPECT_THROW(copy.committed(), H5::DataTypeIException);
}

TEST(H5PredTypeTest, PredefinedConstantsHaveExpectedClassesAndSizes) {
  struct PredCase {
    const H5::PredType* type;
    H5T_class_t expected_class;
    size_t min_size;
  };
  const auto cases = std::to_array<PredCase>({
      {.type = &H5::PredType::STD_I8BE,
       .expected_class = H5T_INTEGER,
       .min_size = 1},
      {.type = &H5::PredType::STD_U64LE,
       .expected_class = H5T_INTEGER,
       .min_size = 8},
      {.type = &H5::PredType::STD_B16LE,
       .expected_class = H5T_BITFIELD,
       .min_size = 2},
      {.type = &H5::PredType::STD_REF_OBJ,
       .expected_class = H5T_REFERENCE,
       .min_size = 1},
      {.type = &H5::PredType::STD_REF_DSETREG,
       .expected_class = H5T_REFERENCE,
       .min_size = 1},
      {.type = &H5::PredType::C_S1,
       .expected_class = H5T_STRING,
       .min_size = 1},
      {.type = &H5::PredType::FORTRAN_S1,
       .expected_class = H5T_STRING,
       .min_size = 1},
      {.type = &H5::PredType::IEEE_F32BE,
       .expected_class = H5T_FLOAT,
       .min_size = 4},
      {.type = &H5::PredType::IEEE_F64LE,
       .expected_class = H5T_FLOAT,
       .min_size = 8},
      {.type = &H5::PredType::FLOAT_BFLOAT16LE,
       .expected_class = H5T_FLOAT,
       .min_size = 2},
      {.type = &H5::PredType::FLOAT_F8E4M3,
       .expected_class = H5T_FLOAT,
       .min_size = 1},
      {.type = &H5::PredType::FLOAT_F8E5M2,
       .expected_class = H5T_FLOAT,
       .min_size = 1},
      {.type = &H5::PredType::FLOAT_F6E2M3,
       .expected_class = H5T_FLOAT,
       .min_size = 1},
      {.type = &H5::PredType::FLOAT_F6E3M2,
       .expected_class = H5T_FLOAT,
       .min_size = 1},
      {.type = &H5::PredType::FLOAT_F4E2M1,
       .expected_class = H5T_FLOAT,
       .min_size = 1},
      {.type = &H5::PredType::UNIX_D32LE,
       .expected_class = H5T_TIME,
       .min_size = 4},
      {.type = &H5::PredType::INTEL_I32,
       .expected_class = H5T_INTEGER,
       .min_size = 4},
      {.type = &H5::PredType::ALPHA_F64,
       .expected_class = H5T_FLOAT,
       .min_size = 8},
      {.type = &H5::PredType::MIPS_U16,
       .expected_class = H5T_INTEGER,
       .min_size = 2},
      {.type = &H5::PredType::NATIVE_INT,
       .expected_class = H5T_INTEGER,
       .min_size = sizeof(int)},
      {.type = &H5::PredType::NATIVE_DOUBLE,
       .expected_class = H5T_FLOAT,
       .min_size = sizeof(double)},
      {.type = &H5::PredType::NATIVE_OPAQUE,
       .expected_class = H5T_OPAQUE,
       .min_size = 1},
      {.type = &H5::PredType::NATIVE_INT_LEAST32,
       .expected_class = H5T_INTEGER,
       .min_size = 4},
      {.type = &H5::PredType::NATIVE_UINT_FAST64,
       .expected_class = H5T_INTEGER,
       .min_size = 8},
  });

  for (const auto& tc : cases) {
    EXPECT_NE(tc.type->getId(), H5I_INVALID_HID);
    EXPECT_EQ(tc.type->getClass(), tc.expected_class);
    EXPECT_GE(tc.type->getSize(), tc.min_size);
  }
  EXPECT_EQ(H5::PredType::PREDTYPE_CONST.getId(), H5I_INVALID_HID);
}

TEST(H5PredTypeTest, GetPredTypesDuplicateCallThrows) {
  ExpectDuplicateGetConstantThrows<H5::PredType, H5::DataTypeIException>(
      "allocated PREDTYPE_CONST_");
}

}  // namespace
}  // namespace hdf5
