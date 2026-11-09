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
// Tests file creation property list class (H5P_FILE_CREATE) public and internal
// APIs, argument validation, removed-property error paths, encode/decode
// serialization callbacks, Core VFD file integration, and internal registration
// failure handling (third_party/hdf5/src/H5Pfcpl.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Pfcpl.c

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG; match internal struct layouts cleanly.
#ifndef NDEBUG
#define H5PFCPL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5P_FRIEND
#include "third_party/hdf5/src/H5Bprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Ppkg.h"
H5_DLLVAR const H5P_libclass_t H5P_CLS_FCRT[1];
}  // extern "C"
#ifdef H5PFCPL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5PFCPL_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

std::vector<uint8_t> EncodePlist(hid_t plist_id) {
  size_t size = 0;
  if (H5Pencode2(plist_id, nullptr, &size, H5P_DEFAULT) < 0 || size == 0) {
    return {};
  }
  std::vector<uint8_t> buf(size, 0);
  if (H5Pencode2(plist_id, buf.data(), &size, H5P_DEFAULT) < 0) {
    return {};
  }
  return buf;
}

TEST(H5PfcplTest, UserblockGetSetValidationAndErrors) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(fcpl.get(), 0);

  hsize_t ub_size = 999;
  ASSERT_GE(H5Pget_userblock(H5P_FILE_CREATE_DEFAULT, &ub_size), 0);
  EXPECT_EQ(ub_size, 0u);

  for (const hsize_t valid : {hsize_t{0}, hsize_t{512}, hsize_t{1024},
                              hsize_t{4096}, hsize_t{1048576}}) {
    ASSERT_GE(H5Pset_userblock(fcpl.get(), valid), 0);
    hsize_t got = valid + 1;
    ASSERT_GE(H5Pget_userblock(fcpl.get(), &got), 0);
    EXPECT_EQ(got, valid);
  }
  EXPECT_GE(H5Pget_userblock(fcpl.get(), nullptr), 0);

  ScopedErrorSilence silence;
  // Non-zero < 512 and non-power-of-two sizes are rejected.
  for (const hsize_t bad :
       {hsize_t{1}, hsize_t{256}, hsize_t{511}, hsize_t{513}, hsize_t{1536}}) {
    EXPECT_LT(H5Pset_userblock(fcpl.get(), bad), 0);
  }
  EXPECT_LT(H5Pset_userblock(H5I_INVALID_HID, 512), 0);
  EXPECT_LT(H5Pset_userblock(H5P_FILE_CREATE_DEFAULT, 512), 0);
  EXPECT_LT(H5Pget_userblock(H5I_INVALID_HID, &ub_size), 0);

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_GE(gcpl.get(), 0);
  EXPECT_LT(H5Pset_userblock(gcpl.get(), 512), 0);
  EXPECT_LT(H5Pget_userblock(gcpl.get(), &ub_size), 0);

  ASSERT_GE(H5Premove(fcpl.get(), H5F_CRT_USER_BLOCK_NAME), 0);
  EXPECT_LT(H5Pset_userblock(fcpl.get(), 512), 0);
  EXPECT_LT(H5Pget_userblock(fcpl.get(), &ub_size), 0);
}

