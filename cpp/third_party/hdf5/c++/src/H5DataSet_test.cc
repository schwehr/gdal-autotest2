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
// Unit tests for third_party/hdf5/c++/src/H5DataSet.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5DataSet.cpp

#include <array>
#include <cstdint>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

herr_t SumIntElem(void* elem, hid_t /*type_id*/, unsigned /*ndim*/,
                  const hsize_t* /*point*/, void* operator_data) {
  auto* sum = static_cast<int32_t*>(operator_data);
  *sum += *static_cast<const int32_t*>(elem);
  return 0;
}

herr_t FailElem(void* /*elem*/, hid_t /*type_id*/, unsigned /*ndim*/,
                const hsize_t* /*point*/, void* /*operator_data*/) {
  return -1;
}

TEST(H5DataSetTest, ConstructorsAssignmentDereferenceAndRawIo) {
  H5::DataSet stub;
  EXPECT_EQ(stub.getId(), H5I_INVALID_HID);
  EXPECT_EQ(stub.fromClass(), "DataSet");

  H5::H5File file = CreateInMemoryFile();
  const hsize_t dim = 4;
  H5::DataSpace space(1, &dim);
  H5::DataSet ds =
      file.createDataSet("nums", H5::PredType::NATIVE_INT32, space);

  const std::array<int32_t, 4> in_vals = {10, 20, 30, 40};
  ds.write(in_vals.data(), H5::PredType::NATIVE_INT32);
  file.flush(H5F_SCOPE_LOCAL);

  std::array<int32_t, 4> out_vals{};
  ds.read(out_vals.data(), H5::PredType::NATIVE_INT32);
  EXPECT_THAT(out_vals, ElementsAre(10, 20, 30, 40));

  H5D_space_status_t status = H5D_SPACE_STATUS_ERROR;
  ds.getSpaceStatus(status);
  EXPECT_EQ(status, H5D_SPACE_STATUS_ALLOCATED);
  EXPECT_EQ(ds.getSpace().getSimpleExtentNpoints(), 4);
  EXPECT_NE(ds.getCreatePlist().getId(), H5I_INVALID_HID);
  EXPECT_NE(ds.getAccessPlist().getId(), H5I_INVALID_HID);
  EXPECT_EQ(ds.getInMemDataSize(), 4 * sizeof(int32_t));
  EXPECT_EQ(ds.getStorageSize(), 4 * sizeof(int32_t));
  EXPECT_NE(ds.getOffset(), HADDR_UNDEF);

  int32_t sum = 0;
  EXPECT_EQ(ds.iterateElems(out_vals.data(), H5::PredType::NATIVE_INT32, space,
                            SumIntElem, &sum),
            0);
  EXPECT_EQ(sum, 100);

  // Copy constructor, id constructor, assignment, and self-assignment.
  H5::DataSet copied(ds);
  EXPECT_EQ(copied.getId(), ds.getId());
  H5::DataSet from_id(ds.getId());
  EXPECT_EQ(from_id.getId(), ds.getId());
  H5::DataSet assigned;
  assigned = ds;
  EXPECT_EQ(assigned.getId(), ds.getId());
  H5::DataSet* self_ptr = &assigned;
  assigned = *self_ptr;
  EXPECT_EQ(assigned.getId(), ds.getId());
  assigned.close();
  EXPECT_EQ(assigned.getId(), H5I_INVALID_HID);

  // Dereference from H5Location and from Attribute.
  hobj_ref_t ref{};
  file.reference(&ref, "/nums", H5R_OBJECT);
  H5::DataSet deref_loc(file, &ref, H5R_OBJECT);
  EXPECT_EQ(deref_loc.getInMemDataSize(), 4 * sizeof(int32_t));

  H5::DataSpace scalar(H5S_SCALAR);
  H5::Attribute ref_attr =
      file.createAttribute("ds_ref", H5::PredType::STD_REF_OBJ, scalar);
  ref_attr.write(H5::PredType::STD_REF_OBJ, &ref);
  hobj_ref_t read_ref{};
  ref_attr.read(H5::PredType::STD_REF_OBJ, &read_ref);
  H5::DataSet deref_attr(ref_attr, &read_ref, H5R_OBJECT);
  EXPECT_EQ(deref_attr.getInMemDataSize(), 4 * sizeof(int32_t));

  PublicSetId<H5::DataSet> sub;
  sub.CallSetId(H5I_INVALID_HID);
  EXPECT_EQ(sub.getId(), H5I_INVALID_HID);
}

