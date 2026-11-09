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
// Tests metadata cache epoch marker descriptor metadata, unreachable callback
// sentinels, and integration with age-out cache LRU lists
// (third_party/hdf5/src/H5Cepoch.c).
//
// Build caveats: `:hdf5` in google3 is compiled with `-DNDEBUG` and without
// `H5_HAVE_PARALLEL`. `NDEBUG` is temporarily defined around internal HDF5
// headers so `H5C_t` and `H5C_cache_entry_t` struct layouts match `:hdf5`.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Cepoch.c

#include <cstddef>
#include <cstdint>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5CEPOCH_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5C_FRIEND
#define H5F_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5Cpkg.h"
#pragma push_macro("try")
#ifdef try
#undef try
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define try h5f_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#undef try
#pragma clang diagnostic pop
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5CEPOCH_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5CEPOCH_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

struct RecordedError {
  hid_t maj_num = H5I_INVALID_HID;
  hid_t min_num = H5I_INVALID_HID;
  std::string desc;
  unsigned count = 0;
};

herr_t CaptureErrorCallback(unsigned /*n*/, const H5E_error2_t* err_desc,
                            void* client_data) {
  auto* out = static_cast<RecordedError*>(client_data);
  if (err_desc != nullptr) {
    if (out->count == 0) {
      out->maj_num = err_desc->maj_num;
      out->min_num = err_desc->min_num;
      out->desc = err_desc->desc ? err_desc->desc : "";
    }
    ++out->count;
  }
  return 0;
}

template <typename Fn>
void ExpectUnreachableErrorPushed(Fn&& fn) {
  ASSERT_GE(H5Eclear2(H5E_DEFAULT), 0);
  fn();
  const ssize_t num = H5Eget_num(H5E_DEFAULT);
  EXPECT_EQ(num, 1);

  RecordedError recorded;
  ASSERT_GE(H5Ewalk2(H5E_DEFAULT, H5E_WALK_DOWNWARD, CaptureErrorCallback,
                     &recorded),
            0);
  EXPECT_EQ(recorded.count, 1U);
  EXPECT_EQ(recorded.maj_num, H5E_CACHE);
  EXPECT_EQ(recorded.min_num, H5E_SYSTEM);
  EXPECT_THAT(recorded.desc, HasSubstr("called unreachable fcn."));
  EXPECT_GE(H5Eclear2(H5E_DEFAULT), 0);
}

TEST(H5CepochTest, DescriptorMetadataMatchesSpec) {
  const H5AC_class_t& marker = H5AC_EPOCH_MARKER[0];
  EXPECT_EQ(marker.id, H5AC_EPOCH_MARKER_ID);
  EXPECT_STREQ(marker.name, "epoch marker");
  EXPECT_EQ(marker.mem_type, H5FD_MEM_DEFAULT);
  EXPECT_EQ(marker.flags, H5AC__CLASS_NO_FLAGS_SET);

  EXPECT_NE(marker.get_initial_load_size, nullptr);
  EXPECT_NE(marker.get_final_load_size, nullptr);
  EXPECT_NE(marker.verify_chksum, nullptr);
  EXPECT_NE(marker.deserialize, nullptr);
  EXPECT_NE(marker.image_len, nullptr);
  EXPECT_NE(marker.pre_serialize, nullptr);
  EXPECT_NE(marker.serialize, nullptr);
  EXPECT_NE(marker.notify, nullptr);
  EXPECT_NE(marker.free_icr, nullptr);
  EXPECT_NE(marker.fsf_size, nullptr);
}