TEST(H5PfcplTest, SizesGetSetZeroPreservesValidationAndErrors) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(fcpl.get(), 0);

  size_t sizeof_addr = 0;
  size_t sizeof_size = 0;
  ASSERT_GE(H5Pget_sizes(H5P_FILE_CREATE_DEFAULT, &sizeof_addr, &sizeof_size),
            0);
  EXPECT_EQ(sizeof_addr, sizeof(haddr_t));
  EXPECT_EQ(sizeof_size, sizeof(hsize_t));

  for (const size_t valid : {size_t{2}, size_t{4}, size_t{8}, size_t{16}}) {
    ASSERT_GE(H5Pset_sizes(fcpl.get(), valid, valid), 0);
    size_t got_addr = 0;
    size_t got_size = 0;
    ASSERT_GE(H5Pget_sizes(fcpl.get(), &got_addr, &got_size), 0);
    EXPECT_EQ(got_addr, valid);
    EXPECT_EQ(got_size, valid);
  }

  // Zero preserves the corresponding current value.
  ASSERT_GE(H5Pset_sizes(fcpl.get(), 8, 8), 0);
  ASSERT_GE(H5Pset_sizes(fcpl.get(), 0, 4), 0);
  ASSERT_GE(H5Pget_sizes(fcpl.get(), &sizeof_addr, &sizeof_size), 0);
  EXPECT_EQ(sizeof_addr, 8u);
  EXPECT_EQ(sizeof_size, 4u);

  ASSERT_GE(H5Pset_sizes(fcpl.get(), 2, 0), 0);
  ASSERT_GE(H5Pget_sizes(fcpl.get(), &sizeof_addr, nullptr), 0);
  EXPECT_EQ(sizeof_addr, 2u);
  ASSERT_GE(H5Pget_sizes(fcpl.get(), nullptr, &sizeof_size), 0);
  EXPECT_EQ(sizeof_size, 4u);
  EXPECT_GE(H5Pget_sizes(fcpl.get(), nullptr, nullptr), 0);

  ScopedErrorSilence silence;
  for (const size_t bad : {size_t{1}, size_t{3}, size_t{12}, size_t{32}}) {
    EXPECT_LT(H5Pset_sizes(fcpl.get(), bad, 8), 0);
    EXPECT_LT(H5Pset_sizes(fcpl.get(), 8, bad), 0);
  }
  EXPECT_LT(H5Pset_sizes(H5I_INVALID_HID, 8, 8), 0);
  EXPECT_LT(H5Pset_sizes(H5P_FILE_CREATE_DEFAULT, 8, 8), 0);
  EXPECT_LT(H5Pget_sizes(H5I_INVALID_HID, &sizeof_addr, &sizeof_size), 0);

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(fapl.get(), 0);
  EXPECT_LT(H5Pset_sizes(fapl.get(), 8, 8), 0);
  EXPECT_LT(H5Pget_sizes(fapl.get(), &sizeof_addr, &sizeof_size), 0);

  // Removing addr vs size property exercises independent set/get errors.
  ScopedPlist rm_addr(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(rm_addr.get(), 0);
  ASSERT_GE(H5Premove(rm_addr.get(), H5F_CRT_ADDR_BYTE_NUM_NAME), 0);
  EXPECT_LT(H5Pset_sizes(rm_addr.get(), 4, 8), 0);
  EXPECT_LT(H5Pget_sizes(rm_addr.get(), &sizeof_addr, nullptr), 0);

  ScopedPlist rm_obj(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(rm_obj.get(), 0);
  ASSERT_GE(H5Premove(rm_obj.get(), H5F_CRT_OBJ_BYTE_NUM_NAME), 0);
  EXPECT_LT(H5Pset_sizes(rm_obj.get(), 4, 8), 0);
  EXPECT_LT(H5Pget_sizes(rm_obj.get(), nullptr, &sizeof_size), 0);
}

TEST(H5PfcplTest, SymKAndIstoreKGetSetRangesAndErrors) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(fcpl.get(), 0);

  unsigned sym_ik = 0;
  unsigned sym_lk = 0;
  unsigned istore_ik = 0;
  ASSERT_GE(H5Pget_sym_k(H5P_FILE_CREATE_DEFAULT, &sym_ik, &sym_lk), 0);
  EXPECT_EQ(sym_ik, static_cast<unsigned>(HDF5_BTREE_SNODE_IK_DEF));
  EXPECT_EQ(sym_lk, static_cast<unsigned>(H5F_CRT_SYM_LEAF_DEF));

  ASSERT_GE(H5Pget_istore_k(H5P_FILE_CREATE_DEFAULT, &istore_ik), 0);
  EXPECT_EQ(istore_ik, static_cast<unsigned>(HDF5_BTREE_CHUNK_IK_DEF));

  // Zero preserves prior values in H5Pset_sym_k; boundaries 1 and 32767 work.
  ASSERT_GE(H5Pset_sym_k(fcpl.get(), 24, 12), 0);
  ASSERT_GE(H5Pset_sym_k(fcpl.get(), 0, 8), 0);
  ASSERT_GE(H5Pset_sym_k(fcpl.get(), 32767, 0), 0);
  ASSERT_GE(H5Pget_sym_k(fcpl.get(), &sym_ik, &sym_lk), 0);
  EXPECT_EQ(sym_ik, 32767u);
  EXPECT_EQ(sym_lk, 8u);
  EXPECT_GE(H5Pget_sym_k(fcpl.get(), &sym_ik, nullptr), 0);
  EXPECT_GE(H5Pget_sym_k(fcpl.get(), nullptr, &sym_lk), 0);
  EXPECT_GE(H5Pget_sym_k(fcpl.get(), nullptr, nullptr), 0);

  ASSERT_GE(H5Pset_istore_k(fcpl.get(), 1), 0);
  ASSERT_GE(H5Pget_istore_k(fcpl.get(), &istore_ik), 0);
  EXPECT_EQ(istore_ik, 1u);
  ASSERT_GE(H5Pset_istore_k(fcpl.get(), 32767), 0);
  ASSERT_GE(H5Pget_istore_k(fcpl.get(), &istore_ik), 0);
  EXPECT_EQ(istore_ik, 32767u);
  EXPECT_GE(H5Pget_istore_k(fcpl.get(), nullptr), 0);

  ScopedErrorSilence silence;
  // ik * 2 >= HDF5_BTREE_IK_MAX_ENTRIES (65536) fails; istore ik == 0 fails.
  EXPECT_LT(H5Pset_sym_k(fcpl.get(), 32768, 4), 0);
  EXPECT_LT(H5Pset_istore_k(fcpl.get(), 0), 0);
  EXPECT_LT(H5Pset_istore_k(fcpl.get(), 32768), 0);

  EXPECT_LT(H5Pset_sym_k(H5I_INVALID_HID, 16, 4), 0);
  EXPECT_LT(H5Pset_sym_k(H5P_FILE_CREATE_DEFAULT, 16, 4), 0);
  EXPECT_LT(H5Pget_sym_k(H5I_INVALID_HID, &sym_ik, &sym_lk), 0);
  EXPECT_LT(H5Pset_istore_k(H5I_INVALID_HID, 32), 0);
  EXPECT_LT(H5Pset_istore_k(H5P_FILE_CREATE_DEFAULT, 32), 0);
  EXPECT_LT(H5Pget_istore_k(H5I_INVALID_HID, &istore_ik), 0);

  ScopedPlist rm_btree(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(rm_btree.get(), 0);
  ASSERT_GE(H5Premove(rm_btree.get(), H5F_CRT_BTREE_RANK_NAME), 0);
  EXPECT_LT(H5Pset_sym_k(rm_btree.get(), 16, 0), 0);
  EXPECT_LT(H5Pget_sym_k(rm_btree.get(), &sym_ik, nullptr), 0);
  EXPECT_LT(H5Pset_istore_k(rm_btree.get(), 32), 0);
  EXPECT_LT(H5Pget_istore_k(rm_btree.get(), &istore_ik), 0);

  ScopedPlist rm_leaf(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(rm_leaf.get(), 0);
  ASSERT_GE(H5Premove(rm_leaf.get(), H5F_CRT_SYM_LEAF_NAME), 0);
  EXPECT_LT(H5Pset_sym_k(rm_leaf.get(), 0, 8), 0);
  EXPECT_LT(H5Pget_sym_k(rm_leaf.get(), nullptr, &sym_lk), 0);
}

TEST(H5PfcplTest, SharedMesgIndexesAndPhaseChangeGetSetAndErrors) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(fcpl.get(), 0);

  unsigned nindexes = 99;
  ASSERT_GE(H5Pget_shared_mesg_nindexes(H5P_FILE_CREATE_DEFAULT, &nindexes), 0);
  EXPECT_EQ(nindexes, 0u);

  unsigned max_list = 0;
  unsigned min_btree = 0;
  ASSERT_GE(H5Pget_shared_mesg_phase_change(H5P_FILE_CREATE_DEFAULT, &max_list,
                                            &min_btree),
            0);
  EXPECT_EQ(max_list, 50u);
  EXPECT_EQ(min_btree, 40u);

  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), H5O_SHMESG_MAX_NINDEXES),
            0);
  ASSERT_GE(H5Pget_shared_mesg_nindexes(fcpl.get(), &nindexes), 0);
  EXPECT_EQ(nindexes, static_cast<unsigned>(H5O_SHMESG_MAX_NINDEXES));

  // Verify initial default on index 0 before modifying all index slots.
  unsigned types = 999;
  unsigned minsize = 0;
  ASSERT_GE(H5Pget_shared_mesg_index(fcpl.get(), 0, &types, &minsize), 0);
  EXPECT_EQ(types, static_cast<unsigned>(H5O_SHMESG_NONE_FLAG));
  EXPECT_EQ(minsize, 250u);

  constexpr auto kIndexFlags = std::to_array<unsigned>({
      H5O_SHMESG_SDSPACE_FLAG,
      H5O_SHMESG_DTYPE_FLAG,
      H5O_SHMESG_FILL_FLAG,
      H5O_SHMESG_PLINE_FLAG,
      H5O_SHMESG_ATTR_FLAG,
      H5O_SHMESG_ALL_FLAG,
      H5O_SHMESG_DTYPE_FLAG | H5O_SHMESG_ATTR_FLAG,
      H5O_SHMESG_NONE_FLAG,
  });
  static_assert(kIndexFlags.size() == H5O_SHMESG_MAX_NINDEXES);
  for (unsigned i = 0; i < H5O_SHMESG_MAX_NINDEXES; ++i) {
    const unsigned expected_min = 64u + i * 16u;
    ASSERT_GE(
        H5Pset_shared_mesg_index(fcpl.get(), i, kIndexFlags[i], expected_min),
        0);
    unsigned got_flags = 0;
    unsigned got_min = 0;
    ASSERT_GE(H5Pget_shared_mesg_index(fcpl.get(), i, &got_flags, &got_min), 0);
    EXPECT_EQ(got_flags, kIndexFlags[i]);
    EXPECT_EQ(got_min, expected_min);
  }
  EXPECT_GE(H5Pget_shared_mesg_index(fcpl.get(), 0, &types, nullptr), 0);
  EXPECT_GE(H5Pget_shared_mesg_index(fcpl.get(), 0, nullptr, &minsize), 0);
  EXPECT_GE(H5Pget_shared_mesg_index(fcpl.get(), 0, nullptr, nullptr), 0);

  // max_list == 0 forces min_btree = 0 even when min_btree == 1 is passed.
  ASSERT_GE(H5Pset_shared_mesg_phase_change(fcpl.get(), 0, 1), 0);
  ASSERT_GE(H5Pget_shared_mesg_phase_change(fcpl.get(), &max_list, &min_btree),
            0);
  EXPECT_EQ(max_list, 0u);
  EXPECT_EQ(min_btree, 0u);

  // Valid boundary where min_btree == max_list + 1 and
  // H5O_SHMESG_MAX_LIST_SIZE.
  ASSERT_GE(H5Pset_shared_mesg_phase_change(fcpl.get(), 100, 101), 0);
  ASSERT_GE(H5Pget_shared_mesg_phase_change(fcpl.get(), &max_list, &min_btree),
            0);
  EXPECT_EQ(max_list, 100u);
  EXPECT_EQ(min_btree, 101u);

  ASSERT_GE(H5Pset_shared_mesg_phase_change(
                fcpl.get(), H5O_SHMESG_MAX_LIST_SIZE, H5O_SHMESG_MAX_LIST_SIZE),
            0);
  EXPECT_GE(H5Pget_shared_mesg_phase_change(fcpl.get(), &max_list, nullptr), 0);
  EXPECT_GE(H5Pget_shared_mesg_phase_change(fcpl.get(), nullptr, &min_btree),
            0);
  EXPECT_GE(H5Pget_shared_mesg_phase_change(fcpl.get(), nullptr, nullptr), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(
      H5Pset_shared_mesg_nindexes(fcpl.get(), H5O_SHMESG_MAX_NINDEXES + 1), 0);
  EXPECT_LT(H5Pset_shared_mesg_nindexes(H5I_INVALID_HID, 1), 0);
  EXPECT_LT(H5Pset_shared_mesg_nindexes(H5P_FILE_CREATE_DEFAULT, 1), 0);
  EXPECT_LT(H5Pget_shared_mesg_nindexes(H5I_INVALID_HID, &nindexes), 0);

  EXPECT_LT(H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_ALL_FLAG + 1, 0),
            0);
  EXPECT_LT(
      H5Pset_shared_mesg_index(fcpl.get(), H5O_SHMESG_MAX_NINDEXES, 0, 10), 0);
  EXPECT_LT(H5Pget_shared_mesg_index(fcpl.get(), H5O_SHMESG_MAX_NINDEXES,
                                     &types, &minsize),
            0);
  EXPECT_LT(H5Pset_shared_mesg_index(H5I_INVALID_HID, 0, 0, 10), 0);
  EXPECT_LT(H5Pset_shared_mesg_index(H5P_FILE_CREATE_DEFAULT, 0, 0, 10), 0);
  EXPECT_LT(H5Pget_shared_mesg_index(H5I_INVALID_HID, 0, &types, &minsize), 0);

  EXPECT_LT(H5Pset_shared_mesg_phase_change(fcpl.get(), 10, 12), 0);
  EXPECT_LT(H5Pset_shared_mesg_phase_change(fcpl.get(),
                                            H5O_SHMESG_MAX_LIST_SIZE + 1, 10),
            0);
  EXPECT_LT(
      H5Pset_shared_mesg_phase_change(fcpl.get(), H5O_SHMESG_MAX_LIST_SIZE,
                                      H5O_SHMESG_MAX_LIST_SIZE + 1),
      0);
  EXPECT_LT(H5Pset_shared_mesg_phase_change(H5I_INVALID_HID, 10, 5), 0);
  EXPECT_LT(H5Pset_shared_mesg_phase_change(H5P_FILE_CREATE_DEFAULT, 10, 5), 0);
  EXPECT_LT(
      H5Pget_shared_mesg_phase_change(H5I_INVALID_HID, &max_list, &min_btree),
      0);

  // Missing-property branches for each SOHM property name.
  {
    ScopedPlist copy(H5Pcopy(fcpl.get()));
    ASSERT_GE(copy.get(), 0);
    ASSERT_GE(H5Premove(copy.get(), H5F_CRT_SHMSG_NINDEXES_NAME), 0);
    EXPECT_LT(H5Pset_shared_mesg_nindexes(copy.get(), 1), 0);
    EXPECT_LT(H5Pget_shared_mesg_nindexes(copy.get(), &nindexes), 0);
    EXPECT_LT(
        H5Pset_shared_mesg_index(copy.get(), 0, H5O_SHMESG_DTYPE_FLAG, 100), 0);
    EXPECT_LT(H5Pget_shared_mesg_index(copy.get(), 0, &types, &minsize), 0);
  }
  for (const char* prop_name :
       {H5F_CRT_SHMSG_INDEX_TYPES_NAME, H5F_CRT_SHMSG_INDEX_MINSIZE_NAME}) {
    ScopedPlist copy(H5Pcopy(fcpl.get()));
    ASSERT_GE(copy.get(), 0);
    ASSERT_GE(H5Premove(copy.get(), prop_name), 0);
    EXPECT_LT(
        H5Pset_shared_mesg_index(copy.get(), 0, H5O_SHMESG_DTYPE_FLAG, 100), 0);
    EXPECT_LT(H5Pget_shared_mesg_index(copy.get(), 0, &types, &minsize), 0);
  }
  for (const char* prop_name :
       {H5F_CRT_SHMSG_LIST_MAX_NAME, H5F_CRT_SHMSG_BTREE_MIN_NAME}) {
    ScopedPlist copy(H5Pcopy(fcpl.get()));
    ASSERT_GE(copy.get(), 0);
    ASSERT_GE(H5Premove(copy.get(), prop_name), 0);
    EXPECT_LT(H5Pset_shared_mesg_phase_change(copy.get(), 20, 10), 0);
    EXPECT_LT(
        H5Pget_shared_mesg_phase_change(copy.get(), &max_list, &min_btree), 0);
  }
}

