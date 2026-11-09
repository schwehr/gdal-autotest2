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
// Tests internal open-object tracking (`open_objs` skip list) and per-file-top
// object reference counts (`obj_count` skip list), plus public group/dataset/
// datatype sharing, H5Freopen handle isolation, and deferred deletion upon
// unlinking open objects (third_party/hdf5/src/H5FO.c). Note: built in google3
// non-MPI Linux configuration (`-DNDEBUG`, without `H5_HAVE_PARALLEL`).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FO.c

#include <array>
#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match internal struct layouts across
// H5private.h and H5Fpkg.h before entering extern "C".
#ifndef NDEBUG
#define H5FO_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5F_FRIEND
#include "third_party/hdf5/src/H5FOprivate.h"
// H5Fpkg.h declares `H5F__efc_open(bool try, ...)`, which collides with the
// C++ `try` keyword.
#pragma push_macro("try")
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#undef try
#define try h5fpkg_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#undef try
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5FO_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FO_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

H5F_t* GetFileStruct(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

haddr_t GetObjectAddr(hid_t obj_id) {
  H5O_info2_t info{};
  if (H5Oget_info3(obj_id, &info, H5O_INFO_BASIC) < 0) {
    return HADDR_UNDEF;
  }
  haddr_t addr = HADDR_UNDEF;
  if (H5VLnative_token_to_addr(obj_id, info.token, &addr) < 0) {
    return HADDR_UNDEF;
  }
  return addr;
}

TEST(H5FOTest, CreateInsertQueryMarkDeleteAndDestroyOpenObjectSet) {
  ASSERT_GE(H5open(), 0);

  H5F_shared_t shared{};
  H5F_t file{};
  file.shared = &shared;

  ASSERT_GE(H5FO_create(&file), 0);
  ASSERT_NE(shared.open_objs, nullptr);

  constexpr auto kAddrs =
      std::to_array<haddr_t>({0x0, 0x100, 0x2000, 0x40000000ULL});
  std::array<int, kAddrs.size()> dummy_objs{10, 20, 30, 40};
  std::array<bool, kAddrs.size()> inserted{};

  // Ensure every inserted entry and the container itself are torn down even on
  // early ASSERT failure. Clear `deleted` first so H5FO_delete does not call
  // H5O_delete on the synthetic H5F_t without a cache/VFD.
  absl::Cleanup cleanup = [&] {
    if (shared.open_objs != nullptr) {
      for (size_t i = 0; i < kAddrs.size(); ++i) {
        if (inserted[i]) {
          H5FO_mark(&file, kAddrs[i], false);
          H5FO_delete(&file, kAddrs[i]);
        }
      }
      H5FO_dest(&file);
    }
  };

  // Empty container queries and error paths.
  EXPECT_EQ(H5FO_opened(&file, kAddrs[0]), nullptr);
  EXPECT_FALSE(H5FO_marked(&file, kAddrs[0]));
  EXPECT_LT(H5FO_mark(&file, kAddrs[0], true), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FO_delete(&file, kAddrs[0]), 0);
  }

  // Insert entries with both delete_flag == false and delete_flag == true.
  for (size_t i = 0; i < kAddrs.size(); ++i) {
    const bool initial_delete_flag = (i % 2) == 1;
    ASSERT_GE(
        H5FO_insert(&file, kAddrs[i], &dummy_objs[i], initial_delete_flag), 0);
    inserted[i] = true;
  }

  // Verify lookups, initial deletion flags, and missing-address misses.
  for (size_t i = 0; i < kAddrs.size(); ++i) {
    EXPECT_EQ(H5FO_opened(&file, kAddrs[i]), &dummy_objs[i]);
    EXPECT_EQ(H5FO_marked(&file, kAddrs[i]), (i % 2) == 1);
  }
  constexpr haddr_t kAbsentAddr = 0x9999;
  EXPECT_EQ(H5FO_opened(&file, kAbsentAddr), nullptr);
  EXPECT_FALSE(H5FO_marked(&file, kAbsentAddr));
  EXPECT_LT(H5FO_mark(&file, kAbsentAddr, true), 0);

  // Toggle deletion flags via H5FO_mark.
  EXPECT_GE(H5FO_mark(&file, kAddrs[0], true), 0);
  EXPECT_TRUE(H5FO_marked(&file, kAddrs[0]));
  EXPECT_GE(H5FO_mark(&file, kAddrs[0], false), 0);
  EXPECT_FALSE(H5FO_marked(&file, kAddrs[0]));
  EXPECT_GE(H5FO_mark(&file, kAddrs[1], false), 0);
  EXPECT_FALSE(H5FO_marked(&file, kAddrs[1]));
  EXPECT_GE(H5FO_mark(&file, kAddrs[3], false), 0);
  EXPECT_FALSE(H5FO_marked(&file, kAddrs[3]));

  // Non-empty container must reject H5FO_dest without leaking or freeing it.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FO_dest(&file), 0);
  }
  ASSERT_NE(shared.open_objs, nullptr);

  // Remove all entries cleanly and verify lookups miss afterward.
  for (size_t i = 0; i < kAddrs.size(); ++i) {
    ASSERT_GE(H5FO_delete(&file, kAddrs[i]), 0);
    inserted[i] = false;
    EXPECT_EQ(H5FO_opened(&file, kAddrs[i]), nullptr);
    EXPECT_FALSE(H5FO_marked(&file, kAddrs[i]));
  }

  // Deleting an already-removed entry fails cleanly.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FO_delete(&file, kAddrs[0]), 0);
  }

  // Empty container destroys cleanly and clears open_objs.
  ASSERT_GE(H5FO_dest(&file), 0);
  EXPECT_EQ(shared.open_objs, nullptr);
}

