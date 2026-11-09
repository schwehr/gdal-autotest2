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
// Tests internal VOL testing helper routines (third_party/hdf5/src/H5VLtest.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLtest.c

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5VL_FRIEND
#define H5VL_TESTING
#include "third_party/hdf5/src/H5VLnative.h"
#include "third_party/hdf5/src/H5VLpassthru.h"
#include "third_party/hdf5/src/H5VLpkg.h"
}  // extern "C"

#include <cstdlib>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

herr_t DummyGroupClose(void* grp, hid_t /*dxpl_id*/, void** /*req*/) {
  delete static_cast<int*>(grp);
  return 0;
}

H5VL_class_t MakeDummyConnectorClass() {
  H5VL_class_t cls{};
  cls.version = H5VL_VERSION;
  cls.value = 505;
  cls.name = "google_h5vl_test_dummy_vol";
  cls.conn_version = 1;
  cls.group_cls.close = DummyGroupClose;
  return cls;
}

TEST(H5VlTestTest, ReparsesDefaultVolConnectorEnvVar) {
  ASSERT_GE(H5open(), 0);

  const char* prev_env = std::getenv(HDF5_VOL_CONNECTOR);
  const bool had_env = (prev_env != nullptr);
  const std::string saved_env = prev_env ? prev_env : "";

  absl::Cleanup restore_env = [&] {
    if (had_env) {
      setenv(HDF5_VOL_CONNECTOR, saved_env.c_str(), 1);
    } else {
      unsetenv(HDF5_VOL_CONNECTOR);
    }
    ScopedErrorSilence silence;
    H5VL__reparse_def_vol_conn_variable_test();
  };

  // 1. Unset environment variable selects the built-in native connector.
  ASSERT_EQ(unsetenv(HDF5_VOL_CONNECTOR), 0);
  ASSERT_GE(H5VL__reparse_def_vol_conn_variable_test(), 0);
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    hid_t vol_id = H5I_INVALID_HID;
    ASSERT_GE(H5Pget_vol_id(fapl.get(), &vol_id), 0);
    absl::Cleanup close_vol = [vol_id] { H5VLclose(vol_id); };
    EXPECT_GT(H5VL__is_native_connector_test(vol_id), 0);
  }

  // 2. Explicit pass_through connector with empty native under_info.
  ASSERT_EQ(
      setenv(HDF5_VOL_CONNECTOR, "pass_through under_vol=0;under_info={}", 1),
      0);
  ASSERT_GE(H5VL__reparse_def_vol_conn_variable_test(), 0);
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    hid_t vol_id = H5I_INVALID_HID;
    ASSERT_GE(H5Pget_vol_id(fapl.get(), &vol_id), 0);
    absl::Cleanup close_vol = [vol_id] { H5VLclose(vol_id); };
    EXPECT_EQ(H5VL__is_native_connector_test(vol_id), 0);
    void* info = nullptr;
    ASSERT_GE(H5Pget_vol_info(fapl.get(), &info), 0);
    ASSERT_NE(info, nullptr);
    EXPECT_GE(H5VLfree_connector_info(vol_id, info), 0);
  }

  // 3. Explicit "native" restores native default.
  ASSERT_EQ(setenv(HDF5_VOL_CONNECTOR, "native", 1), 0);
  ASSERT_GE(H5VL__reparse_def_vol_conn_variable_test(), 0);
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    hid_t vol_id = H5I_INVALID_HID;
    ASSERT_GE(H5Pget_vol_id(fapl.get(), &vol_id), 0);
    absl::Cleanup close_vol = [vol_id] { H5VLclose(vol_id); };
    EXPECT_GT(H5VL__is_native_connector_test(vol_id), 0);
  }

  // 4. Non-existent VOL connector triggers the error return path.
  ASSERT_EQ(setenv(HDF5_VOL_CONNECTOR, "nonexistent_vol_connector", 1), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5VL__reparse_def_vol_conn_variable_test(), 0);
  }
}

TEST(H5VlTestTest, IdentifiesNativePassthruAndInvalidConnectors) {
  ASSERT_GE(H5open(), 0);

  EXPECT_GT(H5VL__is_native_connector_test(H5VL_NATIVE), 0);
  EXPECT_EQ(H5VL__is_native_connector_test(H5VL_PASSTHRU), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5VL__is_native_connector_test(H5I_INVALID_HID), 0);
  EXPECT_LT(H5VL__is_native_connector_test(H5P_DEFAULT), 0);
}

TEST(H5VlTestTest, RegistersObjectUsingVolIdAndHandlesErrors) {
  ASSERT_GE(H5open(), 0);

  const H5VL_class_t dummy_cls = MakeDummyConnectorClass();
  const hid_t vol_id = H5VLregister_connector(&dummy_cls, H5P_DEFAULT);
  ASSERT_NE(vol_id, H5I_INVALID_HID);
  absl::Cleanup cleanup_vol = [vol_id] {
    H5VLunregister_connector(vol_id);
  };

  int* raw_obj = new int(123);
  hid_t gid = H5VL__register_using_vol_id_test(H5I_GROUP, raw_obj, vol_id);
  if (gid == H5I_INVALID_HID) {
    delete raw_obj;
    ADD_FAILURE() << "H5VL__register_using_vol_id_test failed on valid input";
    return;
  }
  ScopedGroup group(gid);
  EXPECT_EQ(H5Iget_type(group.get()), H5I_GROUP);

  ScopedErrorSilence silence;
  int unused_obj = 0;
  EXPECT_EQ(
      H5VL__register_using_vol_id_test(H5I_GROUP, &unused_obj, H5I_INVALID_HID),
      H5I_INVALID_HID);
  EXPECT_EQ(
      H5VL__register_using_vol_id_test(H5I_BADID, &unused_obj, vol_id),
      H5I_INVALID_HID);
}

}  // namespace
}  // namespace hdf5
