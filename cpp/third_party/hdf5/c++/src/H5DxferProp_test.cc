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
// Unit tests for third_party/hdf5/c++/src/H5DxferProp.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5DxferProp.cpp

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {

template struct ConstantGetterBypass<ConstantGetterTag<H5::DSetMemXferPropList>,
                                     &H5::DSetMemXferPropList::getConstant>;

namespace {

H5T_conv_ret_t DummyTypeConvCallback(H5T_conv_except_t, hid_t, hid_t, void*,
                                     void*, void*) {
  return H5T_CONV_HANDLED;
}

void* CustomAlloc(size_t size, void*) { return std::malloc(size); }

void CustomFree(void* mem, void*) { std::free(mem); }

TEST(H5DxferPropTest, ConstructorsBufferTransformAndCallbacks) {
  H5::DSetMemXferPropList dxpl;
  EXPECT_EQ(dxpl.fromClass(), "DSetMemXferPropList");
  EXPECT_GE(dxpl.getId(), 0);
  EXPECT_GE(H5::DSetMemXferPropList::DEFAULT.getId(), 0);
  {
    ScopedQuietHdf5Errors quiet;
    EXPECT_THROW(dxpl.getDataTransform(), H5::PropListIException);
  }

  H5::DSetMemXferPropList copied(dxpl);
  EXPECT_EQ(copied.getId(), dxpl.getId());

  H5::DSetMemXferPropList from_id(dxpl.getId());
  EXPECT_TRUE(from_id == dxpl);

  H5::DSetMemXferPropList expr_dxpl("2*x + 1");
  EXPECT_EQ(expr_dxpl.getDataTransform(), "2*x + 1");

  std::array<char, 32> buf{};
  EXPECT_EQ(expr_dxpl.getDataTransform(buf.data(), buf.size()), 7);
  EXPECT_STREQ(buf.data(), "2*x + 1");

  dxpl.setDataTransform("x + 3");
  EXPECT_EQ(dxpl.getDataTransform(), "x + 3");
  dxpl.setDataTransform(H5std_string("x * 4"));
  EXPECT_EQ(dxpl.getDataTransform(), "x * 4");

  std::array<uint8_t, 4096> tconv_buf{};
  std::array<uint8_t, 4096> bkg_buf{};
  dxpl.setBuffer(tconv_buf.size(), tconv_buf.data(), bkg_buf.data());
  void* out_tconv = nullptr;
  void* out_bkg = nullptr;
  EXPECT_EQ(dxpl.getBuffer(&out_tconv, &out_bkg), tconv_buf.size());
  EXPECT_EQ(out_tconv, tconv_buf.data());
  EXPECT_EQ(out_bkg, bkg_buf.data());

  dxpl.setPreserve(true);
  EXPECT_TRUE(dxpl.getPreserve());
  dxpl.setPreserve(false);
  EXPECT_FALSE(dxpl.getPreserve());

  dxpl.setBtreeRatios(0.2, 0.5, 0.8);
  double left = 0.0;
  double middle = 0.0;
  double right = 0.0;
  dxpl.getBtreeRatios(left, middle, right);
  EXPECT_DOUBLE_EQ(left, 0.2);
  EXPECT_DOUBLE_EQ(middle, 0.5);
  EXPECT_DOUBLE_EQ(right, 0.8);

  int cb_tag = 77;
  dxpl.setTypeConvCB(&DummyTypeConvCallback, &cb_tag);
  H5T_conv_except_func_t out_op = nullptr;
  void* out_data = nullptr;
  dxpl.getTypeConvCB(&out_op, &out_data);
  EXPECT_EQ(out_op, &DummyTypeConvCallback);
  EXPECT_EQ(out_data, &cb_tag);

  int alloc_tag = 1;
  int free_tag = 2;
  dxpl.setVlenMemManager(&CustomAlloc, &alloc_tag, &CustomFree, &free_tag);
  H5MM_allocate_t out_alloc = nullptr;
  void* out_alloc_info = nullptr;
  H5MM_free_t out_free = nullptr;
  void* out_free_info = nullptr;
  dxpl.getVlenMemManager(out_alloc, &out_alloc_info, out_free, &out_free_info);
  EXPECT_EQ(out_alloc, &CustomAlloc);
  EXPECT_EQ(out_free, &CustomFree);
  dxpl.setVlenMemManager();

  // H5Pset_small_data_block_size / H5Pget_small_data_block_size require an
  // H5P_FILE_ACCESS property list; wrap a FileAccPropList ID to cover the
  // DSetMemXferPropList forwarding wrappers.
  H5::FileAccPropList fapl;
  H5::DSetMemXferPropList fapl_dxpl(fapl.getId());
  fapl_dxpl.setSmallDataBlockSize(8192);
  EXPECT_EQ(fapl_dxpl.getSmallDataBlockSize(), 8192u);

  dxpl.setHyperVectorSize(128);
  EXPECT_EQ(dxpl.getHyperVectorSize(), 128u);

  dxpl.setEDCCheck(H5Z_DISABLE_EDC);
  EXPECT_EQ(dxpl.getEDCCheck(), H5Z_DISABLE_EDC);
  dxpl.setEDCCheck(H5Z_ENABLE_EDC);
  EXPECT_EQ(dxpl.getEDCCheck(), H5Z_ENABLE_EDC);
}

TEST(H5DxferPropTest, ErrorPathsThrowPropListIException) {
  ScopedQuietHdf5Errors quiet;
  H5::DSetMemXferPropList dxpl;

  EXPECT_THROW(dxpl.setBuffer(0, nullptr, nullptr), H5::PropListIException);
  EXPECT_THROW(dxpl.setBtreeRatios(-0.1, 0.5, 0.5), H5::PropListIException);
  EXPECT_THROW(dxpl.setDataTransform("??invalid??"), H5::PropListIException);
  EXPECT_THROW(dxpl.setHyperVectorSize(0), H5::PropListIException);

  dxpl.close();
  void* tconv = nullptr;
  void* bkg = nullptr;
  double left = 0.0;
  double middle = 0.0;
  double right = 0.0;
  std::array<char, 16> buf{};
  H5T_conv_except_func_t op = nullptr;
  void* user_data = nullptr;
  H5MM_allocate_t alloc_fn = nullptr;
  void* alloc_info = nullptr;
  H5MM_free_t free_fn = nullptr;
  void* free_info = nullptr;

  EXPECT_THROW(dxpl.getBuffer(&tconv, &bkg), H5::PropListIException);
  EXPECT_THROW(dxpl.setPreserve(true), H5::PropListIException);
  EXPECT_THROW(dxpl.getPreserve(), H5::PropListIException);
  EXPECT_THROW(dxpl.getBtreeRatios(left, middle, right),
               H5::PropListIException);
  EXPECT_THROW(dxpl.getDataTransform(buf.data(), buf.size()),
               H5::PropListIException);
  EXPECT_THROW(dxpl.getDataTransform(), H5::PropListIException);
  EXPECT_THROW(dxpl.setTypeConvCB(&DummyTypeConvCallback, nullptr),
               H5::PropListIException);
  EXPECT_THROW(dxpl.getTypeConvCB(&op, &user_data), H5::PropListIException);
  EXPECT_THROW(dxpl.setVlenMemManager(), H5::PropListIException);
  EXPECT_THROW(
      dxpl.getVlenMemManager(alloc_fn, &alloc_info, free_fn, &free_info),
      H5::PropListIException);
  EXPECT_THROW(dxpl.setSmallDataBlockSize(4096), H5::PropListIException);
  EXPECT_THROW(dxpl.getSmallDataBlockSize(), H5::PropListIException);
  EXPECT_THROW(dxpl.getHyperVectorSize(), H5::PropListIException);
  EXPECT_THROW(dxpl.setEDCCheck(H5Z_DISABLE_EDC), H5::PropListIException);
  EXPECT_THROW(dxpl.getEDCCheck(), H5::PropListIException);
}

TEST(H5DxferPropTest, GetConstantDuplicateCallThrows) {
  ExpectDuplicateGetConstantThrows<H5::DSetMemXferPropList>();
}

}  // namespace
}  // namespace hdf5
