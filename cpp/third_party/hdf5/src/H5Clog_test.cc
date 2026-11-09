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
// Tests metadata cache logging lifecycle, status queries, error paths, and
// message dispatch callbacks (third_party/hdf5/src/H5Clog.c). Note: the default
// google3 non-MPI Linux build compiles without `H5_HAVE_PARALLEL` and with
// `-DNDEBUG`.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Clog.c

#include <cstdio>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <iterator>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` compiles with `-DNDEBUG`, which omits debug/stats fields from
// `H5C_cache_entry_t` and `H5C_t`. Define NDEBUG around both the prelude and
// internal headers so C++ test callers match `:hdf5` struct layouts.
#ifndef NDEBUG
#define H5CLOG_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5AC_FRIEND
#define H5C_FRIEND
#define H5F_FRIEND
#include "third_party/hdf5/src/H5ACpkg.h"
#include "third_party/hdf5/src/H5Clog.h"
#include "third_party/hdf5/src/H5Cpkg.h"
#pragma push_macro("try")
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#undef try
#define try h5fpkg_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#undef try
#pragma clang diagnostic pop
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5CLOG_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5CLOG_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

H5F_t* GetInternalFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

std::string ReadWholeFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

H5AC_cache_config_t MakeSampleCacheConfig() {
  H5AC_cache_config_t cfg{};
  cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  std::snprintf(cfg.trace_file_name, sizeof(cfg.trace_file_name), "%s",
                "sample.trace");
  cfg.evictions_enabled = true;
  cfg.min_clean_fraction = 0.25;
  cfg.max_size = 64 * 1024;
  cfg.min_size = 16 * 1024;
  cfg.epoch_length = 100;
  cfg.incr_mode = H5C_incr__off;
  cfg.flash_incr_mode = H5C_flash_incr__off;
  cfg.decr_mode = H5C_decr__off;
  cfg.dirty_bytes_threshold = H5AC__DEFAULT_DIRTY_BYTES_THRESHOLD;
  cfg.metadata_write_strategy = H5AC__DEFAULT_METADATA_WRITE_STRATEGY;
  return cfg;
}

void ExerciseAllWriteMessagesSuccess(H5C_t* cache) {
  H5C_cache_entry_t parent{};
  parent.addr = 0x1000;
  parent.size = 64;

  H5C_cache_entry_t child{};
  child.addr = 0x2000;
  child.size = 128;

  const H5AC_cache_config_t cfg = MakeSampleCacheConfig();

  EXPECT_GE(H5C_log_write_create_cache_msg(cache, 0), 0);
  EXPECT_GE(H5C_log_write_destroy_cache_msg(cache), 0);
  EXPECT_GE(H5C_log_write_evict_cache_msg(cache, 0), 0);
  EXPECT_GE(H5C_log_write_expunge_entry_msg(cache, 0x1000, 1, 0), 0);
  EXPECT_GE(H5C_log_write_flush_cache_msg(cache, 0), 0);
  EXPECT_GE(H5C_log_write_insert_entry_msg(cache, 0x1000, 1, 0x2, 64, 0), 0);
  EXPECT_GE(H5C_log_write_mark_entry_dirty_msg(cache, &parent, 0), 0);
  EXPECT_GE(H5C_log_write_mark_entry_clean_msg(cache, &parent, 0), 0);
  EXPECT_GE(H5C_log_write_mark_unserialized_entry_msg(cache, &parent, 0), 0);
  EXPECT_GE(H5C_log_write_mark_serialized_entry_msg(cache, &parent, 0), 0);
  EXPECT_GE(H5C_log_write_move_entry_msg(cache, 0x1000, 0x3000, 1, 0), 0);
  EXPECT_GE(H5C_log_write_pin_entry_msg(cache, &parent, 0), 0);
  EXPECT_GE(H5C_log_write_create_fd_msg(cache, &parent, &child, 0), 0);
  EXPECT_GE(H5C_log_write_protect_entry_msg(cache, &parent, 1,
                                            H5C__READ_ONLY_FLAG, 0),
            0);
  EXPECT_GE(H5C_log_write_protect_entry_msg(cache, &parent, 1,
                                            H5C__NO_FLAGS_SET, 0),
            0);
  EXPECT_GE(H5C_log_write_resize_entry_msg(cache, &parent, 256, 0), 0);
  EXPECT_GE(H5C_log_write_unpin_entry_msg(cache, &parent, 0), 0);
  EXPECT_GE(H5C_log_write_destroy_fd_msg(cache, &parent, &child, 0), 0);
  EXPECT_GE(H5C_log_write_unprotect_entry_msg(cache, 0x1000, 1,
                                              H5C__DIRTIED_FLAG, 0),
            0);
  EXPECT_GE(H5C_log_write_set_cache_config_msg(cache, &cfg, 0), 0);
  EXPECT_GE(H5C_log_write_remove_entry_msg(cache, &parent, 0), 0);
}

