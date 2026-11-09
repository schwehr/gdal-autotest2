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
// Unit tests for third_party/hdf5/c++/src/H5DataType.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5DataType.cpp

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

herr_t XorByteConverter(hid_t /*src_id*/, hid_t /*dst_id*/, H5T_cdata_t* cdata,
                        size_t nelmts, size_t /*buf_stride*/,
                        size_t /*bkg_stride*/, void* buf, void* /*bkg*/,
                        hid_t /*dset_xfer_plist*/) {
  if (cdata->command == H5T_CONV_INIT) {
    cdata->need_bkg = H5T_BKG_NO;
    return 0;
  }
  if (cdata->command == H5T_CONV_CONV) {
    auto* bytes = static_cast<uint8_t*>(buf);
    for (size_t i = 0; i < nelmts * 4; ++i) {
      bytes[i] ^= 0x5Au;
    }
    return 0;
  }
  if (cdata->command == H5T_CONV_FREE) {
    return 0;
  }
  return -1;
}

TEST(H5DataTypeTest, ConstructorsAssignmentAndComparisons) {
  H5::DataType stub;
  EXPECT_EQ(stub.getId(), H5I_INVALID_HID);
  EXPECT_EQ(stub.fromClass(), "DataType");
  EXPECT_FALSE(stub.hasBinaryDesc());

  H5::DataType opaque(H5T_OPAQUE, 8);
  EXPECT_EQ(opaque.getClass(), H5T_OPAQUE);
  EXPECT_EQ(opaque.getSize(), 8u);

  H5::DataType from_existing(opaque.getId());
  EXPECT_EQ(from_existing.getId(), opaque.getId());
  EXPECT_EQ(from_existing.getCounter(), 2);

  H5::DataType copied(opaque);
  EXPECT_TRUE(copied == opaque);
  EXPECT_FALSE(copied != opaque);

  H5::DataType from_pred(H5::PredType::NATIVE_INT32);
  EXPECT_EQ(from_pred.getClass(), H5T_INTEGER);
  EXPECT_TRUE(from_pred != opaque);

  // Assignment changes reference and self-assignment preserves it.
  H5::DataType assigned;
  assigned = from_pred;
  EXPECT_TRUE(assigned == from_pred);
  H5::DataType* self_ptr = &assigned;
  assigned = *self_ptr;
  EXPECT_TRUE(assigned == from_pred);

  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(H5::DataType(H5T_NO_CLASS, 4), H5::DataTypeIException);
  EXPECT_THROW((H5::DataType(H5::PredType::PREDTYPE_CONST)),
               H5::DataTypeIException);
  EXPECT_THROW(static_cast<void>(stub == from_pred), H5::DataTypeIException);
  EXPECT_THROW(stub.getClass(), H5::DataTypeIException);
  EXPECT_THROW(stub.getSize(), H5::DataTypeIException);
  EXPECT_THROW(stub.getSuper(), H5::DataTypeIException);
  EXPECT_THROW(stub.getCreatePlist(), H5::DataTypeIException);
}

TEST(H5DataTypeTest, CommitOpenByNameAndDereference) {
  H5::H5File file = CreateInMemoryFile();

  H5::DataType dt_char(H5::PredType::NATIVE_INT32);
  EXPECT_FALSE(dt_char.committed());
  dt_char.commit(file, "named_i32");
  EXPECT_TRUE(dt_char.committed());

  H5::DataType dt_str(H5::PredType::NATIVE_DOUBLE);
  dt_str.commit(file, H5std_string("named_f64"));
  EXPECT_TRUE(dt_str.committed());

  H5::DataType opened_c(file, "named_i32");
  EXPECT_TRUE(opened_c.committed());
  EXPECT_TRUE(opened_c == H5::PredType::NATIVE_INT32);

  H5::DataType opened_s(file, H5std_string("named_f64"));
  EXPECT_TRUE(opened_s.committed());
  EXPECT_TRUE(opened_s == H5::PredType::NATIVE_DOUBLE);

  hobj_ref_t ref{};
  file.reference(&ref, "/named_i32", H5R_OBJECT);
  H5::DataType deref(file, &ref, H5R_OBJECT);
  EXPECT_TRUE(deref == H5::PredType::NATIVE_INT32);

  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(H5::DataType(file, "missing_dtype"), H5::DataTypeIException);
  EXPECT_THROW(H5::DataType(file, nullptr, H5R_OBJECT), H5::ReferenceException);
  H5::DataType transient(H5::PredType::NATIVE_INT);
  EXPECT_THROW(transient.commit(H5::H5File(), "bad_loc"),
               H5::DataTypeIException);
  EXPECT_THROW(H5::DataType().committed(), H5::DataTypeIException);
}