TEST(H5PfcplTest, FileSpaceStrategyAndPageSizeGetSetAndErrors) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(fcpl.get(), 0);

  H5F_fspace_strategy_t strategy = H5F_FSPACE_STRATEGY_NTYPES;
  bool persist = true;
  hsize_t threshold = 0;
  ASSERT_GE(H5Pget_file_space_strategy(H5P_FILE_CREATE_DEFAULT, &strategy,
                                       &persist, &threshold),
            0);
  EXPECT_EQ(strategy, H5F_FSPACE_STRATEGY_FSM_AGGR);
  EXPECT_FALSE(persist);
  EXPECT_EQ(threshold, 1u);

  hsize_t page_size = 0;
  ASSERT_GE(H5Pget_file_space_page_size(H5P_FILE_CREATE_DEFAULT, &page_size),
            0);
  EXPECT_EQ(page_size, static_cast<hsize_t>(H5F_FILE_SPACE_PAGE_SIZE_DEF));

  // FSM_AGGR and PAGE update persist and threshold.
  ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE,
                                       true, 64),
            0);
  ASSERT_GE(
      H5Pget_file_space_strategy(fcpl.get(), &strategy, &persist, &threshold),
      0);
  EXPECT_EQ(strategy, H5F_FSPACE_STRATEGY_PAGE);
  EXPECT_TRUE(persist);
  EXPECT_EQ(threshold, 64u);

  // AGGR and NONE ignore persist/threshold and keep previous values.
  for (const H5F_fspace_strategy_t non_fsm :
       {H5F_FSPACE_STRATEGY_AGGR, H5F_FSPACE_STRATEGY_NONE}) {
    ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), non_fsm, false, 999), 0);
    ASSERT_GE(
        H5Pget_file_space_strategy(fcpl.get(), &strategy, &persist, &threshold),
        0);
    EXPECT_EQ(strategy, non_fsm);
    EXPECT_TRUE(persist);
    EXPECT_EQ(threshold, 64u);
  }
  EXPECT_GE(H5Pget_file_space_strategy(fcpl.get(), &strategy, nullptr, nullptr),
            0);
  EXPECT_GE(H5Pget_file_space_strategy(fcpl.get(), nullptr, &persist, nullptr),
            0);
  EXPECT_GE(
      H5Pget_file_space_strategy(fcpl.get(), nullptr, nullptr, &threshold), 0);
  EXPECT_GE(H5Pget_file_space_strategy(fcpl.get(), nullptr, nullptr, nullptr),
            0);

  // Page size valid boundaries: [512, 1 GiB].
  for (const hsize_t valid :
       {static_cast<hsize_t>(H5F_FILE_SPACE_PAGE_SIZE_MIN), hsize_t{8192},
        static_cast<hsize_t>(H5F_FILE_SPACE_PAGE_SIZE_MAX)}) {
    ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), valid), 0);
    page_size = 0;
    ASSERT_GE(H5Pget_file_space_page_size(fcpl.get(), &page_size), 0);
    EXPECT_EQ(page_size, valid);
  }
  EXPECT_GE(H5Pget_file_space_page_size(fcpl.get(), nullptr), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_NTYPES,
                                       false, 1),
            0);
  EXPECT_LT(H5Pset_file_space_strategy(
                fcpl.get(), static_cast<H5F_fspace_strategy_t>(99), false, 1),
            0);
  EXPECT_LT(H5Pset_file_space_strategy(H5I_INVALID_HID,
                                       H5F_FSPACE_STRATEGY_PAGE, true, 8),
            0);
  EXPECT_LT(H5Pset_file_space_strategy(H5P_FILE_CREATE_DEFAULT,
                                       H5F_FSPACE_STRATEGY_PAGE, true, 8),
            0);
  EXPECT_LT(H5Pget_file_space_strategy(H5I_INVALID_HID, &strategy, &persist,
                                       &threshold),
            0);

  EXPECT_LT(H5Pset_file_space_page_size(fcpl.get(), 511), 0);
  EXPECT_LT(
      H5Pset_file_space_page_size(
          fcpl.get(), static_cast<hsize_t>(H5F_FILE_SPACE_PAGE_SIZE_MAX) + 1),
      0);
  EXPECT_LT(H5Pset_file_space_page_size(H5I_INVALID_HID, 4096), 0);
  EXPECT_LT(H5Pset_file_space_page_size(H5P_FILE_CREATE_DEFAULT, 4096), 0);
  EXPECT_LT(H5Pget_file_space_page_size(H5I_INVALID_HID, &page_size), 0);

  // Missing-property branches for file space strategy and page size.
  for (const char* prop_name :
       {H5F_CRT_FILE_SPACE_STRATEGY_NAME, H5F_CRT_FREE_SPACE_PERSIST_NAME,
        H5F_CRT_FREE_SPACE_THRESHOLD_NAME}) {
    ScopedPlist copy(H5Pcopy(fcpl.get()));
    ASSERT_GE(copy.get(), 0);
    ASSERT_GE(H5Premove(copy.get(), prop_name), 0);
    EXPECT_LT(H5Pset_file_space_strategy(copy.get(), H5F_FSPACE_STRATEGY_PAGE,
                                         true, 16),
              0);
    EXPECT_LT(
        H5Pget_file_space_strategy(copy.get(), &strategy, &persist, &threshold),
        0);
  }
  ASSERT_GE(H5Premove(fcpl.get(), H5F_CRT_FILE_SPACE_PAGE_SIZE_NAME), 0);
  EXPECT_LT(H5Pset_file_space_page_size(fcpl.get(), 4096), 0);
  EXPECT_LT(H5Pget_file_space_page_size(fcpl.get(), &page_size), 0);
}