struct MockLogState {
  int call_count = 0;
  bool fail_tear_down = false;
  bool fail_start = false;
  bool fail_stop = false;
  bool fail_write_start = false;
  bool fail_write_stop = false;
  bool fail_messages = false;
};

herr_t MockTearDown(H5C_log_info_t* log_info) {
  auto* state = static_cast<MockLogState*>(log_info->udata);
  ++state->call_count;
  return state->fail_tear_down ? FAIL : SUCCEED;
}

herr_t MockStart(H5C_log_info_t* log_info) {
  auto* state = static_cast<MockLogState*>(log_info->udata);
  ++state->call_count;
  return state->fail_start ? FAIL : SUCCEED;
}

herr_t MockStop(H5C_log_info_t* log_info) {
  auto* state = static_cast<MockLogState*>(log_info->udata);
  ++state->call_count;
  return state->fail_stop ? FAIL : SUCCEED;
}

herr_t MockWriteUdataOnly(void* udata) {
  auto* state = static_cast<MockLogState*>(udata);
  ++state->call_count;
  return state->fail_messages ? FAIL : SUCCEED;
}

herr_t MockWriteStart(void* udata) {
  auto* state = static_cast<MockLogState*>(udata);
  ++state->call_count;
  return state->fail_write_start ? FAIL : SUCCEED;
}

herr_t MockWriteStop(void* udata) {
  auto* state = static_cast<MockLogState*>(udata);
  ++state->call_count;
  return state->fail_write_stop ? FAIL : SUCCEED;
}

herr_t MockWriteFxnRet(void* udata, herr_t /*fxn_ret*/) {
  return MockWriteUdataOnly(udata);
}

herr_t MockWriteExpunge(void* udata, haddr_t /*address*/, int /*type_id*/,
                        herr_t /*fxn_ret*/) {
  return MockWriteUdataOnly(udata);
}

herr_t MockWriteInsert(void* udata, haddr_t /*address*/, int /*type_id*/,
                       unsigned /*flags*/, size_t /*size*/,
                       herr_t /*fxn_ret*/) {
  return MockWriteUdataOnly(udata);
}

herr_t MockWriteEntry(void* udata, const H5C_cache_entry_t* /*entry*/,
                      herr_t /*fxn_ret*/) {
  return MockWriteUdataOnly(udata);
}

herr_t MockWriteMove(void* udata, haddr_t /*old_addr*/, haddr_t /*new_addr*/,
                     int /*type_id*/, herr_t /*fxn_ret*/) {
  return MockWriteUdataOnly(udata);
}

herr_t MockWriteFd(void* udata, const H5C_cache_entry_t* /*parent*/,
                 const H5C_cache_entry_t* /*child*/, herr_t /*fxn_ret*/) {
  return MockWriteUdataOnly(udata);
}

herr_t MockWriteProtect(void* udata, const H5C_cache_entry_t* /*entry*/,
                        int /*type_id*/, unsigned /*flags*/,
                        herr_t /*fxn_ret*/) {
  return MockWriteUdataOnly(udata);
}

herr_t MockWriteResize(void* udata, const H5C_cache_entry_t* /*entry*/,
                       size_t /*new_size*/, herr_t /*fxn_ret*/) {
  return MockWriteUdataOnly(udata);
}

herr_t MockWriteUnprotect(void* udata, haddr_t /*address*/, int /*type_id*/,
                          unsigned /*flags*/, herr_t /*fxn_ret*/) {
  return MockWriteUdataOnly(udata);
}

herr_t MockWriteSetConfig(void* udata, const H5AC_cache_config_t* /*config*/,
                          herr_t /*fxn_ret*/) {
  return MockWriteUdataOnly(udata);
}