TEST(H5DataTypeTest, CopyFromTypeAndDataSetAndEncodeDecode) {
  H5::H5File file = CreateInMemoryFile();
  const hsize_t dim = 2;
  H5::DataSpace space(1, &dim);
  H5::DataSet dset =
      file.createDataSet("int_ds", H5::PredType::NATIVE_INT32, space);

  H5::DataType dt;
  dt.copy(dset);
  EXPECT_TRUE(dt == H5::PredType::NATIVE_INT32);

  H5::DataType opaque(H5T_OPAQUE, 4);
  opaque.setTag("raw4");
  EXPECT_EQ(opaque.getTag(), "raw4");
  opaque.setTag(H5std_string("raw4_s"));
  EXPECT_EQ(opaque.getTag(), "raw4_s");

  EXPECT_FALSE(opaque.hasBinaryDesc());
  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(opaque.decode(), H5::DataTypeIException);
  EXPECT_THROW(H5::DataType().encode(), H5::DataTypeIException);

  opaque.encode();
  EXPECT_TRUE(opaque.hasBinaryDesc());
  std::unique_ptr<H5::DataType> decoded(opaque.decode());
  EXPECT_TRUE(*decoded == opaque);
  EXPECT_EQ(decoded->getTag(), "raw4_s");

  // Explicitly closing frees encoded_buf cleanly.
  opaque.close();
  EXPECT_FALSE(opaque.hasBinaryDesc());

  H5::DataType copy_target(H5::PredType::NATIVE_INT);
  EXPECT_THROW(copy_target.copy(H5::DataType()), H5::DataTypeIException);
  EXPECT_THROW(copy_target.copy(H5::DataSet()), H5::DataTypeIException);

  // Locking makes close() fail, exercising close-error handling in copy(),
  // p_setId(), and ~DataType() while keeping ScopedQuietHdf5Errors alive.
  H5::DataType locked_copy(H5::PredType::NATIVE_INT);
  locked_copy.lock();
  EXPECT_THROW(locked_copy.copy(H5::PredType::NATIVE_DOUBLE),
               H5::DataTypeIException);

  H5::DataType locked_ds(H5::PredType::NATIVE_INT);
  locked_ds.lock();
  EXPECT_THROW(locked_ds.copy(dset), H5::DataTypeIException);

  PublicSetId<H5::DataType> locked_sub(H5::PredType::NATIVE_INT);
  locked_sub.lock();
  EXPECT_THROW(locked_sub.CallSetId(H5I_INVALID_HID), H5::DataTypeIException);

  EXPECT_THROW(H5::DataType().lock(), H5::DataTypeIException);
}

