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
// Tests group creation property list class (H5P_GROUP_CREATE) APIs, range and
// missing-property error paths, encode/decode serialization callbacks, group
// creation round-trips, and internal registration failure handling
// (third_party/hdf5/src/H5Pgcpl.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Pgcpl.c

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG; match internal struct layouts cleanly.
#ifndef NDEBUG
#define H5PGCPL_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5P_FRIEND
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5Ppkg.h"
H5_DLLVAR const H5P_libclass_t H5P_CLS_GCRT[1];
}  // extern "C"
#ifdef H5PGCPL_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5PGCPL_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// Helper to encode a property list to a byte vector via H5Pencode2.
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

TEST(H5PgcplTest, LocalHeapSizeHintGetSetAndErrors) {
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_GE(gcpl.get(), 0);

  // Default value on both H5P_GROUP_CREATE_DEFAULT and a new instance is 0.
  size_t size_hint = 999;
  ASSERT_GE(H5Pget_local_heap_size_hint(H5P_GROUP_CREATE_DEFAULT, &size_hint),
            0);
  EXPECT_EQ(size_hint, 0u);

  size_hint = 999;
  ASSERT_GE(H5Pget_local_heap_size_hint(gcpl.get(), &size_hint), 0);
  EXPECT_EQ(size_hint, 0u);

  // Round-trip non-default values, nullptr output, and FCPL subclass usage.
  for (const size_t val :
       {size_t{0}, size_t{512}, size_t{4096}, size_t{65536}}) {
    ASSERT_GE(H5Pset_local_heap_size_hint(gcpl.get(), val), 0);
    size_t read_val = val + 1;
    ASSERT_GE(H5Pget_local_heap_size_hint(gcpl.get(), &read_val), 0);
    EXPECT_EQ(read_val, val);
  }
  EXPECT_GE(H5Pget_local_heap_size_hint(gcpl.get(), nullptr), 0);

  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_GE(fcpl.get(), 0);
  ASSERT_GE(H5Pset_local_heap_size_hint(fcpl.get(), 2048), 0);
  ASSERT_GE(H5Pget_local_heap_size_hint(fcpl.get(), &size_hint), 0);
  EXPECT_EQ(size_hint, 2048u);

  // Negative checks: bad IDs, wrong plist class, H5P_DEFAULT on set, removed
  // prop.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_local_heap_size_hint(H5I_INVALID_HID, 1024), 0);
  EXPECT_LT(H5Pset_local_heap_size_hint(H5P_GROUP_CREATE_DEFAULT, 1024), 0);
  EXPECT_LT(H5Pget_local_heap_size_hint(H5I_INVALID_HID, &size_hint), 0);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(dcpl.get(), 0);
  EXPECT_LT(H5Pset_local_heap_size_hint(dcpl.get(), 1024), 0);
  EXPECT_LT(H5Pget_local_heap_size_hint(dcpl.get(), &size_hint), 0);

  ASSERT_GE(H5Premove(gcpl.get(), H5G_CRT_GROUP_INFO_NAME), 0);
  EXPECT_LT(H5Pset_local_heap_size_hint(gcpl.get(), 1024), 0);
  EXPECT_LT(H5Pget_local_heap_size_hint(gcpl.get(), &size_hint), 0);
}

