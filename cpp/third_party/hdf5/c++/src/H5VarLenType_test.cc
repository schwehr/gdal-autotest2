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
// Unit tests for third_party/hdf5/c++/src/H5VarLenType.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5VarLenType.cpp

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5VarLenTypeTest, ConstructorsNamedTypesDecodeAndErrors) {
  H5::VarLenType stub;
  EXPECT_EQ(stub.getId(), H5I_INVALID_HID);
  EXPECT_EQ(stub.fromClass(), "VarLenType");

  H5::VarLenType by_ref(H5::PredType::NATIVE_INT32);
  EXPECT_EQ(by_ref.getClass(), H5T_VLEN);

  // Exercise pointer overload retained for backwards compatibility.
  H5::VarLenType by_ptr(&H5::PredType::NATIVE_INT32);
  EXPECT_TRUE(by_ptr == by_ref);

  H5::VarLenType copied(by_ref);
  EXPECT_TRUE(copied == by_ref);

  H5::VarLenType from_id(by_ref.getId());
  EXPECT_EQ(from_id.getId(), by_ref.getId());

  H5::H5File file = CreateInMemoryFile();
  by_ref.commit(file, "committed_vlen");
  H5::VarLenType opened_c(file, "committed_vlen");
  EXPECT_TRUE(opened_c == by_ref);
  H5::VarLenType opened_s(file, H5std_string("committed_vlen"));
  EXPECT_TRUE(opened_s == by_ref);

  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(opened_s.decode(), H5::DataTypeIException);

  by_ref.encode();
  std::unique_ptr<H5::DataType> decoded(by_ref.decode());
  ASSERT_NE(decoded.get(), nullptr);
  EXPECT_EQ(decoded->fromClass(), "VarLenType");
  EXPECT_TRUE(*decoded == by_ref);

  H5::DataType invalid_base;
  EXPECT_THROW((H5::VarLenType(invalid_base)), H5::DataTypeIException);
  EXPECT_THROW((H5::VarLenType(&invalid_base)), H5::DataTypeIException);
  EXPECT_THROW(H5::VarLenType(file, "missing_vlen"), H5::DataTypeIException);
}

}  // namespace
}  // namespace hdf5
