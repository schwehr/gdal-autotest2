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
// Tests internal plugin management routines (third_party/hdf5/src/H5PLint.c).
// In google3, dlopen/dlsym/dlclose plugin loading is disabled by patch so
// H5PL_load reports not-found and H5PL__open unconditionally fails.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5PLint.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

extern "C" {
#define H5PL_FRIEND
#include "third_party/hdf5/src/H5PLpkg.h"
}  // extern "C"

namespace hdf5 {
namespace {

// Tests HDF5_PLUGIN_PRELOAD handling in forked child processes before the
// parent initializes H5PL, because H5PL_allow_plugins_g is sticky once false.
class PreInitPreloadEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    preload_no_plugin_ok_ = RunChild([]() {
      if (setenv(HDF5_PLUGIN_PRELOAD, H5PL_NO_PLUGIN, 1) != 0) return 1;
      // Initializing via public API sets H5PL_allow_plugins_g = false and
      // forces mask to 0; subsequent mask changes are silently ignored.
      if (H5PLset_loading_state(H5PL_ALL_PLUGIN) < 0) return 2;
      unsigned int mask = 123;
      if (H5PLget_loading_state(&mask) < 0) return 3;
      if (mask != 0) return 4;
      if (H5close() < 0) return 5;
      return 0;
    });

    preload_other_ok_ = RunChild([]() {
      if (setenv(HDF5_PLUGIN_PRELOAD, "some_other_value", 1) != 0) return 1;
      unsigned int mask = 0;
      if (H5PLget_loading_state(&mask) < 0) return 2;
      if (mask != H5PL_ALL_PLUGIN) return 3;
      if (H5PLset_loading_state(H5PL_FILTER_PLUGIN) < 0) return 4;
      if (H5PLget_loading_state(&mask) < 0) return 5;
      if (mask != H5PL_FILTER_PLUGIN) return 6;
      if (H5close() < 0) return 7;
      return 0;
    });
  }

  static bool preload_no_plugin_ok() { return preload_no_plugin_ok_; }
  static bool preload_other_ok() { return preload_other_ok_; }

 private:
  template <typename Fn>
  static bool RunChild(Fn&& fn) {
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
      const int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
      }
      std::exit(fn());
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }

  static inline bool preload_no_plugin_ok_ = false;
  static inline bool preload_other_ok_ = false;
};

[[maybe_unused]] const ::testing::Environment* const kPreInitPreloadEnv =
    ::testing::AddGlobalTestEnvironment(new PreInitPreloadEnvironment);

void EnsureH5PLReady() {
  ASSERT_GE(H5open(), 0);
  unsigned int num_paths = 0;
  ASSERT_GE(H5PLsize(&num_paths), 0);
}

herr_t DummyIterateOp(H5PL_type_t /*plugin_type*/, const void* /*plugin_info*/,
                      void* op_data) {
  if (op_data) {
    ++(*static_cast<int*>(op_data));
  }
  return H5_ITER_CONT;
}

TEST(H5PLintTest, PreInitPreloadEnvironmentDisablesOrAllowsPlugins) {
  EXPECT_TRUE(PreInitPreloadEnvironment::preload_no_plugin_ok());
  EXPECT_TRUE(PreInitPreloadEnvironment::preload_other_ok());
}

TEST(H5PLintTest, GetAndSetPluginControlMask) {
  EnsureH5PLReady();
  unsigned int saved_mask = 0;
  ASSERT_GE(H5PL__get_plugin_control_mask(&saved_mask), 0);
  absl::Cleanup restore = [&] { H5PL__set_plugin_control_mask(saved_mask); };

  ASSERT_GE(H5PL__set_plugin_control_mask(H5PL_VOL_PLUGIN), 0);
  unsigned int current = 0;
  ASSERT_GE(H5PL__get_plugin_control_mask(&current), 0);
  EXPECT_EQ(current, H5PL_VOL_PLUGIN);

  ASSERT_GE(H5PL__set_plugin_control_mask(0u), 0);
  ASSERT_GE(H5PL__get_plugin_control_mask(&current), 0);
  EXPECT_EQ(current, 0u);
}

TEST(H5PLintTest, TermPackageCyclesCacheThenResetsInitFlag) {
  EnsureH5PLReady();

  const char* prev_preload = std::getenv(HDF5_PLUGIN_PRELOAD);
  const bool had_preload = (prev_preload != nullptr);
  std::string saved_preload = prev_preload ? prev_preload : "";
  absl::Cleanup restore_env = [&] {
    if (had_preload) {
      setenv(HDF5_PLUGIN_PRELOAD, saved_preload.c_str(), 1);
    } else {
      unsetenv(HDF5_PLUGIN_PRELOAD);
    }
    EnsureH5PLReady();
  };

  // First H5PL_term_package() closes the cache and returns 1 (work done),
  // leaving H5_PKG_INIT_VAR true for a second pass.
  EXPECT_EQ(H5PL_term_package(), 1);
  // Second pass sees cache already closed, returns 0, and resets init flag.
  EXPECT_EQ(H5PL_term_package(), 0);
  // Third pass sees H5_PKG_INIT_VAR == false and immediately returns 0.
  EXPECT_EQ(H5PL_term_package(), 0);

  // Re-initialize with HDF5_PLUGIN_PRELOAD set to a non-"::" value.
  ASSERT_EQ(setenv(HDF5_PLUGIN_PRELOAD, "enabled_value", 1), 0);
  unsigned int count = 0;
  ASSERT_GE(H5PLsize(&count), 0);
  unsigned int mask = 0;
  ASSERT_GE(H5PL__get_plugin_control_mask(&mask), 0);
  EXPECT_EQ(mask, H5PL_ALL_PLUGIN);

  // Also verify H5close() / H5open() clean teardown and re-init.
  ASSERT_GE(H5close(), 0);
  EnsureH5PLReady();
}

