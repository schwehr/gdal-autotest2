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
// Tests the experimental VOL Data Mapping (H5M) package lifecycle, ID group
// registration/cleanup, and close callback dispatch
// (third_party/hdf5/src/H5M.c). Note: H5_HAVE_MAP_API is disabled in google3's
// H5pubconf.h, so the public H5M* API functions are compiled out and only
// package initialization, termination, close callback, and header definitions
// are active.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5M.c

// Match :hdf5's compiled -DNDEBUG struct layout across internal headers.
#ifndef NDEBUG
#define HDF5_TEST_RESTORE_UNDEF_NDEBUG_ 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Mprivate.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"

#ifdef HDF5_TEST_RESTORE_UNDEF_NDEBUG_
#undef NDEBUG
#undef HDF5_TEST_RESTORE_UNDEF_NDEBUG_
#endif

#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

constexpr H5VL_class_value_t kTestVolValueBase = 480;

struct MockMapObject {
  int optional_calls = 0;
  int last_op_type = -1;
  void* last_args = nullptr;
  void** last_req = nullptr;
  bool has_recorded_args = false;
  bool fail_optional = false;
  herr_t optional_rc = -1;
};

herr_t MockMapOptional(void* obj, H5VL_optional_args_t* args, hid_t /*dxpl_id*/,
                       void** req) {
  auto* mock = static_cast<MockMapObject*>(obj);
  if (mock == nullptr) {
    return -1;
  }
  ++mock->optional_calls;
  if (args != nullptr) {
    mock->last_op_type = args->op_type;
    mock->last_args = args->args;
    mock->has_recorded_args = true;
  }
  mock->last_req = req;
  return mock->fail_optional ? mock->optional_rc : 0;
}

void RegisterTestVolConnector(const char* name, H5VL_class_value_t value,
                              hid_t* vol_id, H5VL_connector_t** connector) {
  H5VL_class_t cls;
  std::memset(&cls, 0, sizeof(cls));
  cls.version = H5VL_VERSION;
  cls.value = value;
  cls.name = name;
  cls.conn_version = 1;
  cls.cap_flags = H5VL_CAP_FLAG_NONE;
  cls.optional = MockMapOptional;

  *vol_id = H5VLregister_connector(&cls, H5P_DEFAULT);
  if (*vol_id == H5I_INVALID_HID) {
    ADD_FAILURE() << "H5VLregister_connector failed for " << name;
    return;
  }

  *connector =
      static_cast<H5VL_connector_t*>(H5I_object_verify(*vol_id, H5I_VOL));
  if (*connector == nullptr) {
    ADD_FAILURE() << "H5I_object_verify failed for VOL connector " << name;
  }
}

TEST(H5MTest, PublicHeaderConstantsEnumsAndPropertyListsExist) {
  ASSERT_GE(H5open(), 0);

  // Verify H5M VOL optional operation codes and argument structs from
  // H5Mpublic.h.
  EXPECT_EQ(H5VL_MAP_CREATE, 1);
  EXPECT_EQ(H5VL_MAP_OPEN, 2);
  EXPECT_EQ(H5VL_MAP_GET_VAL, 3);
  EXPECT_EQ(H5VL_MAP_EXISTS, 4);
  EXPECT_EQ(H5VL_MAP_PUT, 5);
  EXPECT_EQ(H5VL_MAP_GET, 6);
  EXPECT_EQ(H5VL_MAP_SPECIFIC, 7);
  EXPECT_EQ(H5VL_MAP_OPTIONAL, 8);
  EXPECT_EQ(H5VL_MAP_CLOSE, 9);

  EXPECT_EQ(static_cast<int>(H5VL_MAP_GET_MAPL), 0);
  EXPECT_EQ(static_cast<int>(H5VL_MAP_GET_MCPL), 1);
  EXPECT_EQ(static_cast<int>(H5VL_MAP_GET_KEY_TYPE), 2);
  EXPECT_EQ(static_cast<int>(H5VL_MAP_GET_VAL_TYPE), 3);
  EXPECT_EQ(static_cast<int>(H5VL_MAP_GET_COUNT), 4);

  EXPECT_EQ(static_cast<int>(H5VL_MAP_ITER), 0);
  EXPECT_EQ(static_cast<int>(H5VL_MAP_DELETE), 1);

  H5VL_map_args_t map_args{};
  map_args.get.get_type = H5VL_MAP_GET_COUNT;
  map_args.get.args.get_count.count = 42;
  EXPECT_EQ(map_args.get.args.get_count.count, 42);

  // H5I_MAP is a library ID type, so public H5I mutation routines reject it.
  int dummy_obj = 0;
  hsize_t count = 99;
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Iregister(H5I_MAP, &dummy_obj), H5I_INVALID_HID);
    EXPECT_LT(H5Inmembers(H5I_MAP, &count), 0);
    EXPECT_LT(H5Iclear_type(H5I_MAP, false), 0);
    EXPECT_LT(H5Idestroy_type(H5I_MAP), 0);
  }

  // Map property list classes and access properties remain registered.
  ScopedPlist mcpl(H5Pcreate(H5P_MAP_CREATE));
  ASSERT_TRUE(mcpl.valid());

  ScopedPlist mapl(H5Pcreate(H5P_MAP_ACCESS));
  ASSERT_TRUE(mapl.valid());
  EXPECT_GT(H5Pexist(mapl.get(), H5M_ACS_KEY_PREFETCH_SIZE_NAME), 0);
  EXPECT_GT(H5Pexist(mapl.get(), H5M_ACS_KEY_ALLOC_SIZE_NAME), 0);
}

