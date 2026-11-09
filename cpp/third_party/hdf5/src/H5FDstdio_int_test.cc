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
// Tests internal registration and unregistration routines for the stdio VFD
// (third_party/hdf5/src/H5FDstdio_int.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDstdio_int.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

extern "C" {
#define H5FD_FRIEND
#include "third_party/hdf5/src/H5FDpkg.h"
#include "third_party/hdf5/src/H5FDstdio_private.h"
#include "third_party/hdf5/src/H5Iprivate.h"
}  // extern "C"

namespace hdf5 {
namespace {

TEST(H5FdStdioIntTest, DefaultRegistrationAndClassDescriptor) {
  ASSERT_GE(H5open(), 0);

  EXPECT_NE(H5FD_STDIO_id_g, H5I_INVALID_HID);
  EXPECT_EQ(H5FD_STDIO, H5FD_STDIO_id_g);
  EXPECT_EQ(H5Iget_type(H5FD_STDIO_id_g), H5I_VFL);
  EXPECT_GT(H5FDis_driver_registered_by_name("stdio"), 0);
  EXPECT_GT(H5FDis_driver_registered_by_value(H5_VFD_STDIO), 0);

  EXPECT_EQ(H5FD_stdio_g.version, static_cast<unsigned>(H5FD_CLASS_VERSION));
  EXPECT_EQ(H5FD_stdio_g.value, H5_VFD_STDIO);
  EXPECT_STREQ(H5FD_stdio_g.name, "stdio");
  EXPECT_EQ(H5FD_stdio_g.fc_degree, H5F_CLOSE_WEAK);
  EXPECT_NE(H5FD_stdio_g.open, nullptr);
  EXPECT_NE(H5FD_stdio_g.close, nullptr);
  EXPECT_NE(H5FD_stdio_g.cmp, nullptr);
  EXPECT_NE(H5FD_stdio_g.query, nullptr);
  EXPECT_NE(H5FD_stdio_g.alloc, nullptr);
  EXPECT_NE(H5FD_stdio_g.get_eoa, nullptr);
  EXPECT_NE(H5FD_stdio_g.set_eoa, nullptr);
  EXPECT_NE(H5FD_stdio_g.get_eof, nullptr);
  EXPECT_NE(H5FD_stdio_g.get_handle, nullptr);
  EXPECT_NE(H5FD_stdio_g.read, nullptr);
  EXPECT_NE(H5FD_stdio_g.write, nullptr);
  EXPECT_NE(H5FD_stdio_g.flush, nullptr);
  EXPECT_NE(H5FD_stdio_g.truncate, nullptr);
  EXPECT_NE(H5FD_stdio_g.lock, nullptr);
  EXPECT_NE(H5FD_stdio_g.unlock, nullptr);
  EXPECT_NE(H5FD_stdio_g.del, nullptr);
}

TEST(H5FdStdioIntTest, RegisterIsIdempotentWhenAlreadyRegistered) {
  ASSERT_GE(H5open(), 0);
  const hid_t initial_id = H5FD_STDIO_id_g;
  ASSERT_NE(initial_id, H5I_INVALID_HID);

  EXPECT_GE(H5FD__stdio_register(), 0);
  EXPECT_EQ(H5FD_STDIO_id_g, initial_id);
}

TEST(H5FdStdioIntTest, UnregisterReregisterAndFailWhenVflTypeDestroyed) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_library = [] {
    H5close();
    H5open();
  };

  // 1. First package termination clears H5I_VFL members and invokes
  // H5FD__stdio_unregister(), resetting H5FD_STDIO_id_g to H5I_INVALID_HID.
  EXPECT_GT(H5FD_term_package(), 0);
  EXPECT_EQ(H5FD_STDIO_id_g, H5I_INVALID_HID);

  // 2. While H5I_VFL type still exists, H5FD__stdio_register() succeeds and
  // assigns a new valid VFL ID.
  ASSERT_GE(H5FD__stdio_register(), 0);
  EXPECT_NE(H5FD_STDIO_id_g, H5I_INVALID_HID);
  EXPECT_EQ(H5Iget_type(H5FD_STDIO_id_g), H5I_VFL);

  // 3. Direct H5FD__stdio_unregister() resets the global ID.
  EXPECT_GE(H5FD__stdio_unregister(), 0);
  EXPECT_EQ(H5FD_STDIO_id_g, H5I_INVALID_HID);

  // 4. Terminate H5P first to release default FAPL references on built-in VFD
  // IDs (such as H5FD_SEC2_id_g), clear H5I_VFL via H5FD_term_package(), and
  // destroy H5I_VFL directly while keeping H5FD's init flag true so
  // H5FD_register() does not re-initialize H5FD before failing.
  for (int i = 0; i < 16 && H5P_term_package() > 0; ++i) {
  }
  ASSERT_EQ(H5P_term_package(), 0);
  EXPECT_GT(H5FD_term_package(), 0);
  ASSERT_EQ(H5I_dec_type_ref(H5I_VFL), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FD__stdio_register(), 0);
    EXPECT_EQ(H5FD_term_package(), 0);
  }
  EXPECT_EQ(H5FD_STDIO_id_g, H5I_INVALID_HID);

  // 5. Re-opening after H5close() restores H5I_VFL and all built-in drivers.
  ASSERT_GE(H5close(), 0);
  ASSERT_GE(H5open(), 0);
  EXPECT_NE(H5FD_STDIO_id_g, H5I_INVALID_HID);
}

}  // namespace
}  // namespace hdf5