TEST(H5DataSetTest, StringsExtendFillMemBufVlenAndErrors) {
  H5::H5File file = CreateInMemoryFile();
  H5::DataSpace scalar(H5S_SCALAR);

  // Fixed-length string dataset.
  H5::StrType fixed_str(H5::PredType::C_S1, 12);
  H5::DataSet fstr_ds = file.createDataSet("fstr_ds", fixed_str, scalar);
  const H5std_string fstr_in("fixed_ds_str");
  fstr_ds.write(fstr_in, fixed_str);
  H5std_string fstr_out;
  fstr_ds.read(fstr_out, fixed_str);
  EXPECT_EQ(fstr_out, "fixed_ds_str");

  // Variable-length string dataset.
  H5::StrType vlen_str(H5::PredType::C_S1, H5T_VARIABLE);
  H5::DataSet vstr_ds = file.createDataSet("vstr_ds", vlen_str, scalar);
  const H5std_string vstr_in("variable_ds_payload");
  vstr_ds.write(vstr_in, vlen_str);
  H5std_string vstr_out;
  vstr_ds.read(vstr_out, vlen_str);
  EXPECT_EQ(vstr_out, "variable_ds_payload");

  // Extendable chunked dataset + fillMemBuf overloads.
  const hsize_t init_dim = 2;
  const hsize_t max_dim = H5S_UNLIMITED;
  H5::DataSpace ext_space(1, &init_dim, &max_dim);
  H5::DSetCreatPropList dcpl;
  const hsize_t chunk_dim = 2;
  dcpl.setChunk(1, &chunk_dim);
  H5::DataSet ext_ds =
      file.createDataSet("ext_ds", H5::PredType::NATIVE_INT32, ext_space, dcpl);
  const hsize_t grown_dim = 5;
  ext_ds.extend(&grown_dim);
  EXPECT_EQ(ext_ds.getSpace().getSimpleExtentNpoints(), 5);

  H5::DataSpace mem_space = ext_ds.getSpace();
  std::array<int32_t, 5> filled = {1, 2, 3, 4, 5};
  ext_ds.fillMemBuf(filled.data(), H5::PredType::NATIVE_INT32, mem_space);
  EXPECT_THAT(filled, ElementsAre(0, 0, 0, 0, 0));
  const int32_t fill_val = 77;
  ext_ds.fillMemBuf(&fill_val, H5::PredType::NATIVE_INT32, filled.data(),
                    H5::PredType::NATIVE_INT32, mem_space);
  EXPECT_THAT(filled, ElementsAre(77, 77, 77, 77, 77));

  // Variable-length sequence dataset for getVlenBufSize and both vlenReclaim
  // static overloads.
  H5::VarLenType vlen_int(H5::PredType::NATIVE_INT32);
  const hsize_t vdim = 2;
  H5::DataSpace vspace(1, &vdim);
  H5::DataSet vlen_ds = file.createDataSet("vlen_int_ds", vlen_int, vspace);
  std::array<int32_t, 2> seq0 = {10, 20};
  std::array<int32_t, 3> seq1 = {30, 40, 50};
  std::array<hvl_t, 2> wdata = {
      hvl_t{.len = seq0.size(), .p = seq0.data()},
      hvl_t{.len = seq1.size(), .p = seq1.data()},
  };
  vlen_ds.write(wdata.data(), vlen_int);
  EXPECT_EQ(vlen_ds.getVlenBufSize(vlen_int, vspace), 5 * sizeof(int32_t));

  std::array<hvl_t, 2> rdata1{};
  vlen_ds.read(rdata1.data(), vlen_int);
  EXPECT_EQ(rdata1[0].len, 2u);
  EXPECT_EQ(rdata1[1].len, 3u);
  H5::DataSet::vlenReclaim(vlen_int, vspace, H5::DSetMemXferPropList::DEFAULT,
                           rdata1.data());

  std::array<hvl_t, 2> rdata2{};
  vlen_ds.read(rdata2.data(), vlen_int);
  H5::DataSet::vlenReclaim(rdata2.data(), vlen_int, vspace);

  // Error paths throwing H5::DataSetIException / H5::ReferenceException.
  ScopedQuietHdf5Errors quiet;
  H5::DataSet closed;
  std::array<int32_t, 2> dummy{};
  EXPECT_THROW((H5::DataSet(file, nullptr, H5R_OBJECT)),
               H5::ReferenceException);
  EXPECT_THROW((H5::DataSet(H5::Attribute(), nullptr, H5R_OBJECT)),
               H5::ReferenceException);
  EXPECT_THROW(closed.getSpace(), H5::DataSetIException);
  EXPECT_THROW(closed.getCreatePlist(), H5::DataSetIException);
  EXPECT_THROW(closed.getAccessPlist(), H5::DataSetIException);
  EXPECT_THROW(closed.getInMemDataSize(), H5::DataSetIException);
  EXPECT_THROW(closed.getOffset(), H5::DataSetIException);
  EXPECT_THROW(ext_ds.getOffset(), H5::DataSetIException);
  H5D_space_status_t err_status = H5D_SPACE_STATUS_ERROR;
  EXPECT_THROW(closed.getSpaceStatus(err_status), H5::DataSetIException);
  EXPECT_THROW(closed.getVlenBufSize(vlen_int, vspace), H5::DataSetIException);
  EXPECT_THROW(
      H5::DataSet::vlenReclaim(H5::DataType(), vspace,
                               H5::DSetMemXferPropList::DEFAULT, rdata1.data()),
      H5::DataSetIException);
  EXPECT_THROW(H5::DataSet::vlenReclaim(rdata1.data(), H5::DataType(), vspace),
               H5::DataSetIException);
  EXPECT_THROW(closed.read(dummy.data(), H5::PredType::NATIVE_INT32),
               H5::DataSetIException);
  EXPECT_THROW(closed.read(dummy.data(), H5::DataType()),
               H5::DataSetIException);
  EXPECT_THROW(closed.read(fstr_out, H5::DataType()), H5::DataSetIException);
  EXPECT_THROW(closed.read(fstr_out, fixed_str), H5::DataSetIException);
  EXPECT_THROW(closed.read(vstr_out, vlen_str), H5::DataSetIException);
  EXPECT_THROW(closed.write(dummy.data(), H5::PredType::NATIVE_INT32),
               H5::DataSetIException);
  EXPECT_THROW(closed.write(fstr_in, H5::DataType()), H5::DataSetIException);
  EXPECT_THROW(closed.write(fstr_in, fixed_str), H5::DataSetIException);
  EXPECT_THROW(closed.iterateElems(filled.data(), H5::PredType::NATIVE_INT32,
                                   mem_space, FailElem, nullptr),
               H5::DataSetIException);
  EXPECT_THROW(closed.extend(&grown_dim), H5::DataSetIException);
  EXPECT_THROW(closed.fillMemBuf(&fill_val, H5::PredType::NATIVE_INT32, nullptr,
                                 H5::PredType::NATIVE_INT32, mem_space),
               H5::DataSetIException);
  EXPECT_THROW(
      closed.fillMemBuf(nullptr, H5::PredType::NATIVE_INT32, mem_space),
      H5::DataSetIException);
  EXPECT_THROW(closed.getDataType(), H5::DataTypeIException);
}

}  // namespace
}  // namespace hdf5
