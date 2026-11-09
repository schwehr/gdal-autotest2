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
// Unit tests for third_party/hdf5/c++/src/H5DcreatProp.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5DcreatProp.cpp

#include <array>
#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {

template struct ConstantGetterBypass<ConstantGetterTag<H5::DSetCreatPropList>,
                                     &H5::DSetCreatPropList::getConstant>;

namespace {

TEST(H5DcreatPropTest, ConstructorsLayoutChunksFillExternalAndVirtual) {
  H5::DSetCreatPropList dcpl;
  EXPECT_EQ(dcpl.fromClass(), "DSetCreatPropList");
  EXPECT_GE(dcpl.getId(), 0);
  EXPECT_GE(H5::DSetCreatPropList::DEFAULT.getId(), 0);

  H5::DSetCreatPropList copied(dcpl);
  EXPECT_EQ(copied.getId(), dcpl.getId());

  H5::DSetCreatPropList from_id(dcpl.getId());
  EXPECT_TRUE(from_id == dcpl);

  dcpl.setLayout(H5D_COMPACT);
  EXPECT_EQ(dcpl.getLayout(), H5D_COMPACT);
  dcpl.setLayout(H5D_CONTIGUOUS);
  EXPECT_EQ(dcpl.getLayout(), H5D_CONTIGUOUS);

  constexpr std::array<hsize_t, 2> kChunkDims = {4, 8};
  dcpl.setChunk(2, kChunkDims.data());
  EXPECT_EQ(dcpl.getLayout(), H5D_CHUNKED);
  std::array<hsize_t, 2> out_chunks = {0, 0};
  EXPECT_EQ(dcpl.getChunk(2, out_chunks.data()), 2);
  EXPECT_EQ(out_chunks[0], 4u);
  EXPECT_EQ(out_chunks[1], 8u);

  constexpr int32_t kFillVal = 42;
  dcpl.setFillValue(H5::PredType::NATIVE_INT32, &kFillVal);
  EXPECT_EQ(dcpl.isFillValueDefined(), H5D_FILL_VALUE_USER_DEFINED);
  int32_t out_fill = 0;
  dcpl.getFillValue(H5::PredType::NATIVE_INT32, &out_fill);
  EXPECT_EQ(out_fill, 42);

  dcpl.setAllocTime(H5D_ALLOC_TIME_EARLY);
  EXPECT_EQ(dcpl.getAllocTime(), H5D_ALLOC_TIME_EARLY);
  dcpl.setFillTime(H5D_FILL_TIME_ALLOC);
  EXPECT_EQ(dcpl.getFillTime(), H5D_FILL_TIME_ALLOC);

  H5::DSetCreatPropList ext_dcpl;
  ext_dcpl.setExternal("ext_slice.bin", 64, 1024);
  EXPECT_EQ(ext_dcpl.getExternalCount(), 1);
  std::array<char, 32> ext_name{};
  HDoff_t ext_offset = -1;
  hsize_t ext_size = 0;
  ext_dcpl.getExternal(0, ext_name.size(), ext_name.data(), ext_offset,
                       ext_size);
  EXPECT_STREQ(ext_name.data(), "ext_slice.bin");
  EXPECT_EQ(ext_offset, 64);
  EXPECT_EQ(ext_size, 1024u);

  H5::DSetCreatPropList virt_dcpl;
  constexpr std::array<hsize_t, 1> kDims = {10};
  H5::DataSpace vspace(1, kDims.data());
  H5::DataSpace sspace(1, kDims.data());
  virt_dcpl.setVirtual(vspace, "src_a.h5", "/dset_a", sspace);
  virt_dcpl.setVirtual(vspace, H5std_string("src_b.h5"),
                       H5std_string("/dset_b"), sspace);
  EXPECT_EQ(virt_dcpl.getLayout(), H5D_VIRTUAL);
}

TEST(H5DcreatPropTest, FiltersPipelineAndModifications) {
  H5::DSetCreatPropList dcpl;
  constexpr std::array<hsize_t, 2> kChunkDims = {8, 8};
  dcpl.setChunk(2, kChunkDims.data());

  dcpl.setShuffle();
  dcpl.setDeflate(6);
  dcpl.setFletcher32();
  dcpl.setNbit();

  EXPECT_EQ(dcpl.getNfilters(), 4);
  EXPECT_TRUE(dcpl.allFiltersAvail());

  unsigned int flags = 0;
  size_t cd_nelmts = 4;
  std::array<unsigned int, 4> cd_values = {};
  std::array<char, 64> filter_name = {};
  unsigned int filter_config = 0;
  EXPECT_EQ(
      dcpl.getFilter(1, flags, cd_nelmts, cd_values.data(), filter_name.size(),
                     filter_name.data(), filter_config),
      H5Z_FILTER_DEFLATE);
  EXPECT_EQ(cd_values[0], 6u);

  cd_nelmts = 4;
  dcpl.getFilterById(H5Z_FILTER_DEFLATE, flags, cd_nelmts, cd_values.data(),
                     filter_name.size(), filter_name.data(), filter_config);
  EXPECT_EQ(cd_values[0], 6u);

  constexpr unsigned int kNewLevel = 3;
  dcpl.modifyFilter(H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL, 1, &kNewLevel);
  dcpl.removeFilter(H5Z_FILTER_NBIT);
  EXPECT_EQ(dcpl.getNfilters(), 3);

  constexpr unsigned int kCustomCd = 2;
  dcpl.removeFilter(H5Z_FILTER_ALL);
  dcpl.setFilter(H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL, 1, &kCustomCd);
  EXPECT_EQ(dcpl.getNfilters(), 1);

  ScopedQuietHdf5Errors quiet;
  if (H5Zfilter_avail(H5Z_FILTER_SZIP) > 0) {
    dcpl.setSzip(H5_SZIP_NN_OPTION_MASK, 8);
    EXPECT_THROW(dcpl.setSzip(H5_SZIP_NN_OPTION_MASK, 3),
                 H5::PropListIException);
  } else {
    EXPECT_THROW(dcpl.setSzip(H5_SZIP_NN_OPTION_MASK, 8),
                 H5::PropListIException);
  }
}

TEST(H5DcreatPropTest, ErrorPathsThrowPropListIException) {
  ScopedQuietHdf5Errors quiet;
  H5::DSetCreatPropList dcpl;

  std::array<hsize_t, 2> dims = {4, 4};
  EXPECT_THROW(dcpl.setChunk(0, dims.data()), H5::PropListIException);
  EXPECT_THROW(dcpl.getChunk(2, dims.data()), H5::PropListIException);
  EXPECT_THROW(dcpl.setDeflate(-1), H5::PropListIException);
  EXPECT_THROW(dcpl.setDeflate(15), H5::PropListIException);

  unsigned int flags = 0;
  size_t cd_nelmts = 1;
  unsigned int cd_val = 0;
  std::array<char, 16> name{};
  unsigned int filter_cfg = 0;
  EXPECT_THROW(dcpl.getFilter(-1, flags, cd_nelmts, &cd_val, name.size(),
                              name.data(), filter_cfg),
               H5::PropListIException);
  EXPECT_THROW(dcpl.getFilter(0, flags, cd_nelmts, &cd_val, name.size(),
                              name.data(), filter_cfg),
               H5::PropListIException);
  EXPECT_THROW(dcpl.getFilterById(H5Z_FILTER_DEFLATE, flags, cd_nelmts, &cd_val,
                                  name.size(), name.data(), filter_cfg),
               H5::PropListIException);
  EXPECT_THROW(dcpl.setFilter(H5Z_FILTER_ERROR, 0, 0, nullptr),
               H5::PropListIException);

  HDoff_t ext_offset = 0;
  hsize_t ext_size = 0;
  EXPECT_THROW(
      dcpl.getExternal(0, name.size(), name.data(), ext_offset, ext_size),
      H5::PropListIException);

  dcpl.close();
  int32_t val = 1;
  H5::DataSpace space(H5S_SCALAR);
  EXPECT_THROW(dcpl.modifyFilter(H5Z_FILTER_DEFLATE, 0, 1, &cd_val),
               H5::PropListIException);
  EXPECT_THROW(dcpl.removeFilter(H5Z_FILTER_DEFLATE), H5::PropListIException);
  EXPECT_THROW(dcpl.setLayout(H5D_CONTIGUOUS), H5::PropListIException);
  EXPECT_THROW(dcpl.getLayout(), H5::PropListIException);
  EXPECT_THROW(dcpl.setFillValue(H5::PredType::NATIVE_INT32, &val),
               H5::PropListIException);
  EXPECT_THROW(dcpl.getFillValue(H5::PredType::NATIVE_INT32, &val),
               H5::PropListIException);
  EXPECT_THROW(dcpl.isFillValueDefined(), H5::PropListIException);
  EXPECT_THROW(dcpl.getNfilters(), H5::PropListIException);
  EXPECT_THROW(dcpl.allFiltersAvail(), H5::PropListIException);
  EXPECT_THROW(dcpl.setShuffle(), H5::PropListIException);
  EXPECT_THROW(dcpl.setFletcher32(), H5::PropListIException);
  EXPECT_THROW(dcpl.setNbit(), H5::PropListIException);
  EXPECT_THROW(dcpl.getAllocTime(), H5::PropListIException);
  EXPECT_THROW(dcpl.setAllocTime(H5D_ALLOC_TIME_EARLY), H5::PropListIException);
  EXPECT_THROW(dcpl.getFillTime(), H5::PropListIException);
  EXPECT_THROW(dcpl.setFillTime(H5D_FILL_TIME_NEVER), H5::PropListIException);
  EXPECT_THROW(dcpl.setExternal("bad.bin", 0, 16), H5::PropListIException);
  EXPECT_THROW(dcpl.getExternalCount(), H5::PropListIException);
  EXPECT_THROW(dcpl.setVirtual(space, "bad.h5", "d", space),
               H5::PropListIException);
}

TEST(H5DcreatPropTest, GetConstantDuplicateCallThrows) {
  ExpectDuplicateGetConstantThrows<H5::DSetCreatPropList>();
}

}  // namespace
}  // namespace hdf5
