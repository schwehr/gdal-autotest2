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
// Tests metadata cache trace logging setup, teardown, public MDC trace config
// controls, rank-suffixed trace files, message formatting across all trace
// callbacks, and write-error propagation (third_party/hdf5/src/H5Clog_trace.c).
//
// Build caveat: //third_party/hdf5 compiles with -DNDEBUG and without
// H5_HAVE_PARALLEL. Internal headers are wrapped with matching NDEBUG so
// C++20 tests observe the exact H5C_t / H5C_cache_entry_t ABI layout.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Clog_trace.c

#ifndef NDEBUG
#define H5CLOG_TRACE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5C_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5Clog.h"
#include "third_party/hdf5/src/H5Cpkg.h"
}  // extern "C"

#ifdef H5CLOG_TRACE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5CLOG_TRACE_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
using ::testing::StartsWith;

std::string ReadFileToString(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return "";
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

TEST(H5ClogTraceTest, PublicMdcConfigOpensAndClosesTraceFile) {
  // 1. Create an in-memory HDF5 file and enable trace logging via MDC config.
  TempFile trace_file("mdc_trace_public.log");
  ScopedFile file = CreateInMemoryFile("trace_public.h5");
  ASSERT_TRUE(file.valid());

  H5AC_cache_config_t cfg{};
  cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
  ASSERT_GE(H5Fget_mdc_config(file.get(), &cfg), 0);

  ASSERT_LT(trace_file.string().size(), sizeof(cfg.trace_file_name));
  cfg.open_trace_file = true;
  cfg.close_trace_file = false;
  std::snprintf(cfg.trace_file_name, sizeof(cfg.trace_file_name), "%s",
                trace_file.c_str());
  ASSERT_GE(H5Fset_mdc_config(file.get(), &cfg), 0);

  // 2. Drive metadata cache activity and flush.
  {
    ScopedGroup grp(
        H5Gcreate2(file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
  }
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  // 3. Close the trace file cleanly via H5Fset_mdc_config.
  ASSERT_GE(H5Fget_mdc_config(file.get(), &cfg), 0);
  cfg.open_trace_file = false;
  cfg.close_trace_file = true;
  ASSERT_GE(H5Fset_mdc_config(file.get(), &cfg), 0);

  // 4. Re-open another trace path and verify file close also tears down trace.
  TempFile trace_close_file("mdc_trace_close.log");
  ASSERT_LT(trace_close_file.string().size(), sizeof(cfg.trace_file_name));
  cfg.open_trace_file = true;
  cfg.close_trace_file = false;
  std::snprintf(cfg.trace_file_name, sizeof(cfg.trace_file_name), "%s",
                trace_close_file.c_str());
  ASSERT_GE(H5Fset_mdc_config(file.get(), &cfg), 0);
  file.reset();

  const std::string trace_text = ReadFileToString(trace_file.path());
  EXPECT_THAT(trace_text,
              StartsWith("### HDF5 metadata cache trace file version 1 ###\n"));
  EXPECT_THAT(trace_text, HasSubstr("H5AC_set_cache_auto_resize_config "));
  EXPECT_THAT(trace_text, HasSubstr("H5AC_protect "));
  EXPECT_THAT(trace_text, HasSubstr("H5AC_unprotect "));
  EXPECT_THAT(trace_text, HasSubstr("H5AC_flush "));

  const std::string close_trace_text =
      ReadFileToString(trace_close_file.path());
  EXPECT_THAT(close_trace_text,
              StartsWith("### HDF5 metadata cache trace file version 1 ###\n"));
}

TEST(H5ClogTraceTest, AllTraceCallbacksWithMpiRankSuffixEmitExpectedLines) {
  ASSERT_GE(H5open(), 0);

  TempDir dir("h5c_trace_rank");
  const std::filesystem::path base_path = dir.child("mdc.trace");
  const std::filesystem::path ranked_path = dir.child("mdc.trace.5");

  H5C_log_info_t log_info{};
  H5C_t cache{};
  cache.log_info = &log_info;

  // Non-negative mpi_rank appends ".<rank>" to the log path.
  ASSERT_GE(H5C__log_trace_set_up(&log_info, base_path.c_str(), /*mpi_rank=*/5),
            0);
  ASSERT_NE(log_info.cls, nullptr);
  ASSERT_NE(log_info.udata, nullptr);
  EXPECT_STREQ(log_info.cls->name, "trace");
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
  std::snprintf(cfg.trace_file_name, sizeof(cfg.trace_file_name), "sample.trc");

  // Trace leaves start/stop/create/destroy/evict callbacks NULL; verify no-ops
  // and all 17 implemented trace message writers.
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
  ASSERT_GE(H5C_log_write_protect_entry_msg(&cache, &parent, 3,
                                            H5C__READ_ONLY_FLAG, 0),
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

  const std::string trace_text = ReadFileToString(ranked_path);
  constexpr auto kExpectedLines = std::to_array<std::string_view>({
      "### HDF5 metadata cache trace file version 1 ###\n",
      "H5AC_expunge_entry 0x1110 1 0\n",
      "H5AC_flush 0\n",
      "H5AC_insert_entry 0x1230 2 0x3 64 0\n",
      "H5AC_mark_entry_dirty 0x1000 0\n",
      "H5AC_mark_entry_clean 0x1000 0\n",
      "H5AC_mark_entry_unserialized 0x1000 0\n",
      "H5AC_mark_entry_serialized 0x1000 0\n",
      "H5AC_move_entry 0x1000 0x3000 4 0\n",
      "H5AC_pin_protected_entry 0x1000 0\n",
      "H5AC_create_flush_dependency 0x1000 0x2000 0\n",
      "H5AC_protect 0x1000 3 ",
      "H5AC_resize_entry 0x2000 1024 0\n",
      "H5AC_unpin_entry 0x1000 0\n",
      "H5AC_destroy_flush_dependency 0x1000 0x2000 0\n",
      "H5AC_unprotect 0x2000 3 0x5 0\n",
      "H5AC_set_cache_auto_resize_config 1 ",
      "\"sample.trc\"",
      "H5AC_remove_entry 0x2000 0\n",
  });
  for (std::string_view line : kExpectedLines) {
    EXPECT_THAT(trace_text, HasSubstr(std::string(line)));
  }
}

TEST(H5ClogTraceTest, SetUpInvalidPathCleansUpState) {
  ASSERT_GE(H5open(), 0);
  TempDir dir("h5c_trace_bad");
  const std::string missing_path =
      (dir.path() / "nonexistent_subdir" / "mdc.trace").string();

  ScopedErrorSilence silence;
  H5C_log_info_t log_info{};
  EXPECT_LT(H5C__log_trace_set_up(&log_info, missing_path.c_str(), -1), 0);
  EXPECT_EQ(log_info.udata, nullptr);
  EXPECT_EQ(log_info.cls, nullptr);
}

TEST(H5ClogTraceTest, BrokenPipeWriteFailurePropagatesAcrossAllCallbacks) {
  ASSERT_GE(H5open(), 0);
  TempDir fifo_dir("h5c_trace_fifo_err");
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

  // Set up succeeds while read end holds pipe open for initial header write.
  ASSERT_GE(
      H5C__log_trace_set_up(&log_info, fifo_path.c_str(), /*mpi_rank=*/-1), 0);
  log_info.enabled = true;
  ASSERT_GE(H5C_start_logging(&cache), 0);

  absl::Cleanup teardown = [&cache, &log_info] {
    if (log_info.enabled) {
      H5C_log_tear_down(&cache);
    }
  };

  // Close read end so subsequent fprintf writes fail with EPIPE.
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

  EXPECT_GE(H5C_log_tear_down(&cache), 0);
}

}  // namespace
}  // namespace hdf5
