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
// Unit tests for third_party/hdf5/c++/src/H5FloatType.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5FloatType.cpp

#include <cstddef>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5FloatTypeTest, ConstructorsFieldsEbiasNormInpadDecodeAndErrors) {
  H5::FloatType stub;
  EXPECT_EQ(stub.getId(), H5I_INVALID_HID);
  EXPECT_EQ(stub.fromClass(), "FloatType");

  H5::FloatType ft(H5::PredType::IEEE_F32LE);
  H5::FloatType copied(ft);
  EXPECT_TRUE(copied == ft);
  H5::FloatType from_id(ft.getId());
  EXPECT_EQ(from_id.getId(), ft.getId());

  size_t spos = 0;
  size_t epos = 0;
  size_t esize = 0;
  size_t mpos = 0;
  size_t msize = 0;
  ft.getFields(spos, epos, esize, mpos, msize);
  EXPECT_EQ(spos, 31u);
  EXPECT_EQ(esize, 8u);
  EXPECT_EQ(msize, 23u);
  EXPECT_EQ(ft.getEbias(), 127u);

  // Shrink exponent + mantissa so internal bits remain to exercise all inpad
  // modes and custom field/ebias setters.
  ft.setFields(31, 24, 7, 0, 20);
  ft.setEbias(63);
  EXPECT_EQ(ft.getEbias(), 63u);

  H5std_string norm_str;
  ft.setNorm(H5T_NORM_IMPLIED);
  EXPECT_EQ(ft.getNorm(norm_str), H5T_NORM_IMPLIED);
  EXPECT_EQ(norm_str, "H5T_NORM_IMPLIED (0)");

  ft.setNorm(H5T_NORM_MSBSET);
  EXPECT_EQ(ft.getNorm(norm_str), H5T_NORM_MSBSET);
  EXPECT_EQ(norm_str, "H5T_NORM_MSBSET (1)");

  ft.setNorm(H5T_NORM_NONE);
  EXPECT_EQ(ft.getNorm(norm_str), H5T_NORM_NONE);
  EXPECT_EQ(norm_str, "H5T_NORM_NONE (2)");

  H5std_string pad_str;
  ft.setInpad(H5T_PAD_ZERO);
  EXPECT_EQ(ft.getInpad(pad_str), H5T_PAD_ZERO);
  EXPECT_EQ(pad_str, "H5T_PAD_ZERO (0)");

  ft.setInpad(H5T_PAD_ONE);
  EXPECT_EQ(ft.getInpad(pad_str), H5T_PAD_ONE);
  EXPECT_EQ(pad_str, "H5T_PAD_ONE (1)");

  ft.setInpad(H5T_PAD_BACKGROUND);
  EXPECT_EQ(ft.getInpad(pad_str), H5T_PAD_BACKGROUND);
  // Pinned upstream typo in H5FloatType.cpp: "H5T_PAD_BACKGROUD (2)".
  EXPECT_EQ(pad_str, "H5T_PAD_BACKGROUD (2)");
  // File format encoding rejects H5T_PAD_BACKGROUND; reset before
  // commit/encode.
  ft.setInpad(H5T_PAD_ZERO);

  H5::H5File file = CreateInMemoryFile();
  const hsize_t dim = 2;
  H5::DataSpace space(1, &dim);
  H5::DataSet dset =
      file.createDataSet("floats", H5::PredType::IEEE_F32LE, space);
  H5::FloatType from_ds(dset);
  EXPECT_EQ(from_ds.getClass(), H5T_FLOAT);

  ft.commit(file, "committed_f32");
  H5::FloatType opened_c(file, "committed_f32");
  EXPECT_EQ(opened_c.getEbias(), 63u);
  H5::FloatType opened_s(file, H5std_string("committed_f32"));
  EXPECT_EQ(opened_s.getEbias(), 63u);

  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(opened_s.decode(), H5::DataTypeIException);

  ft.encode();
  std::unique_ptr<H5::DataType> decoded(ft.decode());
  ASSERT_NE(decoded.get(), nullptr);
  EXPECT_EQ(decoded->fromClass(), "FloatType");
  EXPECT_TRUE(*decoded == ft);

  EXPECT_THROW((H5::FloatType(H5::DataSet())), H5::DataSetIException);
  EXPECT_THROW(H5::FloatType(file, "missing_float"), H5::DataTypeIException);
  EXPECT_THROW(stub.getFields(spos, epos, esize, mpos, msize),
               H5::DataTypeIException);
  EXPECT_THROW(ft.setFields(100, 0, 0, 0, 0), H5::DataTypeIException);
  EXPECT_THROW(stub.getEbias(), H5::DataTypeIException);
  EXPECT_THROW(stub.setEbias(127), H5::DataTypeIException);
  EXPECT_THROW(stub.getNorm(norm_str), H5::DataTypeIException);
  EXPECT_THROW(ft.setNorm(H5T_NORM_ERROR), H5::DataTypeIException);
  EXPECT_THROW(stub.getInpad(pad_str), H5::DataTypeIException);
  EXPECT_THROW(ft.setInpad(H5T_PAD_ERROR), H5::DataTypeIException);
}

}  // namespace
}  // namespace hdf5
