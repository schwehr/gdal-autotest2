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
// Tests object creation property list class registration, attribute storage
// phase change & creation order, time tracking, filter pipeline management,
// encode/decode, copy/compare/close callbacks, and deprecated H5Pget_filter1/
// H5Pget_filter_by_id1 wrappers (third_party/hdf5/src/H5Pocpl.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Pocpl.c

#if !defined(NDEBUG)
#define NDEBUG 1
#define HDF5_TEST_UNDEF_NDEBUG_ 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5P_FRIEND
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5Ppkg.h"
}  // extern "C"
#if defined(HDF5_TEST_UNDEF_NDEBUG_)
#undef NDEBUG
#undef HDF5_TEST_UNDEF_NDEBUG_
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

constexpr H5Z_filter_t kCustomFilterIdA = 305;
constexpr H5Z_filter_t kCustomFilterIdB = 306;

size_t IdentityFilter(unsigned int /*flags*/, size_t /*cd_nelmts*/,
                      const unsigned int /*cd_values*/[], size_t nbytes,
                      size_t* /*buf_size*/, void** /*buf*/) {
  return nbytes;
}

TEST(H5PocplTest, ClassMetadataAndDefaults) {
  char* class_name = H5Pget_class_name(H5P_OBJECT_CREATE);
  ASSERT_NE(class_name, nullptr);
  absl::Cleanup free_name = [class_name] { H5free_memory(class_name); };
  EXPECT_STREQ(class_name, "object create");

  ScopedPlist parent(H5Pget_class_parent(H5P_OBJECT_CREATE));
  ASSERT_TRUE(parent.valid());
  EXPECT_GT(H5Pequal(parent.get(), H5P_ROOT), 0);

  ScopedPlist ocpl(H5Pcreate(H5P_OBJECT_CREATE));
  ASSERT_TRUE(ocpl.valid());

  unsigned max_compact = 0;
  unsigned min_dense = 0;
  ASSERT_GE(
      H5Pget_attr_phase_change(H5P_DATASET_CREATE_DEFAULT, &max_compact,
                               &min_dense),
      0);
  EXPECT_EQ(max_compact, 8u);
  EXPECT_EQ(min_dense, 6u);

  unsigned crt_order = 99u;
  ASSERT_GE(H5Pget_attr_creation_order(H5P_DATASET_CREATE_DEFAULT, &crt_order),
            0);
  EXPECT_EQ(crt_order, 0u);

  bool track_times = false;
  ASSERT_GE(
      H5Pget_obj_track_times(H5P_DATASET_CREATE_DEFAULT, &track_times), 0);
  EXPECT_TRUE(track_times);

  EXPECT_EQ(H5Pget_nfilters(H5P_DATASET_CREATE_DEFAULT), 0);
  EXPECT_GT(H5Pall_filters_avail(H5P_DATASET_CREATE_DEFAULT), 0);
}

TEST(H5PocplTest, AttrPhaseChangeValidationAndNullOutputs) {
  ScopedPlist ocpl(H5Pcreate(H5P_OBJECT_CREATE));
  ASSERT_TRUE(ocpl.valid());

  ASSERT_GE(H5Pset_attr_phase_change(ocpl.get(), 0u, 0u), 0);
  unsigned max_c = 1u;
  unsigned min_d = 1u;
  ASSERT_GE(H5Pget_attr_phase_change(ocpl.get(), &max_c, &min_d), 0);
  EXPECT_EQ(max_c, 0u);
  EXPECT_EQ(min_d, 0u);

  ASSERT_GE(H5Pset_attr_phase_change(ocpl.get(), 65535u, 65535u), 0);
  ASSERT_GE(H5Pget_attr_phase_change(ocpl.get(), &max_c, nullptr), 0);
  EXPECT_EQ(max_c, 65535u);
  ASSERT_GE(H5Pget_attr_phase_change(ocpl.get(), nullptr, &min_d), 0);
  EXPECT_EQ(min_d, 65535u);
  EXPECT_GE(H5Pget_attr_phase_change(ocpl.get(), nullptr, nullptr), 0);

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ScopedPlist removed(H5Pcreate(H5P_OBJECT_CREATE));
  ASSERT_TRUE(removed.valid());
  ASSERT_GE(H5Premove(removed.get(), H5O_CRT_ATTR_MAX_COMPACT_NAME), 0);
  ScopedPlist removed_min(H5Pcreate(H5P_OBJECT_CREATE));
  ASSERT_TRUE(removed_min.valid());
  ASSERT_GE(H5Premove(removed_min.get(), H5O_CRT_ATTR_MIN_DENSE_NAME), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_attr_phase_change(ocpl.get(), 4u, 5u), 0);
  EXPECT_LT(H5Pset_attr_phase_change(ocpl.get(), 65536u, 0u), 0);
  EXPECT_LT(H5Pset_attr_phase_change(H5I_INVALID_HID, 8u, 6u), 0);
  EXPECT_LT(H5Pset_attr_phase_change(H5P_DATASET_CREATE_DEFAULT, 8u, 6u), 0);
  EXPECT_LT(H5Pset_attr_phase_change(fapl.get(), 8u, 6u), 0);
  EXPECT_LT(H5Pset_attr_phase_change(removed.get(), 8u, 6u), 0);
  EXPECT_LT(H5Pset_attr_phase_change(removed_min.get(), 8u, 6u), 0);

  EXPECT_LT(H5Pget_attr_phase_change(H5I_INVALID_HID, &max_c, &min_d), 0);
  EXPECT_LT(H5Pget_attr_phase_change(fapl.get(), &max_c, &min_d), 0);
  EXPECT_LT(H5Pget_attr_phase_change(removed.get(), &max_c, &min_d), 0);
  EXPECT_LT(H5Pget_attr_phase_change(removed_min.get(), &max_c, &min_d), 0);
}

