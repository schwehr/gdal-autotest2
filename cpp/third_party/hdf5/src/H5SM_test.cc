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
// Tests Shared Object Header Messages (SOHM) table initialization, sharing
// eligibility and deferred writes, list <-> v2 B-tree index transitions,
// reference counting, deletion, superblock extension reload, storage sizing,
// and debug output (third_party/hdf5/src/H5SM.c).
//
// Build caveats: google3 builds with zlib deflate and libaec szip enabled,
// HDF5_HAVE_PARALLEL / ROS3 / subfiling disabled, and -DNDEBUG.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5SM.c

#include <stdio.h>

#include <array>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// clang-format off
// `:hdf5` is built with -DNDEBUG, omitting debug fields from H5AC_info_t and
// H5O_t. Define NDEBUG around internal headers so C++ struct layouts match.
#ifndef NDEBUG
#define H5SM_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#ifdef try
#undef try
#endif
#define try h5b2_try_param
extern "C" {
#define H5A_FRIEND
#define H5F_FRIEND
#define H5G_FRIEND
#define H5O_FRIEND
#define H5SM_FRIEND
#define H5S_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5Apkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5HFprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Pprivate.h"
#include "third_party/hdf5/src/H5SMpkg.h"
#include "third_party/hdf5/src/H5Spkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5SM_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5SM_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = H5AC__SOHM_TAG) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_tag(tag, &prev_tag_);
      tagged_ = true;
    }
  }

  ~ScopedApiContext() {
    if (tagged_) {
      H5AC_tag(prev_tag_, nullptr);
    }
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

H5S_t* UnwrapSpace(hid_t space_id) {
  return static_cast<H5S_t*>(H5I_object_verify(space_id, H5I_DATASPACE));
}

void* UnwrapDatatype(hid_t type_id) {
  return H5I_object_verify(type_id, H5I_DATATYPE);
}

// Reopens a flushed in-memory HDF5 file via H5Fget_file_image +
// H5Pset_file_image, exercising H5SM_get_info on open.
ScopedFile ReopenInMemory(hid_t file_id, unsigned flags = H5F_ACC_RDWR) {
  const ssize_t image_size = H5Fget_file_image(file_id, nullptr, 0);
  if (image_size <= 0) {
    return ScopedFile();
  }
  std::vector<uint8_t> image(static_cast<size_t>(image_size));
  if (H5Fget_file_image(file_id, image.data(), image.size()) != image_size) {
    return ScopedFile();
  }
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) {
    return ScopedFile();
  }
  if (H5Pset_file_image(fapl.get(), image.data(), image.size()) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fopen("sohm_reopen.h5", flags, fapl.get()));
}

ScopedPlist MakeSohmFcpl(unsigned nindexes, unsigned list_max,
                         unsigned btree_min, unsigned index0_flags,
                         unsigned index0_minsize) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  if (!fcpl.valid()) {
    return ScopedPlist();
  }
  if (H5Pset_shared_mesg_nindexes(fcpl.get(), nindexes) < 0) {
    return ScopedPlist();
  }
  if (H5Pset_shared_mesg_index(fcpl.get(), 0, index0_flags, index0_minsize) <
      0) {
    return ScopedPlist();
  }
  if (H5Pset_shared_mesg_phase_change(fcpl.get(), list_max, btree_min) < 0) {
    return ScopedPlist();
  }
  return fcpl;
}

std::string CaptureTableDebug(H5F_t* f, haddr_t table_addr,
                              unsigned table_vers, unsigned num_indexes,
                              herr_t* rc_out) {
  char* raw = nullptr;
  size_t raw_size = 0;
  FILE* mem = open_memstream(&raw, &raw_size);
  absl::Cleanup free_mem = [&] {
    if (mem != nullptr) {
      std::fclose(mem);
    }
    std::free(raw);
  };
  if (mem == nullptr) {
    *rc_out = -1;
    return "";
  }
  *rc_out = H5SM_table_debug(f, table_addr, mem, 0, 20, table_vers, num_indexes);
  std::fclose(mem);
  mem = nullptr;
  return raw != nullptr ? std::string(raw, raw_size) : std::string();
}

std::string CaptureListDebug(H5F_t* f, haddr_t list_addr, haddr_t table_addr,
                             herr_t* rc_out) {
  char* raw = nullptr;
  size_t raw_size = 0;
  FILE* mem = open_memstream(&raw, &raw_size);
  absl::Cleanup free_mem = [&] {
    if (mem != nullptr) {
      std::fclose(mem);
    }
    std::free(raw);
  };
  if (mem == nullptr) {
    *rc_out = -1;
    return "";
  }
  *rc_out = H5SM_list_debug(f, list_addr, mem, 0, 20, table_addr);
  std::fclose(mem);
  mem = nullptr;
  return raw != nullptr ? std::string(raw, raw_size) : std::string();
}

TEST(H5SMTest, InitConfigVariantsReopenAndErrorPaths) {
  // 1. Multi-index SOHM file with attributes + custom cutoffs round-trips via
  //    H5SM_get_info on file reopen.
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 3), 0);
  ASSERT_GE(
      H5Pset_shared_mesg_index(
          fcpl.get(), 0, H5O_SHMESG_SDSPACE_FLAG | H5O_SHMESG_DTYPE_FLAG, 12),
      0);
  ASSERT_GE(
      H5Pset_shared_mesg_index(
          fcpl.get(), 1, H5O_SHMESG_FILL_FLAG | H5O_SHMESG_PLINE_FLAG, 16),
      0);
  ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 2, H5O_SHMESG_ATTR_FLAG, 20),
            0);
  ASSERT_GE(H5Pset_shared_mesg_phase_change(fcpl.get(), 7, 3), 0);

  ScopedFile file = CreateInMemoryFile("sohm_init.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  ScopedFile reopened = ReopenInMemory(file.get());
  ASSERT_TRUE(reopened.valid());
  ScopedPlist got_fcpl(H5Fget_create_plist(reopened.get()));
  ASSERT_TRUE(got_fcpl.valid());

  unsigned nindexes = 0;
  ASSERT_GE(H5Pget_shared_mesg_nindexes(got_fcpl.get(), &nindexes), 0);
  EXPECT_EQ(nindexes, 3u);

  unsigned flags = 0;
  unsigned minsize = 0;
  ASSERT_GE(H5Pget_shared_mesg_index(got_fcpl.get(), 0, &flags, &minsize), 0);
  EXPECT_EQ(flags, H5O_SHMESG_SDSPACE_FLAG | H5O_SHMESG_DTYPE_FLAG);
  EXPECT_EQ(minsize, 12u);
  ASSERT_GE(H5Pget_shared_mesg_index(got_fcpl.get(), 2, &flags, &minsize), 0);
  EXPECT_EQ(flags, H5O_SHMESG_ATTR_FLAG);
  EXPECT_EQ(minsize, 20u);

  unsigned max_list = 0;
  unsigned min_btree = 0;
  ASSERT_GE(H5Pget_shared_mesg_phase_change(got_fcpl.get(), &max_list, &min_btree),
            0);
  EXPECT_EQ(max_list, 7u);
  EXPECT_EQ(min_btree, 3u);
  reopened.reset();
  file.reset();

  // 2. Reopening a non-SOHM file that still has a superblock extension
  //    (persistent free-space manager -> H5O_FSINFO_ID, no H5O_SHMESG_ID)
  //    covers H5SM_get_info's !status branch.
  ScopedPlist no_sohm_fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(no_sohm_fcpl.valid());
  ASSERT_GE(
      H5Pset_file_space_strategy(no_sohm_fcpl.get(),
                                 H5F_FSPACE_STRATEGY_FSM_AGGR, true, 1),
      0);
  ScopedPlist latest_fapl = MakeCoreFapl();
  ASSERT_TRUE(latest_fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(latest_fapl.get(), H5F_LIBVER_LATEST,
                                 H5F_LIBVER_LATEST),
            0);
  ScopedFile sbe_file(H5Fcreate("sbe_nosohm.h5", H5F_ACC_TRUNC,
                                no_sohm_fcpl.get(), latest_fapl.get()));
  ASSERT_TRUE(sbe_file.valid());
  ASSERT_GE(H5Fflush(sbe_file.get(), H5F_SCOPE_GLOBAL), 0);
  ScopedFile sbe_reopen = ReopenInMemory(sbe_file.get(), H5F_ACC_RDONLY);
  ASSERT_TRUE(sbe_reopen.valid());
  ScopedPlist sbe_got_fcpl(H5Fget_create_plist(sbe_reopen.get()));
  ASSERT_TRUE(sbe_got_fcpl.valid());
  ASSERT_GE(H5Pget_shared_mesg_nindexes(sbe_got_fcpl.get(), &nindexes), 0);
  EXPECT_EQ(nindexes, 0u);
  sbe_reopen.reset();

  // 3. Duplicate type flags across indexes fails in H5SM_init during H5Fcreate.
  {
    ScopedErrorSilence silence;
    ScopedPlist dup_fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(dup_fcpl.valid());
    ASSERT_GE(H5Pset_shared_mesg_nindexes(dup_fcpl.get(), 2), 0);
    ASSERT_GE(
        H5Pset_shared_mesg_index(dup_fcpl.get(), 0, H5O_SHMESG_SDSPACE_FLAG, 1),
        0);
    ASSERT_GE(
        H5Pset_shared_mesg_index(dup_fcpl.get(), 1, H5O_SHMESG_SDSPACE_FLAG, 1),
        0);
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    EXPECT_EQ(H5Fcreate("dup_sohm.h5", H5F_ACC_TRUNC, dup_fcpl.get(), fapl.get()),
              H5I_INVALID_HID);
  }

  // 4. H5SM_init rejects table->num_indexes > H5O_SHMESG_MAX_NINDEXES.
  {
    ScopedGroup root(H5Gopen2(sbe_file.get(), "/", H5P_DEFAULT));
    ASSERT_TRUE(root.valid());
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(sbe_file.get());
    ASSERT_NE(f, nullptr);
    H5O_loc_t* root_loc = H5O_get_loc(root.get());
    ASSERT_NE(root_loc, nullptr);
    H5P_genplist_t* plist =
        H5P_object_verify(H5F_FCPL(f), H5P_FILE_CREATE, false);
    ASSERT_NE(plist, nullptr);

    const unsigned orig_nindexes = H5F_SOHM_NINDEXES(f);
    H5F_SET_SOHM_NINDEXES(f, H5O_SHMESG_MAX_NINDEXES + 1);
    absl::Cleanup restore_nindexes = [&] {
      H5F_SET_SOHM_NINDEXES(f, orig_nindexes);
    };

    ScopedErrorSilence silence;
    EXPECT_LT(H5SM_init(f, plist, root_loc), 0);
  }
}

TEST(H5SMTest, TypeSharedAndFheapAddrQueriesAndErrors) {
  // 1. Non-SOHM file: H5SM_type_shared returns false for shareable IDs and
  //    errors on unmapped message types.
  ScopedFile plain = CreateInMemoryFile("plain.h5");
  ASSERT_TRUE(plain.valid());
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(plain.get());
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(H5SM_type_shared(f, H5O_SDSPACE_ID), 0);
    ScopedErrorSilence silence;
    EXPECT_LT(H5SM_type_shared(f, H5O_LINK_ID), 0);
  }
  plain.reset();

  // 2. SOHM file covering all 5 flag types (including H5O_FILL_ID alias).
  ScopedPlist fcpl =
      MakeSohmFcpl(1, 5, 2,
                   H5O_SHMESG_SDSPACE_FLAG | H5O_SHMESG_DTYPE_FLAG |
                       H5O_SHMESG_FILL_FLAG | H5O_SHMESG_ATTR_FLAG,
                   1);
  ASSERT_TRUE(fcpl.valid());
  ScopedFile sohm = CreateInMemoryFile("sohm_types.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(sohm.valid());

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(sohm.get());
    ASSERT_NE(f, nullptr);

    EXPECT_EQ(H5SM_type_shared(f, H5O_SDSPACE_ID), 1);
    EXPECT_EQ(H5SM_type_shared(f, H5O_DTYPE_ID), 1);
    EXPECT_EQ(H5SM_type_shared(f, H5O_FILL_ID), 1);
    EXPECT_EQ(H5SM_type_shared(f, H5O_FILL_NEW_ID), 1);
    EXPECT_EQ(H5SM_type_shared(f, H5O_ATTR_ID), 1);
    EXPECT_EQ(H5SM_type_shared(f, H5O_PLINE_ID), 0);

    haddr_t heap_addr = 0;
    ASSERT_GE(H5SM_get_fheap_addr(f, H5O_SDSPACE_ID, &heap_addr), 0);
    EXPECT_EQ(heap_addr, HADDR_UNDEF);
  }

  // Write one shared dataspace so the index heap gets created.
  const std::array<hsize_t, 2> dims = {4, 5};
  ScopedSpace sp(H5Screate_simple(2, dims.data(), nullptr));
  ASSERT_TRUE(sp.valid());
  ScopedDataset ds(H5Dcreate2(sohm.get(), "/ds", H5T_NATIVE_INT, sp.get(),
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds.valid());
  ds.reset();

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(sohm.get());
    ASSERT_NE(f, nullptr);

    haddr_t heap_addr = HADDR_UNDEF;
    ASSERT_GE(H5SM_get_fheap_addr(f, H5O_SDSPACE_ID, &heap_addr), 0);
    EXPECT_NE(heap_addr, HADDR_UNDEF);

    ScopedErrorSilence silence;
    EXPECT_LT(H5SM_get_fheap_addr(f, H5O_LINK_ID, &heap_addr), 0);
    EXPECT_LT(H5SM_get_fheap_addr(f, H5O_PLINE_ID, &heap_addr), 0);

    const haddr_t orig_sohm_addr = H5F_SOHM_ADDR(f);
    H5F_SET_SOHM_ADDR(f, 0x7FFFFFFFULL);
    absl::Cleanup restore_sohm_addr = [&] {
      H5F_SET_SOHM_ADDR(f, orig_sohm_addr);
    };
    EXPECT_LT(H5SM_type_shared(f, H5O_SDSPACE_ID), 0);
    EXPECT_LT(H5SM_get_fheap_addr(f, H5O_SDSPACE_ID, &heap_addr), 0);
  }
}