constexpr H5C_log_class_t kMockLogClass = {
    /*name=*/"mock_log",
    /*tear_down_logging=*/MockTearDown,
    /*start_logging=*/MockStart,
    /*stop_logging=*/MockStop,
    /*write_start_log_msg=*/MockWriteStart,
    /*write_stop_log_msg=*/MockWriteStop,
    /*write_create_cache_log_msg=*/MockWriteFxnRet,
    /*write_destroy_cache_log_msg=*/MockWriteUdataOnly,
    /*write_evict_cache_log_msg=*/MockWriteFxnRet,
    /*write_expunge_entry_log_msg=*/MockWriteExpunge,
    /*write_flush_cache_log_msg=*/MockWriteFxnRet,
    /*write_insert_entry_log_msg=*/MockWriteInsert,
    /*write_mark_entry_dirty_log_msg=*/MockWriteEntry,
    /*write_mark_entry_clean_log_msg=*/MockWriteEntry,
    /*write_mark_unserialized_entry_log_msg=*/MockWriteEntry,
    /*write_mark_serialized_entry_log_msg=*/MockWriteEntry,
    /*write_move_entry_log_msg=*/MockWriteMove,
    /*write_pin_entry_log_msg=*/MockWriteEntry,
    /*write_create_fd_log_msg=*/MockWriteFd,
    /*write_protect_entry_log_msg=*/MockWriteProtect,
    /*write_resize_entry_log_msg=*/MockWriteResize,
    /*write_unpin_entry_log_msg=*/MockWriteEntry,
    /*write_destroy_fd_log_msg=*/MockWriteFd,
    /*write_unprotect_entry_log_msg=*/MockWriteUnprotect,
    /*write_set_cache_config_log_msg=*/MockWriteSetConfig,
    /*write_remove_entry_log_msg=*/MockWriteEntry,
};

constexpr H5C_log_class_t kNullCallbacksLogClass = {
    /*name=*/"null_callbacks_log",
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr,
};

TEST(H5ClogTest, SetUpErrorPathsAndStatusChecks) {
  ScopedFile file = CreateInMemoryFile("clog_errors.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* internal_f = GetInternalFile(file.get());
  ASSERT_NE(internal_f, nullptr);
  H5C_t* cache = internal_f->shared->cache;
  ASSERT_NE(cache, nullptr);

  bool enabled = true;
  bool logging = true;
  EXPECT_GE(H5C_get_logging_status(cache, &enabled, &logging), 0);
  EXPECT_FALSE(enabled);
  EXPECT_FALSE(logging);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5C_log_tear_down(cache), 0);
    EXPECT_LT(H5C_start_logging(cache), 0);
    EXPECT_LT(H5C_stop_logging(cache), 0);
    EXPECT_LT(H5C_log_set_up(cache, "ignored.log",
                             static_cast<H5C_log_style_t>(999), false),
              0);

    TempDir dir("h5c_log_err_dir");
    const std::string bad_json_path =
        (dir.path() / "nonexistent_subdir" / "cache.json").string();
    const std::string bad_trace_path =
        (dir.path() / "nonexistent_subdir" / "cache.trace").string();
    EXPECT_LT(H5C_log_set_up(cache, bad_json_path.c_str(), H5C_LOG_STYLE_JSON,
                             false),
              0);
    EXPECT_LT(H5C_log_set_up(cache, bad_trace_path.c_str(), H5C_LOG_STYLE_TRACE,
                             false),
              0);
  }

  // Test start_immediately failure on write error when /dev/full is writable.
  if (FILE* full_probe = std::fopen("/dev/full", "w")) {
    std::fclose(full_probe);
    ScopedErrorSilence silence;
    const absl::Cleanup cleanup_log = [cache]() {
      if (cache->log_info->enabled) {
        cache->log_info->logging = false;
        H5C_log_tear_down(cache);
      }
    };
    EXPECT_LT(
        H5C_log_set_up(cache, "/dev/full", H5C_LOG_STYLE_JSON, true), 0);
  }
}