TEST(H5PocplTest, AttrCreationOrderAndObjTrackTimesAndObjectBehavior) {
  ScopedPlist gcpl_tracked(H5Pcreate(H5P_GROUP_CREATE));
  ScopedPlist gcpl_notime(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl_tracked.valid());
  ASSERT_TRUE(gcpl_notime.valid());

  ASSERT_GE(
      H5Pset_attr_creation_order(
          gcpl_tracked.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
      0);
  unsigned flags = 0;
  ASSERT_GE(H5Pget_attr_creation_order(gcpl_tracked.get(), &flags), 0);
  EXPECT_EQ(flags,
            static_cast<unsigned>(H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED));

  // Verify clearing indexed bit preserves tracked bit, then clear both.
  ASSERT_GE(
      H5Pset_attr_creation_order(gcpl_tracked.get(), H5P_CRT_ORDER_TRACKED), 0);
  ASSERT_GE(H5Pget_attr_creation_order(gcpl_tracked.get(), &flags), 0);
  EXPECT_EQ(flags, static_cast<unsigned>(H5P_CRT_ORDER_TRACKED));
  EXPECT_GE(H5Pget_attr_creation_order(gcpl_tracked.get(), nullptr), 0);

  ASSERT_GE(H5Pset_attr_creation_order(
                gcpl_tracked.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);

  ASSERT_GE(H5Pset_obj_track_times(gcpl_notime.get(), false), 0);
  bool track = true;
  ASSERT_GE(H5Pget_obj_track_times(gcpl_notime.get(), &track), 0);
  EXPECT_FALSE(track);
  EXPECT_GE(H5Pget_obj_track_times(gcpl_notime.get(), nullptr), 0);
  ASSERT_GE(H5Pset_obj_track_times(gcpl_tracked.get(), true), 0);
  ASSERT_GE(H5Pget_obj_track_times(gcpl_tracked.get(), &track), 0);
  EXPECT_TRUE(track);

  // Functional check in Core VFD: verify untracked times report zero.
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedGroup grp_timed(H5Gcreate2(file.get(), "/timed", H5P_DEFAULT,
                                   gcpl_tracked.get(), H5P_DEFAULT));
  ScopedGroup grp_notime(H5Gcreate2(file.get(), "/notime", H5P_DEFAULT,
                                    gcpl_notime.get(), H5P_DEFAULT));
  ASSERT_TRUE(grp_timed.valid());
  ASSERT_TRUE(grp_notime.valid());

  H5O_info2_t timed_info{};
  H5O_info2_t notime_info{};
  ASSERT_GE(H5Oget_info3(grp_timed.get(), &timed_info, H5O_INFO_TIME), 0);
  ASSERT_GE(H5Oget_info3(grp_notime.get(), &notime_info, H5O_INFO_TIME), 0);
  EXPECT_GT(timed_info.btime, 0);
  EXPECT_EQ(notime_info.atime, 0);
  EXPECT_EQ(notime_info.mtime, 0);
  EXPECT_EQ(notime_info.ctime, 0);
  EXPECT_EQ(notime_info.btime, 0);

  // Validation error paths.
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ScopedPlist removed_flags(H5Pcreate(H5P_OBJECT_CREATE));
  ASSERT_TRUE(removed_flags.valid());
  ASSERT_GE(H5Premove(removed_flags.get(), H5O_CRT_OHDR_FLAGS_NAME), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(
      H5Pset_attr_creation_order(gcpl_tracked.get(), H5P_CRT_ORDER_INDEXED), 0);
  EXPECT_LT(H5Pset_attr_creation_order(H5I_INVALID_HID, 0u), 0);
  EXPECT_LT(H5Pset_attr_creation_order(H5P_DATASET_CREATE_DEFAULT, 0u), 0);
  EXPECT_LT(H5Pset_attr_creation_order(fapl.get(), 0u), 0);
  EXPECT_LT(H5Pset_attr_creation_order(removed_flags.get(), 0u), 0);

  EXPECT_LT(H5Pget_attr_creation_order(H5I_INVALID_HID, &flags), 0);
  EXPECT_LT(H5Pget_attr_creation_order(fapl.get(), &flags), 0);
  EXPECT_LT(H5Pget_attr_creation_order(removed_flags.get(), &flags), 0);

  EXPECT_LT(H5Pset_obj_track_times(H5I_INVALID_HID, false), 0);
  EXPECT_LT(H5Pset_obj_track_times(H5P_DATASET_CREATE_DEFAULT, false), 0);
  EXPECT_LT(H5Pset_obj_track_times(fapl.get(), false), 0);
  EXPECT_LT(H5Pset_obj_track_times(removed_flags.get(), false), 0);

  EXPECT_LT(H5Pget_obj_track_times(H5I_INVALID_HID, &track), 0);
  EXPECT_LT(H5Pget_obj_track_times(fapl.get(), &track), 0);
  EXPECT_LT(H5Pget_obj_track_times(removed_flags.get(), &track), 0);
}

TEST(H5PocplTest, FilterPipelineAddModifyQueryAndRemove) {
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());

  EXPECT_EQ(H5Pget_nfilters(dcpl.get()), 0);
  // Removing from an empty pipeline succeeds without modifying anything.
  EXPECT_GE(H5Premove_filter(dcpl.get(), H5Z_FILTER_DEFLATE), 0);

  // Add deflate, fletcher32, and a multi-parameter (>4) custom ID filter.
  ASSERT_GE(H5Pset_deflate(dcpl.get(), 6u), 0);
  ASSERT_GE(H5Pset_fletcher32(dcpl.get()), 0);
  constexpr std::array<unsigned, 5> kManyCdValues = {11u, 22u, 33u, 44u, 55u};
  ASSERT_GE(H5Pset_filter(dcpl.get(), 400, H5Z_FLAG_OPTIONAL,
                          kManyCdValues.size(), kManyCdValues.data()),
            0);
  EXPECT_EQ(H5Pget_nfilters(dcpl.get()), 3);

  // Filter 400 is unregistered, so H5Pall_filters_avail returns false (0).
  EXPECT_EQ(H5Pall_filters_avail(dcpl.get()), 0);

  // Modify existing filter 400 in-place (flags and client data count).
  // Note: upstream H5Z_modify checks `idx > pline->nused` instead of `>=`, so
  // H5Pmodify_filter is only called on filters already present in the pipeline.
  constexpr std::array<unsigned, 2> kNewCdValues = {77u, 88u};
  ASSERT_GE(H5Pmodify_filter(dcpl.get(), 400, H5Z_FLAG_MANDATORY,
                             kNewCdValues.size(), kNewCdValues.data()),
            0);

  // Internal H5P_filter_in_pline query.
  H5P_genplist_t* plist =
      H5P_object_verify(dcpl.get(), H5P_OBJECT_CREATE, false);
  ASSERT_NE(plist, nullptr);
  EXPECT_GT(H5P_filter_in_pline(plist, H5Z_FILTER_DEFLATE), 0);
  EXPECT_EQ(H5P_filter_in_pline(plist, 499), 0);

  // Query by index (H5Pget_filter2) on registered deflate filter.
  unsigned flags = 0;
  size_t cd_nelmts = 1;
  std::array<unsigned, 2> cd_out = {999u, 999u};
  std::array<char, 32> name_buf{};
  unsigned filter_config = 0;
  EXPECT_EQ(
      H5Pget_filter2(dcpl.get(), 0u, &flags, &cd_nelmts, cd_out.data(),
                     name_buf.size(), name_buf.data(), &filter_config),
      H5Z_FILTER_DEFLATE);
  EXPECT_EQ(flags, static_cast<unsigned>(H5Z_FLAG_OPTIONAL));
  EXPECT_EQ(cd_nelmts, 1u);
  EXPECT_EQ(cd_out[0], 6u);
  EXPECT_STREQ(name_buf.data(), "deflate");
  EXPECT_NE(filter_config & H5Z_FILTER_CONFIG_ENCODE_ENABLED, 0u);

  // Verify cd_nelmts == nullptr causes cd_values to be ignored safely.
  EXPECT_EQ(H5Pget_filter2(dcpl.get(), 0u, nullptr, nullptr, cd_out.data(), 0u,
                           nullptr, nullptr),
            H5Z_FILTER_DEFLATE);

  // Query unregistered filter >= 256 -> empty name.
  cd_nelmts = 2;
  name_buf.fill('Z');
  ASSERT_GE(
      H5Pget_filter_by_id2(dcpl.get(), 400, &flags, &cd_nelmts, cd_out.data(),
                           name_buf.size(), name_buf.data(), nullptr),
      0);
  EXPECT_EQ(flags, static_cast<unsigned>(H5Z_FLAG_MANDATORY));
  EXPECT_EQ(cd_nelmts, 2u);
  EXPECT_THAT(cd_out, ElementsAre(77u, 88u));
  EXPECT_EQ(name_buf[0], '\0');

  // Query with truncated cd_nelmts and cd_nelmts == nullptr.
  cd_nelmts = 1;
  std::array<unsigned, 1> partial_cd = {0u};
  ASSERT_GE(H5Pget_filter_by_id2(dcpl.get(), 400, nullptr, &cd_nelmts,
                                 partial_cd.data(), 0u, nullptr, nullptr),
            0);
  EXPECT_EQ(partial_cd[0], 77u);
  EXPECT_EQ(cd_nelmts, 2u);  // Returns total number defined for the filter.
  EXPECT_GE(H5Pget_filter_by_id2(dcpl.get(), 400, nullptr, nullptr,
                                 partial_cd.data(), 0u, nullptr, nullptr),
            0);

  // Deprecated H5Pget_filter1 and H5Pget_filter_by_id1 wrappers.
  cd_nelmts = 1;
  EXPECT_EQ(H5Pget_filter1(dcpl.get(), 0u, &flags, &cd_nelmts, cd_out.data(),
                           name_buf.size(), name_buf.data()),
            H5Z_FILTER_DEFLATE);
  EXPECT_STREQ(name_buf.data(), "deflate");
  EXPECT_EQ(H5Pget_filter1(dcpl.get(), 0u, nullptr, nullptr, cd_out.data(), 0u,
                           nullptr),
            H5Z_FILTER_DEFLATE);

  cd_nelmts = 2;
  ASSERT_GE(H5Pget_filter_by_id1(dcpl.get(), 400, &flags, &cd_nelmts,
                                 cd_out.data(), name_buf.size(),
                                 name_buf.data()),
            0);
  EXPECT_THAT(cd_out, ElementsAre(77u, 88u));
  EXPECT_GE(H5Pget_filter_by_id1(dcpl.get(), 400, nullptr, nullptr,
                                 cd_out.data(), 0u, nullptr),
            0);

  // Unregistered library filter (< 256) yields "Unknown library filter".
  ASSERT_GE(
      H5Pset_filter(dcpl.get(), 200, H5Z_FLAG_OPTIONAL, 0u, nullptr), 0);
  std::array<char, 8> trunc_name{};
  ASSERT_GE(
      H5Pget_filter_by_id2(dcpl.get(), 200, nullptr, nullptr, nullptr,
                           trunc_name.size(), trunc_name.data(), nullptr),
      0);
  EXPECT_STREQ(trunc_name.data(), "Unknown");

  // Remove unregistered filters, leaving only registered ones.
  ASSERT_GE(H5Premove_filter(dcpl.get(), 400), 0);
  ASSERT_GE(H5Premove_filter(dcpl.get(), 200), 0);
  EXPECT_EQ(H5Pget_nfilters(dcpl.get()), 2);
  EXPECT_GT(H5Pall_filters_avail(dcpl.get()), 0);

  // Remove all filters cleanly.
  ASSERT_GE(H5Premove_filter(dcpl.get(), H5Z_FILTER_ALL), 0);
  EXPECT_EQ(H5Pget_nfilters(dcpl.get()), 0);
}

TEST(H5PocplTest, FilterValidationAndMaxFilterLimitErrors) {
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_TRUE(fapl.valid());

  ASSERT_GE(H5Pset_deflate(dcpl.get(), 1u), 0);

  ScopedPlist too_many(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(too_many.valid());
  for (int i = 0; i < H5Z_MAX_NFILTERS; ++i) {
    ASSERT_GE(H5Pset_filter(too_many.get(), 300 + i, H5Z_FLAG_OPTIONAL, 0u,
                            nullptr),
              0);
  }

  ScopedPlist removed_pline(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(removed_pline.valid());
  ASSERT_GE(H5Premove(removed_pline.get(), H5O_CRT_PIPELINE_NAME), 0);
  H5P_genplist_t* removed_internal =
      H5P_object_verify(removed_pline.get(), H5P_OBJECT_CREATE, false);
  ASSERT_NE(removed_internal, nullptr);

  size_t bad_nelmts = 300u;
  size_t one_nelmt = 1u;
  unsigned dummy_val = 1u;
  std::array<unsigned, 1> dummy_arr{};

  ScopedErrorSilence silence;

  // H5Pset_deflate / H5Pset_fletcher32 error checks.
  EXPECT_LT(H5Pset_deflate(dcpl.get(), 10u), 0);
  EXPECT_LT(H5Pset_deflate(H5I_INVALID_HID, 5u), 0);
  EXPECT_LT(H5Pset_deflate(H5P_DATASET_CREATE_DEFAULT, 5u), 0);
  EXPECT_LT(H5Pset_deflate(fapl.get(), 5u), 0);
  EXPECT_LT(H5Pset_deflate(removed_pline.get(), 5u), 0);
  EXPECT_LT(H5Pset_deflate(too_many.get(), 5u), 0);

  EXPECT_LT(H5Pset_fletcher32(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Pset_fletcher32(H5P_DATASET_CREATE_DEFAULT), 0);
  EXPECT_LT(H5Pset_fletcher32(fapl.get()), 0);
  EXPECT_LT(H5Pset_fletcher32(removed_pline.get()), 0);
  EXPECT_LT(H5Pset_fletcher32(too_many.get()), 0);

  // H5Pset_filter / H5Pmodify_filter validation checks.
  EXPECT_LT(H5Pset_filter(dcpl.get(), -1, 0u, 0u, nullptr), 0);
  EXPECT_LT(H5Pset_filter(dcpl.get(), H5Z_FILTER_MAX + 1, 0u, 0u, nullptr), 0);
  EXPECT_LT(H5Pset_filter(dcpl.get(), 300, ~unsigned{0}, 0u, nullptr), 0);
  EXPECT_LT(H5Pset_filter(dcpl.get(), 300, 0u, 1u, nullptr), 0);
  EXPECT_LT(H5Pset_filter(fapl.get(), 300, 0u, 0u, nullptr), 0);
  EXPECT_LT(H5Pset_filter(H5P_DATASET_CREATE_DEFAULT, 300, 0u, 0u, nullptr), 0);
  EXPECT_LT(H5Pset_filter(removed_pline.get(), 300, 0u, 0u, nullptr), 0);
  EXPECT_LT(H5Pset_filter(too_many.get(), 400, 0u, 0u, nullptr), 0);

  EXPECT_LT(H5Pmodify_filter(dcpl.get(), -1, 0u, 0u, nullptr), 0);
  EXPECT_LT(H5Pmodify_filter(dcpl.get(), H5Z_FILTER_MAX + 1, 0u, 0u, nullptr),
            0);
  EXPECT_LT(H5Pmodify_filter(dcpl.get(), H5Z_FILTER_DEFLATE, ~unsigned{0}, 0u,
                             nullptr),
            0);
  EXPECT_LT(
      H5Pmodify_filter(dcpl.get(), H5Z_FILTER_DEFLATE, 0u, 1u, nullptr), 0);
  EXPECT_LT(
      H5Pmodify_filter(fapl.get(), H5Z_FILTER_DEFLATE, 0u, 0u, nullptr), 0);
  EXPECT_LT(H5Pmodify_filter(H5P_DATASET_CREATE_DEFAULT, H5Z_FILTER_DEFLATE, 0u,
                             0u, nullptr),
            0);
  EXPECT_LT(H5Pmodify_filter(removed_pline.get(), H5Z_FILTER_DEFLATE, 0u, 0u,
                             nullptr),
            0);

  // H5Pget_nfilters / H5Pall_filters_avail / H5Premove_filter error paths.
  EXPECT_LT(H5Pget_nfilters(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Pget_nfilters(fapl.get()), 0);
  EXPECT_LT(H5Pget_nfilters(removed_pline.get()), 0);

  EXPECT_LT(H5Pall_filters_avail(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Pall_filters_avail(fapl.get()), 0);
  EXPECT_LT(H5Pall_filters_avail(removed_pline.get()), 0);

  EXPECT_LT(H5P_filter_in_pline(removed_internal, H5Z_FILTER_DEFLATE), 0);

  EXPECT_LT(H5Premove_filter(H5I_INVALID_HID, H5Z_FILTER_DEFLATE), 0);
  EXPECT_LT(H5Premove_filter(H5P_DATASET_CREATE_DEFAULT, H5Z_FILTER_DEFLATE),
            0);
  EXPECT_LT(H5Premove_filter(fapl.get(), H5Z_FILTER_DEFLATE), 0);
  EXPECT_LT(H5Premove_filter(removed_pline.get(), H5Z_FILTER_DEFLATE), 0);
  EXPECT_LT(H5Premove_filter(dcpl.get(), 499), 0);

  // H5Pget_filter2 / H5Pget_filter_by_id2 / deprecated *1 error paths.
  EXPECT_EQ(H5Pget_filter2(dcpl.get(), 0u, nullptr, &bad_nelmts,
                           dummy_arr.data(), 0u, nullptr, nullptr),
            H5Z_FILTER_ERROR);
  EXPECT_EQ(H5Pget_filter2(dcpl.get(), 0u, nullptr, &one_nelmt, nullptr, 0u,
                           nullptr, nullptr),
            H5Z_FILTER_ERROR);
  EXPECT_EQ(H5Pget_filter2(fapl.get(), 0u, nullptr, nullptr, nullptr, 0u,
                           nullptr, nullptr),
            H5Z_FILTER_ERROR);
  EXPECT_EQ(H5Pget_filter2(removed_pline.get(), 0u, nullptr, nullptr, nullptr,
                           0u, nullptr, nullptr),
            H5Z_FILTER_ERROR);
  EXPECT_EQ(H5Pget_filter2(dcpl.get(), 5u, nullptr, nullptr, nullptr, 0u,
                           nullptr, nullptr),
            H5Z_FILTER_ERROR);

  EXPECT_LT(H5Pget_filter_by_id2(dcpl.get(), -1, nullptr, nullptr, nullptr, 0u,
                                 nullptr, nullptr),
            0);
  EXPECT_LT(H5Pget_filter_by_id2(dcpl.get(), H5Z_FILTER_MAX + 1, nullptr,
                                 nullptr, nullptr, 0u, nullptr, nullptr),
            0);
  EXPECT_LT(H5Pget_filter_by_id2(dcpl.get(), H5Z_FILTER_DEFLATE, nullptr,
                                 &bad_nelmts, dummy_arr.data(), 0u, nullptr,
                                 nullptr),
            0);
  EXPECT_LT(H5Pget_filter_by_id2(dcpl.get(), H5Z_FILTER_DEFLATE, nullptr,
                                 &one_nelmt, nullptr, 0u, nullptr, nullptr),
            0);
  EXPECT_LT(H5Pget_filter_by_id2(fapl.get(), H5Z_FILTER_DEFLATE, nullptr,
                                 nullptr, nullptr, 0u, nullptr, nullptr),
            0);
  EXPECT_LT(H5Pget_filter_by_id2(removed_pline.get(), H5Z_FILTER_DEFLATE,
                                 nullptr, nullptr, nullptr, 0u, nullptr,
                                 nullptr),
            0);
  EXPECT_LT(H5Pget_filter_by_id2(dcpl.get(), 499, nullptr, nullptr, nullptr,
                                 0u, nullptr, nullptr),
            0);

  EXPECT_EQ(H5Pget_filter1(dcpl.get(), 0u, nullptr, &bad_nelmts,
                           dummy_arr.data(), 0u, nullptr),
            H5Z_FILTER_ERROR);
  EXPECT_EQ(H5Pget_filter1(dcpl.get(), 0u, nullptr, &one_nelmt, nullptr, 0u,
                           nullptr),
            H5Z_FILTER_ERROR);
  EXPECT_EQ(
      H5Pget_filter1(fapl.get(), 0u, nullptr, nullptr, nullptr, 0u, nullptr),
      H5Z_FILTER_ERROR);
  EXPECT_EQ(H5Pget_filter1(removed_pline.get(), 0u, nullptr, nullptr, nullptr,
                           0u, nullptr),
            H5Z_FILTER_ERROR);
  EXPECT_EQ(
      H5Pget_filter1(dcpl.get(), 5u, nullptr, nullptr, nullptr, 0u, nullptr),
      H5Z_FILTER_ERROR);

  EXPECT_LT(H5Pget_filter_by_id1(dcpl.get(), -1, nullptr, nullptr, nullptr, 0u,
                                 nullptr),
            0);
  EXPECT_LT(H5Pget_filter_by_id1(dcpl.get(), H5Z_FILTER_MAX + 1, nullptr,
                                 nullptr, nullptr, 0u, nullptr),
            0);
  EXPECT_LT(H5Pget_filter_by_id1(dcpl.get(), H5Z_FILTER_DEFLATE, nullptr,
                                 &bad_nelmts, dummy_arr.data(), 0u, nullptr),
            0);
  EXPECT_LT(H5Pget_filter_by_id1(dcpl.get(), H5Z_FILTER_DEFLATE, nullptr,
                                 &one_nelmt, nullptr, 0u, nullptr),
            0);
  EXPECT_LT(H5Pget_filter_by_id1(fapl.get(), H5Z_FILTER_DEFLATE, nullptr,
                                 nullptr, nullptr, 0u, nullptr),
            0);
  EXPECT_LT(H5Pget_filter_by_id1(removed_pline.get(), H5Z_FILTER_DEFLATE,
                                 nullptr, nullptr, nullptr, 0u, nullptr),
            0);
  EXPECT_LT(H5Pget_filter_by_id1(dcpl.get(), 499, nullptr, nullptr, nullptr,
                                 0u, nullptr),
            0);
  (void)dummy_val;
}

TEST(H5PocplTest, PipelineGetSetCopyCompareAndEncodeDecode) {
  ScopedPlist p1(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist p2(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(p1.valid());
  ASSERT_TRUE(p2.valid());

  // 1. Compare empty pipeline vs post-removal pipeline (filter != NULL,
  //    nused == 0) in both directions.
  ASSERT_GE(H5Pset_deflate(p1.get(), 3u), 0);
  ASSERT_GE(H5Premove_filter(p1.get(), H5Z_FILTER_DEFLATE), 0);
  EXPECT_EQ(H5Pget_nfilters(p1.get()), 0);
  EXPECT_EQ(H5Pequal(p2.get(), p1.get()), 0);
  EXPECT_EQ(H5Pequal(p1.get(), p2.get()), 0);

  // 2. Compare different nused, filter ID, flags, cd_nelmts, and cd_values.
  ASSERT_GE(H5Pset_deflate(p1.get(), 3u), 0);
  EXPECT_EQ(H5Pequal(p2.get(), p1.get()), 0);
  EXPECT_EQ(H5Pequal(p1.get(), p2.get()), 0);

  ASSERT_GE(H5Pset_fletcher32(p2.get()), 0);
  EXPECT_EQ(H5Pequal(p1.get(), p2.get()), 0);
  EXPECT_EQ(H5Pequal(p2.get(), p1.get()), 0);

  ASSERT_GE(H5Premove_filter(p2.get(), H5Z_FILTER_ALL), 0);
  ASSERT_GE(H5Pset_deflate(p2.get(), 7u), 0);
  EXPECT_EQ(H5Pequal(p1.get(), p2.get()), 0);
  EXPECT_EQ(H5Pequal(p2.get(), p1.get()), 0);

  constexpr unsigned kDeflateLevel3 = 3u;
  ASSERT_GE(
      H5Pmodify_filter(p2.get(), H5Z_FILTER_DEFLATE, H5Z_FLAG_MANDATORY, 1u,
                       &kDeflateLevel3),
      0);
  EXPECT_EQ(H5Pequal(p1.get(), p2.get()), 0);
  EXPECT_EQ(H5Pequal(p2.get(), p1.get()), 0);

  // Restore p2 flags and parameter count mismatch test on filter 310.
  ASSERT_GE(
      H5Pmodify_filter(p2.get(), H5Z_FILTER_DEFLATE, H5Z_FLAG_OPTIONAL, 1u,
                       &kDeflateLevel3),
      0);
  EXPECT_GT(H5Pequal(p1.get(), p2.get()), 0);

  constexpr std::array<unsigned, 1> kOneParam = {1u};
  constexpr std::array<unsigned, 2> kTwoParams = {1u, 2u};
  ASSERT_GE(H5Pset_filter(p1.get(), 310, 0u, kOneParam.size(), kOneParam.data()),
            0);
  ASSERT_GE(
      H5Pset_filter(p2.get(), 310, 0u, kTwoParams.size(), kTwoParams.data()),
      0);
  EXPECT_EQ(H5Pequal(p1.get(), p2.get()), 0);
  EXPECT_EQ(H5Pequal(p2.get(), p1.get()), 0);

  // 3. Exercise H5Pget/H5Pset on "pline" (invokes _get and _set callbacks).
  H5O_pline_t retrieved_pline{};
  ASSERT_GE(H5Pget(p2.get(), H5O_CRT_PIPELINE_NAME, &retrieved_pline), 0);
  absl::Cleanup reset_pline = [&retrieved_pline] {
    H5O_msg_reset(H5O_PLINE_ID, &retrieved_pline);
  };
  ScopedPlist p_assigned(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(p_assigned.valid());
  ASSERT_GE(H5Pset(p_assigned.get(), H5O_CRT_PIPELINE_NAME, &retrieved_pline),
            0);
  EXPECT_GT(H5Pequal(p2.get(), p_assigned.get()), 0);

  // 4. Encode and decode pipeline containing filters with and without cd_values.
  ASSERT_GE(H5Pset_fletcher32(p2.get()), 0);
  size_t enc_size = 0;
  ASSERT_GE(H5Pencode2(p2.get(), nullptr, &enc_size, H5P_DEFAULT), 0);
  ASSERT_GT(enc_size, 0u);
  std::vector<uint8_t> enc(enc_size, 0);
  ASSERT_GE(H5Pencode2(p2.get(), enc.data(), &enc_size, H5P_DEFAULT), 0);

  ScopedPlist decoded(H5Pdecode(enc.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Pequal(p2.get(), decoded.get()), 0);

  // Corrupting sizeof(unsigned) inside "pline" triggers decode BADVALUE check.
  std::vector<uint8_t> bad_enc = enc;
  for (size_t i = 0; i + sizeof(H5O_CRT_PIPELINE_NAME) < bad_enc.size(); ++i) {
    if (std::memcmp(bad_enc.data() + i, H5O_CRT_PIPELINE_NAME,
                    sizeof(H5O_CRT_PIPELINE_NAME)) == 0) {
      bad_enc[i + sizeof(H5O_CRT_PIPELINE_NAME)] = 99;
      break;
    }
  }
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Pdecode(bad_enc.data()), H5I_INVALID_HID);
  }
}

TEST(H5PocplTest, PersistedNamedCustomFilterExercisesNameCompareAndEncode) {
  H5Z_class2_t filter_a{};
  filter_a.version = H5Z_CLASS_T_VERS;
  filter_a.id = kCustomFilterIdA;
  filter_a.encoder_present = 1;
  filter_a.decoder_present = 1;
  filter_a.name = "flt_alpha";  // <= 11 chars fits zeroed _name[12] buffer.
  filter_a.filter = &IdentityFilter;

  H5Z_class2_t filter_b = filter_a;
  filter_b.id = kCustomFilterIdB;
  filter_b.name = "flt_bravo";

  ASSERT_GE(H5Zregister(&filter_a), 0);
  ASSERT_GE(H5Zregister(&filter_b), 0);
  absl::Cleanup unregister_filters = [] {
    H5Zunregister(kCustomFilterIdB);
    H5Zunregister(kCustomFilterIdA);
  };

  std::vector<uint8_t> image;
  {
    ScopedFile mem_file = CreateInMemoryFile();
    ASSERT_TRUE(mem_file.valid());

    constexpr hsize_t kDims[1] = {4};
    constexpr hsize_t kChunkDims[1] = {4};
    ScopedSpace space(H5Screate_simple(1, kDims, nullptr));
    ASSERT_TRUE(space.valid());

    auto make_chunked_dset = [&](const char* name, H5Z_filter_t fid) {
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl.valid());
      ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, kChunkDims), 0);
      ASSERT_GE(H5Pset_filter(dcpl.get(), fid, H5Z_FLAG_OPTIONAL, 0u, nullptr),
                0);
      ScopedDataset dset(H5Dcreate2(mem_file.get(), name, H5T_NATIVE_INT32,
                                    space.get(), H5P_DEFAULT, dcpl.get(),
                                    H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());
      constexpr std::array<int32_t, 4> kData = {1, 2, 3, 4};
      ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                         H5P_DEFAULT, kData.data()),
                0);
    };
    make_chunked_dset("/ds_a", kCustomFilterIdA);
    make_chunked_dset("/ds_b", kCustomFilterIdB);
    ASSERT_GE(H5Fflush(mem_file.get(), H5F_SCOPE_GLOBAL), 0);

    ssize_t img_size = H5Fget_file_image(mem_file.get(), nullptr, 0);
    ASSERT_GT(img_size, 0);
    image.resize(static_cast<size_t>(img_size));
    ASSERT_EQ(H5Fget_file_image(mem_file.get(), image.data(), image.size()),
              img_size);
  }

  // Reopen serialized file image so H5O__pline_decode populates filter.name.
  ScopedPlist img_fapl = MakeCoreFapl();
  ASSERT_TRUE(img_fapl.valid());
  ASSERT_GE(H5Pset_file_image(img_fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(
      H5Fopen("img.h5", H5F_ACC_RDONLY, img_fapl.get()));
  ASSERT_TRUE(reopened.valid());

  ScopedDataset ds_a(H5Dopen2(reopened.get(), "/ds_a", H5P_DEFAULT));
  ScopedDataset ds_b(H5Dopen2(reopened.get(), "/ds_b", H5P_DEFAULT));
  ASSERT_TRUE(ds_a.valid());
  ASSERT_TRUE(ds_b.valid());

  ScopedPlist dcpl_a(H5Dget_create_plist(ds_a.get()));
  ScopedPlist dcpl_b(H5Dget_create_plist(ds_b.get()));
  ASSERT_TRUE(dcpl_a.valid());
  ASSERT_TRUE(dcpl_b.valid());

  std::array<char, 32> queried_name{};
  ASSERT_GE(H5Pget_filter_by_id2(dcpl_a.get(), kCustomFilterIdA, nullptr,
                                 nullptr, nullptr, queried_name.size(),
                                 queried_name.data(), nullptr),
            0);
  EXPECT_STREQ(queried_name.data(), "flt_alpha");

  // Compare named vs unnamed filter of same ID, and named vs differently named.
  ScopedPlist unnamed_a(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(unnamed_a.valid());
  ASSERT_GE(H5Pset_filter(unnamed_a.get(), kCustomFilterIdA, H5Z_FLAG_OPTIONAL,
                          0u, nullptr),
            0);
  EXPECT_EQ(H5Pequal(dcpl_a.get(), unnamed_a.get()), 0);
  EXPECT_EQ(H5Pequal(unnamed_a.get(), dcpl_a.get()), 0);
  EXPECT_EQ(H5Pequal(dcpl_a.get(), dcpl_b.get()), 0);
  EXPECT_GT(H5Pequal(dcpl_a.get(), dcpl_a.get()), 0);

  // Encode named pipeline (safe size + byte copy; note upstream
  // H5P__ocrt_pipeline_dec leaks filter.name on has_name != 0 so we do not
  // pass this encoded buffer into H5Pdecode).
  size_t enc_size = 0;
  ASSERT_GE(H5Pencode2(dcpl_a.get(), nullptr, &enc_size, H5P_DEFAULT), 0);
  ASSERT_GT(enc_size, 0u);
  std::vector<uint8_t> enc_buf(enc_size, 0);
  ASSERT_GE(H5Pencode2(dcpl_a.get(), enc_buf.data(), &enc_size, H5P_DEFAULT),
            0);
}

}  // namespace
}  // namespace hdf5