TEST(H5MTest, InitAndTermPackageLifecycleRestoresCleanState) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_h5m = [] { H5M_init(); };

  // Re-initializing an active H5M package is a valid no-op.
  EXPECT_GE(H5M_init(), 0);
  ASSERT_EQ(H5I_nmembers(H5I_MAP), 0);

  // First top-term call clears the initialized-top flag cleanly when empty.
  EXPECT_EQ(H5M_top_term_package(), 0);
  // Second top-term call observes the top-closed flag and skips clearing.
  EXPECT_EQ(H5M_top_term_package(), 0);

  // Add a synthetic ref to H5I_MAP so H5M_term_package exercises both the
  // non-zero remaining-ref path (returns 1) and final teardown path (returns
  // 0).
  const H5I_class_t extra_ref_cls = {H5I_MAP, 0, 0, nullptr};
  ASSERT_GE(H5I_register_type(&extra_ref_cls), 0);
  EXPECT_EQ(H5M_term_package(), 1);
  EXPECT_EQ(H5M_term_package(), 0);

  // Subsequent termination call observes H5_PKG_INIT_VAR == false.
  EXPECT_EQ(H5M_term_package(), 0);

  // Re-initialize triggers H5M__init_package() and restores H5I_MAP.
  EXPECT_GE(H5M_init(), 0);
  EXPECT_EQ(H5I_nmembers(H5I_MAP), 0);
}