TEST(H5FOTest, TopObjectCountLifecycleAndNonEmptyDestroy) {
  ASSERT_GE(H5open(), 0);

  H5F_t file{};
  ASSERT_GE(H5FO_top_create(&file), 0);
  ASSERT_NE(file.obj_count, nullptr);

  constexpr haddr_t kAddrA = 0x1000;
  constexpr haddr_t kAddrB = 0x2000;
  constexpr haddr_t kAbsentAddr = 0x3000;

  absl::Cleanup cleanup = [&] {
    if (file.obj_count != nullptr) {
      while (H5FO_top_count(&file, kAddrA) > 0) {
        H5FO_top_decr(&file, kAddrA);
      }
      while (H5FO_top_count(&file, kAddrB) > 0) {
        H5FO_top_decr(&file, kAddrB);
      }
      H5FO_top_dest(&file);
    }
  };

  // Empty set checks.
  EXPECT_EQ(H5FO_top_count(&file, kAddrA), hsize_t{0});
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FO_top_decr(&file, kAddrA), 0);
  }

  // Increment multiple objects independently.
  ASSERT_GE(H5FO_top_incr(&file, kAddrA), 0);
  EXPECT_EQ(H5FO_top_count(&file, kAddrA), hsize_t{1});
  ASSERT_GE(H5FO_top_incr(&file, kAddrA), 0);
  ASSERT_GE(H5FO_top_incr(&file, kAddrA), 0);
  EXPECT_EQ(H5FO_top_count(&file, kAddrA), hsize_t{3});

  ASSERT_GE(H5FO_top_incr(&file, kAddrB), 0);
  EXPECT_EQ(H5FO_top_count(&file, kAddrB), hsize_t{1});
  EXPECT_EQ(H5FO_top_count(&file, kAbsentAddr), hsize_t{0});

  // Non-empty top count container rejects H5FO_top_dest.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FO_top_dest(&file), 0);
  }
  ASSERT_NE(file.obj_count, nullptr);

  // Decrement kAddrA back to zero; node is removed at count 0.
  ASSERT_GE(H5FO_top_decr(&file, kAddrA), 0);
  EXPECT_EQ(H5FO_top_count(&file, kAddrA), hsize_t{2});
  ASSERT_GE(H5FO_top_decr(&file, kAddrA), 0);
  EXPECT_EQ(H5FO_top_count(&file, kAddrA), hsize_t{1});
  ASSERT_GE(H5FO_top_decr(&file, kAddrA), 0);
  EXPECT_EQ(H5FO_top_count(&file, kAddrA), hsize_t{0});
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FO_top_decr(&file, kAddrA), 0);
  }

  // Decrement remaining kAddrB and destroy empty container.
  ASSERT_GE(H5FO_top_decr(&file, kAddrB), 0);
  EXPECT_EQ(H5FO_top_count(&file, kAddrB), hsize_t{0});

  ASSERT_GE(H5FO_top_dest(&file), 0);
  EXPECT_EQ(file.obj_count, nullptr);
}

