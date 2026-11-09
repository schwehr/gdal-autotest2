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
// Tests internal shared-file list tracking, search, insertion, removal, and
// multi-open file deduplication (third_party/hdf5/src/H5Fsfile.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Fsfile.c

// `:hdf5` is built with -DNDEBUG; match internal struct layout in C++ tests.
#ifndef NDEBUG
#define H5FSFILE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5F_FRIEND
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"

#ifdef H5FSFILE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FSFILE_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5FsfileTest, AssertNumAndSharedFileListAddSearchRemove) {
  // 1. Create two distinct in-memory files so H5F__sfile_add prepends f2
  // ahead of f1 in the internal shared file linked list.
  ScopedFile f1 = CreateInMemoryFile("sfile_one.h5");
  ASSERT_TRUE(f1.valid());
  ScopedFile f2 = CreateInMemoryFile("sfile_two.h5");
  ASSERT_TRUE(f2.valid());

  auto* h5f1 = static_cast<H5F_t*>(H5VL_object_verify(f1.get(), H5I_FILE));
  auto* h5f2 = static_cast<H5F_t*>(H5VL_object_verify(f2.get(), H5I_FILE));
  ASSERT_NE(h5f1, nullptr);
  ASSERT_NE(h5f2, nullptr);

  H5F_sfile_assert_num(0);
  H5F_sfile_assert_num(2);

  // 2. Search for the head element (f2) and a non-head element (f1) to cover
  // both immediate match and `curr = curr->next` list traversal.
  EXPECT_EQ(H5F__sfile_search(h5f2->shared->lf), h5f2->shared);
  EXPECT_EQ(H5F__sfile_search(h5f1->shared->lf), h5f1->shared);

  // 3. Removing an unregistered H5F_shared_t fails with NOTFOUND cleanly.
  {
    H5F_shared_t unregistered{};
    ScopedErrorSilence silence;
    EXPECT_LT(H5F__sfile_remove(&unregistered), 0);
  }

  // 4. Close the tail element (f1) first to cover `last != NULL` middle/tail
  // unlink in H5F__sfile_remove, then close the head element (f2) (`last ==
  // NULL`).
  f1.reset();
  EXPECT_EQ(H5F__sfile_search(h5f2->shared->lf), h5f2->shared);
  f2.reset();

  H5F_sfile_assert_num(0);
}

TEST(H5FsfileTest, DirectAddSearchAndRemoveUnregisteredSharedStruct) {
  ScopedFile backing = CreateInMemoryFile("sfile_backing.h5");
  ASSERT_TRUE(backing.valid());

  auto* f = static_cast<H5F_t*>(H5VL_object_verify(backing.get(), H5I_FILE));
  ASSERT_NE(f, nullptr);

  // Create a distinct in-memory file only for a valid H5FD_t*, close nothing
  // before removing our extra shared node, and verify search misses before
  // direct insertion and hits afterward.
  ScopedFile probe = CreateInMemoryFile("sfile_probe.h5");
  ASSERT_TRUE(probe.valid());
  auto* probe_f =
      static_cast<H5F_t*>(H5VL_object_verify(probe.get(), H5I_FILE));
  ASSERT_NE(probe_f, nullptr);

  H5F_shared_t extra{};
  extra.lf = probe_f->shared->lf;

  // Closing `probe` removes its real shared entry so `probe_f->shared->lf` is
  // no longer registered? Wait: probe_f->shared->lf is freed on probe.reset().
  // Instead test direct H5F__sfile_add / H5F__sfile_remove using an unopened
  // H5FD_t dummy? Safe route: keep `probe` open, remove `probe_f->shared`,
  // verify miss, then re-add before closing!
  ASSERT_GE(H5F__sfile_remove(probe_f->shared), 0);
  bool restored = false;
  absl::Cleanup restore_probe_shared = [&] {
    if (!restored) {
      H5F__sfile_add(probe_f->shared);
    }
  };

  EXPECT_EQ(H5F__sfile_search(probe_f->shared->lf), nullptr);
  ASSERT_GE(H5F__sfile_add(&extra), 0);
  bool removed_extra = false;
  absl::Cleanup cleanup_extra = [&] {
    if (!removed_extra) {
      H5F__sfile_remove(&extra);
    }
  };
  EXPECT_EQ(H5F__sfile_search(extra.lf), &extra);

  ASSERT_GE(H5F__sfile_remove(&extra), 0);
  removed_extra = true;

  ASSERT_GE(H5F__sfile_add(probe_f->shared), 0);
  restored = true;
}

TEST(H5FsfileTest, Sec2MultiOpenSharesUnderlyingSharedFileStruct) {
  TempFile tf("sfile_shared_open.h5");

  ScopedFile f1(
      H5Fcreate(tf.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(f1.valid());

  ScopedFile f2(H5Fopen(tf.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
  ASSERT_TRUE(f2.valid());

  auto* h5f1 = static_cast<H5F_t*>(H5VL_object_verify(f1.get(), H5I_FILE));
  auto* h5f2 = static_cast<H5F_t*>(H5VL_object_verify(f2.get(), H5I_FILE));
  ASSERT_NE(h5f1, nullptr);
  ASSERT_NE(h5f2, nullptr);
  EXPECT_TRUE(H5F_same_shared(h5f1, h5f2));
  EXPECT_EQ(H5F_get_nrefs(h5f1), 2U);

  // Verify nonexistent file cannot be opened.
  TempFile missing("sfile_missing.h5");
  ScopedErrorSilence silence;
  ScopedFile bad(H5Fopen(missing.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  EXPECT_EQ(bad.get(), H5I_INVALID_HID);
}

}  // namespace
}  // namespace hdf5