TEST(H5CepochTest, UnreachableCallbacksReportCacheSystemError) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  const H5AC_class_t& marker = H5AC_EPOCH_MARKER[0];
  int dummy_udata = 0;
  const uint8_t dummy_image[16] = {};
  uint8_t out_image[16] = {};
  int dummy_thing = 0;
  size_t len = 0;
  haddr_t new_addr = HADDR_UNDEF;
  size_t new_len = 0;
  unsigned flags = 0;
  bool dirty = false;
  hsize_t fsf_size = 0;

  ExpectUnreachableErrorPushed([&] {
    EXPECT_LT(marker.get_initial_load_size(&dummy_udata, &len), 0);
  });
  ExpectUnreachableErrorPushed([&] {
    EXPECT_LT(marker.get_final_load_size(dummy_image, sizeof(dummy_image),
                                           &dummy_udata, &len),
              0);
  });
  ExpectUnreachableErrorPushed([&] {
    EXPECT_EQ(marker.verify_chksum(dummy_image, sizeof(dummy_image),
                                   &dummy_udata),
              0);
  });
  ExpectUnreachableErrorPushed([&] {
    EXPECT_EQ(
        marker.deserialize(dummy_image, sizeof(dummy_image), &dummy_udata,
                           &dirty),
        nullptr);
  });
  ExpectUnreachableErrorPushed(
      [&] { EXPECT_LT(marker.image_len(&dummy_thing, &len), 0); });
  ExpectUnreachableErrorPushed([&] {
    EXPECT_LT(marker.pre_serialize(nullptr, &dummy_thing, 0, sizeof(out_image),
                                   &new_addr, &new_len, &flags),
              0);
  });
  ExpectUnreachableErrorPushed([&] {
    EXPECT_LT(
        marker.serialize(nullptr, out_image, sizeof(out_image), &dummy_thing),
        0);
  });
  ExpectUnreachableErrorPushed([&] {
    EXPECT_LT(marker.notify(H5C_NOTIFY_ACTION_AFTER_INSERT, &dummy_thing), 0);
  });
  ExpectUnreachableErrorPushed(
      [&] { EXPECT_LT(marker.free_icr(&dummy_thing), 0); });
  ExpectUnreachableErrorPushed(
      [&] { EXPECT_LT(marker.fsf_size(&dummy_thing, &fsf_size), 0); });
}

TEST(H5CepochTest, EpochMarkersIntegrateIntoAgeOutCache) {
  ScopedFile file = CreateInMemoryFile("h5cepoch_ageout.h5");
  ASSERT_TRUE(file.valid());

  // 1. Configure minimal epoch length and pure age-out decrement so metadata
  //    accesses insert H5AC_EPOCH_MARKER entries into the LRU list.
  H5AC_cache_config_t cfg{};
  cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  ASSERT_GE(H5Fget_mdc_config(file.get(), &cfg), 0);
  cfg.set_initial_size = true;
  cfg.min_size = 4096;
  cfg.initial_size = 64 * 1024;
  cfg.max_size = 64 * 1024;
  cfg.epoch_length = H5C__MIN_AR_EPOCH_LENGTH;
  cfg.incr_mode = H5C_incr__off;
  cfg.flash_incr_mode = H5C_flash_incr__off;
  cfg.decr_mode = H5C_decr__age_out;
  cfg.epochs_before_eviction = 3;
  cfg.apply_empty_reserve = false;
  ASSERT_GE(H5Fset_mdc_config(file.get(), &cfg), 0);

  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
  ASSERT_NE(f, nullptr);
  H5C_t* cache_ptr = f->shared->cache;
  ASSERT_NE(cache_ptr, nullptr);
  EXPECT_EQ(cache_ptr->epoch_markers_active, 0);
  EXPECT_EQ(cache_ptr->epoch_markers[0].type, H5AC_EPOCH_MARKER);

  // 2. Drive well over H5C__MIN_AR_EPOCH_LENGTH cache accesses across uncached
  //    groups to trigger at least one epoch marker insertion into LRU.
  for (int i = 0; i < 60; ++i) {
    const std::string name = "grp_" + std::to_string(i);
    ScopedGroup grp(
        H5Gcreate2(file.get(), name.c_str(), H5P_DEFAULT, H5P_DEFAULT,
                   H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
  }
  for (int i = 0; i < 60; ++i) {
    const std::string name = "grp_" + std::to_string(i);
    ScopedGroup grp(H5Gopen2(file.get(), name.c_str(), H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
  }

  EXPECT_GT(cache_ptr->epoch_markers_active, 0);
  int marker_count_in_lru = 0;
  for (const H5C_cache_entry_t* cur = cache_ptr->LRU_head_ptr; cur != nullptr;
       cur = cur->next) {
    if (cur->type == H5AC_EPOCH_MARKER) {
      ++marker_count_in_lru;
      EXPECT_EQ(cur->type->id, H5AC_EPOCH_MARKER_ID);
    }
  }
  EXPECT_EQ(marker_count_in_lru, cache_ptr->epoch_markers_active);
}

}  // namespace
}  // namespace hdf5