TEST(H5PfcplTest, EncodeDecodeRoundTripAndCorruptHeaderErrors) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(fcpl.get(), 0);
  ASSERT_GE(H5Pset_userblock(fcpl.get(), 1024), 0);
  ASSERT_GE(H5Pset_sizes(fcpl.get(), 4, 8), 0);
  ASSERT_GE(H5Pset_sym_k(fcpl.get(), 24, 12), 0);
  ASSERT_GE(H5Pset_istore_k(fcpl.get(), 48), 0);
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 2), 0);
  ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_DTYPE_FLAG, 32),
            0);
  ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 1, H5O_SHMESG_ATTR_FLAG, 48),
            0);
  ASSERT_GE(H5Pset_shared_mesg_phase_change(fcpl.get(), 120, 60), 0);
  ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE,
                                       true, 64),
            0);
  ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), 8192), 0);

  std::vector<uint8_t> encoded = EncodePlist(fcpl.get());
  ASSERT_FALSE(encoded.empty());

  ScopedPlist decoded(H5Pdecode(encoded.data()));
  ASSERT_GE(decoded.get(), 0);
  EXPECT_GT(H5Pequal(fcpl.get(), decoded.get()), 0);

  hsize_t ub = 0;
  size_t saddr = 0;
  size_t ssize = 0;
  unsigned sym_ik = 0;
  unsigned sym_lk = 0;
  unsigned istore_ik = 0;
  unsigned nidx = 0;
  unsigned idx1_flags = 0;
  unsigned idx1_min = 0;
  unsigned max_list = 0;
  unsigned min_btree = 0;
  H5F_fspace_strategy_t strat = H5F_FSPACE_STRATEGY_NTYPES;
  bool persist = false;
  hsize_t thresh = 0;
  hsize_t page_sz = 0;

  ASSERT_GE(H5Pget_userblock(decoded.get(), &ub), 0);
  ASSERT_GE(H5Pget_sizes(decoded.get(), &saddr, &ssize), 0);
  ASSERT_GE(H5Pget_sym_k(decoded.get(), &sym_ik, &sym_lk), 0);
  ASSERT_GE(H5Pget_istore_k(decoded.get(), &istore_ik), 0);
  ASSERT_GE(H5Pget_shared_mesg_nindexes(decoded.get(), &nidx), 0);
  ASSERT_GE(H5Pget_shared_mesg_index(decoded.get(), 1, &idx1_flags, &idx1_min),
            0);
  ASSERT_GE(
      H5Pget_shared_mesg_phase_change(decoded.get(), &max_list, &min_btree), 0);
  ASSERT_GE(
      H5Pget_file_space_strategy(decoded.get(), &strat, &persist, &thresh), 0);
  ASSERT_GE(H5Pget_file_space_page_size(decoded.get(), &page_sz), 0);

  EXPECT_EQ(ub, 1024u);
  EXPECT_EQ(saddr, 4u);
  EXPECT_EQ(ssize, 8u);
  EXPECT_EQ(sym_ik, 24u);
  EXPECT_EQ(sym_lk, 12u);
  EXPECT_EQ(istore_ik, 48u);
  EXPECT_EQ(nidx, 2u);
  EXPECT_EQ(idx1_flags, static_cast<unsigned>(H5O_SHMESG_ATTR_FLAG));
  EXPECT_EQ(idx1_min, 48u);
  EXPECT_EQ(max_list, 120u);
  EXPECT_EQ(min_btree, 60u);
  EXPECT_EQ(strat, H5F_FSPACE_STRATEGY_PAGE);
  EXPECT_TRUE(persist);
  EXPECT_EQ(thresh, 64u);
  EXPECT_EQ(page_sz, 8192u);

  // Corrupt the 1-byte sizeof(unsigned) prefix after each encoded array name
  // to cover the size-mismatch guard in btree_rank / shmsg_types / minsize dec.
  ScopedErrorSilence silence;
  for (const std::string_view prop_name :
       {std::string_view{H5F_CRT_BTREE_RANK_NAME},
        std::string_view{H5F_CRT_SHMSG_INDEX_TYPES_NAME},
        std::string_view{H5F_CRT_SHMSG_INDEX_MINSIZE_NAME}}) {
    std::vector<uint8_t> corrupt = encoded;
    std::vector<uint8_t> marker(prop_name.begin(), prop_name.end());
    marker.push_back('\0');
    auto it = std::ranges::search(corrupt, marker).begin();
    ASSERT_NE(it, corrupt.end());
    const size_t size_byte_idx =
        static_cast<size_t>(it - corrupt.begin()) + marker.size();
    ASSERT_LT(size_byte_idx, corrupt.size());
    ASSERT_EQ(corrupt[size_byte_idx], static_cast<uint8_t>(sizeof(unsigned)));
    corrupt[size_byte_idx] = 0x7fu;
    EXPECT_EQ(H5Pdecode(corrupt.data()), H5I_INVALID_HID);
  }
}