TEST(H5DataTypeTest, CustomConversionMetadataQueriesAndErrors) {
  H5::DataType op_src(H5T_OPAQUE, 4);
  op_src.setTag("conv_src");
  H5::DataType op_dst(H5T_OPAQUE, 4);
  op_dst.setTag("conv_dst");

  op_src.registerFunc(H5T_PERS_HARD, "xor_conv", op_dst, XorByteConverter);
  absl::Cleanup cleanup_conv = [&]() {
    try {
      op_src.unregister(H5T_PERS_HARD, "xor_conv", op_dst, XorByteConverter);
    } catch (const H5::DataTypeIException&) {
    }
  };

  H5T_cdata_t* cdata = nullptr;
  H5T_conv_t found = op_src.find(op_dst, &cdata);
  EXPECT_EQ(found, &XorByteConverter);

  std::array<uint8_t, 8> buf = {0x00, 0xFF, 0x12, 0x34, 0xA5, 0x5A, 0x0F, 0xF0};
  op_src.convert(op_dst, 2, buf.data(), nullptr);
  EXPECT_EQ(buf[0], 0x5Au);
  EXPECT_EQ(buf[1], 0xA5u);

  op_src.unregister(H5T_PERS_HARD, H5std_string("xor_conv"), op_dst,
                    XorByteConverter);

  op_src.registerFunc(H5T_PERS_HARD, H5std_string("xor_conv2"), op_dst,
                      XorByteConverter);
  op_src.unregister(H5T_PERS_HARD, "xor_conv2", op_dst, XorByteConverter);

  H5::CompType comp(sizeof(int) + sizeof(float));
  comp.insertMember("i", 0, H5::PredType::NATIVE_INT);
  comp.insertMember("f", sizeof(int), H5::PredType::NATIVE_FLOAT);
  EXPECT_TRUE(comp.detectClass(H5T_INTEGER));
  EXPECT_FALSE(comp.detectClass(H5T_STRING));
  EXPECT_TRUE(H5::DataType::detectClass(H5::PredType::NATIVE_FLOAT, H5T_FLOAT));
  EXPECT_FALSE(
      H5::DataType::detectClass(H5::PredType::NATIVE_FLOAT, H5T_INTEGER));

  H5::EnumType enum_dt(sizeof(int));
  H5::DataType super = enum_dt.getSuper();
  EXPECT_EQ(super.getClass(), H5T_INTEGER);

  H5::StrType vlen_str(H5::PredType::C_S1, H5T_VARIABLE);
  EXPECT_TRUE(vlen_str.isVariableStr());
  EXPECT_FALSE(H5::PredType::NATIVE_INT.isVariableStr());

  H5::PropList plist = H5::PredType::NATIVE_INT.getCreatePlist();
  EXPECT_NE(plist.getId(), H5I_INVALID_HID);

  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(op_src.find(op_dst, &cdata), H5::DataTypeIException);
  EXPECT_THROW(op_src.convert(op_dst, 1, buf.data(), nullptr),
               H5::DataTypeIException);
  EXPECT_THROW(H5::DataType().registerFunc(H5T_PERS_HARD, "bad", op_dst,
                                           XorByteConverter),
               H5::DataTypeIException);
  // H5Tunregister treats src_id <= 0 as a wildcard; pass a closed positive ID.
  H5::DataType closed_dt(H5::PredType::NATIVE_INT);
  const hid_t stale_id = closed_dt.getId();
  closed_dt.close();
  PublicSetId<H5::DataType> bad_src;
  bad_src.CallSetId(stale_id);
  EXPECT_THROW(
      bad_src.unregister(H5T_PERS_HARD, "bad", op_dst, XorByteConverter),
      H5::DataTypeIException);
  EXPECT_THROW(H5::PredType::NATIVE_INT.setTag("not_opaque"),
               H5::DataTypeIException);
  EXPECT_THROW(H5::PredType::NATIVE_INT.getTag(), H5::DataTypeIException);
  EXPECT_THROW(H5::DataType().detectClass(H5T_INTEGER), H5::DataTypeIException);
  EXPECT_THROW(
      H5::DataType::detectClass(H5::PredType::PREDTYPE_CONST, H5T_INTEGER),
      H5::DataTypeIException);
  EXPECT_THROW(H5::DataType().isVariableStr(), H5::DataTypeIException);
}

}  // namespace
}  // namespace hdf5
