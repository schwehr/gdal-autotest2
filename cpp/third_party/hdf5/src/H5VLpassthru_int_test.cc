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
// Tests internal registration and unregistration of the pass-through VOL
// connector (third_party/hdf5/src/H5VLpassthru_int.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLpassthru_int.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5VL_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5VLpassthru_private.h"
#include "third_party/hdf5/src/H5VLpkg.h"
}  // extern "C"

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

TEST(H5VlPassthruIntTest, RegisterIsIdempotentWhenAlreadyRegistered) {
  ASSERT_GE(H5open(), 0);
  EXPECT_NE(H5VL_PASSTHRU_g, H5I_INVALID_HID);
  EXPECT_NE(H5VL_PASSTHRU_conn_g, nullptr);

  const hid_t initial_id = H5VL_PASSTHRU_g;
  const H5VL_connector_t* initial_conn = H5VL_PASSTHRU_conn_g;

  // Both H5VL_PASSTHRU_conn_g and H5VL_PASSTHRU_g are already set, so
  // re-invoking registration must be a no-op.
  EXPECT_GE(H5VL__passthru_register(), 0);
  EXPECT_EQ(H5VL_PASSTHRU_g, initial_id);
  EXPECT_EQ(H5VL_PASSTHRU_conn_g, initial_conn);
}

TEST(H5VlPassthruIntTest, ReregistersMissingIdForExistingConnector) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_lib = [] {
    H5close();
    H5open();
  };

  ASSERT_NE(H5VL_PASSTHRU_g, H5I_INVALID_HID);
  ASSERT_NE(H5VL_PASSTHRU_conn_g, nullptr);

  // Hold an extra reference on the underlying connector so releasing the ID does
  // not free H5VL_PASSTHRU_conn_g, exercising the conn_g != NULL / ID missing
  // path in H5VL__passthru_register().
  H5VL_conn_inc_rc(H5VL_PASSTHRU_conn_g);
  ASSERT_GE(H5I_dec_ref(H5VL_PASSTHRU_g), 0);
  H5VL_PASSTHRU_g = H5I_INVALID_HID;

  ASSERT_GE(H5VL__passthru_register(), 0);
  EXPECT_NE(H5VL_PASSTHRU_g, H5I_INVALID_HID);
  EXPECT_EQ(H5I_get_type(H5VL_PASSTHRU_g), H5I_VOL);

  // Balance the temporary reference acquired above.
  EXPECT_GE(H5VL_conn_dec_rc(H5VL_PASSTHRU_conn_g), 1);
}

TEST(H5VlPassthruIntTest, UnregisterAndFullReregisterLifecycle) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_lib = [] { H5open(); };

  // H5close() runs H5VL_term_package() -> H5VL__passthru_unregister().
  ASSERT_GE(H5close(), 0);
  EXPECT_EQ(H5VL_PASSTHRU_g, H5I_INVALID_HID);
  EXPECT_EQ(H5VL_PASSTHRU_conn_g, nullptr);

  // H5open() re-initializes H5VL and runs H5VL__passthru_register() with
  // H5VL_PASSTHRU_conn_g == NULL.
  ASSERT_GE(H5open(), 0);
  EXPECT_NE(H5VL_PASSTHRU_g, H5I_INVALID_HID);
  EXPECT_NE(H5VL_PASSTHRU_conn_g, nullptr);
  EXPECT_EQ(H5I_get_type(H5VL_PASSTHRU_g), H5I_VOL);
}

TEST(H5VlPassthruIntTest, RegisterFailsWhenVolIdTypeIsUnavailable) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_lib = [] {
    H5close();
    H5open();
  };

  ScopedErrorSilence silence;

  // Tear down the H5I_VOL ID type (which drops H5VL_PASSTHRU_conn_g's refcount
  // from 1 to 0 and frees it while H5VL_NATIVE_conn_g remains pinned by
  // H5VL_def_conn_s and the default FAPL), then clear the passthru and native
  // globals so H5close()/H5open() in cleanup restarts from NULL cleanly.
  ASSERT_EQ(H5I_dec_type_ref(H5I_VOL), 0);
  ASSERT_GE(H5VL__native_unregister(), 0);
  ASSERT_GE(H5VL__passthru_unregister(), 0);
  ASSERT_EQ(H5VL_PASSTHRU_conn_g, nullptr);

  // Creating H5VL_PASSTHRU_conn_g succeeds (rc=0), then H5I_register(H5I_VOL)
  // fails because the H5I_VOL ID group is destroyed.
  EXPECT_LT(H5VL__passthru_register(), 0);
  ASSERT_NE(H5VL_PASSTHRU_conn_g, nullptr);

  // Cleanly release the newly allocated unreferenced connector (0 -> 1 -> 0) and
  // clear globals so library re-initialization has zero leaks.
  H5VL_conn_inc_rc(H5VL_PASSTHRU_conn_g);
  EXPECT_EQ(H5VL_conn_dec_rc(H5VL_PASSTHRU_conn_g), 0);
  EXPECT_GE(H5VL__passthru_unregister(), 0);
}

}  // namespace
}  // namespace hdf5
