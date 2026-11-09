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
// Tests compile-time and registry sentinels for the "bogus" object header
// message class (third_party/hdf5/src/H5Obogus.c). Note that H5Obogus.c is
// completely guarded by `#ifdef H5O_ENABLE_BOGUS`, which is disabled in the
// default google3 non-MPI Linux build (LF=0 in default coverage).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Obogus.c

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// Match `:hdf5` NDEBUG layout when inspecting internal H5O tables.
#ifndef NDEBUG
#define H5OBOGUS_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5O_FRIEND
#include "third_party/hdf5/src/H5Opkg.h"
}  // extern "C"
#ifdef H5OBOGUS_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OBOGUS_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

TEST(H5ObogusTest, BogusMessageSupportDisabledInDefaultBuild) {
  // H5O_ENABLE_BOGUS is commented out in H5Oprivate.h in the default build
  // (LF=0 in default google3 non-MPI Linux build).
#ifdef H5O_ENABLE_BOGUS
  ADD_FAILURE() << "H5O_ENABLE_BOGUS unexpectedly defined";
#else
  EXPECT_EQ(H5O_BOGUS_VALID_ID, 0x0009u);
  EXPECT_EQ(H5O_BOGUS_INVALID_ID, 0x001bu);
  EXPECT_EQ(H5O_BOGUS_INVALID_ID, static_cast<unsigned>(H5O_MSG_TYPES));
  EXPECT_LT(H5O_BOGUS_VALID_ID, static_cast<unsigned>(H5O_MSG_TYPES));
  EXPECT_EQ(H5O_msg_class_g[H5O_BOGUS_VALID_ID], nullptr);
  EXPECT_NE(H5O_msg_class_g[H5O_ATTR_ID], nullptr);

  // Confirm valid shareable messages report normally via H5O package helpers.
  EXPECT_GE(H5O_msg_can_share_in_ohdr(H5O_ATTR_ID), 0);
#endif
}

}  // namespace
}  // namespace hdf5
