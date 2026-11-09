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
// Tests metadata cache JSON logging setup, teardown, public MDC log controls,
// message formatting across all JSON log callbacks, and write-error propagation
// (third_party/hdf5/src/H5Clog_json.c).
//
// Build caveat: //third_party/hdf5 compiles with -DNDEBUG and without
// H5_HAVE_PARALLEL. Internal headers are wrapped with matching NDEBUG so
// C++20 tests observe the exact H5C_t / H5C_cache_entry_t ABI layout.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Clog_json.c

#ifndef NDEBUG
#define H5CLOG_JSON_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5C_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5Clog.h"
#include "third_party/hdf5/src/H5Cpkg.h"
}  // extern "C"

#ifdef H5CLOG_JSON_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5CLOG_JSON_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

std::string ReadFileToString(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return "";
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

TEST(H5ClogJsonTest, PublicMdcLogOptionsLifecycleWritesValidJsonLog) {
  // 1. Configure Core VFD FAPL with JSON MDC logging enabled on access.
  TempFile log_file("mdc_json_public.json");
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  ASSERT_GE(H5Pset_mdc_log_options(fapl.get(), /*is_enabled=*/true,
                                   log_file.c_str(), /*start_on_access=*/true),
            0);

  bool opt_enabled = false;
  bool opt_start = false;
  size_t opt_len = 0;
  ASSERT_GE(H5Pget_mdc_log_options(fapl.get(), &opt_enabled, nullptr, &opt_len,
                                   &opt_start),
            0);
  EXPECT_TRUE(opt_enabled);
  EXPECT_TRUE(opt_start);
  ASSERT_GT(opt_len, 0U);
  std::string opt_loc(opt_len, '\0');
  ASSERT_GE(H5Pget_mdc_log_options(fapl.get(), &opt_enabled, opt_loc.data(),
                                   &opt_len, &opt_start),
            0);
  if (!opt_loc.empty() && opt_loc.back() == '\0') {
    opt_loc.pop_back();
  }
  EXPECT_EQ(opt_loc, log_file.string());

  // 2. Create in-memory HDF5 file and verify logging status.
  ScopedFile file(
      H5Fcreate("json_public.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  bool is_enabled = false;
  bool is_logging = false;
  ASSERT_GE(H5Fget_mdc_logging_status(file.get(), &is_enabled, &is_logging), 0);
  EXPECT_TRUE(is_enabled);
  EXPECT_TRUE(is_logging);

  // 3. Exercise metadata operations, cache config update, flush, stop/start.
  {
    ScopedGroup grp(
        H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    constexpr hsize_t kDims[1] = {4};
    ScopedSpace space(H5Screate_simple(1, kDims, nullptr));
    ASSERT_TRUE(space.valid());

    ScopedDataset dset(H5Dcreate2(grp.get(), "ds", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
  }

  H5AC_cache_config_t cfg{};
  cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  ASSERT_GE(H5Fget_mdc_config(file.get(), &cfg), 0);
  ASSERT_GE(H5Fset_mdc_config(file.get(), &cfg), 0);

  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
  ASSERT_GE(H5Fstop_mdc_logging(file.get()), 0);
  ASSERT_GE(H5Fget_mdc_logging_status(file.get(), &is_enabled, &is_logging), 0);
  EXPECT_TRUE(is_enabled);
  EXPECT_FALSE(is_logging);

  ASSERT_GE(H5Fstart_mdc_logging(file.get()), 0);
  ASSERT_GE(H5Fget_mdc_logging_status(file.get(), &is_enabled, &is_logging), 0);
  EXPECT_TRUE(is_enabled);
  EXPECT_TRUE(is_logging);

  // Closing the file emits destroy and logging stop and closes the log FILE*.
  file.reset();

  // 4. Verify the persisted JSON log file contains expected records.
  const std::string log_text = ReadFileToString(log_file.path());
  EXPECT_THAT(log_text, HasSubstr("\"HDF5 metadata cache log messages\" : ["));
  EXPECT_THAT(log_text, HasSubstr("\"action\":\"logging start\""));
  EXPECT_THAT(log_text, HasSubstr("\"action\":\"create\""));
  EXPECT_THAT(log_text, HasSubstr("\"action\":\"insert\""));
  EXPECT_THAT(log_text, HasSubstr("\"action\":\"protect\""));
  EXPECT_THAT(log_text, HasSubstr("\"action\":\"unprotect\""));
  EXPECT_THAT(log_text, HasSubstr("\"action\":\"set_config\""));
  EXPECT_THAT(log_text, HasSubstr("\"action\":\"flush\""));
  EXPECT_THAT(log_text, HasSubstr("\"action\":\"destroy\""));
  EXPECT_THAT(log_text, HasSubstr("\"action\":\"logging stop\"}\n]}"));
}

TEST(H5ClogJsonTest, PausedStartAndErrorPathsHandleInvalidStateCleanly) {
  TempFile paused_log("mdc_json_paused.json");
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  ASSERT_GE(H5Pset_mdc_log_options(fapl.get(), /*is_enabled=*/true,
                                   paused_log.c_str(),
                                   /*start_on_access=*/false),
            0);

  ScopedFile file(
      H5Fcreate("json_paused.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());

  bool is_enabled = false;
  bool is_logging = true;
  ASSERT_GE(H5Fget_mdc_logging_status(file.get(), &is_enabled, &is_logging), 0);
  EXPECT_TRUE(is_enabled);
  EXPECT_FALSE(is_logging);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Fstop_mdc_logging(file.get()), 0);
  }
  file.reset();

  // Calling start/stop logging on a file without MDC logging enabled fails.
  ScopedFile plain = CreateInMemoryFile("json_disabled.h5");
  ASSERT_TRUE(plain.valid());
  ASSERT_GE(H5Fget_mdc_logging_status(plain.get(), &is_enabled, &is_logging),
            0);
  EXPECT_FALSE(is_enabled);
  EXPECT_FALSE(is_logging);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Fstart_mdc_logging(plain.get()), 0);
    EXPECT_LT(H5Fstop_mdc_logging(plain.get()), 0);
  }
}

TEST(H5ClogJsonTest, AllJsonCallbacksEmitFormattedLogRecords) {
  ASSERT_GE(H5open(), 0);

  TempFile log_file("mdc_json_all_callbacks.json");
  H5C_log_info_t log_info{};
  H5C_t cache{};
  cache.log_info = &log_info;

  ASSERT_GE(H5C__log_json_set_up(&log_info, log_file.c_str(), /*mpi_rank=*/-1),
            0);
  ASSERT_NE(log_info.cls, nullptr);
  ASSERT_NE(log_info.udata, nullptr);
  EXPECT_STREQ(log_info.cls->name, "json");
  log_info.enabled = true;

  absl::Cleanup teardown = [&cache, &log_info] {
    if (log_info.enabled) {
      H5C_log_tear_down(&cache);
    }
  };

  H5C_cache_entry_t parent{};
  parent.addr = 0x1000;
  parent.size = 256;

  H5C_cache_entry_t child{};
  child.addr = 0x2000;
  child.size = 512;

  H5AC_cache_config_t cfg = H5AC__DEFAULT_CACHE_CONFIG;

  ASSERT_GE(H5C_start_logging(&cache), 0);
  ASSERT_GE(H5C_log_write_create_cache_msg(&cache, 0), 0);
  ASSERT_GE(H5C_log_write_evict_cache_msg(&cache, 0), 0);
  ASSERT_GE(H5C_log_write_expunge_entry_msg(&cache, 0x1110, 1, 0), 0);
  ASSERT_GE(H5C_log_write_flush_cache_msg(&cache, 0), 0);
  ASSERT_GE(H5C_log_write_insert_entry_msg(&cache, 0x1230, 2, 0x3, 64, 0), 0);
  ASSERT_GE(H5C_log_write_mark_entry_dirty_msg(&cache, &parent, 0), 0);
  ASSERT_GE(H5C_log_write_mark_entry_clean_msg(&cache, &parent, 0), 0);
  ASSERT_GE(H5C_log_write_mark_unserialized_entry_msg(&cache, &parent, 0), 0);
  ASSERT_GE(H5C_log_write_mark_serialized_entry_msg(&cache, &parent, 0), 0);
  ASSERT_GE(H5C_log_write_move_entry_msg(&cache, 0x1000, 0x3000, 4, 0), 0);
  ASSERT_GE(H5C_log_write_pin_entry_msg(&cache, &parent, 0), 0);
  ASSERT_GE(H5C_log_write_create_fd_msg(&cache, &parent, &child, 0), 0);
  // Cover both READ_ONLY and non-READ_ONLY flag branches in protect.
  ASSERT_GE(H5C_log_write_protect_entry_msg(&cache, &parent, 3,
                                            H5C__READ_ONLY_FLAG, 0),
            0);
  ASSERT_GE(
      H5C_log_write_protect_entry_msg(&cache, &child, 3, H5C__NO_FLAGS_SET, -1),
      0);
  ASSERT_GE(H5C_log_write_resize_entry_msg(&cache, &child, 1024, 0), 0);
  ASSERT_GE(H5C_log_write_unpin_entry_msg(&cache, &parent, 0), 0);
  ASSERT_GE(H5C_log_write_destroy_fd_msg(&cache, &parent, &child, 0), 0);
  ASSERT_GE(H5C_log_write_unprotect_entry_msg(&cache, 0x2000, 3, 0x5, 0), 0);
  ASSERT_GE(H5C_log_write_set_cache_config_msg(&cache, &cfg, 0), 0);
  ASSERT_GE(H5C_log_write_remove_entry_msg(&cache, &child, 0), 0);
  ASSERT_GE(H5C_log_write_destroy_cache_msg(&cache), 0);

  ASSERT_GE(H5C_log_tear_down(&cache), 0);
  EXPECT_FALSE(log_info.enabled);
  EXPECT_EQ(log_info.cls, nullptr);
  EXPECT_EQ(log_info.udata, nullptr);

  const std::string log_text = ReadFileToString(log_file.path());
  constexpr auto kExpectedFragments = std::to_array<std::string_view>({
      "\"HDF5 metadata cache log messages\" : [",
      "\"action\":\"logging start\"",
      "\"action\":\"create\",\"returned\":0",
      "\"action\":\"evict\",\"returned\":0",
      "\"action\":\"expunge\",\"address\":0x1110,\"type_id\":1,\"returned\":0",
      "\"action\":\"flush\",\"returned\":0",
      "\"action\":\"insert\",\"address\":0x1230,\"type_id\":2,\"flags\":0x3,"
      "\"size\":64,\"returned\":0",
      "\"action\":\"dirty\",\"address\":0x1000,\"returned\":0",
      "\"action\":\"clean\",\"address\":0x1000,\"returned\":0",
      "\"action\":\"unserialized\",\"address\":0x1000,\"returned\":0",
      "\"action\":\"serialized\",\"address\":0x1000,\"returned\":0",
      "\"action\":\"move\",\"old_address\":0x1000,\"new_address\":0x3000,"
      "\"type_id\":4,\"returned\":0",
      "\"action\":\"pin\",\"address\":0x1000,\"returned\":0",
      "\"action\":\"create_fd\",\"parent_addr\":0x1000,\"child_addr\":0x2000,"
      "\"returned\":0",
      "\"action\":\"protect\",\"address\":0x1000,\"type_id\":3,"
      "\"readwrite\":\"READ\",\"size\":256,\"returned\":0",
      "\"action\":\"protect\",\"address\":0x2000,\"type_id\":3,"
      "\"readwrite\":\"WRITE\",\"size\":512,\"returned\":-1",
      "\"action\":\"resize\",\"address\":0x2000,\"new_size\":1024,\"returned\":"
      "0",
      "\"action\":\"unpin\",\"address\":0x1000,\"returned\":0",
      "\"action\":\"destroy_fd\",\"parent_addr\":0x1000,\"child_addr\":0x2000,"
      "\"returned\":0",
      "\"action\":\"unprotect\",\"address\":0x2000,\"id\":3,\"flags\":5,"
      "\"returned\":0",
      "\"action\":\"set_config\",\"returned\":0",
      "\"action\":\"remove\",\"address\":0x2000,\"returned\":0",
      "\"action\":\"destroy\"",
      "\"action\":\"logging stop\"",
  });
  for (std::string_view fragment : kExpectedFragments) {
    EXPECT_THAT(log_text, HasSubstr(std::string(fragment)));
  }
}

TEST(H5ClogJsonTest, SetUpWithMpiRankAndInvalidPathCleansUpState) {
  ASSERT_GE(H5open(), 0);
  ScopedErrorSilence silence;

  TempDir parent_dir("h5c_json_bad_setup");
  const std::string missing_path =
      (parent_dir.path() / "nonexistent_subdir" / "mdc.json").string();

  H5C_log_info_t serial_info{};
  EXPECT_LT(
      H5C__log_json_set_up(&serial_info, missing_path.c_str(), /*mpi_rank=*/-1),
      0);
  EXPECT_EQ(serial_info.udata, nullptr);
  EXPECT_EQ(serial_info.cls, nullptr);

  H5C_log_info_t rank_info{};
  // Parallel naming prepends "RANK_<rank>." before log_location, which fails
  // fopen because no "RANK_2." directory exists while covering rank naming
  // and failure cleanup paths without writing inside the workspace.
  EXPECT_LT(H5C__log_json_set_up(&rank_info, "/nonexistent_h5c_dir/mdc.json",
                                 /*mpi_rank=*/2),
            0);
  EXPECT_EQ(rank_info.udata, nullptr);
  EXPECT_EQ(rank_info.cls, nullptr);
}

TEST(H5ClogJsonTest, BrokenPipeWriteFailurePropagatesAcrossAllCallbacks) {
  ASSERT_GE(H5open(), 0);
  TempDir fifo_dir("h5c_json_fifo_err");
  const std::string fifo_path = fifo_dir.child("broken.fifo").string();
  ASSERT_EQ(mkfifo(fifo_path.c_str(), 0600), 0);

  int rfd = open(fifo_path.c_str(), O_RDONLY | O_NONBLOCK);
  ASSERT_GE(rfd, 0);
  absl::Cleanup close_rfd = [&rfd] {
    if (rfd >= 0) {
      close(rfd);
    }
  };

  struct sigaction ignore_sa{};
  ignore_sa.sa_handler = SIG_IGN;
  sigemptyset(&ignore_sa.sa_mask);
  struct sigaction prev_sa{};
  ASSERT_EQ(sigaction(SIGPIPE, &ignore_sa, &prev_sa), 0);
  absl::Cleanup restore_sigpipe = [&prev_sa] {
    sigaction(SIGPIPE, &prev_sa, nullptr);
  };

  H5C_log_info_t log_info{};
  H5C_t cache{};
  cache.log_info = &log_info;

  ASSERT_GE(H5C__log_json_set_up(&log_info, fifo_path.c_str(), /*mpi_rank=*/-1),
            0);
  log_info.enabled = true;

  absl::Cleanup teardown = [&cache, &log_info] {
    if (log_info.enabled) {
      // Ensure write_stop_log_msg failure inside H5C_stop_logging cannot bypass
      // tear_down_logging inside H5C_log_tear_down.
      log_info.logging = false;
      H5C_log_tear_down(&cache);
    }
  };

  // Close the read end so unbuffered fprintf fails with EPIPE.
  close(rfd);
  rfd = -1;

  H5C_cache_entry_t parent{};
  parent.addr = 0x100;
  parent.size = 64;
  H5C_cache_entry_t child{};
  child.addr = 0x200;
  child.size = 128;
  H5AC_cache_config_t cfg = H5AC__DEFAULT_CACHE_CONFIG;

  ScopedErrorSilence silence;
  auto expect_fail = [](herr_t rc) {
    EXPECT_LT(rc, 0);
    H5Eclear2(H5E_DEFAULT);
  };

  expect_fail(H5C_start_logging(&cache));
  expect_fail(H5C_log_write_create_cache_msg(&cache, 0));
  expect_fail(H5C_log_write_destroy_cache_msg(&cache));
  expect_fail(H5C_log_write_evict_cache_msg(&cache, 0));
  expect_fail(H5C_log_write_expunge_entry_msg(&cache, 0x100, 1, 0));
  expect_fail(H5C_log_write_flush_cache_msg(&cache, 0));
  expect_fail(H5C_log_write_insert_entry_msg(&cache, 0x100, 1, 0, 64, 0));
  expect_fail(H5C_log_write_mark_entry_dirty_msg(&cache, &parent, 0));
  expect_fail(H5C_log_write_mark_entry_clean_msg(&cache, &parent, 0));
  expect_fail(H5C_log_write_mark_unserialized_entry_msg(&cache, &parent, 0));
  expect_fail(H5C_log_write_mark_serialized_entry_msg(&cache, &parent, 0));
  expect_fail(H5C_log_write_move_entry_msg(&cache, 0x100, 0x200, 1, 0));
  expect_fail(H5C_log_write_pin_entry_msg(&cache, &parent, 0));
  expect_fail(H5C_log_write_create_fd_msg(&cache, &parent, &child, 0));
  expect_fail(H5C_log_write_protect_entry_msg(&cache, &parent, 1, 0, 0));
  expect_fail(H5C_log_write_resize_entry_msg(&cache, &parent, 128, 0));
  expect_fail(H5C_log_write_unpin_entry_msg(&cache, &parent, 0));
  expect_fail(H5C_log_write_destroy_fd_msg(&cache, &parent, &child, 0));
  expect_fail(H5C_log_write_unprotect_entry_msg(&cache, 0x100, 1, 0, 0));
  expect_fail(H5C_log_write_set_cache_config_msg(&cache, &cfg, 0));
  expect_fail(H5C_log_write_remove_entry_msg(&cache, &parent, 0));
  expect_fail(H5C_stop_logging(&cache));

  log_info.logging = false;
  EXPECT_GE(H5C_log_tear_down(&cache), 0);
}

}  // namespace
}  // namespace hdf5