TEST(H5PgcplTest, LinkPhaseChangeGetSetRangesAndNullOutputs) {
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_GE(gcpl.get(), 0);

  unsigned max_compact = 0;
  unsigned min_dense = 0;
  ASSERT_GE(H5Pget_link_phase_change(H5P_GROUP_CREATE_DEFAULT, &max_compact,
                                     &min_dense),
            0);
  EXPECT_EQ(max_compact, H5G_CRT_GINFO_MAX_COMPACT);
  EXPECT_EQ(min_dense, H5G_CRT_GINFO_MIN_DENSE);

  struct PhaseCase {
    unsigned max_c;
    unsigned min_d;
  };
  constexpr auto kValidCases = std::to_array<PhaseCase>({
      {.max_c = 20, .min_d = 10},
      {.max_c = H5G_CRT_GINFO_MAX_COMPACT, .min_d = 4},
      {.max_c = 12, .min_d = H5G_CRT_GINFO_MIN_DENSE},
      {.max_c = 0, .min_d = 0},
      {.max_c = 65535, .min_d = 65535},
      // Resetting to library defaults toggles store_link_phase_change to false.
      {.max_c = H5G_CRT_GINFO_MAX_COMPACT, .min_d = H5G_CRT_GINFO_MIN_DENSE},
  });
  for (const auto& tc : kValidCases) {
    ASSERT_GE(H5Pset_link_phase_change(gcpl.get(), tc.max_c, tc.min_d), 0);
    unsigned got_max = tc.max_c + 1;
    unsigned got_min = tc.min_d + 1;
    ASSERT_GE(H5Pget_link_phase_change(gcpl.get(), &got_max, &got_min), 0);
    EXPECT_EQ(got_max, tc.max_c);
    EXPECT_EQ(got_min, tc.min_d);
  }

  // Test partial/all-null out-pointers.
  ASSERT_GE(H5Pset_link_phase_change(gcpl.get(), 14, 5), 0);
  max_compact = 0;
  min_dense = 0;
  EXPECT_GE(H5Pget_link_phase_change(gcpl.get(), &max_compact, nullptr), 0);
  EXPECT_EQ(max_compact, 14u);
  EXPECT_GE(H5Pget_link_phase_change(gcpl.get(), nullptr, &min_dense), 0);
  EXPECT_EQ(min_dense, 5u);
  EXPECT_GE(H5Pget_link_phase_change(gcpl.get(), nullptr, nullptr), 0);

  // Range errors, invalid handles, and removed property.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_link_phase_change(gcpl.get(), 5, 6), 0);
  EXPECT_LT(H5Pset_link_phase_change(gcpl.get(), 0, 1), 0);
  EXPECT_LT(H5Pset_link_phase_change(gcpl.get(), 65536, 6), 0);
  EXPECT_LT(H5Pset_link_phase_change(H5I_INVALID_HID, 10, 5), 0);
  EXPECT_LT(H5Pset_link_phase_change(H5P_GROUP_CREATE_DEFAULT, 10, 5), 0);
  EXPECT_LT(H5Pget_link_phase_change(H5I_INVALID_HID, &max_compact, &min_dense),
            0);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(dcpl.get(), 0);
  EXPECT_LT(H5Pset_link_phase_change(dcpl.get(), 10, 5), 0);
  EXPECT_LT(H5Pget_link_phase_change(dcpl.get(), &max_compact, &min_dense), 0);

  ASSERT_GE(H5Premove(gcpl.get(), H5G_CRT_GROUP_INFO_NAME), 0);
  EXPECT_LT(H5Pset_link_phase_change(gcpl.get(), 10, 5), 0);
  EXPECT_LT(H5Pget_link_phase_change(gcpl.get(), &max_compact, &min_dense), 0);
}