TEST(H5ClogTest, JsonAndTraceLifecycleWritesExpectedMessages) {
  TempDir dir("h5c_log_lifecycle");
  const std::filesystem::path json_path = dir.child("cache.json");
  const std::filesystem::path trace_path = dir.child("cache.trace");

  ScopedFile file = CreateInMemoryFile("clog_lifecycle.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* internal_f = GetInternalFile(file.get());
  ASSERT_NE(internal_f, nullptr);
  H5C_t* cache = internal_f->shared->cache;
  ASSERT_NE(cache, nullptr);

  // Set up JSON logging without immediately starting.
  ASSERT_GE(
      H5C_log_set_up(cache, json_path.c_str(), H5C_LOG_STYLE_JSON, false), 0);
  absl::Cleanup ensure_teardown = [cache]() {
    if (cache->log_info->enabled) {
      H5C_log_tear_down(cache);
    }
  };

  bool enabled = false;
  bool logging = true;
  EXPECT_GE(H5C_get_logging_status(cache, &enabled, &logging), 0);
  EXPECT_TRUE(enabled);
  EXPECT_FALSE(logging);

  {
    ScopedErrorSilence silence;
    // Double setup and stop-before-start must fail.
    EXPECT_LT(
        H5C_log_set_up(cache, json_path.c_str(), H5C_LOG_STYLE_JSON, false), 0);
    EXPECT_LT(H5C_stop_logging(cache), 0);
  }

  // Start, write all 20 message types, then explicitly stop.
  ASSERT_GE(H5C_start_logging(cache), 0);
  EXPECT_GE(H5C_get_logging_status(cache, &enabled, &logging), 0);
  EXPECT_TRUE(enabled);
  EXPECT_TRUE(logging);

  ExerciseAllWriteMessagesSuccess(cache);

  ASSERT_GE(H5C_stop_logging(cache), 0);
  EXPECT_GE(H5C_get_logging_status(cache, &enabled, &logging), 0);
  EXPECT_TRUE(enabled);
  EXPECT_FALSE(logging);

  // Restart and let H5C_log_tear_down stop active logging automatically.
  ASSERT_GE(H5C_start_logging(cache), 0);
  ASSERT_GE(H5C_log_tear_down(cache), 0);
  EXPECT_GE(H5C_get_logging_status(cache, &enabled, &logging), 0);
  EXPECT_FALSE(enabled);
  EXPECT_FALSE(logging);

  const std::string json_contents = ReadWholeFile(json_path);
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"logging start\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"create\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"destroy\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"evict\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"expunge\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"flush\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"insert\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"dirty\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"clean\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"unserialized\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"serialized\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"move\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"pin\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"create_fd\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"protect\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"resize\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"unpin\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"destroy_fd\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"unprotect\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"set_config\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"remove\""));
  EXPECT_THAT(json_contents, HasSubstr("\"action\":\"logging stop\""));

  // Now set up TRACE logging with start_immediately = true.
  ASSERT_GE(
      H5C_log_set_up(cache, trace_path.c_str(), H5C_LOG_STYLE_TRACE, true), 0);
  EXPECT_GE(H5C_get_logging_status(cache, &enabled, &logging), 0);
  EXPECT_TRUE(enabled);
  EXPECT_TRUE(logging);

  // Also exercises trace class's NULL callbacks for create/destroy/evict.
  ExerciseAllWriteMessagesSuccess(cache);
  ASSERT_GE(H5C_log_tear_down(cache), 0);

  const std::string trace_contents = ReadWholeFile(trace_path);
  EXPECT_THAT(trace_contents,
              HasSubstr("### HDF5 metadata cache trace file version 1 ###"));
  EXPECT_THAT(trace_contents, HasSubstr("H5AC_insert_entry"));
  EXPECT_THAT(trace_contents, HasSubstr("H5AC_remove_entry"));
}

