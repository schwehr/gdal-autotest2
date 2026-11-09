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
// Unit tests for third_party/hdf5/c++/src/H5FcreatProp.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5FcreatProp.cpp

#include <cstddef>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {

template struct ConstantGetterBypass<ConstantGetterTag<H5::FileCreatPropList>,
                                     &H5::FileCreatPropList::getConstant>;

namespace {

TEST(H5FcreatPropTest, ConstructorsVersionSizesSymkAndIstorek) {
  H5::FileCreatPropList fcpl;
  EXPECT_EQ(fcpl.fromClass(), "FileCreatPropList");
  EXPECT_GE(fcpl.getId(), 0);
  EXPECT_GE(H5::FileCreatPropList::DEFAULT.getId(), 0);

  H5::FileCreatPropList copied(fcpl);
  EXPECT_EQ(copied.getId(), fcpl.getId());

  H5::FileCreatPropList from_id(fcpl.getId());
  EXPECT_TRUE(from_id == fcpl);

  unsigned super = 99;
  unsigned freelist = 99;
  unsigned stab = 99;
  unsigned shhdr = 99;
  fcpl.getVersion(super, freelist, stab, shhdr);
  EXPECT_EQ(super, 0u);

  fcpl.setUserblock(1024);
  EXPECT_EQ(fcpl.getUserblock(), 1024u);

  fcpl.setSizes(8, 8);
  size_t sizeof_addr = 0;
  size_t sizeof_size = 0;
  fcpl.getSizes(sizeof_addr, sizeof_size);
  EXPECT_EQ(sizeof_addr, 8u);
  EXPECT_EQ(sizeof_size, 8u);

  fcpl.setSymk(16, 4);
  unsigned int_k = 0;
  unsigned leaf_k = 0;
  fcpl.getSymk(int_k, leaf_k);
  EXPECT_EQ(int_k, 16u);
  EXPECT_EQ(leaf_k, 4u);

  fcpl.setIstorek(32);
  EXPECT_EQ(fcpl.getIstorek(), 32u);

  fcpl.setFileSpaceStrategy(H5F_FSPACE_STRATEGY_PAGE, true, 1024);
  H5F_fspace_strategy_t strategy = H5F_FSPACE_STRATEGY_FSM_AGGR;
  bool persist = false;
  hsize_t threshold = 0;
  fcpl.getFileSpaceStrategy(strategy, persist, threshold);
  EXPECT_EQ(strategy, H5F_FSPACE_STRATEGY_PAGE);
  EXPECT_TRUE(persist);
  EXPECT_EQ(threshold, 1024u);

  fcpl.setFileSpacePagesize(4096);
  EXPECT_EQ(fcpl.getFileSpacePagesize(), 4096u);
}

TEST(H5FcreatPropTest, ErrorPathsThrowPropListIException) {
  ScopedQuietHdf5Errors quiet;
  H5::FileCreatPropList fcpl;

  EXPECT_THROW(fcpl.setUserblock(100), H5::PropListIException);
  EXPECT_THROW(fcpl.setSizes(3, 4), H5::PropListIException);
  EXPECT_THROW(fcpl.setIstorek(0), H5::PropListIException);
  EXPECT_THROW(fcpl.setFileSpacePagesize(100), H5::PropListIException);

  fcpl.close();
  EXPECT_THROW(fcpl.setSymk(0, 4), H5::PropListIException);
  unsigned super = 0;
  unsigned freelist = 0;
  unsigned stab = 0;
  unsigned shhdr = 0;
  size_t sizeof_addr = 0;
  size_t sizeof_size = 0;
  unsigned int_k = 0;
  unsigned leaf_k = 0;
  H5F_fspace_strategy_t strategy = H5F_FSPACE_STRATEGY_FSM_AGGR;
  bool persist = false;
  hsize_t threshold = 0;

  EXPECT_THROW(fcpl.getVersion(super, freelist, stab, shhdr),
               H5::PropListIException);
  EXPECT_THROW(fcpl.getUserblock(), H5::PropListIException);
  EXPECT_THROW(fcpl.getSizes(sizeof_addr, sizeof_size), H5::PropListIException);
  EXPECT_THROW(fcpl.getSymk(int_k, leaf_k), H5::PropListIException);
  EXPECT_THROW(fcpl.getIstorek(), H5::PropListIException);
  EXPECT_THROW(fcpl.setFileSpaceStrategy(H5F_FSPACE_STRATEGY_PAGE, true, 512),
               H5::PropListIException);
  EXPECT_THROW(fcpl.getFileSpaceStrategy(strategy, persist, threshold),
               H5::PropListIException);
  EXPECT_THROW(fcpl.getFileSpacePagesize(), H5::PropListIException);
}

TEST(H5FcreatPropTest, GetConstantDuplicateCallThrows) {
  ExpectDuplicateGetConstantThrows<H5::FileCreatPropList>();
}

}  // namespace
}  // namespace hdf5