TEST(H5PgcplTest, EstLinkInfoGetSetRangesAndNullOutputs) {
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_GE(gcpl.get(), 0);

  unsigned est_entries = 0;
  unsigned est_name_len = 0;
  ASSERT_GE(H5Pget_est_link_info(H5P_GROUP_CREATE_DEFAULT, &est_entries,
                                 &est_name_len),
            0);
  EXPECT_EQ(est_entries, H5G_CRT_GINFO_EST_NUM_ENTRIES);
  EXPECT_EQ(est_name_len, H5G_CRT_GINFO_EST_NAME_LEN);

  struct EstCase {
    unsigned entries;
    unsigned name_len;
  };
  constexpr auto kValidCases = std::to_array<EstCase>({
      {.entries = 32, .name_len = 24},
      {.entries = H5G_CRT_GINFO_EST_NUM_ENTRIES, .name_len = 20},
      {.entries = 12, .name_len = H5G_CRT_GINFO_EST_NAME_LEN},
      {.entries = 0, .name_len = 0},
      {.entries = 65535, .name_len = 65535},
      // Resetting to defaults clears store_est_entry_info.
      {.entries = H5G_CRT_GINFO_EST_NUM_ENTRIES,
       .name_len = H5G_CRT_GINFO_EST_NAME_LEN},
  });
  for (const auto& tc : kValidCases) {
    ASSERT_GE(H5Pset_est_link_info(gcpl.get(), tc.entries, tc.name_len), 0);
    unsigned got_entries = tc.entries + 1;
    unsigned got_len = tc.name_len + 1;
    ASSERT_GE(H5Pget_est_link_info(gcpl.get(), &got_entries, &got_len), 0);
    EXPECT_EQ(got_entries, tc.entries);
    EXPECT_EQ(got_len, tc.name_len);
  }

  ASSERT_GE(H5Pset_est_link_info(gcpl.get(), 17, 29), 0);
  est_entries = 0;
  est_name_len = 0;
  EXPECT_GE(H5Pget_est_link_info(gcpl.get(), &est_entries, nullptr), 0);
  EXPECT_EQ(est_entries, 17u);
  EXPECT_GE(H5Pget_est_link_info(gcpl.get(), nullptr, &est_name_len), 0);
  EXPECT_EQ(est_name_len, 29u);
  EXPECT_GE(H5Pget_est_link_info(gcpl.get(), nullptr, nullptr), 0);

  // Range errors, invalid handles, and removed property.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_est_link_info(gcpl.get(), 65536, 8), 0);
  EXPECT_LT(H5Pset_est_link_info(gcpl.get(), 4, 65536), 0);
  EXPECT_LT(H5Pset_est_link_info(H5I_INVALID_HID, 4, 8), 0);
  EXPECT_LT(H5Pset_est_link_info(H5P_GROUP_CREATE_DEFAULT, 4, 8), 0);
  EXPECT_LT(H5Pget_est_link_info(H5I_INVALID_HID, &est_entries, &est_name_len),
            0);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(dcpl.get(), 0);
  EXPECT_LT(H5Pset_est_link_info(dcpl.get(), 4, 8), 0);
  EXPECT_LT(H5Pget_est_link_info(dcpl.get(), &est_entries, &est_name_len), 0);

  ASSERT_GE(H5Premove(gcpl.get(), H5G_CRT_GROUP_INFO_NAME), 0);
  EXPECT_LT(H5Pset_est_link_info(gcpl.get(), 4, 8), 0);
  EXPECT_LT(H5Pget_est_link_info(gcpl.get(), &est_entries, &est_name_len), 0);
}

TEST(H5PgcplTest, LinkCreationOrderGetSetFlagsAndErrors) {
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_GE(gcpl.get(), 0);

  unsigned flags = 0xdeadbeefu;
  ASSERT_GE(H5Pget_link_creation_order(H5P_GROUP_CREATE_DEFAULT, &flags), 0);
  EXPECT_EQ(flags, 0u);

  for (const unsigned valid :
       {0u, static_cast<unsigned>(H5P_CRT_ORDER_TRACKED),
        static_cast<unsigned>(H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED)}) {
    ASSERT_GE(H5Pset_link_creation_order(gcpl.get(), valid), 0);
    flags = ~valid;
    ASSERT_GE(H5Pget_link_creation_order(gcpl.get(), &flags), 0);
    EXPECT_EQ(flags, valid);
  }
  EXPECT_GE(H5Pget_link_creation_order(gcpl.get(), nullptr), 0);

  // INDEXED without TRACKED is invalid; test bad IDs and removed link info.
  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_link_creation_order(gcpl.get(), H5P_CRT_ORDER_INDEXED), 0);
  EXPECT_LT(H5Pset_link_creation_order(H5I_INVALID_HID, H5P_CRT_ORDER_TRACKED),
            0);
  EXPECT_LT(H5Pset_link_creation_order(H5P_GROUP_CREATE_DEFAULT,
                                       H5P_CRT_ORDER_TRACKED),
            0);
  EXPECT_LT(H5Pget_link_creation_order(H5I_INVALID_HID, &flags), 0);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(dcpl.get(), 0);
  EXPECT_LT(H5Pset_link_creation_order(dcpl.get(), H5P_CRT_ORDER_TRACKED), 0);
  EXPECT_LT(H5Pget_link_creation_order(dcpl.get(), &flags), 0);

  ASSERT_GE(H5Premove(gcpl.get(), H5G_CRT_LINK_INFO_NAME), 0);
  EXPECT_LT(H5Pset_link_creation_order(gcpl.get(), H5P_CRT_ORDER_TRACKED), 0);
  EXPECT_LT(H5Pget_link_creation_order(gcpl.get(), &flags), 0);
}