TEST(H5ClogTest, CustomAndNullCallbacksCoverSuccessAndFailureBranches) {
  ScopedFile file = CreateInMemoryFile("clog_mock.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* internal_f = GetInternalFile(file.get());
  ASSERT_NE(internal_f, nullptr);
  H5C_t* cache = internal_f->shared->cache;
  ASSERT_NE(cache, nullptr);

  const H5C_log_info_t saved_info = *cache->log_info;
  absl::Cleanup restore_log_info = [cache, saved_info]() {
    *cache->log_info = saved_info;
  };

  MockLogState state{};
  cache->log_info->enabled = true;
  cache->log_info->logging = false;
  cache->log_info->cls = &kMockLogClass;
  cache->log_info->udata = &state;

  // All mock callbacks succeed (covering non-NULL start/stop/write_start/stop).
  EXPECT_GE(H5C_start_logging(cache), 0);
  ExerciseAllWriteMessagesSuccess(cache);
  EXPECT_GE(H5C_stop_logging(cache), 0);
  EXPECT_GE(H5C_start_logging(cache), 0);
  EXPECT_GE(H5C_log_tear_down(cache), 0);
  EXPECT_GT(state.call_count, 25);

  H5C_cache_entry_t parent{};
  parent.addr = 0x100;
  parent.size = 64;
  H5C_cache_entry_t child{};
  child.addr = 0x200;
  child.size = 64;
  const H5AC_cache_config_t cfg = MakeSampleCacheConfig();

  {
    ScopedErrorSilence silence;

    // start_logging failure.
    state = MockLogState{.fail_start = true};
    cache->log_info->enabled = true;
    cache->log_info->logging = false;
    EXPECT_LT(H5C_start_logging(cache), 0);

    // write_start_log_msg failure.
    state = MockLogState{.fail_write_start = true};
    EXPECT_LT(H5C_start_logging(cache), 0);

    // write_stop_log_msg failure.
    state = MockLogState{.fail_write_stop = true};
    cache->log_info->logging = true;
    EXPECT_LT(H5C_stop_logging(cache), 0);

    // stop_logging failure directly and inside tear_down.
    state = MockLogState{.fail_stop = true};
    cache->log_info->logging = true;
    EXPECT_LT(H5C_stop_logging(cache), 0);
    EXPECT_LT(H5C_log_tear_down(cache), 0);

    // tear_down_logging failure while not actively logging.
    state = MockLogState{.fail_tear_down = true};
    cache->log_info->logging = false;
    EXPECT_LT(H5C_log_tear_down(cache), 0);

    // All 20 message writers fail when the class callback returns FAIL.
    state = MockLogState{.fail_messages = true};
    cache->log_info->logging = true;
    EXPECT_LT(H5C_log_write_create_cache_msg(cache, 0), 0);
    EXPECT_LT(H5C_log_write_destroy_cache_msg(cache), 0);
    EXPECT_LT(H5C_log_write_evict_cache_msg(cache, 0), 0);
    EXPECT_LT(H5C_log_write_expunge_entry_msg(cache, 0x100, 1, 0), 0);
    EXPECT_LT(H5C_log_write_flush_cache_msg(cache, 0), 0);
    EXPECT_LT(H5C_log_write_insert_entry_msg(cache, 0x100, 1, 0, 64, 0), 0);
    EXPECT_LT(H5C_log_write_mark_entry_dirty_msg(cache, &parent, 0), 0);
    EXPECT_LT(H5C_log_write_mark_entry_clean_msg(cache, &parent, 0), 0);
    EXPECT_LT(H5C_log_write_mark_unserialized_entry_msg(cache, &parent, 0), 0);
    EXPECT_LT(H5C_log_write_mark_serialized_entry_msg(cache, &parent, 0), 0);
    EXPECT_LT(H5C_log_write_move_entry_msg(cache, 0x100, 0x200, 1, 0), 0);
    EXPECT_LT(H5C_log_write_pin_entry_msg(cache, &parent, 0), 0);
    EXPECT_LT(H5C_log_write_create_fd_msg(cache, &parent, &child, 0), 0);
    EXPECT_LT(H5C_log_write_protect_entry_msg(cache, &parent, 1, 0, 0), 0);
    EXPECT_LT(H5C_log_write_resize_entry_msg(cache, &parent, 128, 0), 0);
    EXPECT_LT(H5C_log_write_unpin_entry_msg(cache, &parent, 0), 0);
    EXPECT_LT(H5C_log_write_destroy_fd_msg(cache, &parent, &child, 0), 0);
    EXPECT_LT(H5C_log_write_unprotect_entry_msg(cache, 0x100, 1, 0, 0), 0);
    EXPECT_LT(H5C_log_write_set_cache_config_msg(cache, &cfg, 0), 0);
    EXPECT_LT(H5C_log_write_remove_entry_msg(cache, &parent, 0), 0);
  }

  // Class with all NULL callbacks succeeds cleanly through full lifecycle.
  cache->log_info->cls = &kNullCallbacksLogClass;
  cache->log_info->udata = nullptr;
  cache->log_info->enabled = true;
  cache->log_info->logging = false;
  EXPECT_GE(H5C_start_logging(cache), 0);
  ExerciseAllWriteMessagesSuccess(cache);
  EXPECT_GE(H5C_stop_logging(cache), 0);
  EXPECT_GE(H5C_log_tear_down(cache), 0);
}

TEST(H5ClogTest, PublicFaplLoggingProducesNonEmptyMdcLog) {
  TempDir dir("h5c_public_log");
  const std::filesystem::path log_path = dir.child("public_mdc.json");
  const std::filesystem::path file_path = dir.child("public_mdc.h5");

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_mdc_log_options(fapl.get(), /*is_enabled=*/true,
                             log_path.c_str(), /*start_on_access=*/true),
      0);

  ScopedFile file(H5Fcreate(file_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl.get()));
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(H5Gcreate2(file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  grp.reset();
  EXPECT_GE(H5Fstop_mdc_logging(file.get()), 0);
  EXPECT_GE(H5Fstart_mdc_logging(file.get()), 0);
  file.reset();

  const std::string contents = ReadWholeFile(log_path);
  EXPECT_FALSE(contents.empty());
  EXPECT_THAT(contents, HasSubstr("logging start"));
}

}  // namespace
}  // namespace hdf5
