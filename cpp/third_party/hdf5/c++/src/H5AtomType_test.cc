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
// Unit tests for third_party/hdf5/c++/src/H5AtomType.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5AtomType.cpp

#include <cstddef>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

class AtomTypeSubclass : public H5::AtomType {
 public:
  AtomTypeSubclass() = default;
  explicit AtomTypeSubclass(hid_t existing_id) : H5::AtomType(existing_id) {}
  AtomTypeSubclass(const AtomTypeSubclass& other) : H5::AtomType(other) {}
};

TEST(H5AtomTypeTest, ConstructorsSizeOrderPrecisionOffsetAndPad) {
  AtomTypeSubclass stub;
  EXPECT_EQ(stub.getId(), H5I_INVALID_HID);
  EXPECT_EQ(stub.fromClass(), "AtomType");

  H5::IntType base_int(H5::PredType::NATIVE_INT32);
  AtomTypeSubclass from_id(base_int.getId());
  EXPECT_EQ(from_id.getId(), base_int.getId());

  AtomTypeSubclass copied(from_id);
  EXPECT_EQ(copied.getId(), base_int.getId());

  H5::IntType itype(H5::PredType::NATIVE_INT32);
  itype.setSize(8);
  EXPECT_EQ(itype.getSize(), 8u);

  // Exercise LE, BE, and VAX order string branches in getOrder(H5std_string&).
  H5std_string order_desc;
  itype.setOrder(H5T_ORDER_LE);
  EXPECT_EQ(itype.getOrder(), H5T_ORDER_LE);
  EXPECT_EQ(itype.getOrder(order_desc), H5T_ORDER_LE);
  EXPECT_EQ(order_desc, "Little endian byte ordering (0)");

  itype.setOrder(H5T_ORDER_BE);
  EXPECT_EQ(itype.getOrder(order_desc), H5T_ORDER_BE);
  EXPECT_EQ(order_desc, "Big endian byte ordering (1)");

  itype.setOrder(H5T_ORDER_VAX);
  EXPECT_EQ(itype.getOrder(order_desc), H5T_ORDER_VAX);
  EXPECT_EQ(order_desc, "VAX mixed byte ordering (2)");
  itype.setOrder(H5T_ORDER_LE);

  itype.setPrecision(24);
  EXPECT_EQ(itype.getPrecision(), 24u);

  itype.setOffset(8);
  EXPECT_EQ(itype.getOffset(), 8);

  itype.setPad(H5T_PAD_ONE, H5T_PAD_ZERO);
  H5T_pad_t lsb = H5T_PAD_ERROR;
  H5T_pad_t msb = H5T_PAD_ERROR;
  itype.getPad(lsb, msb);
  EXPECT_EQ(lsb, H5T_PAD_ONE);
  EXPECT_EQ(msb, H5T_PAD_ZERO);

  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(stub.setSize(4), H5::DataTypeIException);
  EXPECT_THROW(stub.getOrder(), H5::DataTypeIException);
  EXPECT_THROW(itype.setOrder(H5T_ORDER_NONE), H5::DataTypeIException);
  EXPECT_THROW(stub.getPrecision(), H5::DataTypeIException);
  EXPECT_THROW(itype.setPrecision(0), H5::DataTypeIException);
  EXPECT_THROW(stub.getOffset(), H5::DataTypeIException);
  EXPECT_THROW(stub.setOffset(8), H5::DataTypeIException);
  EXPECT_THROW(stub.getPad(lsb, msb), H5::DataTypeIException);
  EXPECT_THROW(itype.setPad(H5T_PAD_ERROR, H5T_PAD_ZERO),
               H5::DataTypeIException);
}

}  // namespace
}  // namespace hdf5
