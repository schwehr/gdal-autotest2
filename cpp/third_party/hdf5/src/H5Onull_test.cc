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
// Tests the constant H5O_MSG_NULL object header message class descriptor
// defined in third_party/hdf5/src/H5Onull.c and object-header workflows that
// produce and preserve null messages.
//
// Note: LF=0 in default google3 non-MPI Linux build (H5Onull.c defines only the
// constant H5O_MSG_NULL descriptor table with no executable functions).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Onull.c

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits debug fields from H5O_t /
// H5C_cache_entry_t exposed via H5Opkg.h. Define NDEBUG around internal
// headers so struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5ONULL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5O_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
}  // extern "C"
#ifdef H5ONULL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5ONULL_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  ScopedApiContext() {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
    }
  }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

TEST(H5OnullTest, DescriptorMetadataAndNullCallbacksMatchSpec) {
  ScopedFile file = CreateInMemoryFile("null_msg_check.h5");
  ASSERT_TRUE(file.valid());

  const H5O_msg_class_t& msg = H5O_MSG_NULL[0];
  EXPECT_EQ(msg.id, static_cast<unsigned>(H5O_NULL_ID));
  EXPECT_STREQ(msg.name, "null");
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

  EXPECT_EQ(H5O_msg_class_g[H5O_NULL_ID], H5O_MSG_NULL);
}

TEST(H5OnullTest, RemovingCommentAndCopyingWithPreserveNullSucceeds) {
  ScopedFile file = CreateInMemoryFile("null_preserve_copy.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ASSERT_GE(H5Oset_comment(grp.get(), "ephemeral comment"), 0);

  H5O_loc_t* oloc = H5O_get_loc(grp.get());
  ASSERT_NE(oloc, nullptr);
  {
    ScopedApiContext ctx;
    // Removing a message converts its slot to H5O_NULL_ID before condensing.
    ASSERT_GE(H5O_msg_remove(oloc, H5O_NAME_ID, 0, false), 0);
    EXPECT_EQ(H5O_msg_exists(oloc, H5O_NAME_ID), 0);
  }
  grp.reset();

  ScopedPlist ocpypl(H5Pcreate(H5P_OBJECT_COPY));
  ASSERT_TRUE(ocpypl.valid());
  ASSERT_GE(H5Pset_copy_object(ocpypl.get(), H5O_COPY_PRESERVE_NULL_FLAG), 0);
  EXPECT_GE(H5Ocopy(file.get(), "/g", file.get(), "/g_copy", ocpypl.get(),
                    H5P_DEFAULT),
            0);
}

}  // namespace
}  // namespace hdf5