TEST(H5PgcplTest, EncodeDecodeRoundTripsAndCorruptLinkInfoFailure) {
  // 1. Round-trip an unmodified default GCPL (covers default decode branch).
  ScopedPlist def_gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_GE(def_gcpl.get(), 0);
  std::vector<uint8_t> def_buf = EncodePlist(def_gcpl.get());
  ASSERT_FALSE(def_buf.empty());

  ScopedPlist def_decoded(H5Pdecode(def_buf.data()));
  ASSERT_GE(def_decoded.get(), 0);
  EXPECT_GT(H5Pequal(def_gcpl.get(), def_decoded.get()), 0);

  // 2. Round-trip a non-default GCPL (covers non-default decode branches).
  ScopedPlist custom(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_GE(custom.get(), 0);
  ASSERT_GE(H5Pset_local_heap_size_hint(custom.get(), 8192), 0);
  ASSERT_GE(H5Pset_link_phase_change(custom.get(), 16, 4), 0);
  ASSERT_GE(H5Pset_est_link_info(custom.get(), 64, 32), 0);
  ASSERT_GE(H5Pset_link_creation_order(
                custom.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);

  std::vector<uint8_t> custom_buf = EncodePlist(custom.get());
  ASSERT_FALSE(custom_buf.empty());

  ScopedPlist decoded(H5Pdecode(custom_buf.data()));
  ASSERT_GE(decoded.get(), 0);
  EXPECT_GT(H5Pequal(custom.get(), decoded.get()), 0);

  size_t lheap = 0;
  unsigned max_c = 0;
  unsigned min_d = 0;
  unsigned est_n = 0;
  unsigned est_l = 0;
  unsigned corder = 0;
  ASSERT_GE(H5Pget_local_heap_size_hint(decoded.get(), &lheap), 0);
  ASSERT_GE(H5Pget_link_phase_change(decoded.get(), &max_c, &min_d), 0);
  ASSERT_GE(H5Pget_est_link_info(decoded.get(), &est_n, &est_l), 0);
  ASSERT_GE(H5Pget_link_creation_order(decoded.get(), &corder), 0);
  EXPECT_EQ(lheap, 8192u);
  EXPECT_EQ(max_c, 16u);
  EXPECT_EQ(min_d, 4u);
  EXPECT_EQ(est_n, 64u);
  EXPECT_EQ(est_l, 32u);
  EXPECT_EQ(corder, static_cast<unsigned>(H5P_CRT_ORDER_TRACKED |
                                          H5P_CRT_ORDER_INDEXED));

  // 3. Corrupt the encoded "link info" unsigned-size prefix byte to trigger
  // H5P__gcrt_link_info_dec's size mismatch failure.
  constexpr std::string_view kLinkInfoProp = H5G_CRT_LINK_INFO_NAME;
  std::vector<uint8_t> prop_marker(kLinkInfoProp.begin(), kLinkInfoProp.end());
  prop_marker.push_back('\0');
  auto it = std::ranges::search(custom_buf, prop_marker).begin();
  ASSERT_NE(it, custom_buf.end());
  const size_t size_byte_idx =
      static_cast<size_t>(it - custom_buf.begin()) + prop_marker.size();
  ASSERT_LT(size_byte_idx, custom_buf.size());
  ASSERT_EQ(custom_buf[size_byte_idx], static_cast<uint8_t>(sizeof(unsigned)));
  custom_buf[size_byte_idx] = 0x7fu;

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Pdecode(custom_buf.data()), H5I_INVALID_HID);
}

TEST(H5PgcplTest, InMemoryGroupCreationPersistsPropertiesAndCreationOrder) {
  ScopedFile file = CreateInMemoryFile("gcpl_group.h5");
  ASSERT_GE(file.get(), 0);

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_GE(gcpl.get(), 0);
  ASSERT_GE(H5Pset_local_heap_size_hint(gcpl.get(), 1024), 0);
  ASSERT_GE(H5Pset_link_phase_change(gcpl.get(), 4, 2), 0);
  ASSERT_GE(H5Pset_est_link_info(gcpl.get(), 10, 16), 0);
  ASSERT_GE(H5Pset_link_creation_order(
                gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/ordered", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
  ASSERT_GE(grp.get(), 0);

  // Add links out of alphabetical order to verify creation-order indexing.
  for (const char* name : {"charlie", "alpha", "bravo", "delta", "echo"}) {
    ScopedGroup child(
        H5Gcreate2(grp.get(), name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_GE(child.get(), 0);
  }

  ScopedPlist stored(H5Gget_create_plist(grp.get()));
  ASSERT_GE(stored.get(), 0);
  unsigned max_c = 0;
  unsigned min_d = 0;
  unsigned corder = 0;
  ASSERT_GE(H5Pget_link_phase_change(stored.get(), &max_c, &min_d), 0);
  ASSERT_GE(H5Pget_link_creation_order(stored.get(), &corder), 0);
  EXPECT_EQ(max_c, 4u);
  EXPECT_EQ(min_d, 2u);
  EXPECT_EQ(corder, static_cast<unsigned>(H5P_CRT_ORDER_TRACKED |
                                          H5P_CRT_ORDER_INDEXED));

  std::vector<std::string> visited;
  auto collect_cb = [](hid_t, const char* link_name, const H5L_info2_t*,
                       void* op_data) -> herr_t {
    static_cast<std::vector<std::string>*>(op_data)->emplace_back(link_name);
    return 0;
  };
  hsize_t idx = 0;
  ASSERT_GE(H5Literate2(grp.get(), H5_INDEX_CRT_ORDER, H5_ITER_INC, &idx,
                        collect_cb, &visited),
            0);
  EXPECT_THAT(visited,
              ElementsAre("charlie", "alpha", "bravo", "delta", "echo"));
}

TEST(H5PgcplTest, InternalRegPropDuplicateRegistrationFailures) {
  ASSERT_GE(H5open(), 0);
  ASSERT_NE(H5P_CLS_GCRT[0].reg_prop_func, nullptr);
  EXPECT_STREQ(H5P_CLS_GCRT[0].name, "group create");

  // Pre-registering each property name in turn on an isolated temporary class
  // exercises every H5P__register_real error path inside H5P__gcrt_reg_prop.
  constexpr auto kPropNames = std::to_array<const char*>(
      {H5G_CRT_GROUP_INFO_NAME, H5G_CRT_LINK_INFO_NAME});
  for (const char* prop_name : kPropNames) {
    H5P_genclass_t* tmp_cls = H5P__create_class(
        H5P_CLS_ROOT_g, "tmp_gcrt_class", H5P_TYPE_GROUP_CREATE, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(tmp_cls, nullptr);
    absl::Cleanup close_tmp_cls = [&] { H5P__close_class(tmp_cls); };

    const int dummy = 0;
    ASSERT_GE(H5P__register_real(tmp_cls, prop_name, sizeof(dummy), &dummy,
                                 nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr),
              0);

    ScopedErrorSilence silence;
    EXPECT_LT(H5P_CLS_GCRT[0].reg_prop_func(tmp_cls), 0);
  }
}

}  // namespace
}  // namespace hdf5
