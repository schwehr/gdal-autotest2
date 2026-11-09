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
// Tests internal multi-VFD registration and unregistration helpers
// (third_party/hdf5/src/H5FDmulti_int.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDmulti_int.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5FD_FRIEND
#include "third_party/hdf5/src/H5FDpkg.h"
#include "third_party/hdf5/src/H5FDmulti_private.h"
#include "third_party/hdf5/src/H5Iprivate.h"
}  // extern "C"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

TEST(H5FDmultiIntTest, RegisterIsIdempotentWhenAlreadyRegistered) {
  ASSERT_GE(H5open(), 0);
  // Touching H5FD_MULTI triggers H5_init_library / H5FD initialization.
  const hid_t initial_id = H5FD_MULTI;
  ASSERT_NE(initial_id, H5I_INVALID_HID);
  EXPECT_EQ(H5I_get_type(H5FD_MULTI_id_g), H5I_VFL);

  // Calling H5FD__multi_register() again takes the already-registered fast
  // path without allocating a duplicate VFL ID.
  EXPECT_GE(H5FD__multi_register(), 0);
  EXPECT_EQ(H5FD_MULTI_id_g, initial_id);
}

TEST(H5FDmultiIntTest, UnregisterResetsGlobalIdAndReRegistersCleanly) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_library = [] {
    H5Eclear2(H5E_DEFAULT);
    H5close();
    H5open();
  };

  // Ensure the VFD package has initialized the multi VFD ID.
  ASSERT_NE(H5FD_MULTI, H5I_INVALID_HID);
  const hid_t prev_id = H5FD_MULTI_id_g;
  ASSERT_EQ(H5I_get_type(prev_id), H5I_VFL);

  // Unregister resets H5FD_MULTI_id_g to H5I_INVALID_HID; decrement the
  // detached ID reference so no stale H5FD_class_t copy stays live in H5I_VFL.
  ASSERT_GE(H5FD__multi_unregister(), 0);
  EXPECT_EQ(H5FD_MULTI_id_g, H5I_INVALID_HID);
  EXPECT_GE(H5I_dec_ref(prev_id), 0);

  // Re-registering registers H5FD_multi_g as a fresh VFL ID.
  ASSERT_GE(H5FD__multi_register(), 0);
  EXPECT_NE(H5FD_MULTI_id_g, H5I_INVALID_HID);
  EXPECT_EQ(H5I_get_type(H5FD_MULTI_id_g), H5I_VFL);

  // Library termination also calls H5FD__multi_unregister() and H5open()
  // calls H5FD__multi_register().
  ASSERT_GE(H5close(), 0);
  EXPECT_EQ(H5FD_MULTI_id_g, H5I_INVALID_HID);
  ASSERT_GE(H5open(), 0);
  EXPECT_NE(H5FD_MULTI, H5I_INVALID_HID);
}

TEST(H5FDmultiIntTest, RegisterFailsWhenVflIdGroupIsUnavailable) {
  ASSERT_GE(H5open(), 0);
  // Ensure VFD package has initialized the multi VFD ID in this process before
  // forking so tearing down H5I_VFL never corrupts sibling tests' library state.
  ASSERT_NE(H5FD_MULTI, H5I_INVALID_HID);

  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    const int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    if (H5FD__multi_unregister() < 0) std::exit(2);
    if (H5I_clear_type(H5I_VFL, true, false) < 0) std::exit(3);
    if (H5I_dec_type_ref(H5I_VFL) < 0) std::exit(4);

    int exit_code = 0;
    {
      ScopedErrorSilence silence;
      const herr_t reg_rc = H5FD__multi_register();
      const hid_t post_id = H5FD_MULTI_id_g;
      exit_code = (reg_rc < 0 && post_id == H5I_INVALID_HID) ? 0 : 5;
    }
    std::exit(exit_code);
  }

  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

}  // namespace
}  // namespace hdf5