TEST(H5PfcplTest, InMemoryFileCreationPersistsFcplProperties) {
  // 1. Paged file space, userblock, and custom B-tree/sym parameters.
  ScopedPlist fcpl1(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(fcpl1.get(), 0);
  ASSERT_GE(H5Pset_userblock(fcpl1.get(), 512), 0);
  ASSERT_GE(H5Pset_sym_k(fcpl1.get(), 20, 10), 0);
  ASSERT_GE(H5Pset_istore_k(fcpl1.get(), 40), 0);
  ASSERT_GE(H5Pset_file_space_strategy(fcpl1.get(), H5F_FSPACE_STRATEGY_PAGE,
                                       true, 16),
            0);
  ASSERT_GE(H5Pset_file_space_page_size(fcpl1.get(), 512), 0);

  ScopedFile file1 =
      CreateInMemoryFile("fcpl_paged.h5", H5F_ACC_TRUNC, fcpl1.get());
  ASSERT_GE(file1.get(), 0);

  ScopedPlist got1(H5Fget_create_plist(file1.get()));
  ASSERT_GE(got1.get(), 0);
  hsize_t ub = 0;
  unsigned sym_ik = 0;
  unsigned sym_lk = 0;
  unsigned istore_ik = 0;
  H5F_fspace_strategy_t strat = H5F_FSPACE_STRATEGY_NTYPES;
  bool persist = false;
  hsize_t thresh = 0;
  hsize_t page_sz = 0;
  ASSERT_GE(H5Pget_userblock(got1.get(), &ub), 0);
  ASSERT_GE(H5Pget_sym_k(got1.get(), &sym_ik, &sym_lk), 0);
  ASSERT_GE(H5Pget_istore_k(got1.get(), &istore_ik), 0);
  ASSERT_GE(H5Pget_file_space_strategy(got1.get(), &strat, &persist, &thresh),
            0);
  ASSERT_GE(H5Pget_file_space_page_size(got1.get(), &page_sz), 0);
  EXPECT_EQ(ub, 512u);
  EXPECT_EQ(sym_ik, 20u);
  EXPECT_EQ(sym_lk, 10u);
  EXPECT_EQ(istore_ik, 40u);
  EXPECT_EQ(strat, H5F_FSPACE_STRATEGY_PAGE);
  EXPECT_TRUE(persist);
  EXPECT_EQ(thresh, 16u);
  EXPECT_EQ(page_sz, 512u);

  // 2. Shared object header message indexes and custom 4-byte sizes.
  ScopedPlist fcpl2(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(fcpl2.get(), 0);
  ASSERT_GE(H5Pset_sizes(fcpl2.get(), 4, 4), 0);
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl2.get(), 2), 0);
  ASSERT_GE(H5Pset_shared_mesg_index(fcpl2.get(), 0, H5O_SHMESG_DTYPE_FLAG, 10),
            0);
  ASSERT_GE(H5Pset_shared_mesg_index(fcpl2.get(), 1, H5O_SHMESG_ATTR_FLAG, 10),
            0);
  ASSERT_GE(H5Pset_shared_mesg_phase_change(fcpl2.get(), 10, 5), 0);

  ScopedFile file2 =
      CreateInMemoryFile("fcpl_sohm.h5", H5F_ACC_TRUNC, fcpl2.get());
  ASSERT_GE(file2.get(), 0);

  ScopedPlist got2(H5Fget_create_plist(file2.get()));
  ASSERT_GE(got2.get(), 0);
  size_t saddr = 0;
  size_t ssize = 0;
  unsigned nidx = 0;
  unsigned max_list = 0;
  unsigned min_btree = 0;
  ASSERT_GE(H5Pget_sizes(got2.get(), &saddr, &ssize), 0);
  ASSERT_GE(H5Pget_shared_mesg_nindexes(got2.get(), &nidx), 0);
  ASSERT_GE(H5Pget_shared_mesg_phase_change(got2.get(), &max_list, &min_btree),
            0);
  EXPECT_EQ(saddr, 4u);
  EXPECT_EQ(ssize, 4u);
  EXPECT_EQ(nidx, 2u);
  EXPECT_EQ(max_list, 10u);
  EXPECT_EQ(min_btree, 5u);
}

