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
// Unit tests for third_party/hdf5/c++/src/H5DaccProp.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5DaccProp.cpp

#include <cstddef>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {

template struct ConstantGetterBypass<ConstantGetterTag<H5::DSetAccPropList>,
                                     &H5::DSetAccPropList::getConstant>;

namespace {

TEST(H5DaccPropTest, ConstructorsAndChunkCache) {
  H5::DSetAccPropList dapl;
  EXPECT_EQ(dapl.fromClass(), "DSetAccPropList");
  EXPECT_GE(dapl.getId(), 0);
  EXPECT_GE(H5::DSetAccPropList::DEFAULT.getId(), 0);

  H5::DSetAccPropList copied(dapl);
  EXPECT_EQ(copied.getId(), dapl.getId());

  H5::DSetAccPropList from_id(dapl.getId());
  EXPECT_TRUE(from_id == dapl);

  dapl.setChunkCache(521, 1024 * 1024, 0.75);
  size_t nslots = 0;
  size_t nbytes = 0;
  double w0 = 0.0;
  dapl.getChunkCache(nslots, nbytes, w0);
  EXPECT_EQ(nslots, 521u);
  EXPECT_EQ(nbytes, 1024u * 1024u);
  EXPECT_DOUBLE_EQ(w0, 0.75);

  ScopedQuietHdf5Errors quiet;
  EXPECT_THROW(dapl.setChunkCache(521, 1024, 1.5), H5::PropListIException);
  dapl.close();
  EXPECT_THROW(dapl.getChunkCache(nslots, nbytes, w0), H5::PropListIException);
}

TEST(H5DaccPropTest, GetConstantDuplicateCallThrows) {
  ExpectDuplicateGetConstantThrows<H5::DSetAccPropList>();
}

}  // namespace
}  // namespace hdf5
