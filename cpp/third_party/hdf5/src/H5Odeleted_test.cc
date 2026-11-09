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
// Tests the constant H5O_MSG_DELETED object header message class descriptor
// defined in third_party/hdf5/src/H5Odeleted.c.
//
// Note: LF=0 in default google3 non-MPI Linux build (H5Odeleted.c defines only
// the constant H5O_MSG_DELETED descriptor table with no executable functions).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Odeleted.c

#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits debug fields from H5O_t /
// H5C_cache_entry_t exposed via H5Opkg.h. Define NDEBUG around the internal
// headers so struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5ODELETED_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5O_FRIEND
#include "third_party/hdf5/src/H5Opkg.h"
}  // extern "C"
#ifdef H5ODELETED_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ODELETED_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

TEST(H5OdeletedTest, DescriptorMetadataAndNullCallbacksMatchSpec) {
  // Ensure the library is initialized before inspecting message class tables.
  ScopedFile file = CreateInMemoryFile("deleted_msg_check.h5");
  ASSERT_TRUE(file.valid());

  const H5O_msg_class_t& msg = H5O_MSG_DELETED[0];
  EXPECT_EQ(msg.id, static_cast<unsigned>(H5O_DELETED_ID));
  EXPECT_STREQ(msg.name, "deleted");
  EXPECT_EQ(msg.native_size, 0U);
  EXPECT_EQ(msg.share_flags, 0U);

  EXPECT_EQ(msg.decode, nullptr);
  EXPECT_EQ(msg.encode, nullptr);
  EXPECT_EQ(msg.copy, nullptr);
  EXPECT_EQ(msg.raw_size, nullptr);
  EXPECT_EQ(msg.reset, nullptr);
  EXPECT_EQ(msg.free, nullptr);
  EXPECT_EQ(msg.del, nullptr);
  EXPECT_EQ(msg.link, nullptr);
  EXPECT_EQ(msg.set_share, nullptr);
  EXPECT_EQ(msg.can_share, nullptr);
  EXPECT_EQ(msg.pre_copy_file, nullptr);
  EXPECT_EQ(msg.copy_file, nullptr);
  EXPECT_EQ(msg.post_copy_file, nullptr);
  EXPECT_EQ(msg.get_crt_index, nullptr);
  EXPECT_EQ(msg.set_crt_index, nullptr);
  EXPECT_EQ(msg.debug, nullptr);

  EXPECT_EQ(H5O_msg_class_g[H5O_DELETED_ID], H5O_MSG_DELETED);
}

TEST(H5OdeletedTest, AttributeDeletionSucceedsWithoutOnDiskDeletedMessage) {
  ScopedFile file = CreateInMemoryFile("deleted_attr_ops.h5");
  ASSERT_TRUE(file.valid());

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  int32_t value = 42;
  ScopedAttr attr(H5Acreate2(file.get(), "temp_attr", H5T_NATIVE_INT32,
                             space.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT32, &value), 0);
  attr.reset();

  EXPECT_GE(H5Adelete(file.get(), "temp_attr"), 0);
  EXPECT_EQ(H5Aexists(file.get(), "temp_attr"), 0);
}

}  // namespace
}  // namespace hdf5