TEST(H5PLintTest, LoadReportsDisabledAndNotFoundAndInvalidType) {
  EnsureH5PLReady();
  unsigned int saved_mask = 0;
  ASSERT_GE(H5PL__get_plugin_control_mask(&saved_mask), 0);
  absl::Cleanup restore = [&] { H5PL__set_plugin_control_mask(saved_mask); };

  ScopedErrorSilence silence;

  H5PL_key_t filter_key{};
  filter_key.id = 9999;
  H5PL_key_t vol_key{};
  vol_key.vol.kind = H5VL_GET_CONNECTOR_BY_NAME;
  vol_key.vol.u.name = "missing_vol";
  H5PL_key_t vfd_key{};
  vfd_key.vfd.kind = H5FD_GET_DRIVER_BY_NAME;
  vfd_key.vfd.u.name = "missing_vfd";

  // 1. With all plugins disabled, each valid type returns CANTLOAD (nullptr).
  ASSERT_GE(H5PL__set_plugin_control_mask(0u), 0);
  EXPECT_EQ(H5PL_load(H5PL_TYPE_FILTER, &filter_key), nullptr);
  EXPECT_EQ(H5PL_load(H5PL_TYPE_VOL, &vol_key), nullptr);
  EXPECT_EQ(H5PL_load(H5PL_TYPE_VFD, &vfd_key), nullptr);

  // 2. With all plugins enabled, plugin search is compiled out in google3 so
  // all valid types hit NOTFOUND (nullptr).
  ASSERT_GE(H5PL__set_plugin_control_mask(H5PL_ALL_PLUGIN), 0);
  EXPECT_EQ(H5PL_load(H5PL_TYPE_FILTER, &filter_key), nullptr);
  EXPECT_EQ(H5PL_load(H5PL_TYPE_VOL, &vol_key), nullptr);
  EXPECT_EQ(H5PL_load(H5PL_TYPE_VFD, &vfd_key), nullptr);

  // 3. Invalid plugin types hit the default error branch.
  EXPECT_EQ(H5PL_load(H5PL_TYPE_ERROR, &filter_key), nullptr);
  EXPECT_EQ(H5PL_load(H5PL_TYPE_NONE, &filter_key), nullptr);
  EXPECT_EQ(H5PL_load(static_cast<H5PL_type_t>(99), &filter_key), nullptr);

  // 4. Exercise public library callers of H5PL_load.
  EXPECT_EQ(H5Zfilter_avail(9999), 0);
  EXPECT_EQ(H5VLis_connector_registered_by_name("missing_vol"), 0);
}

TEST(H5PLintTest, OpenFailsInGoogle3AndCloseSucceeds) {
  EnsureH5PLReady();
  ScopedErrorSilence silence;

  bool success = true;
  H5PL_type_t plugin_type = H5PL_TYPE_FILTER;
  const void* plugin_info = &success;
  EXPECT_LT(H5PL__open("/nonexistent/libfoo.so", H5PL_TYPE_NONE, nullptr,
                       &success, &plugin_type, &plugin_info),
            0);
  EXPECT_FALSE(success);
  EXPECT_EQ(plugin_info, nullptr);

  H5PL_key_t key{};
  key.id = 32000;
  success = true;
  plugin_info = &success;
  EXPECT_LT(H5PL__open("/nonexistent/libbar.so", H5PL_TYPE_FILTER, &key,
                       &success, nullptr, &plugin_info),
            0);
  EXPECT_FALSE(success);
  EXPECT_EQ(plugin_info, nullptr);

  EXPECT_GE(H5PL__close(nullptr), 0);
}

TEST(H5PLintTest, IterateProcessesEmptyAndPluginDirs) {
  EnsureH5PLReady();
  unsigned int saved_count = 0;
  ASSERT_GE(H5PLsize(&saved_count), 0);
  TempDir temp_dir("h5plint_iter");
  absl::Cleanup restore_paths = [&] {
    unsigned int cur = 0;
    while (H5PLsize(&cur) >= 0 && cur > saved_count) {
      H5PLremove(0);
    }
  };

  // Prepend empty directory so H5PL_iterate traverses it cleanly.
  ASSERT_GE(H5PLprepend(temp_dir.c_str()), 0);
  int calls = 0;
  EXPECT_EQ(H5PL_iterate(H5PL_ITER_TYPE_ALL, DummyIterateOp, &calls),
            H5_ITER_CONT);
  EXPECT_EQ(calls, 0);

  // Create a dummy shared library file; H5PL__open fails and aborts iteration.
  {
    std::ofstream dummy(temp_dir.child("libdummy.so"));
    dummy << "not_an_elf";
  }
  ScopedErrorSilence silence;
  EXPECT_EQ(H5PL_iterate(H5PL_ITER_TYPE_ALL, DummyIterateOp, &calls),
            H5_ITER_ERROR);
  EXPECT_EQ(calls, 0);
}

}  // namespace
}  // namespace hdf5