TEST(H5MTest, RegisterAndDecRefInvokesCloseCallback) {
  ASSERT_GE(H5open(), 0);

  constexpr char kVolName[] = "google_h5m_close_cb_vol";
  hid_t vol_id = H5I_INVALID_HID;
  H5VL_connector_t* connector = nullptr;
  absl::Cleanup cleanup_vol = [&vol_id] {
    if (vol_id != H5I_INVALID_HID) {
      H5VLunregister_connector(vol_id);
      vol_id = H5I_INVALID_HID;
    }
  };
  RegisterTestVolConnector(kVolName, kTestVolValueBase, &vol_id, &connector);
  ASSERT_NE(connector, nullptr);

  MockMapObject mock_map;
  hid_t map_id = H5VL_register(H5I_MAP, &mock_map, connector, /*app_ref=*/true);
  ASSERT_NE(map_id, H5I_INVALID_HID);
  absl::Cleanup cleanup_map = [&map_id, &mock_map] {
    if (map_id != H5I_INVALID_HID && H5Iis_valid(map_id) > 0) {
      mock_map.fail_optional = false;
      H5Idec_ref(map_id);
      map_id = H5I_INVALID_HID;
    }
  };

  EXPECT_EQ(H5Iget_type(map_id), H5I_MAP);
  EXPECT_GT(H5Iis_valid(map_id), 0);
  EXPECT_EQ(H5I_nmembers(H5I_MAP), 1);
  EXPECT_EQ(H5Iget_ref(map_id), 1);
  EXPECT_EQ(H5VLobject(map_id), &mock_map);

  std::array<char, 64> conn_name{};
  const ssize_t name_len =
      H5VLget_connector_name(map_id, conn_name.data(), conn_name.size());
  ASSERT_GT(name_len, 0);
  EXPECT_EQ(std::string_view(conn_name.data(), static_cast<size_t>(name_len)),
            kVolName);

  // Incrementing refcount defers H5M__close_cb until the final decrement.
  EXPECT_EQ(H5Iinc_ref(map_id), 2);
  EXPECT_EQ(H5Idec_ref(map_id), 1);
  EXPECT_EQ(mock_map.optional_calls, 0);

  // Final decrement triggers H5M__close_cb -> H5VL_optional(H5VL_MAP_CLOSE).
  EXPECT_EQ(H5Idec_ref(map_id), 0);
  map_id = H5I_INVALID_HID;

  EXPECT_EQ(mock_map.optional_calls, 1);
  EXPECT_TRUE(mock_map.has_recorded_args);
  EXPECT_EQ(mock_map.last_op_type, H5VL_MAP_CLOSE);
  EXPECT_EQ(mock_map.last_args, nullptr);
  EXPECT_EQ(H5I_nmembers(H5I_MAP), 0);
}

TEST(H5MTest, CloseCallbackFailureLeavesIdOpenUntilRetrySucceeds) {
  ASSERT_GE(H5open(), 0);

  constexpr char kVolName[] = "google_h5m_close_err_vol";
  hid_t vol_id = H5I_INVALID_HID;
  H5VL_connector_t* connector = nullptr;
  absl::Cleanup cleanup_vol = [&vol_id] {
    if (vol_id != H5I_INVALID_HID) {
      H5VLunregister_connector(vol_id);
      vol_id = H5I_INVALID_HID;
    }
  };
  RegisterTestVolConnector(kVolName, kTestVolValueBase + 1, &vol_id,
                           &connector);
  ASSERT_NE(connector, nullptr);

  MockMapObject mock_map;
  mock_map.fail_optional = true;
  hid_t map_id = H5VL_register(H5I_MAP, &mock_map, connector, /*app_ref=*/true);
  ASSERT_NE(map_id, H5I_INVALID_HID);
  absl::Cleanup cleanup_map = [&map_id, &mock_map] {
    if (map_id != H5I_INVALID_HID && H5Iis_valid(map_id) > 0) {
      mock_map.fail_optional = false;
      H5Idec_ref(map_id);
      map_id = H5I_INVALID_HID;
    }
  };

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Idec_ref(map_id), 0);
  }
  EXPECT_EQ(mock_map.optional_calls, 1);
  EXPECT_EQ(mock_map.last_op_type, H5VL_MAP_CLOSE);
  EXPECT_GT(H5Iis_valid(map_id), 0);

  // Retry succeeds and releases H5VL_object_t without leaking.
  mock_map.fail_optional = false;
  EXPECT_EQ(H5Idec_ref(map_id), 0);
  map_id = H5I_INVALID_HID;
  EXPECT_EQ(mock_map.optional_calls, 2);
  EXPECT_EQ(H5I_nmembers(H5I_MAP), 0);
}

