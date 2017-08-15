// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests for hdf5/src/H5.c.

#include "hdf5.h"

#include "gunit.h"
#include "util/gtl/cleanup.h"

namespace {

TEST(H5Test, GargbareCollect) {
  // Just insure that it does not return a negative number indicating a failure.
  EXPECT_GE(H5garbage_collect(), 0);
}

TEST(H5Test, SetFreeListLimits) {
  // Set all to 0.
  H5set_free_list_limits(0, 0, 0, 0, 0, 0);

  // Memory allocation should not fail.  Basic allocation is NOT tied to
  // these limits.  What actually is limited?

  void *buf = H5allocate_memory(1, false);
  EXPECT_NE(nullptr, buf);
  H5free_memory(buf);

  // Set to unlimited.
  H5set_free_list_limits(-1, -1, -1, -1, -1, -1);
}

// TODO(schwehr): H5_debug_mask.

TEST(H5Test, Version) {
  unsigned majnum = 0;
  unsigned minnum = 0;
  unsigned relnum = 0;
  ASSERT_GE(H5get_libversion(&majnum, &minnum, &relnum), 0);

  // These numbers need to changed with each update.
  EXPECT_EQ(1, majnum);
  EXPECT_EQ(8, minnum);
  EXPECT_EQ(16, relnum);

  ASSERT_GE(H5check_version(majnum, minnum, relnum), 0);

  // It is not possible fail in the same executable as the result is cached.
}

TEST(H5Test, OpenClose) {
  // Check explicit setup of HDF5 library.
  EXPECT_GE(H5open(), 0);

  // TODO(schwehr): Create a death test for H5close();
}

TEST(H5Test, AllocateMemory) {
  // nullptr if size is zero.
  EXPECT_EQ(nullptr, H5allocate_memory(0, false));

  void *buf = H5allocate_memory(1, false);
  ASSERT_NE(nullptr, buf);
  H5free_memory(buf);

  // Zero the new buffer.
  char *buf2 = static_cast<char *>(H5allocate_memory(1, true));
  ASSERT_NE(nullptr, buf2);
  auto cleanup = gtl::MakeCleanup([buf2] { H5free_memory(buf2); });
  EXPECT_EQ('\0', buf2[0]);
}

TEST(H5Test, ResizeMemory) {
  EXPECT_EQ(nullptr, H5resize_memory(nullptr, 0));

  char *buf = static_cast<char *>(H5allocate_memory(2, true));
  ASSERT_NE(nullptr, buf);
  buf[0] = 'a';
  buf = static_cast<char *>(H5resize_memory(buf, 3));
  auto cleanup = gtl::MakeCleanup([buf] { H5free_memory(buf); });
  buf[1] = 'b';
  buf[2] = '\0';
  EXPECT_STREQ("ab", buf);
}

TEST(H5Test, Free) {
  // Safe to call free on a nullptr.
  EXPECT_GE(H5free_memory(nullptr), 0);
}

TEST(H5Test, IsLibraryThreadsafe) {
  hbool_t is_threadsafe = false;
  EXPECT_GE(H5is_library_threadsafe(&is_threadsafe), 0);
  EXPECT_TRUE(is_threadsafe);
}

}  // namespace