TEST(H5PfcplTest, InternalRegPropDuplicateRegistrationFailures) {
  ASSERT_GE(H5open(), 0);
  ASSERT_NE(H5P_CLS_FCRT[0].reg_prop_func, nullptr);
  EXPECT_STREQ(H5P_CLS_FCRT[0].name, "file create");

  constexpr auto kFcrtPropNames = std::to_array<const char*>({
      H5F_CRT_USER_BLOCK_NAME,
      H5F_CRT_SYM_LEAF_NAME,
      H5F_CRT_BTREE_RANK_NAME,
      H5F_CRT_ADDR_BYTE_NUM_NAME,
      H5F_CRT_OBJ_BYTE_NUM_NAME,
      H5F_CRT_SUPER_VERS_NAME,
      H5F_CRT_SHMSG_NINDEXES_NAME,
      H5F_CRT_SHMSG_INDEX_TYPES_NAME,
      H5F_CRT_SHMSG_INDEX_MINSIZE_NAME,
      H5F_CRT_SHMSG_LIST_MAX_NAME,
      H5F_CRT_SHMSG_BTREE_MIN_NAME,
      H5F_CRT_FILE_SPACE_STRATEGY_NAME,
      H5F_CRT_FREE_SPACE_PERSIST_NAME,
      H5F_CRT_FREE_SPACE_THRESHOLD_NAME,
      H5F_CRT_FILE_SPACE_PAGE_SIZE_NAME,
  });

  for (const char* prop_name : kFcrtPropNames) {
    H5P_genclass_t* tmp_cls = H5P__create_class(
        H5P_CLS_ROOT_g, "tmp_fcrt_class", H5P_TYPE_FILE_CREATE, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(tmp_cls, nullptr);
    absl::Cleanup close_tmp_cls = [&] { H5P__close_class(tmp_cls); };

    const int dummy = 0;
    ASSERT_GE(H5P__register_real(tmp_cls, prop_name, sizeof(dummy), &dummy,
                                 nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr),
              0);

    ScopedErrorSilence silence;
    EXPECT_LT(H5P_CLS_FCRT[0].reg_prop_func(tmp_cls), 0);
  }
}

}  // namespace
}  // namespace hdf5
