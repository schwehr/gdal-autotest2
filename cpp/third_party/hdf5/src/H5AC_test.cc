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
// Tests metadata cache configuration, validation, lifecycle, entry operations,
// flush dependencies, cache image support, tagging, corking, ring controls, and
// trace/JSON logging (third_party/hdf5/src/H5AC.c). Note: the default google3
// non-MPI Linux build compiles with `-DNDEBUG` and without `H5_HAVE_PARALLEL`.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5AC.c

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <iterator>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/match.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug/stats fields from
// H5C_cache_entry_t (H5AC_info_t) and H5C_t. Define NDEBUG around both the
// prelude and internal headers so C++ callers match `:hdf5` struct layouts.
#ifndef NDEBUG
#define H5AC_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5AC_FRIEND
#define H5C_FRIEND
#define H5F_FRIEND
#include "third_party/hdf5/src/H5ACpkg.h"
#include "third_party/hdf5/src/H5Cpkg.h"
#include "third_party/hdf5/src/H5CXprivate.h"
// H5Fpkg.h declares `H5F__efc_open(bool try, ...)`, which collides with both
// the C++ `try` keyword and libc++'s -fno-exceptions `try` macro.
#pragma push_macro("try")
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#undef try
#define try h5fpkg_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#undef try
#pragma clang diagnostic pop
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5MFprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5AC_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5AC_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

H5F_t* GetInternalFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