TEST(H5MTest, TopTermPackageClearsActiveMapObjects) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup restore_h5m = [] {
    if (H5I_nmembers(H5I_MAP) == 0) {
      H5M_top_term_package();
      H5M_term_package();
    }
    H5M_init();
  };

  constexpr char kVolName[] = "google_h5m_top_term_vol";
  hid_t vol_id = H5I_INVALID_HID;
  H5VL_connector_t* connector = nullptr;
  absl::Cleanup cleanup_vol = [&vol_id] {
    if (vol_id != H5I_INVALID_HID) {
      H5VLunregister_connector(vol_id);
      vol_id = H5I_INVALID_HID;
    }
  };
  RegisterTestVolConnector(kVolName, kTestVolValueBase + 2, &vol_id,
                           &connector);
  ASSERT_NE(connector, nullptr);

  MockMapObject mock_map;
  hid_t map_id = H5VL_register(H5I_MAP, &mock_map, connector, /*app_ref=*/true);
  ASSERT_NE(map_id, H5I_INVALID_HID);
  absl::Cleanup cleanup_map = [&map_id, &mock_map] {
    if (map_id != H5I_INVALID_HID && H5Iis_valid(map_id) > 0) {
      mock_map.fail_optional = false;
      H5Idec_ref(map_id);
      map_id = H5I_INVALID_HID;
    }
  };

  // Extra app reference checks that H5I_clear_type(H5I_MAP, false, false)
  // sweeps application handles regardless of app_count.
  ASSERT_EQ(H5Iinc_ref(map_id), 2);
  EXPECT_EQ(H5I_nmembers(H5I_MAP), 1);

  // H5VL_optional -> H5VL_set_vol_wrapper expects an active API context.
  H5CX_node_t api_ctx{};
  ASSERT_GE(H5CX_push(&api_ctx), 0);
  bool ctx_pushed = true;
  absl::Cleanup pop_ctx = [&ctx_pushed] {
    if (ctx_pushed) {
      H5CX_pop(/*update_dxpl_props=*/false);
      ctx_pushed = false;
    }
  };

  EXPECT_EQ(H5M_top_term_package(), 1);
  map_id = H5I_INVALID_HID;

  EXPECT_EQ(mock_map.optional_calls, 1);
  EXPECT_EQ(mock_map.last_op_type, H5VL_MAP_CLOSE);
  EXPECT_EQ(mock_map.last_args, nullptr);
  EXPECT_EQ(H5I_nmembers(H5I_MAP), 0);

  // Next pass sees zero members and clears H5M_top_package_initialize_s.
  EXPECT_EQ(H5M_top_term_package(), 0);
  EXPECT_EQ(H5M_term_package(), 0);
  EXPECT_GE(H5M_init(), 0);
}

TEST(H5MTest, LibraryCloseTerminatesActiveMapAndReopensCleanly) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup reopen_lib = [] { H5open(); };

  constexpr char kVolName[] = "google_h5m_lib_close_vol";
  hid_t vol_id = H5I_INVALID_HID;
  H5VL_connector_t* connector = nullptr;
  bool lib_closed = false;
  absl::Cleanup cleanup_vol = [&vol_id, &lib_closed] {
    if (!lib_closed && vol_id != H5I_INVALID_HID) {
      H5VLunregister_connector(vol_id);
      vol_id = H5I_INVALID_HID;
    }
  };
  RegisterTestVolConnector(kVolName, kTestVolValueBase + 3, &vol_id,
                           &connector);
  ASSERT_NE(connector, nullptr);

  MockMapObject mock_map;
  hid_t map_id = H5VL_register(H5I_MAP, &mock_map, connector, /*app_ref=*/true);
  ASSERT_NE(map_id, H5I_INVALID_HID);
  absl::Cleanup cleanup_map = [&map_id, &mock_map, &lib_closed] {
    if (!lib_closed && map_id != H5I_INVALID_HID && H5Iis_valid(map_id) > 0) {
      mock_map.fail_optional = false;
      H5Idec_ref(map_id);
      map_id = H5I_INVALID_HID;
    }
  };

  // Full library shutdown runs H5M_top_term_package -> H5M__close_cb ->
  // H5M_term_package -> H5VL_term_package in dependency order.
  ASSERT_GE(H5close(), 0);
  lib_closed = true;
  map_id = H5I_INVALID_HID;
  vol_id = H5I_INVALID_HID;

  EXPECT_EQ(mock_map.optional_calls, 1);
  EXPECT_EQ(mock_map.last_op_type, H5VL_MAP_CLOSE);
  EXPECT_EQ(mock_map.last_args, nullptr);

  // Re-open restores HDF5 and re-initializes H5M for subsequent tests.
  ASSERT_GE(H5open(), 0);
  EXPECT_EQ(H5I_nmembers(H5I_MAP), 0);
}

}  // namespace
}  // namespace hdf5