TEST(H5FOTest, PublicObjectsShareOpenObjPtrAndTrackTopCounts) {
  // 1. Create an in-memory file with a group, scalar dataset, and committed
  // datatype, keeping each handle open.
  ScopedFile file = CreateInMemoryFile("fo_public_objects.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* h5f = GetFileStruct(file.get());
  ASSERT_NE(h5f, nullptr);

  ScopedGroup grp1(
      H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp1.valid());
  const haddr_t grp_addr = GetObjectAddr(grp1.get());
  ASSERT_NE(grp_addr, HADDR_UNDEF);

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset1(H5Dcreate2(file.get(), "/dset", H5T_NATIVE_INT32,
                                 space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT));
  ASSERT_TRUE(dset1.valid());
  const haddr_t dset_addr = GetObjectAddr(dset1.get());
  ASSERT_NE(dset_addr, HADDR_UNDEF);

  ScopedType dtype1(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(dtype1.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/dtype", dtype1.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  const haddr_t dtype_addr = GetObjectAddr(dtype1.get());
  ASSERT_NE(dtype_addr, HADDR_UNDEF);

  // 2. Each created & linked object is registered in open_objs (unmarked) with
  // top count 1.
  void* const grp_shared = H5FO_opened(h5f, grp_addr);
  void* const dset_shared = H5FO_opened(h5f, dset_addr);
  void* const dtype_shared = H5FO_opened(h5f, dtype_addr);
  EXPECT_NE(grp_shared, nullptr);
  EXPECT_NE(dset_shared, nullptr);
  EXPECT_NE(dtype_shared, nullptr);
  EXPECT_FALSE(H5FO_marked(h5f, grp_addr));
  EXPECT_FALSE(H5FO_marked(h5f, dset_addr));
  EXPECT_FALSE(H5FO_marked(h5f, dtype_addr));
  EXPECT_EQ(H5FO_top_count(h5f, grp_addr), hsize_t{1});
  EXPECT_EQ(H5FO_top_count(h5f, dset_addr), hsize_t{1});
  EXPECT_EQ(H5FO_top_count(h5f, dtype_addr), hsize_t{1});

  // 3. Opening second handles reuses the shared pointer and increments counts.
  ScopedGroup grp2(H5Gopen2(file.get(), "/grp", H5P_DEFAULT));
  ASSERT_TRUE(grp2.valid());
  ScopedDataset dset2(H5Dopen2(file.get(), "/dset", H5P_DEFAULT));
  ASSERT_TRUE(dset2.valid());
  ScopedType dtype2(H5Topen2(file.get(), "/dtype", H5P_DEFAULT));
  ASSERT_TRUE(dtype2.valid());

  EXPECT_EQ(H5FO_opened(h5f, grp_addr), grp_shared);
  EXPECT_EQ(H5FO_opened(h5f, dset_addr), dset_shared);
  EXPECT_EQ(H5FO_opened(h5f, dtype_addr), dtype_shared);
  EXPECT_EQ(H5FO_top_count(h5f, grp_addr), hsize_t{2});
  EXPECT_EQ(H5FO_top_count(h5f, dset_addr), hsize_t{2});
  EXPECT_EQ(H5FO_top_count(h5f, dtype_addr), hsize_t{2});

  // 4. Closing one handle drops top count to 1 while preserving open_objs.
  grp1.reset();
  dset1.reset();
  dtype1.reset();
  EXPECT_EQ(H5FO_top_count(h5f, grp_addr), hsize_t{1});
  EXPECT_EQ(H5FO_top_count(h5f, dset_addr), hsize_t{1});
  EXPECT_EQ(H5FO_top_count(h5f, dtype_addr), hsize_t{1});
  EXPECT_EQ(H5FO_opened(h5f, grp_addr), grp_shared);
  EXPECT_EQ(H5FO_opened(h5f, dset_addr), dset_shared);
  EXPECT_EQ(H5FO_opened(h5f, dtype_addr), dtype_shared);

  // 5. Closing the final handles clears both top counts and open_objs.
  grp2.reset();
  dset2.reset();
  dtype2.reset();
  EXPECT_EQ(H5FO_top_count(h5f, grp_addr), hsize_t{0});
  EXPECT_EQ(H5FO_top_count(h5f, dset_addr), hsize_t{0});
  EXPECT_EQ(H5FO_top_count(h5f, dtype_addr), hsize_t{0});
  EXPECT_EQ(H5FO_opened(h5f, grp_addr), nullptr);
  EXPECT_EQ(H5FO_opened(h5f, dset_addr), nullptr);
  EXPECT_EQ(H5FO_opened(h5f, dtype_addr), nullptr);
}

TEST(H5FOTest, ReopenedFileSharesOpenObjsWithIndependentTopCounts) {
  // 1. Create a file with a group and reopen the file via H5Freopen so two
  // distinct H5F_t structs share one H5F_shared_t.
  ScopedFile f1 = CreateInMemoryFile("fo_reopen.h5");
  ASSERT_TRUE(f1.valid());
  haddr_t grp_addr = HADDR_UNDEF;
  {
    ScopedGroup g(
        H5Gcreate2(f1.get(), "/shared", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
    grp_addr = GetObjectAddr(g.get());
    ASSERT_NE(grp_addr, HADDR_UNDEF);
  }

  ScopedFile f2(H5Freopen(f1.get()));
  ASSERT_TRUE(f2.valid());

  H5F_t* h5f1 = GetFileStruct(f1.get());
  H5F_t* h5f2 = GetFileStruct(f2.get());
  ASSERT_NE(h5f1, nullptr);
  ASSERT_NE(h5f2, nullptr);
  ASSERT_NE(h5f1, h5f2);
  EXPECT_EQ(h5f1->shared, h5f2->shared);
  EXPECT_NE(h5f1->obj_count, h5f2->obj_count);

  // 2. Open /shared via f1: visible in shared open_objs via both f1 and f2,
  // while top count is 1 on f1 and 0 on f2.
  ScopedGroup g1(H5Gopen2(f1.get(), "/shared", H5P_DEFAULT));
  ASSERT_TRUE(g1.valid());
  void* const shared_ptr = H5FO_opened(h5f1, grp_addr);
  ASSERT_NE(shared_ptr, nullptr);
  EXPECT_EQ(H5FO_opened(h5f2, grp_addr), shared_ptr);
  EXPECT_EQ(H5FO_top_count(h5f1, grp_addr), hsize_t{1});
  EXPECT_EQ(H5FO_top_count(h5f2, grp_addr), hsize_t{0});

  // 3. Open /shared twice via f2: top count on f2 rises 0 -> 1 -> 2 while f1
  // stays at 1.
  ScopedGroup g2a(H5Gopen2(f2.get(), "/shared", H5P_DEFAULT));
  ASSERT_TRUE(g2a.valid());
  EXPECT_EQ(H5FO_opened(h5f2, grp_addr), shared_ptr);
  EXPECT_EQ(H5FO_top_count(h5f1, grp_addr), hsize_t{1});
  EXPECT_EQ(H5FO_top_count(h5f2, grp_addr), hsize_t{1});

  ScopedGroup g2b(H5Gopen2(f2.get(), "/shared", H5P_DEFAULT));
  ASSERT_TRUE(g2b.valid());
  EXPECT_EQ(H5FO_top_count(h5f1, grp_addr), hsize_t{1});
  EXPECT_EQ(H5FO_top_count(h5f2, grp_addr), hsize_t{2});

  // 4. Close g1 and f1 while f2 still holds g2a and g2b open.
  g1.reset();
  EXPECT_EQ(H5FO_top_count(h5f1, grp_addr), hsize_t{0});
  EXPECT_EQ(H5FO_top_count(h5f2, grp_addr), hsize_t{2});
  EXPECT_EQ(H5FO_opened(h5f2, grp_addr), shared_ptr);
  f1.reset();

  // 5. Closing g2a and g2b releases the object from open_objs cleanly before f2
  // closes.
  g2a.reset();
  EXPECT_EQ(H5FO_top_count(h5f2, grp_addr), hsize_t{1});
  EXPECT_EQ(H5FO_opened(h5f2, grp_addr), shared_ptr);
  g2b.reset();
  EXPECT_EQ(H5FO_top_count(h5f2, grp_addr), hsize_t{0});
  EXPECT_EQ(H5FO_opened(h5f2, grp_addr), nullptr);
}

TEST(H5FOTest, UnlinkOpenObjectMarksForDeletionAndRelinkClearsMark) {
  ScopedFile file = CreateInMemoryFile("fo_unlink_mark.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* h5f = GetFileStruct(file.get());
  ASSERT_NE(h5f, nullptr);

  // 1. Unlink an open dataset and close it so H5FO_delete triggers H5O_delete.
  {
    ScopedSpace space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), "/ephemeral", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    const int32_t value = 42;
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, &value),
              0);

    const haddr_t addr = GetObjectAddr(dset.get());
    ASSERT_NE(addr, HADDR_UNDEF);
    EXPECT_FALSE(H5FO_marked(h5f, addr));

    ASSERT_GE(H5Ldelete(file.get(), "/ephemeral", H5P_DEFAULT), 0);
    EXPECT_NE(H5FO_opened(h5f, addr), nullptr);
    EXPECT_TRUE(H5FO_marked(h5f, addr));

    // Dataset remains accessible while open despite being unlinked.
    int32_t read_back = 0;
    ASSERT_GE(H5Dread(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, &read_back),
              0);
    EXPECT_EQ(read_back, 42);

    // Closing invokes H5FO_delete -> H5O_delete on the marked object.
    dset.reset();
    EXPECT_EQ(H5FO_opened(h5f, addr), nullptr);
    EXPECT_FALSE(H5FO_marked(h5f, addr));
    EXPECT_EQ(0, H5Lexists(file.get(), "/ephemeral", H5P_DEFAULT));
  }

  // 2. Unlink an open group (marking it for deletion), then relink it with
  // H5Olink before closing so H5FO_mark clears `deleted`.
  haddr_t saved_addr = HADDR_UNDEF;
  {
    ScopedGroup grp(H5Gcreate2(file.get(), "/transient", H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    saved_addr = GetObjectAddr(grp.get());
    ASSERT_NE(saved_addr, HADDR_UNDEF);

    ASSERT_GE(H5Ldelete(file.get(), "/transient", H5P_DEFAULT), 0);
    EXPECT_TRUE(H5FO_marked(h5f, saved_addr));

    ASSERT_GE(
        H5Olink(grp.get(), file.get(), "/saved", H5P_DEFAULT, H5P_DEFAULT), 0);
    EXPECT_FALSE(H5FO_marked(h5f, saved_addr));
  }

  EXPECT_EQ(H5FO_opened(h5f, saved_addr), nullptr);
  ASSERT_GT(H5Lexists(file.get(), "/saved", H5P_DEFAULT), 0);
  ScopedGroup reopened(H5Gopen2(file.get(), "/saved", H5P_DEFAULT));
  ASSERT_TRUE(reopened.valid());
  EXPECT_EQ(GetObjectAddr(reopened.get()), saved_addr);
}

}  // namespace
}  // namespace hdf5