// RAII wrapper for pushing/popping an HDF5 API context and restoring tag/ring.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = 0x10000,
                            H5AC_ring_t ring = H5AC_RING_USER) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_tag(tag, &prev_tag_);
      H5AC_set_ring(ring, &prev_ring_);
      restored_state_ = true;
    }
  }

  ~ScopedApiContext() {
    if (restored_state_) {
      H5AC_set_ring(prev_ring_, nullptr);
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
  H5AC_ring_t prev_ring_ = H5AC_RING_INV;
  bool pushed_ = false;
  bool restored_state_ = false;
};

struct TestUdata {
  size_t load_size = 64;
  bool fail_deserialize = false;
  bool fail_get_initial_size = false;
  int* live_count = nullptr;
  uint8_t tag_byte = 0x5A;
};

struct TestCacheEntry {
  H5AC_info_t cache_info;  // Must be first member for H5AC/H5C casts.
  size_t entry_size;
  bool fail_image_len;
  size_t reported_image_len;
  int* live_count;
  uint8_t tag_byte;
};

herr_t TestGetInitialLoadSize(void* udata_ptr, size_t* image_len_ptr) {
  const auto* udata = static_cast<const TestUdata*>(udata_ptr);
  if (udata != nullptr && udata->fail_get_initial_size) {
    return FAIL;
  }
  *image_len_ptr = udata != nullptr ? udata->load_size : 64;
  return SUCCEED;
}

void* TestDeserialize(const void* /*image_ptr*/, size_t len, void* udata_ptr,
                      bool* dirty_ptr) {
  const auto* udata = static_cast<const TestUdata*>(udata_ptr);
  if (udata != nullptr && udata->fail_deserialize) {
    return nullptr;
  }
  auto* entry =
      static_cast<TestCacheEntry*>(std::calloc(1, sizeof(TestCacheEntry)));
  if (entry == nullptr) {
    return nullptr;
  }
  entry->entry_size = len;
  entry->reported_image_len = len;
  entry->fail_image_len = false;
  entry->live_count = udata != nullptr ? udata->live_count : nullptr;
  entry->tag_byte = udata != nullptr ? udata->tag_byte : 0x5A;
  if (entry->live_count != nullptr) {
    ++(*entry->live_count);
  }
  *dirty_ptr = false;
  return entry;
}

herr_t TestImageLen(const void* thing, size_t* image_len_ptr) {
  const auto* entry = static_cast<const TestCacheEntry*>(thing);
  if (entry->fail_image_len) {
    return FAIL;
  }
  *image_len_ptr = entry->reported_image_len;
  return SUCCEED;
}

herr_t TestSerialize(const H5F_t* /*f*/, void* image_ptr, size_t len,
                     void* thing) {
  const auto* entry = static_cast<const TestCacheEntry*>(thing);
  std::memset(image_ptr, entry->tag_byte, len);
  return SUCCEED;
}

herr_t TestFreeIcr(void* thing) {
  auto* entry = static_cast<TestCacheEntry*>(thing);
  if (entry->live_count != nullptr) {
    --(*entry->live_count);
  }
  std::free(entry);
  return SUCCEED;
}

herr_t TestFsfSize(const void* thing, hsize_t* fsf_size_ptr) {
  const auto* entry = static_cast<const TestCacheEntry*>(thing);
  *fsf_size_ptr = static_cast<hsize_t>(entry->entry_size);
  return SUCCEED;
}

const H5AC_class_t* TestEntryClass() {
  static const H5AC_class_t kClass = {
      /*id=*/H5AC_BT_ID,
      /*name=*/"test_ac_entry",
      /*mem_type=*/H5FD_MEM_BTREE,
      /*flags=*/H5AC__CLASS_NO_FLAGS_SET,
      /*get_initial_load_size=*/TestGetInitialLoadSize,
      /*get_final_load_size=*/nullptr,
      /*verify_chksum=*/nullptr,
      /*deserialize=*/TestDeserialize,
      /*image_len=*/TestImageLen,
      /*pre_serialize=*/nullptr,
      /*serialize=*/TestSerialize,
      /*notify=*/nullptr,
      /*free_icr=*/TestFreeIcr,
      /*fsf_size=*/TestFsfSize,
  };
  return &kClass;
}

TestCacheEntry* MakeEntry(size_t size, int* live_count,
                          uint8_t tag_byte = 0x5A) {
  auto* entry =
      static_cast<TestCacheEntry*>(std::calloc(1, sizeof(TestCacheEntry)));
  EXPECT_NE(entry, nullptr);
  if (entry != nullptr) {
    entry->entry_size = size;
    entry->reported_image_len = size;
    entry->live_count = live_count;
    entry->tag_byte = tag_byte;
    if (live_count != nullptr) {
      ++(*live_count);
    }
  }
  return entry;
}

H5AC_cache_config_t MakeDefaultConfig() {
  H5AC_cache_config_t cfg{};
  cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  cfg.rpt_fcn_enabled = false;
  cfg.open_trace_file = false;
  cfg.close_trace_file = false;
  cfg.trace_file_name[0] = '\0';
  cfg.evictions_enabled = true;
  cfg.set_initial_size = true;
  cfg.initial_size = 64 * 1024;
  cfg.min_clean_fraction = 0.25;
  cfg.max_size = 256 * 1024;
  cfg.min_size = 16 * 1024;
  cfg.epoch_length = 1000;
  cfg.incr_mode = H5C_incr__threshold;
  cfg.lower_hr_threshold = 0.9;
  cfg.increment = 2.0;
  cfg.apply_max_increment = true;
  cfg.max_increment = 64 * 1024;
  cfg.flash_incr_mode = H5C_flash_incr__add_space;
  cfg.flash_multiple = 1.0;
  cfg.flash_threshold = 0.25;
  cfg.decr_mode = H5C_decr__age_out_with_threshold;
  cfg.upper_hr_threshold = 0.999;
  cfg.decrement = 0.9;
  cfg.apply_max_decrement = true;
  cfg.max_decrement = 64 * 1024;
  cfg.epochs_before_eviction = 3;
  cfg.apply_empty_reserve = true;
  cfg.empty_reserve = 0.1;
  cfg.dirty_bytes_threshold = H5AC__DEFAULT_DIRTY_BYTES_THRESHOLD;
  cfg.metadata_write_strategy = H5AC__DEFAULT_METADATA_WRITE_STRATEGY;
  return cfg;
}

std::string ReadWholeFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

TEST(H5ACPackageTest, InitAndTermPackageWhenIdle) {
  EXPECT_GE(H5AC_init(), 0);
  EXPECT_GE(H5AC_term_package(), 0);
  // Second term when H5_PKG_INIT_VAR is already false tests the idle branch.
  EXPECT_GE(H5AC_term_package(), 0);
  EXPECT_GE(H5AC_init(), 0);
}

TEST(H5ACConfigTest, ValidateCacheConfigAcceptsValidAndRejectsInvalid) {
  ScopedErrorSilence silence;

  H5AC_cache_config_t valid_cfg = MakeDefaultConfig();
  EXPECT_GE(H5AC_validate_config(&valid_cfg), 0);

  // Translation with rpt_fcn_enabled = true and distributed strategy succeeds.
  valid_cfg.rpt_fcn_enabled = true;
  valid_cfg.metadata_write_strategy =
      H5AC_METADATA_WRITE_STRATEGY__DISTRIBUTED;
  EXPECT_GE(H5AC_validate_config(&valid_cfg), 0);

  // Disabling evictions succeeds only when all auto-resize modes are off.
  H5AC_cache_config_t no_evict_cfg = MakeDefaultConfig();
  no_evict_cfg.evictions_enabled = false;
  EXPECT_LT(H5AC_validate_config(&no_evict_cfg), 0);

  no_evict_cfg.incr_mode = H5C_incr__off;
  EXPECT_LT(H5AC_validate_config(&no_evict_cfg), 0);

  no_evict_cfg.flash_incr_mode = H5C_flash_incr__off;
  EXPECT_LT(H5AC_validate_config(&no_evict_cfg), 0);

  no_evict_cfg.decr_mode = H5C_decr__off;
  EXPECT_GE(H5AC_validate_config(&no_evict_cfg), 0);

  // Null and bad version.
  EXPECT_LT(H5AC_validate_config(nullptr), 0);
  H5AC_cache_config_t bad_ver = MakeDefaultConfig();
  bad_ver.version = 999;
  EXPECT_LT(H5AC_validate_config(&bad_ver), 0);

  // Open trace file with empty trace_file_name.
  H5AC_cache_config_t empty_trace = MakeDefaultConfig();
  empty_trace.open_trace_file = true;
  empty_trace.trace_file_name[0] = '\0';
  EXPECT_LT(H5AC_validate_config(&empty_trace), 0);

  // Out-of-range dirty_bytes_threshold.
  H5AC_cache_config_t small_dirty = MakeDefaultConfig();
  small_dirty.dirty_bytes_threshold = H5AC__MIN_DIRTY_BYTES_THRESHOLD - 1;
  EXPECT_LT(H5AC_validate_config(&small_dirty), 0);

  H5AC_cache_config_t big_dirty = MakeDefaultConfig();
  big_dirty.dirty_bytes_threshold = H5AC__MAX_DIRTY_BYTES_THRESHOLD + 1;
  EXPECT_LT(H5AC_validate_config(&big_dirty), 0);

  // Out-of-range metadata_write_strategy.
  H5AC_cache_config_t bad_strat = MakeDefaultConfig();
  bad_strat.metadata_write_strategy = 999;
  EXPECT_LT(H5AC_validate_config(&bad_strat), 0);

  // H5C_validate_resize_config failure (min_size > max_size).
  H5AC_cache_config_t bad_bounds = MakeDefaultConfig();
  bad_bounds.min_size = bad_bounds.max_size + 4096;
  EXPECT_LT(H5AC_validate_config(&bad_bounds), 0);
}

TEST(H5ACConfigTest, ValidateCacheImageConfigAcceptsValidAndRejectsInvalid) {
  ScopedErrorSilence silence;

  EXPECT_LT(H5AC_validate_cache_image_config(nullptr), 0);

  H5AC_cache_image_config_t cfg{};
  cfg.version = 999;
  EXPECT_LT(H5AC_validate_cache_image_config(&cfg), 0);

  cfg.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
  cfg.generate_image = true;
  cfg.save_resize_status = true;
  cfg.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE;
  EXPECT_LT(H5AC_validate_cache_image_config(&cfg), 0);

  cfg.save_resize_status = false;
  cfg.entry_ageout = 5;
  EXPECT_LT(H5AC_validate_cache_image_config(&cfg), 0);

  cfg.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE - 1;
  EXPECT_LT(H5AC_validate_cache_image_config(&cfg), 0);

  cfg.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__MAX + 1;
  EXPECT_LT(H5AC_validate_cache_image_config(&cfg), 0);

  cfg.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE;
  EXPECT_GE(H5AC_validate_cache_image_config(&cfg), 0);
}

TEST(H5ACConfigTest, PublicMdcConfigSizeHitRateAndFlushInProgress) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  H5AC_cache_config_t init_cfg = MakeDefaultConfig();
  init_cfg.rpt_fcn_enabled = true;
  ASSERT_GE(H5Pset_mdc_config(fapl.get(), &init_cfg), 0);

  ScopedFile file(
      H5Fcreate("mdc_config.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  H5AC_cache_config_t queried{};
  queried.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  ASSERT_GE(H5Fget_mdc_config(file.get(), &queried), 0);
  EXPECT_TRUE(queried.rpt_fcn_enabled);
  EXPECT_EQ(queried.initial_size, init_cfg.initial_size);

  queried.rpt_fcn_enabled = false;
  queried.set_initial_size = true;
  queried.max_size = 512 * 1024;
  queried.initial_size = 512 * 1024;
  ASSERT_GE(H5Fset_mdc_config(file.get(), &queried), 0);

  H5AC_cache_config_t updated{};
  updated.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  ASSERT_GE(H5Fget_mdc_config(file.get(), &updated), 0);
  EXPECT_FALSE(updated.rpt_fcn_enabled);
  EXPECT_EQ(updated.max_size, 512 * 1024);

  // Populate groups/attributes so cache hit rate and size become non-trivial.
  for (int i = 0; i < 8; ++i) {
    std::string gname = absl::StrCat("/g", i);
    ScopedGroup grp(
        H5Gcreate2(file.get(), gname.c_str(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
  }
  for (int i = 0; i < 8; ++i) {
    std::string gname = absl::StrCat("/g", i);
    ScopedGroup grp(H5Gopen2(file.get(), gname.c_str(), H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
  }

  size_t max_size = 0;
  size_t min_clean_size = 0;
  size_t cur_size = 0;
  int cur_entries = 0;
  ASSERT_GE(H5Fget_mdc_size(file.get(), &max_size, &min_clean_size, &cur_size,
                            &cur_entries),
            0);
  EXPECT_EQ(max_size, 512 * 1024);
  EXPECT_GT(cur_entries, 0);
  EXPECT_GT(cur_size, 0);

  double hit_rate = -1.0;
  ASSERT_GE(H5Fget_mdc_hit_rate(file.get(), &hit_rate), 0);
  EXPECT_GE(hit_rate, 0.0);
  EXPECT_LE(hit_rate, 1.0);

  H5F_t* f = GetInternalFile(file.get());
  ASSERT_NE(f, nullptr);
  bool flush_in_progress = true;
  ASSERT_GE(H5AC_get_cache_flush_in_progress(f->shared->cache,
                                            &flush_in_progress),
            0);
  EXPECT_FALSE(flush_in_progress);

  ASSERT_GE(H5Freset_mdc_hit_rate_stats(file.get()), 0);
  EXPECT_GE(H5Fget_mdc_hit_rate(file.get(), &hit_rate), 0);
  EXPECT_EQ(hit_rate, 0.0);

  // Verify null/invalid error branches on internal query wrappers.
  ScopedErrorSilence silence;
  H5AC_cache_config_t dummy_cfg = MakeDefaultConfig();
  EXPECT_LT(H5AC_get_cache_auto_resize_config(nullptr, &dummy_cfg), 0);
  EXPECT_LT(H5AC_get_cache_auto_resize_config(f->shared->cache, nullptr), 0);
  dummy_cfg.version = 99;
  EXPECT_LT(H5AC_get_cache_auto_resize_config(f->shared->cache, &dummy_cfg), 0);

  size_t sz = 0;
  uint32_t cnt = 0;
  EXPECT_LT(H5AC_get_cache_size(nullptr, &sz, &sz, &sz, &cnt), 0);
  EXPECT_LT(H5AC_get_cache_flush_in_progress(nullptr, &flush_in_progress), 0);
  EXPECT_LT(H5AC_get_cache_hit_rate(nullptr, &hit_rate), 0);
  EXPECT_LT(H5AC_reset_cache_hit_rate_stats(nullptr), 0);

  haddr_t img_addr = HADDR_UNDEF;
  hsize_t img_len = 0;
  EXPECT_LT(H5AC_get_mdc_image_info(nullptr, &img_addr, &img_len), 0);
  EXPECT_GE(H5AC_get_mdc_image_info(f->shared->cache, &img_addr, &img_len), 0);
  EXPECT_EQ(img_addr, HADDR_UNDEF);
  EXPECT_EQ(img_len, 0);
}

TEST(H5ACLoggingTest, JsonLoggingOnAccessAndDeferredLogging) {
  TempDir temp_dir("h5ac_json_logs");
  std::filesystem::path log1 = temp_dir.child("mdc_on_access.json");
  std::filesystem::path log2 = temp_dir.child("mdc_deferred.json");

  // Case 1: start_on_access = true (H5AC_create logs create, H5AC_dest logs destroy).
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_core(fapl.get(), 4096, /*backing_store=*/false), 0);
    ASSERT_GE(
        H5Pset_mdc_log_options(fapl.get(), /*is_enabled=*/true, log1.c_str(),
                               /*start_on_access=*/true),
        0);

    ScopedFile file(
        H5Fcreate("json_log_1.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    bool enabled = false;
    bool logging = false;
    ASSERT_GE(H5Fget_mdc_logging_status(file.get(), &enabled, &logging), 0);
    EXPECT_TRUE(enabled);
    EXPECT_TRUE(logging);

    ScopedGroup grp(
        H5Gcreate2(file.get(), "/logged_group", H5P_DEFAULT, H5P_DEFAULT,
                   H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    grp.reset();
    H5F_t* f = GetInternalFile(file.get());
    ASSERT_NE(f, nullptr);
    ASSERT_GE(H5AC_evict(f), 0);
  }
  std::string log1_contents = ReadWholeFile(log1);
  EXPECT_THAT(log1_contents, HasSubstr("\"action\":\"create\""));
  EXPECT_THAT(log1_contents, HasSubstr("\"action\":\"evict\""));
  EXPECT_THAT(log1_contents, HasSubstr("\"action\":\"destroy\""));

  // Case 2: start_on_access = false, manual start/stop, close while paused.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_core(fapl.get(), 4096, /*backing_store=*/false), 0);
    ASSERT_GE(
        H5Pset_mdc_log_options(fapl.get(), /*is_enabled=*/true, log2.c_str(),
                               /*start_on_access=*/false),
        0);

    ScopedFile file(
        H5Fcreate("json_log_2.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    bool enabled = false;
    bool logging = true;
    ASSERT_GE(H5Fget_mdc_logging_status(file.get(), &enabled, &logging), 0);
    EXPECT_TRUE(enabled);
    EXPECT_FALSE(logging);

    ASSERT_GE(H5Fstart_mdc_logging(file.get()), 0);
    ScopedGroup grp(
        H5Gcreate2(file.get(), "/manual_log", H5P_DEFAULT, H5P_DEFAULT,
                   H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    ASSERT_GE(H5Fstop_mdc_logging(file.get()), 0);

    ASSERT_GE(H5Fget_mdc_logging_status(file.get(), &enabled, &logging), 0);
    EXPECT_TRUE(enabled);
    EXPECT_FALSE(logging);
  }
  EXPECT_TRUE(std::filesystem::exists(log2));
}

TEST(H5ACCacheImageTest, GenerateAndReloadCacheImageOnFileReopen) {
  TempFile file_path("h5ac_cache_image.h5");

  // Create file with latest format (required for superblock extension) and cache image.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST, H5F_LIBVER_LATEST),
        0);

    H5AC_cache_image_config_t img_cfg{};
    img_cfg.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
    img_cfg.generate_image = true;
    img_cfg.save_resize_status = false;
    img_cfg.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE;
    ASSERT_GE(H5Pset_mdc_image_config(fapl.get(), &img_cfg), 0);

    ScopedFile file(
        H5Fcreate(file_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    for (int i = 0; i < 6; ++i) {
      std::string name = absl::StrCat("/img_grp_", i);
      ScopedGroup grp(H5Gcreate2(file.get(), name.c_str(), H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(grp.valid());
    }
    // Note: HDF5 closes and runs H5AC_prep_for_file_close() to emit the image.
  }

  // Reopen read-only and verify cache image info + pending state.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ScopedFile ro_file(
        H5Fopen(file_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(ro_file.valid());

    haddr_t image_addr = HADDR_UNDEF;
    hsize_t image_len = 0;
    ASSERT_GE(H5Fget_mdc_image_info(ro_file.get(), &image_addr, &image_len), 0);
    EXPECT_NE(image_addr, HADDR_UNDEF);
    EXPECT_GT(image_len, 0);

    H5F_t* ro_f = GetInternalFile(ro_file.get());
    ASSERT_NE(ro_f, nullptr);
    EXPECT_TRUE(H5AC_cache_image_pending(ro_f));

    // Accessing an object triggers loading the cache image on protect.
    ScopedGroup grp(H5Gopen2(ro_file.get(), "/img_grp_0", H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    EXPECT_FALSE(H5AC_cache_image_pending(ro_f));
  }

  // Test H5AC_load_cache_image_on_next_protect + H5AC_cache_image_pending on an
  // in-memory file where no cache image has been loaded yet, and verify reopening
  // R/W also consumes the persisted cache image.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ScopedFile rw_file(
        H5Fopen(file_path.c_str(), H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(rw_file.valid());

    H5F_t* rw_f = GetInternalFile(rw_file.get());
    ASSERT_NE(rw_f, nullptr);
    EXPECT_TRUE(H5AC_cache_image_pending(rw_f));

    ScopedGroup grp(H5Gopen2(rw_file.get(), "/img_grp_1", H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    EXPECT_FALSE(H5AC_cache_image_pending(rw_f));
  }

  {
    ScopedFile fresh = CreateInMemoryFile("pending_image_flag.h5");
    ASSERT_TRUE(fresh.valid());
    H5F_t* fresh_f = GetInternalFile(fresh.get());
    ASSERT_NE(fresh_f, nullptr);
    EXPECT_FALSE(H5AC_cache_image_pending(fresh_f));
    EXPECT_GE(
        H5AC_load_cache_image_on_next_protect(fresh_f, HADDR_UNDEF, 0, false),
        0);
    EXPECT_TRUE(H5AC_cache_image_pending(fresh_f));
    // Reset pending load_image before close so nothing tries to read HADDR_UNDEF.
    fresh_f->shared->cache->load_image = false;
  }
}

TEST(H5ACCorkPublicTest, CorkUncorkViaObjectMdcFlushControl) {
  ScopedFile file = CreateInMemoryFile("cork_public.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(
      H5Gcreate2(file.get(), "/corked_grp", H5P_DEFAULT, H5P_DEFAULT,
                 H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  bool disabled = true;
  ASSERT_GE(H5Oare_mdc_flushes_disabled(grp.get(), &disabled), 0);
  EXPECT_FALSE(disabled);

  ASSERT_GE(H5Odisable_mdc_flushes(grp.get()), 0);
  ASSERT_GE(H5Oare_mdc_flushes_disabled(grp.get(), &disabled), 0);
  EXPECT_TRUE(disabled);

  ASSERT_GE(H5Oenable_mdc_flushes(grp.get()), 0);
  ASSERT_GE(H5Oare_mdc_flushes_disabled(grp.get(), &disabled), 0);
  EXPECT_FALSE(disabled);
}

TEST(H5ACEntryLifecycleTest, CustomEntriesWithTraceLoggingAndAllOperations) {
  int live_entries = 0;
  TempDir temp_dir("h5ac_trace_ops");
  std::filesystem::path trace_path = temp_dir.child("cache_trace.log");

  ScopedFile file = CreateInMemoryFile("entry_lifecycle.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetInternalFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx(/*tag=*/0x20000, H5AC_RING_USER);
  ASSERT_TRUE(api_ctx.valid());

  // Enable trace logging via H5AC_set_cache_auto_resize_config.
  H5AC_cache_config_t trace_cfg = MakeDefaultConfig();
  ASSERT_LT(trace_path.native().size(), sizeof(trace_cfg.trace_file_name));
  std::snprintf(trace_cfg.trace_file_name, sizeof(trace_cfg.trace_file_name),
                "%s", trace_path.c_str());
  trace_cfg.open_trace_file = true;
  ASSERT_GE(H5AC_set_cache_auto_resize_config(f->shared->cache, &trace_cfg), 0);

  auto close_trace_on_exit = absl::MakeCleanup([f] {
    H5AC_cache_config_t close_cfg = MakeDefaultConfig();
    close_cfg.close_trace_file = true;
    H5AC_set_cache_auto_resize_config(f->shared->cache, &close_cfg);
  });

  // 1. Insert entry (parent) and check status + tag.
  haddr_t addr_parent = H5MF_alloc(f, H5FD_MEM_BTREE, 64);
  ASSERT_NE(addr_parent, HADDR_UNDEF);
  TestCacheEntry* parent = MakeEntry(64, &live_entries, 0x11);
  ASSERT_NE(parent, nullptr);
  ASSERT_GE(H5AC_insert_entry(f, TestEntryClass(), addr_parent, parent,
                              H5AC__PIN_ENTRY_FLAG),
            0);

  unsigned status = 0;
  ASSERT_GE(H5AC_get_entry_status(f, addr_parent, &status), 0);
  EXPECT_NE(status & H5AC_ES__IN_CACHE, 0u);
  EXPECT_NE(status & H5AC_ES__IS_DIRTY, 0u);
  EXPECT_NE(status & H5AC_ES__IS_PINNED, 0u);

  haddr_t entry_tag = HADDR_UNDEF;
  ASSERT_GE(H5AC_get_tag(parent, &entry_tag), 0);
  EXPECT_EQ(entry_tag, 0x20000);

  H5AC_ring_t entry_ring = H5AC_RING_INV;
  ASSERT_GE(H5AC_get_entry_ring(f, addr_parent, &entry_ring), 0);
  EXPECT_EQ(entry_ring, H5AC_RING_USER);

  // 2. Mark parent clean, dirty, unserialized, serialized while pinned.
  ASSERT_GE(H5AC_mark_entry_clean(parent), 0);
  status = 0;
  ASSERT_GE(H5AC_get_entry_status(f, addr_parent, &status), 0);
  EXPECT_EQ(status & H5AC_ES__IS_DIRTY, 0u);

  ASSERT_GE(H5AC_mark_entry_dirty(parent), 0);
  ASSERT_GE(H5AC_mark_entry_unserialized(parent), 0);
  ASSERT_GE(H5AC_mark_entry_serialized(parent), 0);
  status = 0;
  ASSERT_GE(H5AC_get_entry_status(f, addr_parent, &status), 0);
  EXPECT_NE(status & H5AC_ES__IMAGE_IS_UP_TO_DATE, 0u);
  ASSERT_GE(H5AC_mark_entry_unserialized(parent), 0);

  // 3. Flush parent cleanly, then create child entry + flush dependency.
  ASSERT_GE(H5AC_prep_for_file_flush(f), 0);
  ASSERT_GE(H5AC_flush(f), 0);
  ASSERT_GE(H5AC_secure_from_file_flush(f), 0);

  haddr_t addr_child = H5MF_alloc(f, H5FD_MEM_BTREE, 64);
  ASSERT_NE(addr_child, HADDR_UNDEF);
  TestCacheEntry* child = MakeEntry(64, &live_entries, 0x22);
  ASSERT_NE(child, nullptr);
  ASSERT_GE(H5AC_insert_entry(f, TestEntryClass(), addr_child, child,
                              H5AC__NO_FLAGS_SET),
            0);

  // Flush child cleanly so we can protect and pin it.
  ASSERT_GE(H5AC_prep_for_file_flush(f), 0);
  ASSERT_GE(H5AC_flush(f), 0);
  ASSERT_GE(H5AC_secure_from_file_flush(f), 0);

  TestUdata child_udata{};
  child_udata.load_size = 64;
  child_udata.live_count = &live_entries;
  auto* protected_child = static_cast<TestCacheEntry*>(H5AC_protect(
      f, TestEntryClass(), addr_child, &child_udata, H5AC__NO_FLAGS_SET));
  ASSERT_NE(protected_child, nullptr);

  status = 0;
  ASSERT_GE(H5AC_get_entry_status(f, addr_child, &status), 0);
  EXPECT_NE(status & H5AC_ES__IS_PROTECTED, 0u);

  ASSERT_GE(H5AC_pin_protected_entry(protected_child), 0);
  ASSERT_GE(H5AC_create_flush_dependency(parent, protected_child), 0);

  status = 0;
  ASSERT_GE(H5AC_get_entry_status(f, addr_parent, &status), 0);
  EXPECT_NE(status & H5AC_ES__IS_FLUSH_DEP_PARENT, 0u);
  status = 0;
  ASSERT_GE(H5AC_get_entry_status(f, addr_child, &status), 0);
  EXPECT_NE(status & H5AC_ES__IS_FLUSH_DEP_CHILD, 0u);

  // Resize protected child.
  haddr_t addr_child_big = H5MF_alloc(f, H5FD_MEM_BTREE, 96);
  ASSERT_NE(addr_child_big, HADDR_UNDEF);
  protected_child->entry_size = 96;
  protected_child->reported_image_len = 96;
  ASSERT_GE(H5AC_resize_entry(protected_child, 96), 0);
  ASSERT_GE(H5AC_move_entry(f, TestEntryClass(), addr_child, addr_child_big),
            0);
  ASSERT_GE(H5AC_unprotect(f, TestEntryClass(), addr_child_big,
                           protected_child, H5AC__DIRTIED_FLAG),
            0);

  // Destroy flush dependency, unpin child, flush & evict.
  ASSERT_GE(H5AC_destroy_flush_dependency(parent, protected_child), 0);
  ASSERT_GE(H5AC_unpin_entry(protected_child), 0);
  ASSERT_GE(H5AC_prep_for_file_flush(f), 0);
  ASSERT_GE(H5AC_flush(f), 0);
  ASSERT_GE(H5AC_secure_from_file_flush(f), 0);
  ASSERT_GE(H5AC_evict(f), 0);

  // Protect clean entry on disk (after eviction), verify reload, unprotect+delete.
  child_udata.load_size = 96;
  auto* reloaded = static_cast<TestCacheEntry*>(H5AC_protect(
      f, TestEntryClass(), addr_child_big, &child_udata, H5AC__READ_ONLY_FLAG));
  ASSERT_NE(reloaded, nullptr);
  ASSERT_GE(H5AC_unprotect(f, TestEntryClass(), addr_child_big, reloaded,
                           H5AC__NO_FLAGS_SET),
            0);

  // Remove clean entry from cache cleanly (caller regains ownership & frees).
  ASSERT_GE(H5AC_remove_entry(reloaded), 0);
  TestFreeIcr(reloaded);

  // Insert another entry and expunge it cleanly.
  haddr_t addr_exp = H5MF_alloc(f, H5FD_MEM_BTREE, 64);
  ASSERT_NE(addr_exp, HADDR_UNDEF);
  TestCacheEntry* exp_entry = MakeEntry(64, &live_entries, 0x33);
  ASSERT_NE(exp_entry, nullptr);
  ASSERT_GE(H5AC_insert_entry(f, TestEntryClass(), addr_exp, exp_entry,
                              H5AC__NO_FLAGS_SET),
            0);
  ASSERT_GE(H5AC_expunge_entry(f, TestEntryClass(), addr_exp,
                               H5AC__NO_FLAGS_SET),
            0);

  // Non-existent entry status is 0 and expunge/move are no-ops.
  status = 0xFFFFu;
  ASSERT_GE(H5AC_get_entry_status(f, addr_exp, &status), 0);
  EXPECT_EQ(status, 0u);
  EXPECT_GE(H5AC_expunge_entry(f, TestEntryClass(), addr_exp,
                               H5AC__NO_FLAGS_SET),
            0);

  // Corked entry status check via H5AC_get_entry_status + H5AC_cork.
  bool corked = true;
  ASSERT_GE(H5AC_cork(f, 0x20000, H5AC__GET_CORKED, &corked), 0);
  EXPECT_FALSE(corked);
  ASSERT_GE(H5AC_cork(f, 0x20000, H5AC__SET_CORK, nullptr), 0);
  status = 0;
  ASSERT_GE(H5AC_get_entry_status(f, addr_parent, &status), 0);
  EXPECT_NE(status & H5AC_ES__IS_CORKED, 0u);
  ASSERT_GE(H5AC_cork(f, 0x20000, H5AC__UNCORK, nullptr), 0);

  // Retag / tagged flush / tagged expunge / tagged evict.
  haddr_t copied_addr = HADDR_UNDEF;
  {
    ScopedApiContext copied_ctx(H5AC__COPIED_TAG, H5AC_RING_USER);
    ASSERT_TRUE(copied_ctx.valid());
    copied_addr = H5MF_alloc(f, H5FD_MEM_BTREE, 64);
    ASSERT_NE(copied_addr, HADDR_UNDEF);
    TestCacheEntry* copied_entry = MakeEntry(64, &live_entries, 0x44);
    ASSERT_NE(copied_entry, nullptr);
    ASSERT_GE(H5AC_insert_entry(f, TestEntryClass(), copied_addr, copied_entry,
                                H5AC__NO_FLAGS_SET),
              0);
  }
  ASSERT_GE(H5AC_retag_copied_metadata(f, 0x30000), 0);
  ASSERT_GE(H5AC_flush_tagged_metadata(f, 0x30000), 0);
  ASSERT_GE(H5AC_evict_tagged_metadata(f, 0x30000, false), 0);

  // Insert with another tag and expunge by tag+type.
  haddr_t ettm_addr = HADDR_UNDEF;
  {
    ScopedApiContext ettm_ctx(/*tag=*/0x40000, H5AC_RING_USER);
    ASSERT_TRUE(ettm_ctx.valid());
    ettm_addr = H5MF_alloc(f, H5FD_MEM_BTREE, 64);
    ASSERT_NE(ettm_addr, HADDR_UNDEF);
    TestCacheEntry* ettm_entry = MakeEntry(64, &live_entries, 0x55);
    ASSERT_NE(ettm_entry, nullptr);
    ASSERT_GE(H5AC_insert_entry(f, TestEntryClass(), ettm_addr, ettm_entry,
                                H5AC__NO_FLAGS_SET),
              0);
  }
  ASSERT_GE(
      H5AC_expunge_tag_type_metadata(f, 0x40000, H5AC_BT_ID, H5AC__NO_FLAGS_SET),
      0);

  // Trigger logging of a failed config update before closing the trace file.
  {
    ScopedErrorSilence silence;
    H5AC_cache_config_t bad_ver = MakeDefaultConfig();
    bad_ver.version = 999;
    EXPECT_LT(H5AC_set_cache_auto_resize_config(f->shared->cache, &bad_ver), 0);
  }

  ASSERT_GE(H5AC_unpin_entry(parent), 0);
  std::move(close_trace_on_exit).Invoke();

  // Verify trace log actually recorded H5AC_* calls.
  std::string trace = ReadWholeFile(trace_path);
  EXPECT_THAT(trace, HasSubstr("H5AC_insert_entry"));
  EXPECT_THAT(trace, HasSubstr("H5AC_protect"));
  EXPECT_THAT(trace, HasSubstr("H5AC_unprotect"));
  EXPECT_THAT(trace, HasSubstr("H5AC_create_flush_dependency"));
  EXPECT_THAT(trace, HasSubstr("H5AC_destroy_flush_dependency"));
  EXPECT_THAT(trace, HasSubstr("H5AC_remove_entry"));
  EXPECT_THAT(trace, HasSubstr("H5AC_expunge_entry"));
  EXPECT_THAT(trace, HasSubstr("H5AC_flush"));

  file.reset();
  EXPECT_EQ(live_entries, 0);
}

TEST(H5ACErrorPathsTest, EntryAndCacheErrorBranchesRecoverCleanly) {
  int live_entries = 0;
  TempDir temp_dir("h5ac_err_paths");

  ScopedFile file = CreateInMemoryFile("entry_errors.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = GetInternalFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx(/*tag=*/0x50000, H5AC_RING_USER);
  ASSERT_TRUE(api_ctx.valid());
  ScopedErrorSilence silence;

  // Trace file setup/teardown failure paths on non-null cache.
  H5AC_cache_config_t close_when_off = MakeDefaultConfig();
  close_when_off.close_trace_file = true;
  EXPECT_LT(H5AC_set_cache_auto_resize_config(f->shared->cache, &close_when_off),
            0);

  H5AC_cache_config_t bad_trace_path = MakeDefaultConfig();
  std::filesystem::path missing = temp_dir.child("nonexistent_subdir/trace.log");
  std::snprintf(bad_trace_path.trace_file_name,
                sizeof(bad_trace_path.trace_file_name), "%s", missing.c_str());
  bad_trace_path.open_trace_file = true;
  EXPECT_LT(H5AC_set_cache_auto_resize_config(f->shared->cache, &bad_trace_path),
            0);

  // H5AC_get_entry_status bad parameter checks.
  unsigned status = 0;
  EXPECT_LT(H5AC_get_entry_status(nullptr, 0, &status), 0);
  EXPECT_LT(H5AC_get_entry_status(f, HADDR_UNDEF, &status), 0);
  EXPECT_LT(H5AC_get_entry_status(f, 0, nullptr), 0);

  // Allocate two valid file addresses for entry tests.
  haddr_t addr1 = H5MF_alloc(f, H5FD_MEM_BTREE, 64);
  haddr_t addr2 = H5MF_alloc(f, H5FD_MEM_BTREE, 64);
  ASSERT_NE(addr1, HADDR_UNDEF);
  ASSERT_NE(addr2, HADDR_UNDEF);

  // Duplicate insert fails cleanly.
  TestCacheEntry* e1 = MakeEntry(64, &live_entries, 0x61);
  ASSERT_GE(
      H5AC_insert_entry(f, TestEntryClass(), addr1, e1, H5AC__NO_FLAGS_SET), 0);
  TestCacheEntry* dup_e = MakeEntry(64, &live_entries, 0x62);
  EXPECT_LT(
      H5AC_insert_entry(f, TestEntryClass(), addr1, dup_e, H5AC__NO_FLAGS_SET),
      0);
  TestFreeIcr(dup_e);

  // Operations on unprotected, unpinned e1 fail cleanly:
  EXPECT_LT(H5AC_mark_entry_dirty(e1), 0);
  EXPECT_LT(H5AC_mark_entry_clean(e1), 0);
  EXPECT_LT(H5AC_mark_entry_unserialized(e1), 0);
  EXPECT_LT(H5AC_mark_entry_serialized(e1), 0);
  EXPECT_LT(H5AC_pin_protected_entry(e1), 0);
  EXPECT_LT(H5AC_unpin_entry(e1), 0);
  EXPECT_LT(H5AC_resize_entry(e1, 0), 0);
  EXPECT_LT(H5AC_resize_entry(e1, 128), 0);

  // Flush clean e1 to disk so remove_entry dirty check can be tested separately,
  // and mark e1 dirty via protect/unprotect or test remove_entry on dirty e1:
  EXPECT_LT(H5AC_remove_entry(e1), 0);  // e1 is dirty -> CANTREMOVE.

  // Flush e1 cleanly first.
  ASSERT_GE(H5AC_prep_for_file_flush(f), 0);
  ASSERT_GE(H5AC_flush(f), 0);
  ASSERT_GE(H5AC_secure_from_file_flush(f), 0);

  // Flush dependency errors and expunge failure on pinned/protected entries.
  TestCacheEntry* e2 = MakeEntry(64, &live_entries, 0x63);
  ASSERT_GE(
      H5AC_insert_entry(f, TestEntryClass(), addr2, e2, H5AC__NO_FLAGS_SET), 0);

  // Self dependency fails.
  EXPECT_LT(H5AC_create_flush_dependency(e1, e1), 0);
  // Destroy flush dependency when parent is not pinned fails.
  EXPECT_LT(H5AC_destroy_flush_dependency(e1, e2), 0);

  // Move to occupied address fails cleanly.
  EXPECT_LT(H5AC_move_entry(f, TestEntryClass(), addr1, addr2), 0);

  // Protect e1 with image_len error + bad size + protected flush + tagged errors.
  TestUdata ok_udata{};
  ok_udata.load_size = 64;
  ok_udata.live_count = &live_entries;
  auto* p1 = static_cast<TestCacheEntry*>(
      H5AC_protect(f, TestEntryClass(), addr1, &ok_udata, H5AC__NO_FLAGS_SET));
  ASSERT_NE(p1, nullptr);

  // Expunge while protected fails; expunge_tag_type_metadata and evict_tagged_metadata fail.
  EXPECT_LT(H5AC_expunge_entry(f, TestEntryClass(), addr1, H5AC__NO_FLAGS_SET),
            0);
  EXPECT_LT(H5AC_evict_tagged_metadata(f, 0x50000, false), 0);
  EXPECT_LT(
      H5AC_expunge_tag_type_metadata(f, 0x50000, H5AC_BT_ID, H5AC__NO_FLAGS_SET),
      0);

  // H5AC_flush fails while p1 is protected (and restores cache flush state cleanly).
  ASSERT_GE(H5AC_prep_for_file_flush(f), 0);
  EXPECT_LT(H5AC_flush(f), 0);
  ASSERT_GE(H5AC_secure_from_file_flush(f), 0);

  // H5AC_unprotect image_len error and bad size checks.
  p1->fail_image_len = true;
  EXPECT_LT(H5AC_unprotect(f, TestEntryClass(), addr1, p1, H5AC__DIRTIED_FLAG),
            0);
  p1->fail_image_len = false;

  p1->reported_image_len = 128;  // Mismatches entry->cache_info.size (64).
  EXPECT_LT(H5AC_unprotect(f, TestEntryClass(), addr1, p1, H5AC__DIRTIED_FLAG),
            0);
  p1->reported_image_len = 64;

  // Normal unprotect succeeds.
  ASSERT_GE(H5AC_unprotect(f, TestEntryClass(), addr1, p1, H5AC__NO_FLAGS_SET),
            0);

  // R/O protect unprotect-with-dirty error branch.
  auto* ro_p1 = static_cast<TestCacheEntry*>(
      H5AC_protect(f, TestEntryClass(), addr1, &ok_udata, H5AC__READ_ONLY_FLAG));
  ASSERT_NE(ro_p1, nullptr);
  EXPECT_LT(
      H5AC_unprotect(f, TestEntryClass(), addr1, ro_p1, H5AC__DIRTIED_FLAG), 0);
  ASSERT_GE(
      H5AC_unprotect(f, TestEntryClass(), addr1, ro_p1, H5AC__NO_FLAGS_SET), 0);

  // Protect failure when entry deserialize returns nullptr (logging must be OFF).
  haddr_t addr3 = H5MF_alloc(f, H5FD_MEM_BTREE, 64);
  ASSERT_NE(addr3, HADDR_UNDEF);
  TestUdata fail_udata{};
  fail_udata.load_size = 64;
  fail_udata.fail_deserialize = true;
  fail_udata.live_count = &live_entries;
  EXPECT_EQ(
      H5AC_protect(f, TestEntryClass(), addr3, &fail_udata, H5AC__READ_ONLY_FLAG),
      nullptr);

  // Entry ring lookup on uncached addr fails cleanly.
  H5AC_ring_t out_ring = H5AC_RING_INV;
  EXPECT_LT(H5AC_get_entry_ring(f, addr3, &out_ring), 0);

  // Cork error paths: duplicate cork, uncork already uncorked, uncork missing tag,
  // and GET_CORKED when another object is corked (bypasses num_objs_corked == 0).
  ASSERT_GE(H5AC_cork(f, 0x50000, H5AC__SET_CORK, nullptr), 0);
  EXPECT_LT(H5AC_cork(f, 0x50000, H5AC__SET_CORK, nullptr), 0);
  bool other_corked = true;
  ASSERT_GE(H5AC_cork(f, 0x59999, H5AC__GET_CORKED, &other_corked), 0);
  EXPECT_FALSE(other_corked);
  ASSERT_GE(H5AC_cork(f, 0x50000, H5AC__UNCORK, nullptr), 0);
  EXPECT_LT(H5AC_cork(f, 0x50000, H5AC__UNCORK, nullptr), 0);
  EXPECT_LT(H5AC_cork(f, 0x58888, H5AC__UNCORK, nullptr), 0);

  // Ignore tags wrapper succeeds.
  EXPECT_GE(H5AC_ignore_tags(f), 0);

  file.reset();
  EXPECT_EQ(live_entries, 0);
}

TEST(H5ACErrorPathsTest, ReadOnlyFileRejectsInsertAndWriteProtectAndRingUnsettle) {
  TempFile disk_file("h5ac_ro_test.h5");
  haddr_t btree_addr = HADDR_UNDEF;

  // Create an on-disk HDF5 file with a chunked dataset or group, plus test unsettle ring.
  {
    ScopedFile rw(
        H5Fcreate(disk_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(rw.valid());
    ScopedGroup grp(
        H5Gcreate2(rw.get(), "/g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    H5F_t* rw_f = GetInternalFile(rw.get());
    ASSERT_NE(rw_f, nullptr);
    ScopedApiContext ctx(/*tag=*/0x60000, H5AC_RING_USER);
    ASSERT_TRUE(ctx.valid());
    btree_addr = H5MF_alloc(rw_f, H5FD_MEM_BTREE, 64);
    ASSERT_NE(btree_addr, HADDR_UNDEF);

    // Unsettle rings normally when unsettled/settled.
    EXPECT_GE(H5AC_unsettle_ring(rw_f, H5AC_RING_RDFSM), 0);
    EXPECT_GE(H5AC_unsettle_ring(rw_f, H5AC_RING_MDFSM), 0);

    // Trigger settled -> error branch in H5AC_unsettle_ring and H5AC_unsettle_entry_ring
    // right before file close (where prep_for_file_close marks close_warning_received).
    int live = 0;
    TestCacheEntry* rdfsm_entry = MakeEntry(64, &live, 0x77);
    {
      ScopedApiContext rdfsm_ctx(/*tag=*/0x61000, H5AC_RING_RDFSM);
      ASSERT_TRUE(rdfsm_ctx.valid());
      haddr_t rdfsm_addr = H5MF_alloc(rw_f, H5FD_MEM_BTREE, 64);
      ASSERT_NE(rdfsm_addr, HADDR_UNDEF);
      ASSERT_GE(H5AC_insert_entry(rw_f, TestEntryClass(), rdfsm_addr,
                                  rdfsm_entry, H5AC__PIN_ENTRY_FLAG),
                0);
    }

    rw_f->shared->cache->rdfsm_settled = true;
    EXPECT_GE(H5AC_unsettle_entry_ring(rdfsm_entry), 0);
    EXPECT_FALSE(rw_f->shared->cache->rdfsm_settled);

    // Now prepare for file close so close_warning_received = true, then test error paths.
    ASSERT_GE(H5AC_prep_for_file_close(rw_f), 0);
    {
      ScopedErrorSilence silence;
      rw_f->shared->cache->rdfsm_settled = true;
      EXPECT_LT(H5AC_unsettle_entry_ring(rdfsm_entry), 0);
      EXPECT_LT(H5AC_unsettle_ring(rw_f, H5AC_RING_RDFSM), 0);
    }
    rw_f->shared->cache->rdfsm_settled = false;
    ASSERT_GE(H5AC_unpin_entry(rdfsm_entry), 0);
    grp.reset();
    rw.reset();
    EXPECT_EQ(live, 0);
  }

  // Reopen R/O and verify H5AC_insert_entry and R/W H5AC_protect reject R/O files.
  ScopedFile ro(
      H5Fopen(disk_file.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(ro.valid());
  H5F_t* ro_f = GetInternalFile(ro.get());
  ASSERT_NE(ro_f, nullptr);

  ScopedApiContext ro_ctx(/*tag=*/0x70000, H5AC_RING_USER);
  ASSERT_TRUE(ro_ctx.valid());
  ScopedErrorSilence silence;

  int ro_live = 0;
  TestCacheEntry* ro_entry = MakeEntry(64, &ro_live, 0x88);
  EXPECT_LT(H5AC_insert_entry(ro_f, TestEntryClass(), btree_addr, ro_entry,
                              H5AC__NO_FLAGS_SET),
            0);
  TestFreeIcr(ro_entry);
  EXPECT_EQ(ro_live, 0);

  TestUdata udata{};
  udata.load_size = 64;
  EXPECT_EQ(
      H5AC_protect(ro_f, TestEntryClass(), btree_addr, &udata,
                   H5AC__NO_FLAGS_SET),
      nullptr);
}

}  // namespace
}  // namespace hdf5