TEST(H5SMTest, CanShareChecksAndTryShareDeferFlows) {
  ScopedFile plain = CreateInMemoryFile("plain_can_share.h5");
  ASSERT_TRUE(plain.valid());

  const std::array<hsize_t, 3> big_dims = {2, 3, 4};
  const std::array<hsize_t, 1> small_dims = {2};
  ScopedSpace big_sp(H5Screate_simple(3, big_dims.data(), nullptr));
  ScopedSpace small_sp(H5Screate_simple(1, small_dims.data(), nullptr));
  ASSERT_TRUE(big_sp.valid());
  ASSERT_TRUE(small_sp.valid());

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(plain.get());
    H5S_t* sp_obj = UnwrapSpace(big_sp.get());
    ASSERT_NE(f, nullptr);
    ASSERT_NE(sp_obj, nullptr);

    // Non-SOHM file: H5SM__can_share_common returns false.
    EXPECT_EQ(H5SM_can_share(f, nullptr, nullptr, H5O_SDSPACE_ID,
                             &sp_obj->extent),
              0);
    unsigned flags = 0;
    EXPECT_EQ(
        H5SM_try_share(f, nullptr, 0, H5O_SDSPACE_ID, &sp_obj->extent, &flags),
        0);
  }
  plain.reset();

  // Configured SOHM file with min_mesg_size = 40 so big_sp (>=40B? let's check
  // with min_mesg_size = 32 for 3D v2 sdspace and >32 for 1D v2 sdspace).
  // Actually 1D v1/v2 sdspace raw_size: 16-24 bytes, 3D sdspace raw_size: ~56B.
  ScopedPlist fcpl = MakeSohmFcpl(
      1, 4, 2, H5O_SHMESG_SDSPACE_FLAG | H5O_SHMESG_ATTR_FLAG, 32);
  ASSERT_TRUE(fcpl.valid());
  ScopedFile file = CreateInMemoryFile("can_share.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());

  // /holder holds `big_sp` (written with H5S_set_version) in index slot 0 so
  // deferred duplicate lookups compare against an authentic OH message.
  ScopedDataset holder(H5Dcreate2(file.get(), "/holder", H5T_NATIVE_INT,
                                  big_sp.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
  ASSERT_TRUE(holder.valid());

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    H5S_t* big_obj = UnwrapSpace(big_sp.get());
    H5S_t* small_obj = UnwrapSpace(small_sp.get());
    ASSERT_NE(f, nullptr);
    ASSERT_NE(big_obj, nullptr);
    ASSERT_NE(small_obj, nullptr);
    ASSERT_GE(H5S_set_version(f, small_obj), 0);

    // Non-shareable type -> false.
    H5O_name_t name_msg{};
    EXPECT_EQ(H5SM_can_share(f, nullptr, nullptr, H5O_NAME_ID, &name_msg), 0);

    // Unconfigured shareable type -> index_num < 0 -> false.
    H5O_pline_t pline_msg{};
    EXPECT_EQ(H5SM_can_share(f, nullptr, nullptr, H5O_PLINE_ID, &pline_msg), 0);

    // Below min_mesg_size -> false.
    EXPECT_EQ(H5SM_can_share(f, nullptr, nullptr, H5O_SDSPACE_ID,
                             &small_obj->extent),
              0);

    // Above min_mesg_size with table == nullptr and non-null index_num.
    ssize_t idx_num = -1;
    EXPECT_EQ(H5SM_can_share(f, nullptr, &idx_num, H5O_SDSPACE_ID,
                             &big_obj->extent),
              1);
    EXPECT_EQ(idx_num, 0);

    // Pass pre-protected master table + nullptr index_num.
    H5SM_table_cache_ud_t udata{f};
    auto* table = static_cast<H5SM_master_table_t*>(
        H5AC_protect(f, H5AC_SOHM_TABLE, H5F_SOHM_ADDR(f), &udata,
                     H5AC__READ_ONLY_FLAG));
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(H5SM_can_share(f, table, nullptr, H5O_SDSPACE_ID,
                             &big_obj->extent),
              1);
    ASSERT_GE(
        H5AC_unprotect(f, H5AC_SOHM_TABLE, H5F_SOHM_ADDR(f), table, 0), 0);

    // H5O_MSG_FLAG_DONTSHARE short-circuits H5SM_try_share.
    unsigned dontshare_flags = H5O_MSG_FLAG_DONTSHARE;
    EXPECT_EQ(H5SM_try_share(f, nullptr, 0, H5O_SDSPACE_ID, &big_obj->extent,
                             &dontshare_flags),
              0);

    // H5SM_WAS_DEFERRED with unshared message exits cleanly in NDEBUG mode.
    ASSERT_GE(H5O_msg_reset_share(H5O_SDSPACE_ID, &big_obj->extent), 0);
    unsigned deferred_flags = 0;
    EXPECT_EQ(H5SM_try_share(f, nullptr, H5SM_WAS_DEFERRED, H5O_SDSPACE_ID,
                             &big_obj->extent, &deferred_flags),
              0);

    // Size < min_mesg_size in H5SM_try_share -> false.
    unsigned small_flags = 0;
    EXPECT_EQ(H5SM_try_share(f, nullptr, 0, H5O_SDSPACE_ID, &small_obj->extent,
                             &small_flags),
              0);

    // Defer + WAS_DEFERRED for a unique heap-shared message (open_oh ==
    // nullptr) in LIST mode.
    const std::array<hsize_t, 3> u1_dims = {3, 4, 5};
    ScopedSpace u1_sp(H5Screate_simple(3, u1_dims.data(), nullptr));
    ASSERT_TRUE(u1_sp.valid());
    H5S_t* u1_obj = UnwrapSpace(u1_sp.get());
    ASSERT_NE(u1_obj, nullptr);
    unsigned flags = 0;
    ASSERT_EQ(H5SM_try_share(f, nullptr, H5SM_DEFER, H5O_SDSPACE_ID,
                             &u1_obj->extent, &flags),
              1);
    EXPECT_EQ(u1_obj->extent.sh_loc.type, H5O_SHARE_TYPE_SOHM);
    ASSERT_EQ(H5SM_try_share(f, nullptr, H5SM_WAS_DEFERRED, H5O_SDSPACE_ID,
                             &u1_obj->extent, &flags),
              1);
    EXPECT_TRUE((flags & H5O_MSG_FLAG_SHARED) != 0);
    EXPECT_GE(H5SM_delete(f, nullptr, &u1_obj->extent.sh_loc), 0);

    // Defer + WAS_DEFERRED with open_oh != nullptr -> IN_OH (SHARE_TYPE_HERE),
    // verifying oh_addr == HADDR_UNDEF while deferred and actual address after.
    // Use a separate group /dummy so /holder stays unprotected for list lookups
    // and its (oh_addr, index=0) SOHM entry is distinct from u2_obj's cleanup.
    ScopedGroup dummy(H5Gcreate2(file.get(), "/dummy", H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT));
    ASSERT_TRUE(dummy.valid());
    H5O_loc_t* dummy_loc = H5O_get_loc(dummy.get());
    ASSERT_NE(dummy_loc, nullptr);
    H5O_t* dummy_oh = H5O_protect(dummy_loc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(dummy_oh, nullptr);
    absl::Cleanup unprotect_oh = [&] {
      if (dummy_oh != nullptr) {
        H5O_unprotect(dummy_loc, dummy_oh, H5AC__DIRTIED_FLAG);
      }
    };

    const std::array<hsize_t, 3> u2_dims = {6, 7, 8};
    ScopedSpace u2_sp(H5Screate_simple(3, u2_dims.data(), nullptr));
    ASSERT_TRUE(u2_sp.valid());
    H5S_t* u2_obj = UnwrapSpace(u2_sp.get());
    ASSERT_NE(u2_obj, nullptr);
    // Append an unshared copy (`DONTSHARE`) into `dummy_oh` so subsequent
    // H5SM_delete reads an authentic message back from `dummy_oh`.
    ASSERT_GE(H5O_msg_append_oh(f, dummy_oh, H5O_SDSPACE_ID,
                                H5O_MSG_FLAG_DONTSHARE, 0, &u2_obj->extent),
              0);
    flags = 0;
    ASSERT_EQ(H5SM_try_share(f, dummy_oh, H5SM_DEFER, H5O_SDSPACE_ID,
                             &u2_obj->extent, &flags),
              1);
    EXPECT_EQ(u2_obj->extent.sh_loc.type, H5O_SHARE_TYPE_HERE);
    EXPECT_EQ(u2_obj->extent.sh_loc.u.loc.oh_addr, HADDR_UNDEF);
    ASSERT_EQ(H5SM_try_share(f, dummy_oh, H5SM_WAS_DEFERRED, H5O_SDSPACE_ID,
                             &u2_obj->extent, &flags),
              1);
    EXPECT_TRUE((flags & H5O_MSG_FLAG_SHAREABLE) != 0);
    EXPECT_EQ(u2_obj->extent.sh_loc.u.loc.oh_addr, H5O_OH_GET_ADDR(dummy_oh));
    ASSERT_GE(H5SM_delete(f, dummy_oh, &u2_obj->extent.sh_loc), 0);

    // Defer + WAS_DEFERRED when duplicate already exists in LIST (matching
    // /holder's IN_OH entry) -> found, promotes slot 0 to IN_HEAP with ref=2.
    ScopedSpace dup_sp(H5Scopy(big_sp.get()));
    ASSERT_TRUE(dup_sp.valid());
    H5S_t* dup_obj = UnwrapSpace(dup_sp.get());
    ASSERT_NE(dup_obj, nullptr);
    ASSERT_GE(H5S_set_version(f, dup_obj), 0);
    unsigned dup_flags = 0;
    ASSERT_EQ(H5SM_try_share(f, dummy_oh, H5SM_DEFER, H5O_SDSPACE_ID,
                             &dup_obj->extent, &dup_flags),
              1);
    EXPECT_EQ(dup_obj->extent.sh_loc.type, H5O_SHARE_TYPE_SOHM);
    ASSERT_EQ(H5SM_try_share(f, dummy_oh, H5SM_WAS_DEFERRED, H5O_SDSPACE_ID,
                             &dup_obj->extent, &dup_flags),
              1);
    EXPECT_TRUE((dup_flags & H5O_MSG_FLAG_SHARED) != 0);

    hsize_t rc = 0;
    ASSERT_GE(
        H5SM_get_refcount(f, H5O_SDSPACE_ID, &dup_obj->extent.sh_loc, &rc), 0);
    EXPECT_EQ(rc, 2u);

    // Clean up duplicate reference (ref 2 -> 1).
    ASSERT_GE(H5SM_delete(f, dummy_oh, &dup_obj->extent.sh_loc), 0);

    ASSERT_GE(H5O_unprotect(dummy_loc, dummy_oh, H5AC__DIRTIED_FLAG), 0);
    dummy_oh = nullptr;
    dummy.reset();

    // Calling H5SM_try_share with open_oh == nullptr and mesg_flags == nullptr
    // goes directly to H5SM_IN_HEAP with ref_count == 1.
    const std::array<hsize_t, 3> u3_dims = {9, 10, 11};
    ScopedSpace u3_sp(H5Screate_simple(3, u3_dims.data(), nullptr));
    ASSERT_TRUE(u3_sp.valid());
    H5S_t* u3_obj = UnwrapSpace(u3_sp.get());
    ASSERT_NE(u3_obj, nullptr);
    ASSERT_EQ(H5SM_try_share(f, nullptr, 0, H5O_SDSPACE_ID, &u3_obj->extent,
                             nullptr),
              1);
    rc = 0;
    ASSERT_GE(
        H5SM_get_refcount(f, H5O_SDSPACE_ID, &u3_obj->extent.sh_loc, &rc), 0);
    EXPECT_EQ(rc, 1u);
    ASSERT_GE(H5SM_delete(f, nullptr, &u3_obj->extent.sh_loc), 0);
  }
}

TEST(H5SMTest, AllShareableMessageClassesAndNestedAttrCleanup) {
  ScopedPlist fcpl = MakeSohmFcpl(
      1, 10, 4,
      H5O_SHMESG_SDSPACE_FLAG | H5O_SHMESG_DTYPE_FLAG | H5O_SHMESG_FILL_FLAG |
          H5O_SHMESG_PLINE_FLAG | H5O_SHMESG_ATTR_FLAG,
      1);
  ASSERT_TRUE(fcpl.valid());
  ScopedFile file = CreateInMemoryFile("all_classes.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());

  // Large compound type, 3D dataspace, struct fill value, deflate pipeline.
  struct SampleRec {
    int32_t a;
    double b;
    int64_t c;
  };
  ScopedType cmpd(H5Tcreate(H5T_COMPOUND, sizeof(SampleRec)));
  ASSERT_TRUE(cmpd.valid());
  ASSERT_GE(H5Tinsert(cmpd.get(), "alpha", offsetof(SampleRec, a),
                      H5T_NATIVE_INT32),
            0);
  ASSERT_GE(
      H5Tinsert(cmpd.get(), "beta", offsetof(SampleRec, b), H5T_NATIVE_DOUBLE),
      0);
  ASSERT_GE(
      H5Tinsert(cmpd.get(), "gamma", offsetof(SampleRec, c), H5T_NATIVE_INT64),
      0);

  const std::array<hsize_t, 2> dims = {16, 16};
  const std::array<hsize_t, 2> chunk_dims = {8, 8};
  ScopedSpace sp(H5Screate_simple(2, dims.data(), nullptr));
  ASSERT_TRUE(sp.valid());

  SampleRec fill{7, 3.5, 42};
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_chunk(dcpl.get(), 2, chunk_dims.data()), 0);
  ASSERT_GE(H5Pset_deflate(dcpl.get(), 4), 0);
  ASSERT_GE(H5Pset_fill_value(dcpl.get(), cmpd.get(), &fill), 0);

  ScopedDataset d1(H5Dcreate2(file.get(), "/d1", cmpd.get(), sp.get(),
                              H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
  ScopedDataset d2(H5Dcreate2(file.get(), "/d2", cmpd.get(), sp.get(),
                              H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(d1.valid());
  ASSERT_TRUE(d2.valid());

  // Committed datatype cannot be shared as an H5O_DTYPE message (can_share==0).
  ScopedType committed(H5Tcopy(cmpd.get()));
  ASSERT_TRUE(committed.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/committed_t", committed.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    void* dt_obj = UnwrapDatatype(committed.get());
    ASSERT_NE(f, nullptr);
    ASSERT_NE(dt_obj, nullptr);
    EXPECT_EQ(H5SM_can_share(f, nullptr, nullptr, H5O_DTYPE_ID, dt_obj), 0);
  }

  // Create identical attributes using the same shared compound type and
  // dataspace, then delete both objects so H5SM_delete runs recursive
  // H5O_msg_delete on shared attribute components.
  const std::array<hsize_t, 1> attr_dims = {2};
  ScopedSpace attr_sp(H5Screate_simple(1, attr_dims.data(), nullptr));
  ASSERT_TRUE(attr_sp.valid());
  const std::array<SampleRec, 2> attr_vals = {{{1, 1.5, 9}, {2, 2.5, 10}}};

  ScopedAttr a1(H5Acreate2(d1.get(), "meta", cmpd.get(), attr_sp.get(),
                           H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(a1.valid());
  ASSERT_GE(H5Awrite(a1.get(), cmpd.get(), attr_vals.data()), 0);
  a1.reset();

  ScopedAttr a2(H5Acreate2(d2.get(), "meta", cmpd.get(), attr_sp.get(),
                           H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(a2.valid());
  ASSERT_GE(H5Awrite(a2.get(), cmpd.get(), attr_vals.data()), 0);
  a2.reset();

  d1.reset();
  d2.reset();

  // Check file SOHM storage info via H5Fget_info2 (exercises H5SM_ih_size).
  H5F_info2_t finfo{};
  ASSERT_GE(H5Fget_info2(file.get(), &finfo), 0);
  EXPECT_GT(finfo.sohm.hdr_size, 0u);
  EXPECT_GT(finfo.sohm.msgs_info.index_size, 0u);
  EXPECT_GT(finfo.sohm.msgs_info.heap_size, 0u);

  // Delete /d1 (decrements SOHM ref counts 2 -> 1) then /d2 (1 -> 0).
  ASSERT_GE(H5Ldelete(file.get(), "/d1", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete(file.get(), "/d2", H5P_DEFAULT), 0);
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
}

TEST(H5SMTest, ListToBtreeAndBtreeToListTransitionsAndExtentShrink) {
  // list_max = 3, btree_min = 2:
  //   0..3 msgs -> LIST; inserting 4th triggers LIST -> BTREE;
  //   deleting down to 1 msg (< btree_min) triggers BTREE -> LIST;
  //   deleting final msg removes empty list index & heap.
  ScopedPlist fcpl = MakeSohmFcpl(1, 3, 2, H5O_SHMESG_SDSPACE_FLAG, 1);
  ASSERT_TRUE(fcpl.valid());
  ScopedFile file =
      CreateInMemoryFile("phase_change.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());

  // 1. Add sp0 (IN_OH), sp1 (IN_HEAP ref=2), sp2 (IN_OH), then delete sp0 so
  //    list slot 0 becomes H5SM_NO_LOC (hole), and add sp0b reusing empty_pos!
  const std::array<hsize_t, 1> d0 = {10};
  const std::array<hsize_t, 1> d1 = {20};
  const std::array<hsize_t, 1> d2 = {30};
  const std::array<hsize_t, 1> d3 = {40};
  ScopedSpace sp0(H5Screate_simple(1, d0.data(), nullptr));
  ScopedSpace sp1(H5Screate_simple(1, d1.data(), nullptr));
  ScopedSpace sp2(H5Screate_simple(1, d2.data(), nullptr));
  ScopedSpace sp3(H5Screate_simple(1, d3.data(), nullptr));
  ASSERT_TRUE(sp0.valid() && sp1.valid() && sp2.valid() && sp3.valid());

  ScopedDataset ds0(H5Dcreate2(file.get(), "/ds0", H5T_NATIVE_INT, sp0.get(),
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedDataset ds1a(H5Dcreate2(file.get(), "/ds1a", H5T_NATIVE_INT, sp1.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedDataset ds1b(H5Dcreate2(file.get(), "/ds1b", H5T_NATIVE_INT, sp1.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedDataset ds2(H5Dcreate2(file.get(), "/ds2", H5T_NATIVE_INT, sp2.get(),
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds0.valid() && ds1a.valid() && ds1b.valid() && ds2.valid());

  // Close handles before deleting /ds0 to leave a hole in the list.
  ds0.reset();
  ASSERT_GE(H5Ldelete(file.get(), "/ds0", H5P_DEFAULT), 0);

  // Add another unique IN_OH dataspace (/ds0_new), which finds the empty hole,
  // then keep another unique IN_OH dataspace (/ds3) to convert LIST -> BTREE.
  const std::array<hsize_t, 1> d0b = {15};
  ScopedSpace sp0b(H5Screate_simple(1, d0b.data(), nullptr));
  ASSERT_TRUE(sp0b.valid());
  ScopedDataset ds0b(H5Dcreate2(file.get(), "/ds0b", H5T_NATIVE_INT, sp0b.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds0b.valid());

  // Leave hole again right before conversion to test `location != H5SM_NO_LOC`
  // skip loop inside H5SM__convert_list_to_btree, by deleting /ds0b then
  // creating /ds0c and /ds3. Wait: with ds1a/ds1b (1) and ds2 (2), adding ds0c
  // gives 3 entries and adding ds3 converts LIST -> BTREE!
  ds0b.reset();
  ASSERT_GE(H5Ldelete(file.get(), "/ds0b", H5P_DEFAULT), 0);

  // Use an unlimited chunked dataset for ds0c so H5Dset_extent exercises
  // H5SM_delete with open_oh pointing to the same object header (line 2362).
  const std::array<hsize_t, 1> unlim = {H5S_UNLIMITED};
  const std::array<hsize_t, 1> chunk = {5};
  ScopedSpace sp_unlim(H5Screate_simple(1, d0b.data(), unlim.data()));
  ScopedPlist unlim_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(sp_unlim.valid() && unlim_dcpl.valid());
  ASSERT_GE(H5Pset_chunk(unlim_dcpl.get(), 1, chunk.data()), 0);
  ScopedDataset ds_ext(H5Dcreate2(file.get(), "/ds_ext", H5T_NATIVE_INT,
                                  sp_unlim.get(), H5P_DEFAULT, unlim_dcpl.get(),
                                  H5P_DEFAULT));
  ASSERT_TRUE(ds_ext.valid());
  const std::array<hsize_t, 1> d_grown = {25};
  ASSERT_GE(H5Dset_extent(ds_ext.get(), d_grown.data()), 0);
  ds_ext.reset();

  // Trigger LIST -> BTREE conversion (num_messages == 3 == list_max -> insert
  // 4th unique dataspace sp3). Both IN_HEAP (sp1) and IN_OH (sp2, sp_ext) are
  // read via H5SM__read_mesg during conversion.
  ScopedDataset ds3(H5Dcreate2(file.get(), "/ds3", H5T_NATIVE_INT, sp3.get(),
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds3.valid());

  // 2. While in BTREE mode:
  //    - Promote sp2 from IN_OH to IN_HEAP (ref=2) and then ref=3 via
  //      H5SM__incr_ref.
  ScopedDataset ds2_dup1(H5Dcreate2(file.get(), "/ds2_dup1", H5T_NATIVE_INT,
                                    sp2.get(), H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
  ScopedDataset ds2_dup2(H5Dcreate2(file.get(), "/ds2_dup2", H5T_NATIVE_INT,
                                    sp2.get(), H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
  ASSERT_TRUE(ds2_dup1.valid() && ds2_dup2.valid());

  // Exercise H5SM_try_share with H5SM_DEFER + H5SM_WAS_DEFERRED in BTREE mode
  // (hits H5B2_find in defer path!).
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedSpace sp2_copy(H5Scopy(sp2.get()));
    ASSERT_TRUE(sp2_copy.valid());
    H5S_t* sp2_obj = UnwrapSpace(sp2_copy.get());
    ASSERT_NE(sp2_obj, nullptr);
    ASSERT_GE(H5S_set_version(f, sp2_obj), 0);

    unsigned def_flags = 0;
    ASSERT_EQ(H5SM_try_share(f, nullptr, H5SM_DEFER, H5O_SDSPACE_ID,
                             &sp2_obj->extent, &def_flags),
              1);
    EXPECT_EQ(sp2_obj->extent.sh_loc.type, H5O_SHARE_TYPE_SOHM);
    ASSERT_EQ(H5SM_try_share(f, nullptr, H5SM_WAS_DEFERRED, H5O_SDSPACE_ID,
                             &sp2_obj->extent, &def_flags),
              1);
    hsize_t rc = 0;
    ASSERT_GE(
        H5SM_get_refcount(f, H5O_SDSPACE_ID, &sp2_obj->extent.sh_loc, &rc), 0);
    EXPECT_EQ(rc, 4u);

    // Check H5SM_ih_size in active BTREE mode before shrinking.
    hsize_t hdr_size = 0;
    H5_ih_info_t ih_info{};
    ASSERT_GE(H5SM_ih_size(f, &hdr_size, &ih_info), 0);
    EXPECT_GT(hdr_size, 0u);
    EXPECT_GT(ih_info.index_size, 0u);
    EXPECT_GT(ih_info.heap_size, 0u);

    // Decrement that extra reference (4 -> 3) via H5SM__decr_ref.
    ASSERT_GE(H5SM_delete(f, nullptr, &sp2_obj->extent.sh_loc), 0);
  }

  // Delete IN_OH entries (/ds_ext and /ds3 -> hits H5SM__decr_ref on IN_OH,
  // reducing num_messages from 4 -> 3 -> 2), then delete one IN_HEAP ref of sp1
  // (ref 2 -> 1, still 2 entries), then second ref of sp1 (ref 1 -> 0 ->
  // num_messages drops from 2 to 1 < btree_min == 2 -> BTREE -> LIST!).
  ds3.reset();
  ds1a.reset();
  ds1b.reset();
  ds2.reset();
  ds2_dup1.reset();
  ds2_dup2.reset();

  ASSERT_GE(H5Ldelete(file.get(), "/ds_ext", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete(file.get(), "/ds3", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete(file.get(), "/ds1a", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete(file.get(), "/ds1b", H5P_DEFAULT), 0);

  // Now index is back to LIST with 1 message (sp2, ref=3). Remove remaining 3
  // links so H5SM_delete clears the empty LIST index and heap.
  ASSERT_GE(H5Ldelete(file.get(), "/ds2", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete(file.get(), "/ds2_dup1", H5P_DEFAULT), 0);
  ASSERT_GE(H5Ldelete(file.get(), "/ds2_dup2", H5P_DEFAULT), 0);
}

TEST(H5SMTest, DirectBtreeStartDeleteEmptyAndDeleteErrorPaths) {
  // 1. list_max = 0, btree_min = 0: starts directly as BTREE and stays BTREE
  //    when emptied (H5SM__delete_index with btree_min == 0).
  {
    ScopedPlist fcpl = MakeSohmFcpl(1, 0, 0, H5O_SHMESG_SDSPACE_FLAG, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedFile f0 =
        CreateInMemoryFile("direct_bt0.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(f0.valid());

    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5F_t* f = UnwrapFile(f0.get());
      ASSERT_NE(f, nullptr);
      hsize_t hdr_size = 0;
      H5_ih_info_t ih_info{};
      ASSERT_GE(H5SM_ih_size(f, &hdr_size, &ih_info), 0);
      EXPECT_GT(hdr_size, 0u);
      EXPECT_EQ(ih_info.index_size, 0u);
    }

    const std::array<hsize_t, 2> dims = {3, 4};
    ScopedSpace sp(H5Screate_simple(2, dims.data(), nullptr));
    ASSERT_TRUE(sp.valid());
    ScopedDataset ds(H5Dcreate2(f0.get(), "/d", H5T_NATIVE_INT, sp.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());
    ds.reset();
    ASSERT_GE(H5Ldelete(f0.get(), "/d", H5P_DEFAULT), 0);
  }

  // 2. list_max = 2, btree_min = 1: grows LIST(2) -> BTREE(3), deletes down
  //    to BTREE(1 >= btree_min), then deleting the last item empties the BTREE
  //    while btree_min > 0, reverting index_type -> H5SM_LIST.
  {
    ScopedPlist fcpl = MakeSohmFcpl(1, 2, 1, H5O_SHMESG_SDSPACE_FLAG, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedFile f1 =
        CreateInMemoryFile("bt_empty_revert.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(f1.valid());

    const std::array<hsize_t, 1> da = {11};
    const std::array<hsize_t, 1> db = {22};
    const std::array<hsize_t, 1> dc = {33};
    ScopedSpace sa(H5Screate_simple(1, da.data(), nullptr));
    ScopedSpace sb(H5Screate_simple(1, db.data(), nullptr));
    ScopedSpace sc(H5Screate_simple(1, dc.data(), nullptr));
    ASSERT_TRUE(sa.valid() && sb.valid() && sc.valid());

    ScopedDataset d_a(H5Dcreate2(f1.get(), "/a", H5T_NATIVE_INT, sa.get(),
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ScopedDataset d_b(H5Dcreate2(f1.get(), "/b", H5T_NATIVE_INT, sb.get(),
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ScopedDataset d_c(H5Dcreate2(f1.get(), "/c", H5T_NATIVE_INT, sc.get(),
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_a.valid() && d_b.valid() && d_c.valid());
    d_a.reset();
    d_b.reset();
    d_c.reset();

    ASSERT_GE(H5Ldelete(f1.get(), "/a", H5P_DEFAULT), 0);
    ASSERT_GE(H5Ldelete(f1.get(), "/b", H5P_DEFAULT), 0);
    ASSERT_GE(H5Ldelete(f1.get(), "/c", H5P_DEFAULT), 0);
  }

  // 3. Delete & get_refcount error paths in both LIST and BTREE modes:
  //    unconfigured/invalid type, bad heap_id, and missing index entry.
  {
    ScopedPlist fcpl = MakeSohmFcpl(1, 2, 1, H5O_SHMESG_SDSPACE_FLAG, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedFile ferr =
        CreateInMemoryFile("sm_errors.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(ferr.valid());

    const std::array<hsize_t, 1> d1 = {5};
    const std::array<hsize_t, 1> d2 = {6};
    const std::array<hsize_t, 1> d3 = {7};
    const std::array<hsize_t, 1> d_unindexed = {99};
    ScopedSpace sp1(H5Screate_simple(1, d1.data(), nullptr));
    ScopedSpace sp2(H5Screate_simple(1, d2.data(), nullptr));
    ScopedSpace sp3(H5Screate_simple(1, d3.data(), nullptr));
    ScopedSpace sp_unindexed(
        H5Screate_simple(1, d_unindexed.data(), nullptr));
    ASSERT_TRUE(sp1.valid() && sp2.valid() && sp3.valid() &&
                sp_unindexed.valid());

    // Create 1 message in LIST mode to allocate the index heap.
    ScopedDataset ds1(H5Dcreate2(ferr.get(), "/ds1", H5T_NATIVE_INT, sp1.get(),
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ds1.valid());
    ds1.reset();

    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5F_t* f = UnwrapFile(ferr.get());
      H5S_t* unidx_obj = UnwrapSpace(sp_unindexed.get());
      ASSERT_NE(f, nullptr);
      ASSERT_NE(unidx_obj, nullptr);

      haddr_t heap_addr = HADDR_UNDEF;
      ASSERT_GE(H5SM_get_fheap_addr(f, H5O_SDSPACE_ID, &heap_addr), 0);
      ASSERT_NE(heap_addr, HADDR_UNDEF);

      const size_t raw_sz =
          H5O_msg_raw_size(f, H5O_SDSPACE_ID, true, &unidx_obj->extent);
      ASSERT_GT(raw_sz, 0u);
      std::vector<uint8_t> encoded(raw_sz);
      ASSERT_GE(H5O_msg_encode(f, H5O_SDSPACE_ID, true, encoded.data(),
                               &unidx_obj->extent),
                0);

      H5HF_t* fheap = H5HF_open(f, heap_addr);
      ASSERT_NE(fheap, nullptr);
      H5O_fheap_id_t orphan_id{};
      ASSERT_GE(H5HF_insert(fheap, encoded.size(), encoded.data(), &orphan_id),
                0);
      ASSERT_GE(H5HF_close(fheap), 0);

      absl::Cleanup remove_orphan_from_heap = [&] {
        H5HF_t* fh = H5HF_open(f, heap_addr);
        if (fh != nullptr) {
          H5HF_remove(fh, &orphan_id);
          H5HF_close(fh);
        }
      };

      H5O_shared_t sh{};
      ASSERT_GE(H5SM_reconstitute(&sh, f, H5O_SDSPACE_ID, orphan_id), 0);
      hsize_t rc = 0;

      {
        ScopedErrorSilence silence;
        // LIST mode: missing entry -> NOTFOUND.
        EXPECT_LT(H5SM_get_refcount(f, H5O_SDSPACE_ID, &sh, &rc), 0);
        EXPECT_LT(H5SM_delete(f, nullptr, &sh), 0);

        // Invalid / unconfigured message types.
        H5O_shared_t bad_sh = sh;
        bad_sh.msg_type_id = H5O_LINK_ID;
        EXPECT_LT(H5SM_get_refcount(f, H5O_LINK_ID, &sh, &rc), 0);
        EXPECT_LT(H5SM_delete(f, nullptr, &bad_sh), 0);
        bad_sh.msg_type_id = H5O_PLINE_ID;
        EXPECT_LT(H5SM_get_refcount(f, H5O_PLINE_ID, &sh, &rc), 0);
        EXPECT_LT(H5SM_delete(f, nullptr, &bad_sh), 0);

        // Corrupt heap ID fails H5SM__read_mesg inside get_refcount & delete.
        H5O_shared_t corrupt_sh{};
        H5O_fheap_id_t bad_id{};
        bad_id.val = 0xDEADBEEFULL;
        ASSERT_GE(H5SM_reconstitute(&corrupt_sh, f, H5O_SDSPACE_ID, bad_id), 0);
        EXPECT_LT(H5SM_get_refcount(f, H5O_SDSPACE_ID, &corrupt_sh, &rc), 0);
        EXPECT_LT(H5SM_delete(f, nullptr, &corrupt_sh), 0);
      }

      // Now add /ds2 and /ds3 to convert index to BTREE mode, then re-test
      // "message not in index" against H5B2_find / H5B2_modify.
      ScopedDataset ds2(H5Dcreate2(ferr.get(), "/ds2", H5T_NATIVE_INT,
                                   sp2.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
      ScopedDataset ds3(H5Dcreate2(ferr.get(), "/ds3", H5T_NATIVE_INT,
                                   sp3.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
      ASSERT_TRUE(ds2.valid() && ds3.valid());
      ds2.reset();
      ds3.reset();

      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5SM_get_refcount(f, H5O_SDSPACE_ID, &sh, &rc), 0);
        EXPECT_LT(H5SM_delete(f, nullptr, &sh), 0);
      }
    }
  }
}

TEST(H5SMTest, TableAndListDebugOutputAndValidationErrors) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 2), 0);
  ASSERT_GE(
      H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_SDSPACE_FLAG, 1), 0);
  ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 1, H5O_SHMESG_DTYPE_FLAG, 1),
            0);
  ASSERT_GE(H5Pset_shared_mesg_phase_change(fcpl.get(), 3, 2), 0);

  ScopedFile file =
      CreateInMemoryFile("sm_debug.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());

  // Populate index 0 (LIST) with:
  //   slot 0: d1 dataspace (IN_OH)
  //   slot 1: d2+d3 dataspace (IN_HEAP, ref=2)
  //   slot 2: d4 dataspace (IN_OH)
  const std::array<hsize_t, 1> da = {1};
  const std::array<hsize_t, 1> db = {2};
  const std::array<hsize_t, 1> dc = {3};
  ScopedSpace sa(H5Screate_simple(1, da.data(), nullptr));
  ScopedSpace sb(H5Screate_simple(1, db.data(), nullptr));
  ScopedSpace sc(H5Screate_simple(1, dc.data(), nullptr));
  ASSERT_TRUE(sa.valid() && sb.valid() && sc.valid());

  ScopedDataset d1(H5Dcreate2(file.get(), "/d1", H5T_NATIVE_INT, sa.get(),
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedDataset d2(H5Dcreate2(file.get(), "/d2", H5T_NATIVE_INT, sb.get(),
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedDataset d3(H5Dcreate2(file.get(), "/d3", H5T_NATIVE_INT, sb.get(),
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedDataset d4(H5Dcreate2(file.get(), "/d4", H5T_NATIVE_INT, sc.get(),
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(d1.valid() && d2.valid() && d3.valid() && d4.valid());
  d1.reset();
  d2.reset();
  d3.reset();
  d4.reset();

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  const haddr_t table_addr = H5F_SOHM_ADDR(f);
  ASSERT_NE(table_addr, HADDR_UNDEF);

  // Inspect table header to fetch index 0's list address and temporarily test
  // "Unknown" index_type string without dirtying cache.
  H5SM_table_cache_ud_t udata{f};
  auto* table = static_cast<H5SM_master_table_t*>(
      H5AC_protect(f, H5AC_SOHM_TABLE, table_addr, &udata, H5AC__READ_ONLY_FLAG));
  ASSERT_NE(table, nullptr);
  const haddr_t list0_addr = table->indexes[0].index_addr;
  const H5SM_index_type_t orig_type1 = table->indexes[1].index_type;
  table->indexes[1].index_type = H5SM_BADTYPE;
  ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_TABLE, table_addr, table, 0), 0);

  // Use a cleanup guard to restore indexes[1].index_type cleanly.
  absl::Cleanup restore_index1_type = [&] {
    auto* tbl = static_cast<H5SM_master_table_t*>(H5AC_protect(
        f, H5AC_SOHM_TABLE, table_addr, &udata, H5AC__READ_ONLY_FLAG));
    if (tbl != nullptr) {
      tbl->indexes[1].index_type = orig_type1;
      H5AC_unprotect(f, H5AC_SOHM_TABLE, table_addr, tbl, 0);
    }
  };

  herr_t rc = -1;
  std::string out = CaptureTableDebug(f, table_addr, UINT_MAX, UINT_MAX, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("Shared Message Master Table"));
  EXPECT_THAT(out, HasSubstr("List"));
  EXPECT_THAT(out, HasSubstr("Unknown"));

  // Explicit mismatching version / num_indexes prints warning banners.
  out = CaptureTableDebug(f, table_addr, 0, 1, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("NUMBER OF SOHM INDEXES DOESN'T MATCH"));

  {
    ScopedErrorSilence silence;
    out = CaptureTableDebug(f, table_addr, 99, 1, &rc);
    EXPECT_LT(rc, 0);
    EXPECT_THAT(out, HasSubstr("SOHM TABLE VERSION DOESN'T MATCH"));

    (void)CaptureTableDebug(f, table_addr, 0, 0, &rc);
    EXPECT_LT(rc, 0);

    (void)CaptureTableDebug(f, table_addr, 0, H5O_SHMESG_MAX_NINDEXES + 1,
                            &rc);
    EXPECT_LT(rc, 0);

    // Mismatched list_addr that isn't in table->indexes -> BADVALUE.
    (void)CaptureListDebug(f, 0x123456ULL, table_addr, &rc);
    EXPECT_LT(rc, 0);
  }

  // Restore index1 type before promoting/deleting.
  std::move(restore_index1_type).Invoke();

  // List debug with slot 0 IN_OH and slot 1 IN_HEAP.
  out = CaptureListDebug(f, list0_addr, table_addr, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("in object header"));
  EXPECT_THAT(out, HasSubstr("in heap"));

  // Delete /d1 (slot 0 becomes H5SM_NO_LOC) while num_messages becomes 2, so
  // x=0 hits "invalid" in H5SM_list_debug.
  ASSERT_GE(H5Ldelete(file.get(), "/d1", H5P_DEFAULT), 0);
  out = CaptureListDebug(f, list0_addr, table_addr, &rc);
  EXPECT_GE(rc, 0);
  EXPECT_THAT(out, HasSubstr("invalid"));
  EXPECT_THAT(out, HasSubstr("in heap"));
}

struct CustomPropList {
  ScopedH5Id<H5Pclose_class> cls;
  ScopedPlist plist;
  H5P_genplist_t* gen = nullptr;
};

CustomPropList MakePartialSohmPlist(const char* name, bool add_nindexes,
                                     bool add_types, bool add_minsize,
                                     bool add_list_max, bool add_btree_min) {
  CustomPropList out;
  out.cls.reset(H5Pcreate_class(H5P_ROOT, name, nullptr, nullptr, nullptr,
                                nullptr, nullptr, nullptr));
  if (!out.cls.valid()) {
    return out;
  }
  unsigned def_u = 0;
  std::array<unsigned, H5O_SHMESG_MAX_NINDEXES> def_arr{};
  if (add_nindexes &&
      H5Pregister2(out.cls.get(), H5F_CRT_SHMSG_NINDEXES_NAME, sizeof(unsigned),
                   &def_u, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                   nullptr) < 0) {
    return CustomPropList{};
  }
  if (add_types &&
      H5Pregister2(out.cls.get(), H5F_CRT_SHMSG_INDEX_TYPES_NAME,
                   sizeof(unsigned[H5O_SHMESG_MAX_NINDEXES]), def_arr.data(),
                   nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                   nullptr) < 0) {
    return CustomPropList{};
  }
  if (add_minsize &&
      H5Pregister2(out.cls.get(), H5F_CRT_SHMSG_INDEX_MINSIZE_NAME,
                   sizeof(unsigned[H5O_SHMESG_MAX_NINDEXES]), def_arr.data(),
                   nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                   nullptr) < 0) {
    return CustomPropList{};
  }
  if (add_list_max &&
      H5Pregister2(out.cls.get(), H5F_CRT_SHMSG_LIST_MAX_NAME, sizeof(unsigned),
                   &def_u, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                   nullptr) < 0) {
    return CustomPropList{};
  }
  if (add_btree_min &&
      H5Pregister2(out.cls.get(), H5F_CRT_SHMSG_BTREE_MIN_NAME,
                   sizeof(unsigned), &def_u, nullptr, nullptr, nullptr, nullptr,
                   nullptr, nullptr, nullptr) < 0) {
    return CustomPropList{};
  }
  out.plist.reset(H5Pcreate(out.cls.get()));
  if (out.plist.valid()) {
    out.gen = static_cast<H5P_genplist_t*>(
        H5I_object_verify(out.plist.get(), H5I_GENPROP_LST));
  }
  return out;
}

bool PadFilePast8K(hid_t file_id) {
  const hsize_t pad_len = 8192;
  ScopedType arr_dt(H5Tarray_create2(H5T_NATIVE_UINT8, 1, &pad_len));
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  if (!arr_dt.valid() || !scalar.valid()) {
    return false;
  }
  ScopedDataset dset(H5Dcreate2(file_id, "/pad", arr_dt.get(), scalar.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  if (!dset.valid()) {
    return false;
  }
  std::vector<uint8_t> zeros(pad_len, 0);
  return H5Dwrite(dset.get(), arr_dt.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                  zeros.data()) >= 0 &&
         H5Fflush(file_id, H5F_SCOPE_GLOBAL) >= 0;
}

TEST(H5SMTest, InitAndGetInfoPropertyErrors) {
  // Exercise H5SM_init property-get failures before index allocation and
  // H5SM_get_info property-set / bad-address / bad-table errors.
  ScopedFile nosohm = CreateInMemoryFile("sm_init_props.h5");
  ASSERT_TRUE(nosohm.valid());
  H5F_t* f_no = UnwrapFile(nosohm.get());
  ASSERT_NE(f_no, nullptr);
  ScopedGroup grp_no(H5Gcreate2(nosohm.get(), "/ext_dummy", H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp_no.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5O_loc_t* loc_no = H5O_get_loc(grp_no.get());
  ASSERT_NE(loc_no, nullptr);

  const unsigned orig_no_nidx = H5F_SOHM_NINDEXES(f_no);
  const unsigned orig_no_vers = H5F_SOHM_VERS(f_no);
  const haddr_t orig_no_addr = H5F_SOHM_ADDR(f_no);
  absl::Cleanup restore_no = [&] {
    H5F_SET_SOHM_ADDR(f_no, orig_no_addr);
    H5F_SET_SOHM_VERS(f_no, orig_no_vers);
    H5F_SET_SOHM_NINDEXES(f_no, orig_no_nidx);
  };

  auto p0 = MakePartialSohmPlist("sm_p0", false, false, false, false, false);
  auto p1 = MakePartialSohmPlist("sm_p1", true, true, false, false, false);
  auto p2 = MakePartialSohmPlist("sm_p2", true, true, false, true, false);
  auto p3 = MakePartialSohmPlist("sm_p3", true, true, false, true, true);
  ASSERT_NE(p0.gen, nullptr);
  ASSERT_NE(p1.gen, nullptr);
  ASSERT_NE(p2.gen, nullptr);
  ASSERT_NE(p3.gen, nullptr);

  {
    ScopedErrorSilence silence;
    // Non-SOHM file + missing NINDEXES property in H5SM_get_info.
    EXPECT_LT(H5SM_get_info(loc_no, p0.gen), 0);
    // Unreadable ext_loc in H5SM_get_info.
    H5O_loc_t bad_loc = *loc_no;
    bad_loc.addr = 0x7FFFFFFFULL;
    EXPECT_LT(H5SM_get_info(&bad_loc, p0.gen), 0);

    // Progressive missing properties in H5SM_init (all fail before line 179).
    H5F_SET_SOHM_NINDEXES(f_no, 1);
    EXPECT_LT(H5SM_init(f_no, p0.gen, loc_no), 0);
    EXPECT_LT(H5SM_init(f_no, p1.gen, loc_no), 0);
    EXPECT_LT(H5SM_init(f_no, p2.gen, loc_no), 0);
    EXPECT_LT(H5SM_init(f_no, p3.gen, loc_no), 0);
  }
  std::move(restore_no).Invoke();

  // SOHM file: attach H5O_SHMESG_ID to scratch groups to exercise all
  // H5SM_get_info set failures and invalid master-table protect error.
  ScopedPlist fcpl = MakeSohmFcpl(1, 3, 2, H5O_SHMESG_SDSPACE_FLAG, 1);
  ASSERT_TRUE(fcpl.valid());
  ScopedFile sohm =
      CreateInMemoryFile("sm_get_info_props.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(sohm.valid());
  H5F_t* f_sohm = UnwrapFile(sohm.get());
  ASSERT_NE(f_sohm, nullptr);

  const haddr_t orig_addr = H5F_SOHM_ADDR(f_sohm);
  const unsigned orig_vers = H5F_SOHM_VERS(f_sohm);
  const unsigned orig_nidx = H5F_SOHM_NINDEXES(f_sohm);
  const bool orig_crt_idx = H5F_STORE_MSG_CRT_IDX(f_sohm);
  absl::Cleanup restore_sohm = [&] {
    H5F_SET_SOHM_ADDR(f_sohm, orig_addr);
    H5F_SET_SOHM_VERS(f_sohm, orig_vers);
    H5F_SET_SOHM_NINDEXES(f_sohm, orig_nidx);
    H5F_SET_STORE_MSG_CRT_IDX(f_sohm, orig_crt_idx);
  };

  ScopedGroup grp_ok(H5Gcreate2(sohm.get(), "/ext_ok", H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup grp_bad(H5Gcreate2(sohm.get(), "/ext_bad", H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp_ok.valid() && grp_bad.valid());
  H5O_loc_t* loc_ok = H5O_get_loc(grp_ok.get());
  H5O_loc_t* loc_bad = H5O_get_loc(grp_bad.get());
  ASSERT_NE(loc_ok, nullptr);
  ASSERT_NE(loc_bad, nullptr);

  H5O_shmesg_table_t msg_ok{};
  msg_ok.addr = orig_addr;
  msg_ok.version = orig_vers;
  msg_ok.nindexes = orig_nidx;
  ASSERT_GE(H5O_msg_create(loc_ok, H5O_SHMESG_ID,
                           H5O_MSG_FLAG_CONSTANT | H5O_MSG_FLAG_DONTSHARE,
                           H5O_UPDATE_TIME, &msg_ok),
            0);

  H5O_shmesg_table_t msg_bad{};
  msg_bad.addr = 0x7FFFFFFFULL;
  msg_bad.version = orig_vers;
  msg_bad.nindexes = 1;
  ASSERT_GE(H5O_msg_create(loc_bad, H5O_SHMESG_ID,
                           H5O_MSG_FLAG_CONSTANT | H5O_MSG_FLAG_DONTSHARE,
                           H5O_UPDATE_TIME, &msg_bad),
            0);

  auto g0 = MakePartialSohmPlist("sm_g0", false, false, false, false, false);
  auto g1 = MakePartialSohmPlist("sm_g1", true, false, false, false, false);
  auto g2 = MakePartialSohmPlist("sm_g2", true, true, false, false, false);
  auto g3 = MakePartialSohmPlist("sm_g3", true, true, true, false, false);
  auto g4 = MakePartialSohmPlist("sm_g4", true, true, true, true, false);
  ASSERT_NE(g0.gen, nullptr);
  ASSERT_NE(g1.gen, nullptr);
  ASSERT_NE(g2.gen, nullptr);
  ASSERT_NE(g3.gen, nullptr);
  ASSERT_NE(g4.gen, nullptr);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5SM_get_info(loc_ok, g0.gen), 0);
    EXPECT_LT(H5SM_get_info(loc_ok, g1.gen), 0);
    EXPECT_LT(H5SM_get_info(loc_ok, g2.gen), 0);
    EXPECT_LT(H5SM_get_info(loc_ok, g3.gen), 0);
    EXPECT_LT(H5SM_get_info(loc_ok, g4.gen), 0);
    EXPECT_LT(H5SM_get_info(loc_bad, g4.gen), 0);
  }
}

TEST(H5SMTest, CorruptedAddressesErrorPaths) {
  // Exercise H5AC_protect / H5HF_open / H5B2_open error branches across SOHM
  // APIs using non-dirty out-of-range metadata addresses and clean restoration.
  {
    ScopedPlist fcpl = MakeSohmFcpl(1, 5, 3, H5O_SHMESG_SDSPACE_FLAG, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedFile file =
        CreateInMemoryFile("sm_bad_addrs_list.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());

    const std::array<hsize_t, 1> dims = {4};
    ScopedSpace sp(H5Screate_simple(1, dims.data(), nullptr));
    ASSERT_TRUE(sp.valid());
    ScopedDataset d1(H5Dcreate2(file.get(), "/d1", H5T_NATIVE_INT, sp.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ScopedDataset d2(H5Dcreate2(file.get(), "/d2", H5T_NATIVE_INT, sp.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d1.valid() && d2.valid());
    d1.reset();
    d2.reset();

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    H5S_t* sp_obj = UnwrapSpace(sp.get());
    ASSERT_NE(f, nullptr);
    ASSERT_NE(sp_obj, nullptr);

    const haddr_t orig_table_addr = H5F_SOHM_ADDR(f);
    H5SM_table_cache_ud_t tud{f};
    auto* tbl = static_cast<H5SM_master_table_t*>(H5AC_protect(
        f, H5AC_SOHM_TABLE, orig_table_addr, &tud, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(tbl, nullptr);
    const haddr_t orig_list_addr = tbl->indexes[0].index_addr;
    const haddr_t orig_heap_addr = tbl->indexes[0].heap_addr;
    ASSERT_NE(orig_list_addr, HADDR_UNDEF);
    ASSERT_NE(orig_heap_addr, HADDR_UNDEF);

    H5SM_list_cache_ud_t lud{f, &tbl->indexes[0]};
    auto* lst = static_cast<H5SM_list_t*>(H5AC_protect(
        f, H5AC_SOHM_LIST, orig_list_addr, &lud, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(lst, nullptr);
    ASSERT_EQ(lst->messages[0].location, H5SM_IN_HEAP);
    const H5O_fheap_id_t sp_heap_id = lst->messages[0].u.heap_loc.fheap_id;
    ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_LIST, orig_list_addr, lst, 0), 0);
    ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_TABLE, orig_table_addr, tbl, 0), 0);

    H5O_shared_t sp_sh{};
    ASSERT_GE(H5SM_reconstitute(&sp_sh, f, H5O_SDSPACE_ID, sp_heap_id), 0);

    herr_t rc = 0;
    hsize_t refcnt = 0;
    hsize_t hdr_sz = 0;
    H5_ih_info_t ih{};
    const haddr_t bogus_addr = 0x7FFFFFFFULL;

    // 1) Bogus master table address.
    {
      ScopedErrorSilence silence;
      (void)CaptureTableDebug(f, bogus_addr, UINT_MAX, UINT_MAX, &rc);
      EXPECT_LT(rc, 0);
      (void)CaptureListDebug(f, orig_list_addr, bogus_addr, &rc);
      EXPECT_LT(rc, 0);

      H5F_SET_SOHM_ADDR(f, bogus_addr);
      absl::Cleanup restore_tbl_addr = [&] {
        H5F_SET_SOHM_ADDR(f, orig_table_addr);
      };
      EXPECT_LT(H5SM_can_share(f, nullptr, nullptr, H5O_SDSPACE_ID, sp_obj), 0);
      EXPECT_LT(H5SM_try_share(f, nullptr, 0, H5O_SDSPACE_ID, sp_obj, nullptr),
                0);
      EXPECT_LT(H5SM_delete(f, nullptr, &sp_sh), 0);
      EXPECT_LT(H5SM_get_refcount(f, H5O_SDSPACE_ID, &sp_sh, &refcnt), 0);
      EXPECT_LT(H5SM_ih_size(f, &hdr_sz, &ih), 0);
    }

    // 2) Bogus fractal heap address inside table->indexes[0].
    {
      tbl = static_cast<H5SM_master_table_t*>(H5AC_protect(
          f, H5AC_SOHM_TABLE, orig_table_addr, &tud, H5AC__READ_ONLY_FLAG));
      ASSERT_NE(tbl, nullptr);
      tbl->indexes[0].heap_addr = bogus_addr;
      ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_TABLE, orig_table_addr, tbl, 0),
                0);
      absl::Cleanup restore_heap = [&] {
        auto* t = static_cast<H5SM_master_table_t*>(H5AC_protect(
            f, H5AC_SOHM_TABLE, orig_table_addr, &tud, H5AC__READ_ONLY_FLAG));
        if (t != nullptr) {
          t->indexes[0].heap_addr = orig_heap_addr;
          H5AC_unprotect(f, H5AC_SOHM_TABLE, orig_table_addr, t, 0);
        }
      };

      ScopedErrorSilence silence;
      EXPECT_LT(H5SM_try_share(f, nullptr, 0, H5O_SDSPACE_ID, sp_obj, nullptr),
                0);
      EXPECT_LT(H5SM_delete(f, nullptr, &sp_sh), 0);
      EXPECT_LT(H5SM_get_refcount(f, H5O_SDSPACE_ID, &sp_sh, &refcnt), 0);
      (void)CaptureListDebug(f, orig_list_addr, orig_table_addr, &rc);
      EXPECT_LT(rc, 0);
      EXPECT_LT(H5SM_ih_size(f, &hdr_sz, &ih), 0);
    }

    // 3) Bogus list index address inside table->indexes[0].
    {
      tbl = static_cast<H5SM_master_table_t*>(H5AC_protect(
          f, H5AC_SOHM_TABLE, orig_table_addr, &tud, H5AC__READ_ONLY_FLAG));
      ASSERT_NE(tbl, nullptr);
      tbl->indexes[0].index_addr = bogus_addr;
      ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_TABLE, orig_table_addr, tbl, 0),
                0);
      absl::Cleanup restore_idx = [&] {
        auto* t = static_cast<H5SM_master_table_t*>(H5AC_protect(
            f, H5AC_SOHM_TABLE, orig_table_addr, &tud, H5AC__READ_ONLY_FLAG));
        if (t != nullptr) {
          t->indexes[0].index_addr = orig_list_addr;
          H5AC_unprotect(f, H5AC_SOHM_TABLE, orig_table_addr, t, 0);
        }
      };

      ScopedErrorSilence silence;
      EXPECT_LT(H5SM_try_share(f, nullptr, 0, H5O_SDSPACE_ID, sp_obj, nullptr),
                0);
      EXPECT_LT(H5SM_delete(f, nullptr, &sp_sh), 0);
      EXPECT_LT(H5SM_get_refcount(f, H5O_SDSPACE_ID, &sp_sh, &refcnt), 0);
      (void)CaptureListDebug(f, bogus_addr, orig_table_addr, &rc);
      EXPECT_LT(rc, 0);
    }
  }

  // 4) Bogus v2 B-tree index address inside table->indexes[0].
  {
    ScopedPlist fcpl = MakeSohmFcpl(1, 1, 1, H5O_SHMESG_SDSPACE_FLAG, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedFile file =
        CreateInMemoryFile("sm_bad_addrs_bt2.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());

    const std::array<hsize_t, 1> dims1 = {4};
    const std::array<hsize_t, 1> dims2 = {8};
    ScopedSpace sp1(H5Screate_simple(1, dims1.data(), nullptr));
    ScopedSpace sp2(H5Screate_simple(1, dims2.data(), nullptr));
    ASSERT_TRUE(sp1.valid() && sp2.valid());

    ScopedDataset d1(H5Dcreate2(file.get(), "/d1", H5T_NATIVE_INT, sp1.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ScopedDataset d2(H5Dcreate2(file.get(), "/d2", H5T_NATIVE_INT, sp1.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ScopedDataset d3(H5Dcreate2(file.get(), "/d3", H5T_NATIVE_INT, sp2.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d1.valid() && d2.valid() && d3.valid());
    d1.reset();
    d2.reset();
    d3.reset();

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    H5S_t* sp1_obj = UnwrapSpace(sp1.get());
    ASSERT_NE(f, nullptr);
    ASSERT_NE(sp1_obj, nullptr);

    // Reopen /d2 (the second dataset sharing sp1, stored via H5O_SHARE_TYPE_SOHM).
    H5O_loc_t* d2_oloc = nullptr;
    ScopedDataset d2_reopen(H5Dopen2(file.get(), "/d2", H5P_DEFAULT));
    ASSERT_TRUE(d2_reopen.valid());
    d2_oloc = H5O_get_loc(d2_reopen.get());
    ASSERT_NE(d2_oloc, nullptr);
    H5O_shared_t sp1_sh{};
    void* read_res = H5O_msg_read(d2_oloc, H5O_SDSPACE_ID, nullptr);
    ASSERT_NE(read_res, nullptr);
    sp1_sh = *static_cast<H5O_shared_t*>(read_res);
    H5O_msg_free(H5O_SDSPACE_ID, read_res);
    d2_reopen.reset();
    ASSERT_EQ(sp1_sh.type, H5O_SHARE_TYPE_SOHM);

    const haddr_t table_addr = H5F_SOHM_ADDR(f);
    H5SM_table_cache_ud_t tud{f};
    auto* tbl = static_cast<H5SM_master_table_t*>(H5AC_protect(
        f, H5AC_SOHM_TABLE, table_addr, &tud, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(tbl, nullptr);
    ASSERT_EQ(tbl->indexes[0].index_type, H5SM_BTREE);
    const haddr_t orig_bt2_addr = tbl->indexes[0].index_addr;
    tbl->indexes[0].index_addr = 0x7FFFFFFFULL;
    ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_TABLE, table_addr, tbl, 0), 0);
    absl::Cleanup restore_bt2 = [&] {
      auto* t = static_cast<H5SM_master_table_t*>(H5AC_protect(
          f, H5AC_SOHM_TABLE, table_addr, &tud, H5AC__READ_ONLY_FLAG));
      if (t != nullptr) {
        t->indexes[0].index_addr = orig_bt2_addr;
        H5AC_unprotect(f, H5AC_SOHM_TABLE, table_addr, t, 0);
      }
    };

    ScopedErrorSilence silence;
    hsize_t refcnt = 0;
    hsize_t hdr_sz = 0;
    H5_ih_info_t ih{};
    EXPECT_LT(H5SM_try_share(f, nullptr, 0, H5O_SDSPACE_ID, sp1_obj, nullptr),
              0);
    EXPECT_LT(H5SM_delete(f, nullptr, &sp1_sh), 0);
    EXPECT_LT(H5SM_get_refcount(f, H5O_SDSPACE_ID, &sp1_sh, &refcnt), 0);
    EXPECT_LT(H5SM_ih_size(f, &hdr_sz, &ih), 0);
  }
}

TEST(H5SMTest, EncodingDeletionComparisonAndBtreeFailurePaths) {
  // 1) Encode failure in H5SM__write_mesg, bogus IN_OH open failure in
  //    H5SM__read_mesg, undecodable heap message in H5SM_delete, and
  //    hash-collision H5SM__message_compare failure in list index.
  {
    ScopedPlist fcpl =
        MakeSohmFcpl(1, 5, 3, H5O_SHMESG_SDSPACE_FLAG | H5O_SHMESG_DTYPE_FLAG,
                     1);
    ASSERT_TRUE(fcpl.valid());
    ScopedFile file =
        CreateInMemoryFile("sm_cmp_list.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());

    const std::array<hsize_t, 1> d_keep = {5};
    ScopedSpace sp_keep(H5Screate_simple(1, d_keep.data(), nullptr));
    ASSERT_TRUE(sp_keep.valid());
    ScopedDataset dk1(H5Dcreate2(file.get(), "/dk1", H5T_NATIVE_INT,
                                 sp_keep.get(), H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT));
    ScopedDataset dk2(H5Dcreate2(file.get(), "/dk2", H5T_NATIVE_INT,
                                 sp_keep.get(), H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT));
    ASSERT_TRUE(dk1.valid() && dk2.valid());
    dk1.reset();
    dk2.reset();

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    // 1a) Datatype encode failure (H5T_PAD_BACKGROUND not encodable to file).
    ScopedType bad_dt(H5Tcopy(H5T_NATIVE_INT));
    ASSERT_TRUE(bad_dt.valid());
    ASSERT_GE(H5Tset_pad(bad_dt.get(), H5T_PAD_BACKGROUND, H5T_PAD_ZERO), 0);
    void* bad_dt_obj = UnwrapDatatype(bad_dt.get());
    ASSERT_NE(bad_dt_obj, nullptr);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(
          H5SM_try_share(f, nullptr, 0, H5O_DTYPE_ID, bad_dt_obj, nullptr), 0);
    }

    // 1b) H5SM_delete with H5O_SHARE_TYPE_HERE pointing to bogus OH addr ->
    //     hits H5O_protect failure (line 2359) and increments nopen_objs once.
    {
      H5O_shared_t bad_oh_sh{};
      bad_oh_sh.type = H5O_SHARE_TYPE_HERE;
      bad_oh_sh.file = f;
      bad_oh_sh.msg_type_id = H5O_SDSPACE_ID;
      bad_oh_sh.u.loc.oh_addr = 0x7FFFFFFFULL;
      bad_oh_sh.u.loc.index = 0;
      ScopedErrorSilence silence;
      EXPECT_LT(H5SM_delete(f, nullptr, &bad_oh_sh), 0);
      H5F_DECR_NOPEN_OBJS(f);
    }

    // Protect table & list to insert synthetic test heap entries.
    const haddr_t table_addr = H5F_SOHM_ADDR(f);
    H5SM_table_cache_ud_t tud{f};
    auto* tbl = static_cast<H5SM_master_table_t*>(H5AC_protect(
        f, H5AC_SOHM_TABLE, table_addr, &tud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(tbl, nullptr);
    const haddr_t list_addr = tbl->indexes[0].index_addr;
    const haddr_t heap_addr = tbl->indexes[0].heap_addr;
    H5HF_t* fheap = H5HF_open(f, heap_addr);
    ASSERT_NE(fheap, nullptr);

    // 1c) Undecodable garbage message with ref=1 in fheap+list -> removed from
    //     index & heap, then fails cleanly at H5O_msg_decode (line 1577).
    {
      std::array<uint8_t, 16> garbage{};
      garbage.fill(0xFF);
      H5O_fheap_id_t bad_hid{};
      ASSERT_GE(H5HF_insert(fheap, garbage.size(), garbage.data(), &bad_hid),
                0);
      ASSERT_GE(H5HF_close(fheap), 0);
      fheap = nullptr;

      H5SM_list_cache_ud_t lud{f, &tbl->indexes[0]};
      auto* lst = static_cast<H5SM_list_t*>(
          H5AC_protect(f, H5AC_SOHM_LIST, list_addr, &lud, H5AC__NO_FLAGS_SET));
      ASSERT_NE(lst, nullptr);
      size_t empty_idx = SIZE_MAX;
      for (size_t i = 0; i < tbl->indexes[0].list_max; ++i) {
        if (lst->messages[i].location == H5SM_NO_LOC) {
          empty_idx = i;
          break;
        }
      }
      ASSERT_NE(empty_idx, SIZE_MAX);
      lst->messages[empty_idx].location = H5SM_IN_HEAP;
      lst->messages[empty_idx].hash = 0x11111111u;
      lst->messages[empty_idx].msg_type_id = H5O_SDSPACE_ID;
      lst->messages[empty_idx].u.heap_loc.ref_count = 1;
      lst->messages[empty_idx].u.heap_loc.fheap_id = bad_hid;
      ++tbl->indexes[0].num_messages;
      ASSERT_GE(
          H5AC_unprotect(f, H5AC_SOHM_LIST, list_addr, lst, H5AC__DIRTIED_FLAG),
          0);
      ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_TABLE, table_addr, tbl,
                               H5AC__DIRTIED_FLAG),
                0);
      tbl = nullptr;

      H5O_shared_t bad_sh{};
      ASSERT_GE(H5SM_reconstitute(&bad_sh, f, H5O_SDSPACE_ID, bad_hid), 0);
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5SM_delete(f, nullptr, &bad_sh), 0);
      }

      tbl = static_cast<H5SM_master_table_t*>(H5AC_protect(
          f, H5AC_SOHM_TABLE, table_addr, &tud, H5AC__NO_FLAGS_SET));
      ASSERT_NE(tbl, nullptr);
      fheap = H5HF_open(f, heap_addr);
      ASSERT_NE(fheap, nullptr);
    }

    // 1d) Hash collision with invalid heap ID in list slot -> triggers
    //     H5HF_op failure in H5SM__message_compare across try_share, delete,
    //     and get_refcount.
    {
      const std::array<hsize_t, 1> d_tgt = {99};
      ScopedSpace sp_tgt(H5Screate_simple(1, d_tgt.data(), nullptr));
      ASSERT_TRUE(sp_tgt.valid());
      H5S_t* sp_tgt_obj = UnwrapSpace(sp_tgt.get());
      ASSERT_NE(sp_tgt_obj, nullptr);

      const size_t raw_sz =
          H5O_msg_raw_size(f, H5O_SDSPACE_ID, true, sp_tgt_obj);
      ASSERT_GT(raw_sz, 0u);
      std::vector<uint8_t> enc(raw_sz);
      ASSERT_GE(H5O_msg_encode(f, H5O_SDSPACE_ID, true, enc.data(), sp_tgt_obj),
                0);
      const uint32_t key_hash =
          H5_checksum_lookup3(enc.data(), raw_sz, H5O_SDSPACE_ID);

      H5O_fheap_id_t real_id{};
      ASSERT_GE(H5HF_insert(fheap, enc.size(), enc.data(), &real_id), 0);
      ASSERT_GE(H5HF_close(fheap), 0);
      fheap = nullptr;

      H5SM_list_cache_ud_t lud{f, &tbl->indexes[0]};
      auto* lst = static_cast<H5SM_list_t*>(
          H5AC_protect(f, H5AC_SOHM_LIST, list_addr, &lud, H5AC__NO_FLAGS_SET));
      ASSERT_NE(lst, nullptr);
      size_t empty_idx = SIZE_MAX;
      for (size_t i = 0; i < tbl->indexes[0].list_max; ++i) {
        if (lst->messages[i].location == H5SM_NO_LOC) {
          empty_idx = i;
          break;
        }
      }
      ASSERT_NE(empty_idx, SIZE_MAX);
      lst->messages[empty_idx].location = H5SM_IN_HEAP;
      lst->messages[empty_idx].hash = key_hash;
      lst->messages[empty_idx].msg_type_id = H5O_SDSPACE_ID;
      lst->messages[empty_idx].u.heap_loc.ref_count = 1;
      lst->messages[empty_idx].u.heap_loc.fheap_id.val = 0xDEADBEEFULL;
      ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_LIST, list_addr, lst, 0), 0);
      ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_TABLE, table_addr, tbl, 0), 0);
      tbl = nullptr;

      absl::Cleanup cleanup_collision = [&] {
        auto* t = static_cast<H5SM_master_table_t*>(H5AC_protect(
            f, H5AC_SOHM_TABLE, table_addr, &tud, H5AC__NO_FLAGS_SET));
        if (t != nullptr) {
          H5SM_list_cache_ud_t l_ud{f, &t->indexes[0]};
          auto* l = static_cast<H5SM_list_t*>(H5AC_protect(
              f, H5AC_SOHM_LIST, list_addr, &l_ud, H5AC__NO_FLAGS_SET));
          if (l != nullptr) {
            l->messages[empty_idx].location = H5SM_NO_LOC;
            H5AC_unprotect(f, H5AC_SOHM_LIST, list_addr, l, 0);
          }
          H5AC_unprotect(f, H5AC_SOHM_TABLE, table_addr, t, 0);
        }
        if (H5HF_t* fh = H5HF_open(f, heap_addr); fh != nullptr) {
          H5HF_remove(fh, &real_id);
          H5HF_close(fh);
        }
      };

      H5O_shared_t orphan_sh{};
      ASSERT_GE(H5SM_reconstitute(&orphan_sh, f, H5O_SDSPACE_ID, real_id), 0);
      hsize_t refcnt = 0;

      ScopedErrorSilence silence;
      EXPECT_LT(
          H5SM_try_share(f, nullptr, 0, H5O_SDSPACE_ID, sp_tgt_obj, nullptr),
          0);
      EXPECT_LT(H5SM_delete(f, nullptr, &orphan_sh), 0);
      EXPECT_LT(H5SM_get_refcount(f, H5O_SDSPACE_ID, &orphan_sh, &refcnt), 0);
    }
  }

  // 2) H5O_msg_delete failure after H5O_msg_decode succeeds in H5SM_delete
  //    (line 1580): shared attribute referencing a shared compound datatype.
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 2), 0);
    ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_ATTR_FLAG, 1),
              0);
    ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 1, H5O_SHMESG_DTYPE_FLAG, 1),
              0);
    ASSERT_GE(H5Pset_shared_mesg_phase_change(fcpl.get(), 5, 3), 0);

    ScopedFile file =
        CreateInMemoryFile("sm_del_msg_fail.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());

    ScopedType cdt(H5Tcreate(H5T_COMPOUND, 8));
    ASSERT_TRUE(cdt.valid());
    ASSERT_GE(H5Tinsert(cdt.get(), "a", 0, H5T_NATIVE_INT), 0);
    ASSERT_GE(H5Tinsert(cdt.get(), "b", 4, H5T_NATIVE_INT), 0);
    ScopedSpace sc(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(sc.valid());

    struct Pair {
      int a;
      int b;
    } val{11, 22};

    ScopedDataset d1(H5Dcreate2(file.get(), "/d1", cdt.get(), sc.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ScopedDataset d2(H5Dcreate2(file.get(), "/d2", cdt.get(), sc.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ScopedGroup grp(H5Gcreate2(file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(d1.valid() && d2.valid() && grp.valid());

    ScopedAttr a1(H5Acreate2(grp.get(), "keep", cdt.get(), sc.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ScopedAttr a2(H5Acreate2(d1.get(), "keep", cdt.get(), sc.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a1.valid() && a2.valid());
    ASSERT_GE(H5Awrite(a1.get(), cdt.get(), &val), 0);
    ASSERT_GE(H5Awrite(a2.get(), cdt.get(), &val), 0);
    a1.reset();
    a2.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    // Reopen attribute to obtain decoded H5A_t with nested shared dtype.
    a1.reset(H5Aopen(grp.get(), "keep", H5P_DEFAULT));
    ASSERT_TRUE(a1.valid());
    auto* a1_obj =
        static_cast<H5A_t*>(H5VL_object_verify(a1.get(), H5I_ATTR));
    ASSERT_NE(a1_obj, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    const haddr_t table_addr = H5F_SOHM_ADDR(f);
    H5SM_table_cache_ud_t tud{f};
    auto* tbl = static_cast<H5SM_master_table_t*>(H5AC_protect(
        f, H5AC_SOHM_TABLE, table_addr, &tud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(tbl, nullptr);
    ASSERT_NE(tbl->indexes[0].index_addr, HADDR_UNDEF);
    ASSERT_NE(tbl->indexes[1].index_addr, HADDR_UNDEF);

    // Temporarily change attribute name ("keep" -> "zbad") to encode a unique
    // attribute that embeds nested SOHM dtype, then restore immediately.
    char* orig_attr_name = a1_obj->shared->name;
    char syn_name[] = "zbad";
    a1_obj->shared->name = syn_name;
    const size_t raw_sz = H5O_msg_raw_size(f, H5O_ATTR_ID, true, a1_obj);
    ASSERT_GT(raw_sz, 0u);
    std::vector<uint8_t> enc(raw_sz);
    ASSERT_GE(H5O_msg_encode(f, H5O_ATTR_ID, true, enc.data(), a1_obj), 0);
    a1_obj->shared->name = orig_attr_name;
    a1.reset();

    const uint32_t syn_hash =
        H5_checksum_lookup3(enc.data(), raw_sz, H5O_ATTR_ID);
    H5HF_t* fh0 = H5HF_open(f, tbl->indexes[0].heap_addr);
    ASSERT_NE(fh0, nullptr);
    H5O_fheap_id_t syn_id{};
    ASSERT_GE(H5HF_insert(fh0, enc.size(), enc.data(), &syn_id), 0);
    ASSERT_GE(H5HF_close(fh0), 0);

    H5SM_list_cache_ud_t lud0{f, &tbl->indexes[0]};
    auto* lst0 = static_cast<H5SM_list_t*>(H5AC_protect(
        f, H5AC_SOHM_LIST, tbl->indexes[0].index_addr, &lud0,
        H5AC__NO_FLAGS_SET));
    ASSERT_NE(lst0, nullptr);
    size_t empty0 = SIZE_MAX;
    for (size_t i = 0; i < tbl->indexes[0].list_max; ++i) {
      if (lst0->messages[i].location == H5SM_NO_LOC) {
        empty0 = i;
        break;
      }
    }
    ASSERT_NE(empty0, SIZE_MAX);
    lst0->messages[empty0].location = H5SM_IN_HEAP;
    lst0->messages[empty0].hash = syn_hash;
    lst0->messages[empty0].msg_type_id = H5O_ATTR_ID;
    lst0->messages[empty0].u.heap_loc.ref_count = 1;
    lst0->messages[empty0].u.heap_loc.fheap_id = syn_id;
    ++tbl->indexes[0].num_messages;
    ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_LIST, tbl->indexes[0].index_addr,
                             lst0, H5AC__DIRTIED_FLAG),
              0);

    // Keep indexes[1].heap_addr intact so H5O__attr_decode succeeds, while
    // corrupting indexes[1].index_addr so nested dtype H5SM_delete fails.
    const haddr_t orig_dt_idx_addr = tbl->indexes[1].index_addr;
    tbl->indexes[1].index_addr = 0x7FFFFFFFULL;
    ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_TABLE, table_addr, tbl,
                             H5AC__DIRTIED_FLAG),
              0);
    absl::Cleanup restore_dt_idx = [&] {
      auto* t = static_cast<H5SM_master_table_t*>(H5AC_protect(
          f, H5AC_SOHM_TABLE, table_addr, &tud, H5AC__READ_ONLY_FLAG));
      if (t != nullptr) {
        t->indexes[1].index_addr = orig_dt_idx_addr;
        H5AC_unprotect(f, H5AC_SOHM_TABLE, table_addr, t, 0);
      }
    };

    H5O_shared_t syn_sh{};
    ASSERT_GE(H5SM_reconstitute(&syn_sh, f, H5O_ATTR_ID, syn_id), 0);
    ScopedErrorSilence silence;
    EXPECT_LT(H5SM_delete(f, nullptr, &syn_sh), 0);
  }

  // 3) H5SM__convert_list_to_btree read failure (lines 732, 1441) and duplicate
  //    B-tree insert failure + encoding_buf free (lines 738, 769).
  {
    ScopedPlist fcpl = MakeSohmFcpl(1, 2, 1, H5O_SHMESG_SDSPACE_FLAG, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedFile file =
        CreateInMemoryFile("sm_conv_bt2_err.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());

    const std::array<hsize_t, 1> d1_dims = {4};
    const std::array<hsize_t, 1> d2_dims = {12};
    ScopedSpace sp1(H5Screate_simple(1, d1_dims.data(), nullptr));
    ScopedSpace sp2(H5Screate_simple(1, d2_dims.data(), nullptr));
    ASSERT_TRUE(sp1.valid() && sp2.valid());

    ScopedDataset d1(H5Dcreate2(file.get(), "/d1", H5T_NATIVE_INT, sp1.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ScopedDataset d2(H5Dcreate2(file.get(), "/d2", H5T_NATIVE_INT, sp1.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ScopedGroup holder(H5Gcreate2(file.get(), "/holder", H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d1.valid() && d2.valid() && holder.valid());
    d1.reset();
    d2.reset();

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    H5S_t* sp2_obj = UnwrapSpace(sp2.get());
    H5O_loc_t* h_oloc = H5O_get_loc(holder.get());
    ASSERT_NE(f, nullptr);
    ASSERT_NE(sp2_obj, nullptr);
    ASSERT_NE(h_oloc, nullptr);

    H5O_t* h_oh = H5O_protect(h_oloc, H5AC__NO_FLAGS_SET, false);
    ASSERT_NE(h_oh, nullptr);
    absl::Cleanup unprot_oh = [&] {
      H5O_unprotect(h_oloc, h_oh, H5AC__NO_FLAGS_SET);
    };

    const haddr_t table_addr = H5F_SOHM_ADDR(f);
    H5SM_table_cache_ud_t tud{f};
    auto* tbl = static_cast<H5SM_master_table_t*>(H5AC_protect(
        f, H5AC_SOHM_TABLE, table_addr, &tud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(tbl, nullptr);
    const haddr_t list_addr = tbl->indexes[0].index_addr;
    H5SM_list_cache_ud_t lud{f, &tbl->indexes[0]};
    auto* lst = static_cast<H5SM_list_t*>(
        H5AC_protect(f, H5AC_SOHM_LIST, list_addr, &lud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(lst, nullptr);
    ASSERT_EQ(lst->messages[0].location, H5SM_IN_HEAP);
    ASSERT_EQ(lst->messages[1].location, H5SM_NO_LOC);

    // 3a) Unreadable entry in slot 1 -> fails H5SM__read_mesg at line 732.
    lst->messages[1].location = H5SM_IN_HEAP;
    lst->messages[1].hash = 0x12345678u;
    lst->messages[1].msg_type_id = H5O_SDSPACE_ID;
    lst->messages[1].u.heap_loc.ref_count = 1;
    lst->messages[1].u.heap_loc.fheap_id.val = 0xDEADBEEFULL;
    tbl->indexes[0].num_messages = 2;
    ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_LIST, list_addr, lst, 0), 0);
    ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_TABLE, table_addr, tbl, 0), 0);

    {
      ScopedErrorSilence silence;
      EXPECT_LT(
          H5SM_try_share(f, h_oh, 0, H5O_SDSPACE_ID, sp2_obj, nullptr), 0);
    }

    // 3b) Duplicate slot 0 into slot 1 -> slot 0 & slot 1 have identical heap
    //     ID and compare equal in H5B2_insert -> fails line 738 and frees
    //     encoding_buf at line 769.
    tbl = static_cast<H5SM_master_table_t*>(H5AC_protect(
        f, H5AC_SOHM_TABLE, table_addr, &tud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(tbl, nullptr);
    lst = static_cast<H5SM_list_t*>(
        H5AC_protect(f, H5AC_SOHM_LIST, list_addr, &lud, H5AC__NO_FLAGS_SET));
    ASSERT_NE(lst, nullptr);
    lst->messages[1] = lst->messages[0];
    ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_LIST, list_addr, lst, 0), 0);
    ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_TABLE, table_addr, tbl, 0), 0);

    absl::Cleanup restore_list = [&] {
      auto* t = static_cast<H5SM_master_table_t*>(H5AC_protect(
          f, H5AC_SOHM_TABLE, table_addr, &tud, H5AC__NO_FLAGS_SET));
      if (t != nullptr) {
        H5SM_list_cache_ud_t l_ud{f, &t->indexes[0]};
        auto* l = static_cast<H5SM_list_t*>(H5AC_protect(
            f, H5AC_SOHM_LIST, list_addr, &l_ud, H5AC__NO_FLAGS_SET));
        if (l != nullptr) {
          l->messages[1].location = H5SM_NO_LOC;
          H5AC_unprotect(f, H5AC_SOHM_LIST, list_addr, l, 0);
        }
        t->indexes[0].num_messages = 1;
        H5AC_unprotect(f, H5AC_SOHM_TABLE, table_addr, t, 0);
      }
    };

    {
      ScopedErrorSilence silence;
      EXPECT_LT(
          H5SM_try_share(f, h_oh, 0, H5O_SDSPACE_ID, sp2_obj, nullptr), 0);
    }
  }

  // 4) Hash collision inside H5SM_BTREE: H5B2_find (defer, line 1335),
  //    H5B2_modify (non-defer, line 1349), and H5SM_get_refcount (line 2190).
  {
    ScopedPlist fcpl = MakeSohmFcpl(1, 1, 1, H5O_SHMESG_SDSPACE_FLAG, 1);
    ASSERT_TRUE(fcpl.valid());
    ScopedFile file =
        CreateInMemoryFile("sm_cmp_bt2.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());

    const std::array<hsize_t, 1> d1_dims = {4};
    const std::array<hsize_t, 1> d2_dims = {8};
    const std::array<hsize_t, 1> dt_dims = {16};
    ScopedSpace sp1(H5Screate_simple(1, d1_dims.data(), nullptr));
    ScopedSpace sp2(H5Screate_simple(1, d2_dims.data(), nullptr));
    ScopedSpace sp_tgt(H5Screate_simple(1, dt_dims.data(), nullptr));
    ASSERT_TRUE(sp1.valid() && sp2.valid() && sp_tgt.valid());

    ScopedDataset d1(H5Dcreate2(file.get(), "/d1", H5T_NATIVE_INT, sp1.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ScopedDataset d2(H5Dcreate2(file.get(), "/d2", H5T_NATIVE_INT, sp1.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ScopedDataset d3(H5Dcreate2(file.get(), "/d3", H5T_NATIVE_INT, sp2.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d1.valid() && d2.valid() && d3.valid());
    d1.reset();
    d2.reset();
    d3.reset();

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    H5S_t* sp_tgt_obj = UnwrapSpace(sp_tgt.get());
    ASSERT_NE(f, nullptr);
    ASSERT_NE(sp_tgt_obj, nullptr);

    const haddr_t table_addr = H5F_SOHM_ADDR(f);
    H5SM_table_cache_ud_t tud{f};
    auto* tbl = static_cast<H5SM_master_table_t*>(H5AC_protect(
        f, H5AC_SOHM_TABLE, table_addr, &tud, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(tbl, nullptr);
    ASSERT_EQ(tbl->indexes[0].index_type, H5SM_BTREE);
    const haddr_t orig_bt2_addr = tbl->indexes[0].index_addr;
    const haddr_t heap_addr = tbl->indexes[0].heap_addr;
    ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_TABLE, table_addr, tbl, 0), 0);

    const size_t raw_sz =
        H5O_msg_raw_size(f, H5O_SDSPACE_ID, true, sp_tgt_obj);
    std::vector<uint8_t> enc(raw_sz);
    ASSERT_GE(H5O_msg_encode(f, H5O_SDSPACE_ID, true, enc.data(), sp_tgt_obj),
              0);
    const uint32_t key_hash =
        H5_checksum_lookup3(enc.data(), raw_sz, H5O_SDSPACE_ID);

    H5HF_t* fheap = H5HF_open(f, heap_addr);
    ASSERT_NE(fheap, nullptr);
    H5O_fheap_id_t real_id{};
    ASSERT_GE(H5HF_insert(fheap, enc.size(), enc.data(), &real_id), 0);
    ASSERT_GE(H5HF_close(fheap), 0);

    H5B2_create_t bt2_cparam{};
    bt2_cparam.cls = H5SM_INDEX;
    bt2_cparam.node_size = static_cast<uint32_t>(H5SM_B2_NODE_SIZE);
    bt2_cparam.rrec_size = static_cast<uint32_t>(H5SM_SOHM_ENTRY_SIZE(f));
    bt2_cparam.split_percent = H5SM_B2_SPLIT_PERCENT;
    bt2_cparam.merge_percent = H5SM_B2_MERGE_PERCENT;
    H5B2_t* bad_bt2 = H5B2_create(f, &bt2_cparam, f);
    ASSERT_NE(bad_bt2, nullptr);

    H5SM_mesg_key_t bad_key{};
    bad_key.file = f;
    bad_key.message.location = H5SM_IN_HEAP;
    bad_key.message.hash = key_hash;
    bad_key.message.msg_type_id = H5O_SDSPACE_ID;
    bad_key.message.u.heap_loc.ref_count = 1;
    bad_key.message.u.heap_loc.fheap_id.val = 0xDEADBEEFULL;
    ASSERT_GE(H5B2_insert(bad_bt2, &bad_key), 0);
    haddr_t bad_bt2_addr = HADDR_UNDEF;
    ASSERT_GE(H5B2_get_addr(bad_bt2, &bad_bt2_addr), 0);
    ASSERT_GE(H5B2_close(bad_bt2), 0);

    tbl = static_cast<H5SM_master_table_t*>(H5AC_protect(
        f, H5AC_SOHM_TABLE, table_addr, &tud, H5AC__READ_ONLY_FLAG));
    ASSERT_NE(tbl, nullptr);
    tbl->indexes[0].index_addr = bad_bt2_addr;
    ASSERT_GE(H5AC_unprotect(f, H5AC_SOHM_TABLE, table_addr, tbl, 0), 0);

    absl::Cleanup cleanup_bt2 = [&] {
      auto* t = static_cast<H5SM_master_table_t*>(H5AC_protect(
          f, H5AC_SOHM_TABLE, table_addr, &tud, H5AC__READ_ONLY_FLAG));
      if (t != nullptr) {
        t->indexes[0].index_addr = orig_bt2_addr;
        H5AC_unprotect(f, H5AC_SOHM_TABLE, table_addr, t, 0);
      }
      H5B2_delete(f, bad_bt2_addr, f, nullptr, nullptr);
      if (H5HF_t* fh = H5HF_open(f, heap_addr); fh != nullptr) {
        H5HF_remove(fh, &real_id);
        H5HF_close(fh);
      }
    };

    H5O_shared_t orphan_sh{};
    ASSERT_GE(H5SM_reconstitute(&orphan_sh, f, H5O_SDSPACE_ID, real_id), 0);
    hsize_t refcnt = 0;

    ScopedErrorSilence silence;
    EXPECT_LT(H5SM_try_share(f, nullptr, H5SM_DEFER, H5O_SDSPACE_ID,
                             sp_tgt_obj, nullptr),
              0);
    EXPECT_LT(
        H5SM_try_share(f, nullptr, 0, H5O_SDSPACE_ID, sp_tgt_obj, nullptr), 0);
    EXPECT_LT(H5SM_get_refcount(f, H5O_SDSPACE_ID, &orphan_sh, &refcnt), 0);
  }
}

TEST(H5SMTest, AllocationFailureErrorPaths) {
  // Use H5F_FSPACE_STRATEGY_NONE so H5MF_alloc/H5MF_alloc_tmp hit H5F__alloc's
  // tmp_addr boundary cleanly after padding EOA past 8 KiB, with no leaks.

  // 1) Uncreated LIST index: H5SM__create_list H5MF_alloc fails (lines 461,
  //    640, 651-654, 1096).
  {
    ScopedPlist fcpl = MakeSohmFcpl(1, 3, 2, H5O_SHMESG_SDSPACE_FLAG, 10);
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(
        H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_NONE, false,
                                   1),
        0);
    ScopedFile file =
        CreateInMemoryFile("sm_alloc_list.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());
    ASSERT_TRUE(PadFilePast8K(file.get()));

    const std::array<hsize_t, 1> d1 = {4};
    ScopedSpace sp1(H5Screate_simple(1, d1.data(), nullptr));
    ASSERT_TRUE(sp1.valid());
    H5S_t* sp1_obj = UnwrapSpace(sp1.get());
    ASSERT_NE(sp1_obj, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    const haddr_t orig_tmp = f->shared->tmp_addr;
    const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
    ASSERT_GT(eoa, 8192u);
    f->shared->tmp_addr = eoa + 1;
    absl::Cleanup restore_tmp = [&] { f->shared->tmp_addr = orig_tmp; };

    ScopedErrorSilence silence;
    EXPECT_LT(H5SM_try_share(f, nullptr, 0, H5O_SDSPACE_ID, sp1_obj, nullptr),
              0);
  }

  // 2) Uncreated BTREE index (list_max == 0): H5B2_create fails (line 479).
  {
    ScopedPlist fcpl = MakeSohmFcpl(1, 0, 0, H5O_SHMESG_SDSPACE_FLAG, 10);
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(
        H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_NONE, false,
                                   1),
        0);
    ScopedFile file =
        CreateInMemoryFile("sm_alloc_bt2_init.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());
    ASSERT_TRUE(PadFilePast8K(file.get()));

    const std::array<hsize_t, 1> d1 = {4};
    ScopedSpace sp1(H5Screate_simple(1, d1.data(), nullptr));
    ASSERT_TRUE(sp1.valid());
    H5S_t* sp1_obj = UnwrapSpace(sp1.get());
    ASSERT_NE(sp1_obj, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    const haddr_t orig_tmp = f->shared->tmp_addr;
    const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
    ASSERT_GT(eoa, 8192u);
    f->shared->tmp_addr = eoa + 1;
    absl::Cleanup restore_tmp = [&] { f->shared->tmp_addr = orig_tmp; };

    ScopedErrorSilence silence;
    EXPECT_LT(H5SM_try_share(f, nullptr, 0, H5O_SDSPACE_ID, sp1_obj, nullptr),
              0);
  }

  // 3) Existing LIST/BTREE index with zero heap direct blocks yet allocated:
  //    hits H5HF_insert failure on unique share (1426), IN_OH->IN_HEAP
  //    promotion in list (1301), list->btree H5B2_create failure (710), and
  //    IN_OH->IN_HEAP promotion in B-tree (1172).
  {
    ScopedPlist fcpl = MakeSohmFcpl(1, 2, 1, H5O_SHMESG_SDSPACE_FLAG, 10);
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(
        H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_NONE, false,
                                   1),
        0);
    ScopedFile file =
        CreateInMemoryFile("sm_alloc_prog.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());
    ASSERT_TRUE(PadFilePast8K(file.get()));

    const std::array<hsize_t, 1> d1_dims = {4};
    const std::array<hsize_t, 1> d2_dims = {8};
    const std::array<hsize_t, 1> d3_dims = {12};
    ScopedSpace sp1(H5Screate_simple(1, d1_dims.data(), nullptr));
    ScopedSpace sp2(H5Screate_simple(1, d2_dims.data(), nullptr));
    ScopedSpace sp3(H5Screate_simple(1, d3_dims.data(), nullptr));
    ASSERT_TRUE(sp1.valid() && sp2.valid() && sp3.valid());
    H5S_t* sp1_obj = UnwrapSpace(sp1.get());
    H5S_t* sp2_obj = UnwrapSpace(sp2.get());
    H5S_t* sp3_obj = UnwrapSpace(sp3.get());
    ASSERT_NE(sp1_obj, nullptr);
    ASSERT_NE(sp2_obj, nullptr);
    ASSERT_NE(sp3_obj, nullptr);

    ScopedDataset d1(H5Dcreate2(file.get(), "/d1", H5T_NATIVE_INT, sp1.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d1.valid());
    d1.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    // 3a & 3b: LIST has 1 IN_OH entry (fheap has 0 direct blocks).
    {
      const haddr_t orig_tmp = f->shared->tmp_addr;
      const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
      ASSERT_GT(eoa, 8192u);
      f->shared->tmp_addr = eoa + 1;
      absl::Cleanup restore_tmp = [&] { f->shared->tmp_addr = orig_tmp; };

      ScopedErrorSilence silence;
      EXPECT_LT(
          H5SM_try_share(f, nullptr, 0, H5O_SDSPACE_ID, sp2_obj, nullptr), 0);
      EXPECT_LT(
          H5SM_try_share(f, nullptr, 0, H5O_SDSPACE_ID, sp1_obj, nullptr), 0);
    }

    // Add second distinct IN_OH dataspace so list is full (num_messages == 2).
    ScopedDataset d2(H5Dcreate2(file.get(), "/d2", H5T_NATIVE_INT, sp2.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ScopedGroup holder(H5Gcreate2(file.get(), "/holder", H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d2.valid() && holder.valid());
    d2.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    // 3c: Full LIST + open_oh skips fheap insert and fails at H5B2_create (710).
    {
      H5O_loc_t* h_oloc = H5O_get_loc(holder.get());
      ASSERT_NE(h_oloc, nullptr);
      H5O_t* h_oh = H5O_protect(h_oloc, H5AC__NO_FLAGS_SET, false);
      ASSERT_NE(h_oh, nullptr);
      absl::Cleanup unprot_h = [&] {
        H5O_unprotect(h_oloc, h_oh, H5AC__NO_FLAGS_SET);
      };

      const haddr_t orig_tmp = f->shared->tmp_addr;
      const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
      ASSERT_GT(eoa, 8192u);
      f->shared->tmp_addr = eoa + 1;
      absl::Cleanup restore_tmp = [&] { f->shared->tmp_addr = orig_tmp; };

      ScopedErrorSilence silence;
      EXPECT_LT(
          H5SM_try_share(f, h_oh, 0, H5O_SDSPACE_ID, sp3_obj, nullptr), 0);
    }
    holder.reset();

    // 3d: Add third unique dataspace to convert LIST -> BTREE while all entries
    //     remain H5SM_IN_OH (fheap still has 0 direct blocks), then duplicate sp1.
    ScopedDataset d3(H5Dcreate2(file.get(), "/d3", H5T_NATIVE_INT, sp3.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d3.valid());
    d3.reset();
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    {
      const haddr_t orig_tmp = f->shared->tmp_addr;
      const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_DEFAULT);
      ASSERT_GT(eoa, 8192u);
      f->shared->tmp_addr = eoa + 1;
      absl::Cleanup restore_tmp = [&] { f->shared->tmp_addr = orig_tmp; };

      ScopedErrorSilence silence;
      EXPECT_LT(
          H5SM_try_share(f, nullptr, 0, H5O_SDSPACE_ID, sp1_obj, nullptr), 0);
    }
  }
}

}  // namespace
}  // namespace hdf5
