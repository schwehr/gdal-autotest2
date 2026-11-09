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
// Tests Virtual Object Layer (VOL) connector registration, lookup, metadata,
// library context management, object wrapping, and dynamic optional operation
// registration (third_party/hdf5/src/H5VL.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VL.c

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ScopedVol = ScopedH5Id<H5VLclose>;

constexpr const char* kTestViplProp = "google.hdf5.test.vipl_magic";
constexpr int kExpectedViplMagic = 0x564f4c2a;

struct CustomVolState {
  int init_calls = 0;
  int term_calls = 0;
  bool saw_vipl_prop = false;
  int vipl_magic = 0;
  bool fail_init = false;
  bool fail_file_get = false;
  bool fail_get_conn_cls = false;
  bool lib_state_ok_in_init = false;
  bool wrap_register_ok_in_visit = false;
  int visit_calls = 0;
  int wrapped_group_closes = 0;
};

struct CustomObj {
  H5I_type_t type = H5I_FILE;
  bool is_wrapped = false;
};

struct CustomWrapCtx {
  int magic = 4242;
};

CustomVolState* g_active_state = nullptr;

herr_t CustomInit(hid_t vipl_id) {
  if (g_active_state == nullptr) return -1;
  ++g_active_state->init_calls;
  if (g_active_state->fail_init) return -1;

  if (vipl_id != H5P_DEFAULT && H5Pexist(vipl_id, kTestViplProp) > 0) {
    int magic = 0;
    if (H5Pget(vipl_id, kTestViplProp, &magic) >= 0) {
      g_active_state->saw_vipl_prop = true;
      g_active_state->vipl_magic = magic;
    }
  }

  // Exercise library state save/open/restore/close/free inside an active API
  // callback context where an H5CX node is guaranteed on the context stack.
  void* state = nullptr;
  void* ctx = nullptr;
  if (H5VLretrieve_lib_state(&state) >= 0) {
    if (H5VLopen_lib_context(&ctx) >= 0) {
      const herr_t restore_rc = H5VLrestore_lib_state(state);
      const herr_t close_rc = H5VLclose_lib_context(ctx);
      const herr_t free_rc = H5VLfree_lib_state(state);
      g_active_state->lib_state_ok_in_init =
          (restore_rc >= 0 && close_rc >= 0 && free_rc >= 0);
    } else {
      (void)H5VLfree_lib_state(state);
    }
  }
  return 0;
}

herr_t CustomTerm() {
  if (g_active_state != nullptr) {
    ++g_active_state->term_calls;
  }
  return 0;
}

void* CustomGetObject(const void* obj) {
  return const_cast<void*>(obj);
}

herr_t CustomGetWrapCtx(const void* /*obj*/, void** wrap_ctx) {
  *wrap_ctx = new CustomWrapCtx{};
  return 0;
}

void* CustomWrapObject(void* obj, H5I_type_t /*obj_type*/, void* /*wrap_ctx*/) {
  auto* custom_obj = static_cast<CustomObj*>(obj);
  custom_obj->is_wrapped = true;
  return custom_obj;
}

void* CustomUnwrapObject(void* obj) { return obj; }

herr_t CustomFreeWrapCtx(void* wrap_ctx) {
  delete static_cast<CustomWrapCtx*>(wrap_ctx);
  return 0;
}

void* CustomFileCreate(const char* /*name*/, unsigned /*flags*/,
                       hid_t /*fcpl_id*/, hid_t /*fapl_id*/, hid_t /*dxpl_id*/,
                       void** /*req*/) {
  return new CustomObj{H5I_FILE, false};
}

herr_t CustomFileGet(void* /*obj*/, H5VL_file_get_args_t* /*args*/,
                     hid_t /*dxpl_id*/, void** /*req*/) {
  if (g_active_state != nullptr && g_active_state->fail_file_get) {
    return -1;
  }
  return 0;
}

herr_t CustomFileClose(void* file, hid_t /*dxpl_id*/, void** /*req*/) {
  delete static_cast<CustomObj*>(file);
  return 0;
}

herr_t CustomGroupClose(void* grp, hid_t /*dxpl_id*/, void** /*req*/) {
  if (g_active_state != nullptr) {
    ++g_active_state->wrapped_group_closes;
  }
  delete static_cast<CustomObj*>(grp);
  return 0;
}

herr_t CustomObjectSpecific(void* /*obj*/, const H5VL_loc_params_t* /*loc*/,
                            H5VL_object_specific_args_t* args,
                            hid_t /*dxpl_id*/, void** /*req*/) {
  if (args == nullptr || args->op_type != H5VL_OBJECT_VISIT) {
    return 0;
  }
  if (g_active_state != nullptr) {
    ++g_active_state->visit_calls;
  }
  auto* inner_grp = new CustomObj{H5I_GROUP, false};
  const hid_t wrapped_id = H5VLwrap_register(inner_grp, H5I_GROUP);
  if (wrapped_id < 0) {
    delete inner_grp;
    return -1;
  }
  const bool valid_wrapped =
      (H5Iget_type(wrapped_id) == H5I_GROUP && H5VLobject(wrapped_id) == inner_grp &&
       inner_grp->is_wrapped);
  H5O_info2_t oinfo{};
  herr_t cb_rc = 0;
  if (args->args.visit.op != nullptr) {
    cb_rc = args->args.visit.op(wrapped_id, "wrapped_group", &oinfo,
                                args->args.visit.op_data);
  }
  const herr_t close_rc = H5Gclose(wrapped_id);
  if (g_active_state != nullptr) {
    g_active_state->wrap_register_ok_in_visit =
        (valid_wrapped && cb_rc >= 0 && close_rc >= 0);
  }
  return (cb_rc >= 0 && close_rc >= 0) ? 0 : -1;
}

// Forward declaration so CustomGetConnCls can return the registered class ptr.
H5VL_class_t MakeCustomVolClass(const char* name, H5VL_class_value_t value,
                                bool include_get_conn_cls = true);

herr_t CustomGetConnCls(void* /*obj*/, H5VL_get_conn_lvl_t /*lvl*/,
                        const H5VL_class_t** conn_cls) {
  if (g_active_state != nullptr && g_active_state->fail_get_conn_cls) {
    return -1;
  }
  static const H5VL_class_t kCustomTerminalCls =
      MakeCustomVolClass("google.hdf5.test.terminal", 420);
  *conn_cls = &kCustomTerminalCls;
  return 0;
}

herr_t CustomOptQuery(void* /*obj*/, H5VL_subclass_t /*subcls*/,
                      int /*opt_type*/, uint64_t* flags) {
  if (flags != nullptr) {
    *flags = 0;
  }
  return 0;
}

H5VL_class_t MakeCustomVolClass(const char* name, H5VL_class_value_t value,
                                bool include_get_conn_cls) {
  H5VL_class_t cls{};
  cls.version = H5VL_VERSION;
  cls.value = value;
  cls.name = name;
  cls.conn_version = 1;
  cls.cap_flags = H5VL_CAP_FLAG_NONE;
  cls.initialize = CustomInit;
  cls.terminate = CustomTerm;
  cls.wrap_cls.get_object = CustomGetObject;
  cls.wrap_cls.get_wrap_ctx = CustomGetWrapCtx;
  cls.wrap_cls.wrap_object = CustomWrapObject;
  cls.wrap_cls.unwrap_object = CustomUnwrapObject;
  cls.wrap_cls.free_wrap_ctx = CustomFreeWrapCtx;
  cls.file_cls.create = CustomFileCreate;
  cls.file_cls.get = CustomFileGet;
  cls.file_cls.close = CustomFileClose;
  cls.group_cls.close = CustomGroupClose;
  cls.object_cls.specific = CustomObjectSpecific;
  if (include_get_conn_cls) {
    cls.introspect_cls.get_conn_cls = CustomGetConnCls;
  }
  cls.introspect_cls.opt_query = CustomOptQuery;
  return cls;
}

ScopedPlist CreateCustomFaplWithPassthru(hid_t under_vol_id) {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) return ScopedPlist();
  H5VL_pass_through_info_t info{under_vol_id, nullptr};
  if (H5Pset_vol(fapl.get(), H5VL_PASSTHRU, &info) < 0) {
    fapl.reset();
  }
  return fapl;
}

TEST(H5VLTest, RegisterAndUnregisterCustomConnectorWithAndWithoutVipl) {
  CustomVolState state;
  g_active_state = &state;
  absl::Cleanup clear_state = [] { g_active_state = nullptr; };

  // Invalid inputs.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5VLregister_connector(nullptr, H5P_DEFAULT), 0);

    H5VL_class_t bad_ver = MakeCustomVolClass("google.hdf5.bad_ver", 401);
    bad_ver.version = H5VL_VERSION + 1;
    EXPECT_LT(H5VLregister_connector(&bad_ver, H5P_DEFAULT), 0);

    H5VL_class_t null_name = MakeCustomVolClass(nullptr, 402);
    EXPECT_LT(H5VLregister_connector(&null_name, H5P_DEFAULT), 0);

    H5VL_class_t empty_name = MakeCustomVolClass("", 403);
    EXPECT_LT(H5VLregister_connector(&empty_name, H5P_DEFAULT), 0);

    ScopedPlist bad_plist(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(bad_plist.valid());
    H5VL_class_t valid_cls = MakeCustomVolClass("google.hdf5.bad_vipl", 404);
    EXPECT_LT(H5VLregister_connector(&valid_cls, bad_plist.get()), 0);
    EXPECT_LT(H5VLregister_connector(&valid_cls, H5I_INVALID_HID), 0);

    state.fail_init = true;
    EXPECT_LT(H5VLregister_connector(&valid_cls, H5P_DEFAULT), 0);
    state.fail_init = false;
  }

  // Register using H5P_DEFAULT.
  constexpr const char* kName1 = "google.hdf5.custom_default";
  constexpr H5VL_class_value_t kVal1 = 405;
  const H5VL_class_t cls1 = MakeCustomVolClass(kName1, kVal1);
  hid_t id1 = H5VLregister_connector(&cls1, H5P_DEFAULT);
  ASSERT_GE(id1, 0);
  absl::Cleanup unreg1 = [&id1] {
    if (id1 >= 0) H5VLunregister_connector(id1);
  };

  EXPECT_EQ(state.init_calls, 2);  // Once for fail_init=true attempt, once here.
  EXPECT_TRUE(state.lib_state_ok_in_init);
  EXPECT_GT(H5VLis_connector_registered_by_name(kName1), 0);
  EXPECT_GT(H5VLis_connector_registered_by_value(kVal1), 0);

  // Unregistering explicitly triggers terminate().
  ASSERT_GE(H5VLunregister_connector(id1), 0);
  id1 = H5I_INVALID_HID;
  EXPECT_EQ(state.term_calls, 1);
  EXPECT_EQ(H5VLis_connector_registered_by_name(kName1), 0);
  EXPECT_EQ(H5VLis_connector_registered_by_value(kVal1), 0);

  // Register using custom H5P_VOL_INITIALIZE property list.
  ScopedPlist vipl(H5Pcreate(H5P_VOL_INITIALIZE));
  ASSERT_TRUE(vipl.valid());
  int default_magic = kExpectedViplMagic;
  ASSERT_GE(H5Pinsert2(vipl.get(), kTestViplProp, sizeof(int), &default_magic,
                       nullptr, nullptr, nullptr, nullptr, nullptr, nullptr),
            0);

  constexpr const char* kName2 = "google.hdf5.custom_vipl";
  constexpr H5VL_class_value_t kVal2 = 406;
  const H5VL_class_t cls2 = MakeCustomVolClass(kName2, kVal2);
  hid_t id2 = H5VLregister_connector(&cls2, vipl.get());
  ASSERT_GE(id2, 0);
  absl::Cleanup unreg2 = [&id2] {
    if (id2 >= 0) H5VLunregister_connector(id2);
  };

  EXPECT_TRUE(state.saw_vipl_prop);
  EXPECT_EQ(state.vipl_magic, kExpectedViplMagic);
}

TEST(H5VLTest, RegisterByNameAndValueAndCheckRegisteredQueries) {
  CustomVolState state;
  g_active_state = &state;
  absl::Cleanup clear_state = [] { g_active_state = nullptr; };

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5VLregister_connector_by_name(nullptr, H5P_DEFAULT), 0);
    EXPECT_LT(H5VLregister_connector_by_name("", H5P_DEFAULT), 0);
    EXPECT_LT(H5VLregister_connector_by_name("google.hdf5.missing", H5P_DEFAULT),
              0);

    ScopedPlist bad_plist(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(bad_plist.valid());
    EXPECT_LT(H5VLregister_connector_by_name("native", bad_plist.get()), 0);
    EXPECT_LT(H5VLregister_connector_by_name("native", H5I_INVALID_HID), 0);

    EXPECT_LT(H5VLregister_connector_by_value(H5_VOL_INVALID, H5P_DEFAULT), 0);
    EXPECT_LT(H5VLregister_connector_by_value(-42, H5P_DEFAULT), 0);
    EXPECT_LT(H5VLregister_connector_by_value(498, H5P_DEFAULT), 0);
    EXPECT_LT(
        H5VLregister_connector_by_value(H5VL_NATIVE_VALUE, bad_plist.get()), 0);
    EXPECT_LT(
        H5VLregister_connector_by_value(H5VL_NATIVE_VALUE, H5I_INVALID_HID), 0);
  }

  EXPECT_GT(H5VLis_connector_registered_by_name(H5VL_NATIVE_NAME), 0);
  EXPECT_GT(H5VLis_connector_registered_by_name(H5VL_PASSTHRU_NAME), 0);
  EXPECT_EQ(H5VLis_connector_registered_by_name("google.hdf5.missing"), 0);

  EXPECT_GT(H5VLis_connector_registered_by_value(H5VL_NATIVE_VALUE), 0);
  EXPECT_GT(H5VLis_connector_registered_by_value(H5VL_PASSTHRU_VALUE), 0);
  EXPECT_EQ(H5VLis_connector_registered_by_value(498), 0);
  EXPECT_EQ(H5VLis_connector_registered_by_value(H5_VOL_INVALID), 0);

  ScopedPlist vipl(H5Pcreate(H5P_VOL_INITIALIZE));
  ASSERT_TRUE(vipl.valid());

  ScopedVol native_by_name(
      H5VLregister_connector_by_name(H5VL_NATIVE_NAME, H5P_DEFAULT));
  ASSERT_TRUE(native_by_name.valid());

  ScopedVol passthru_by_name(
      H5VLregister_connector_by_name(H5VL_PASSTHRU_NAME, vipl.get()));
  ASSERT_TRUE(passthru_by_name.valid());

  ScopedVol native_by_val(
      H5VLregister_connector_by_value(H5VL_NATIVE_VALUE, vipl.get()));
  ASSERT_TRUE(native_by_val.valid());

  ScopedVol passthru_by_val(
      H5VLregister_connector_by_value(H5VL_PASSTHRU_VALUE, H5P_DEFAULT));
  ASSERT_TRUE(passthru_by_val.valid());

  // Register custom connector, then look it up by name and value.
  constexpr const char* kCustomName = "google.hdf5.custom_lookup";
  constexpr H5VL_class_value_t kCustomVal = 407;
  const H5VL_class_t cls = MakeCustomVolClass(kCustomName, kCustomVal);
  hid_t raw_id = H5VLregister_connector(&cls, H5P_DEFAULT);
  ASSERT_GE(raw_id, 0);
  absl::Cleanup unreg = [&raw_id] {
    if (raw_id >= 0) H5VLunregister_connector(raw_id);
  };

  ScopedVol by_name_reg(
      H5VLregister_connector_by_name(kCustomName, H5P_DEFAULT));
  EXPECT_TRUE(by_name_reg.valid());

  ScopedVol by_val_reg(
      H5VLregister_connector_by_value(kCustomVal, H5P_DEFAULT));
  EXPECT_TRUE(by_val_reg.valid());
}

TEST(H5VLTest, GetConnectorIdNameCloseCompareAndNativeUnregisterGuard) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(
      H5Gcreate2(file.get(), "/g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(grp.get(), "d", H5T_NATIVE_INT32, space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ScopedAttr attr(H5Acreate2(dset.get(), "a", H5T_NATIVE_INT32, space.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  ScopedType committed_type(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(committed_type.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/t", committed_type.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Error paths for ID/name lookup, close, unregister, compare.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5VLget_connector_id(H5I_INVALID_HID), 0);
    EXPECT_LT(H5VLget_connector_id(space.get()), 0);
    EXPECT_LT(H5VLget_connector_id(H5T_NATIVE_INT32), 0);
    EXPECT_LT(H5VLget_connector_id_by_name("google.hdf5.missing"), 0);
    EXPECT_LT(H5VLget_connector_id_by_value(498), 0);
    EXPECT_LT(H5VLget_connector_id_by_value(H5_VOL_INVALID), 0);

    char buf[16] = {};
    EXPECT_LT(H5VLget_connector_name(H5I_INVALID_HID, buf, sizeof(buf)), 0);
    EXPECT_LT(H5VLget_connector_name(space.get(), buf, sizeof(buf)), 0);

    EXPECT_LT(H5VLclose(H5I_INVALID_HID), 0);
    EXPECT_LT(H5VLclose(file.get()), 0);
    EXPECT_LT(H5VLunregister_connector(H5I_INVALID_HID), 0);
    EXPECT_LT(H5VLunregister_connector(file.get()), 0);

    int cmp = 0;
    EXPECT_LT(H5VLcmp_connector_cls(&cmp, H5I_INVALID_HID, H5VL_NATIVE), 0);
    EXPECT_LT(H5VLcmp_connector_cls(&cmp, H5VL_NATIVE, H5I_INVALID_HID), 0);
    EXPECT_LT(H5VLcmp_connector_cls(&cmp, file.get(), H5VL_NATIVE), 0);
    EXPECT_LT(H5VLcmp_connector_cls(&cmp, H5VL_NATIVE, file.get()), 0);

    // Unregistering the native VOL connector must fail.
    ScopedVol native_handle(H5VLget_connector_id_by_name(H5VL_NATIVE_NAME));
    ASSERT_TRUE(native_handle.valid());
    EXPECT_LT(H5VLunregister_connector(native_handle.get()), 0);
    EXPECT_LT(H5VLunregister_connector(H5VL_NATIVE), 0);
  }

  for (hid_t obj_id :
       {file.get(), grp.get(), dset.get(), attr.get(), committed_type.get()}) {
    ScopedVol vol_id(H5VLget_connector_id(obj_id));
    ASSERT_TRUE(vol_id.valid());
    int cmp = -99;
    ASSERT_GE(H5VLcmp_connector_cls(&cmp, vol_id.get(), H5VL_NATIVE), 0);
    EXPECT_EQ(cmp, 0);

    EXPECT_EQ(H5VLget_connector_name(obj_id, nullptr, 0), 6);
    std::string full_name(7, '\0');
    EXPECT_EQ(H5VLget_connector_name(obj_id, full_name.data(), full_name.size()),
              6);
    EXPECT_STREQ(full_name.c_str(), H5VL_NATIVE_NAME);

    char truncated[4] = {'x', 'x', 'x', 'x'};
    EXPECT_EQ(H5VLget_connector_name(obj_id, truncated, sizeof(truncated)), 6);
    EXPECT_STREQ(truncated, "nat");
  }

  ScopedVol native_by_name(H5VLget_connector_id_by_name(H5VL_NATIVE_NAME));
  ASSERT_TRUE(native_by_name.valid());
  ScopedVol native_by_val(H5VLget_connector_id_by_value(H5VL_NATIVE_VALUE));
  ASSERT_TRUE(native_by_val.valid());
  ScopedVol passthru_by_val(H5VLget_connector_id_by_value(H5VL_PASSTHRU_VALUE));
  ASSERT_TRUE(passthru_by_val.valid());

  // Close explicitly and verify double-close fails.
  const hid_t closed_id = native_by_name.release();
  EXPECT_GE(H5VLclose(closed_id), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5VLclose(closed_id), 0);
  }

  // Compare same vs distinct connector classes.
  int cmp = 0;
  ASSERT_GE(
      H5VLcmp_connector_cls(&cmp, native_by_val.get(), native_by_val.get()), 0);
  EXPECT_EQ(cmp, 0);
  ASSERT_GE(
      H5VLcmp_connector_cls(&cmp, native_by_val.get(), passthru_by_val.get()),
      0);
  EXPECT_LT(cmp, 0);
  ASSERT_GE(
      H5VLcmp_connector_cls(&cmp, passthru_by_val.get(), native_by_val.get()),
      0);
  EXPECT_GT(cmp, 0);

  // Stacked pass-through connector over native Core VFD file. Keep under_vol
  // alive until after pt_file/pt_fapl close because pass_through info copy
  // checks H5Iis_valid(under_vol_id) (requiring app_count > 0).
  ScopedVol under_vol(H5VLget_connector_id_by_name(H5VL_NATIVE_NAME));
  ASSERT_TRUE(under_vol.valid());
  ScopedPlist pt_fapl = CreateCustomFaplWithPassthru(under_vol.get());
  ASSERT_TRUE(pt_fapl.valid());
  ScopedFile pt_file(H5Fcreate("passthru_mem.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                               pt_fapl.get()));
  ASSERT_TRUE(pt_file.valid());
  ScopedGroup pt_grp(
      H5Gcreate2(pt_file.get(), "/pg", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(pt_grp.valid());

  ScopedVol pt_conn(H5VLget_connector_id(pt_grp.get()));
  ASSERT_TRUE(pt_conn.valid());
  ASSERT_GE(H5VLcmp_connector_cls(&cmp, pt_conn.get(), H5VL_PASSTHRU), 0);
  EXPECT_EQ(cmp, 0);
  char pt_name[32] = {};
  EXPECT_EQ(H5VLget_connector_name(pt_grp.get(), pt_name, sizeof(pt_name)), 12);
  EXPECT_STREQ(pt_name, H5VL_PASSTHRU_NAME);
}

TEST(H5VLTest, ObjectAndIsNativeAcrossNativePassthruAndCustomConnectors) {
  ScopedFile native_file = CreateInMemoryFile();
  ASSERT_TRUE(native_file.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());

  EXPECT_NE(H5VLobject(native_file.get()), nullptr);
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5VLobject(H5I_INVALID_HID), nullptr);
    EXPECT_EQ(H5VLobject(space.get()), nullptr);
    EXPECT_EQ(H5VLobject(H5T_NATIVE_INT32), nullptr);
    EXPECT_EQ(H5VLobject(H5VL_NATIVE), nullptr);
  }

  bool is_native = false;
  ASSERT_GE(H5VLobject_is_native(native_file.get(), &is_native), 0);
  EXPECT_TRUE(is_native);

  ScopedVol under_vol(H5VLget_connector_id_by_name(H5VL_NATIVE_NAME));
  ASSERT_TRUE(under_vol.valid());
  ScopedPlist pt_fapl = CreateCustomFaplWithPassthru(under_vol.get());
  ASSERT_TRUE(pt_fapl.valid());
  ScopedFile pt_file(H5Fcreate("passthru_native_check.h5", H5F_ACC_TRUNC,
                               H5P_DEFAULT, pt_fapl.get()));
  ASSERT_TRUE(pt_file.valid());
  EXPECT_NE(H5VLobject(pt_file.get()), nullptr);
  is_native = false;
  ASSERT_GE(H5VLobject_is_native(pt_file.get(), &is_native), 0);
  EXPECT_TRUE(is_native);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5VLobject_is_native(native_file.get(), nullptr), 0);
    EXPECT_LT(H5VLobject_is_native(H5I_INVALID_HID, &is_native), 0);
    EXPECT_LT(H5VLobject_is_native(space.get(), &is_native), 0);
  }

  // Custom terminal connector reports is_native == false, and failing
  // introspect.get_conn_cls makes H5VLobject_is_native return < 0.
  CustomVolState state;
  g_active_state = &state;
  absl::Cleanup clear_state = [] { g_active_state = nullptr; };

  const H5VL_class_t cls = MakeCustomVolClass("google.hdf5.custom_term", 420);
  hid_t custom_vol = H5VLregister_connector(&cls, H5P_DEFAULT);
  ASSERT_GE(custom_vol, 0);
  absl::Cleanup unreg_custom = [&custom_vol] {
    H5VLunregister_connector(custom_vol);
  };

  ScopedPlist custom_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(custom_fapl.valid());
  ASSERT_GE(H5Pset_vol(custom_fapl.get(), custom_vol, nullptr), 0);
  ScopedFile custom_file(
      H5Fcreate("custom_term", H5F_ACC_TRUNC, H5P_DEFAULT, custom_fapl.get()));
  ASSERT_TRUE(custom_file.valid());
  EXPECT_NE(H5VLobject(custom_file.get()), nullptr);

  is_native = true;
  ASSERT_GE(H5VLobject_is_native(custom_file.get(), &is_native), 0);
  EXPECT_FALSE(is_native);

  state.fail_get_conn_cls = true;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5VLobject_is_native(custom_file.get(), &is_native), 0);
  }
  state.fail_get_conn_cls = false;

  // Connector omitting introspect_cls.get_conn_cls also returns < 0.
  const H5VL_class_t no_introspect_cls = MakeCustomVolClass(
      "google.hdf5.custom_no_get_conn", 423, /*include_get_conn_cls=*/false);
  hid_t no_intro_vol = H5VLregister_connector(&no_introspect_cls, H5P_DEFAULT);
  ASSERT_GE(no_intro_vol, 0);
  absl::Cleanup unreg_no_intro = [&no_intro_vol] {
    H5VLunregister_connector(no_intro_vol);
  };
  ScopedPlist no_intro_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(no_intro_fapl.valid());
  ASSERT_GE(H5Pset_vol(no_intro_fapl.get(), no_intro_vol, nullptr), 0);
  ScopedFile no_intro_file(H5Fcreate("custom_no_intro", H5F_ACC_TRUNC,
                                     H5P_DEFAULT, no_intro_fapl.get()));
  ASSERT_TRUE(no_intro_file.valid());
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5VLobject_is_native(no_intro_file.get(), &is_native), 0);
  }
}

TEST(H5VLTest, WrapRegisterRejectsInvalidInputsAndWrapsInActiveContext) {
  CustomVolState state;
  g_active_state = &state;
  absl::Cleanup clear_state = [] { g_active_state = nullptr; };

  ScopedFile native_file = CreateInMemoryFile();
  ASSERT_TRUE(native_file.valid());
  void* native_obj = H5VLobject(native_file.get());
  ASSERT_NE(native_obj, nullptr);

  // Invalid type numbers, null obj, and valid types with no wrap context set.
  {
    ScopedErrorSilence silence;
    for (H5I_type_t bad_type : {H5I_UNINIT, H5I_BADID, H5I_DATASPACE, H5I_VFL,
                                H5I_VOL, H5I_GENPROP_CLS, H5I_GENPROP_LST,
                                H5I_ERROR_CLASS, H5I_ERROR_MSG, H5I_ERROR_STACK,
                                H5I_SPACE_SEL_ITER, H5I_EVENTSET, H5I_NTYPES}) {
      EXPECT_LT(H5VLwrap_register(native_obj, bad_type), 0);
    }
    EXPECT_LT(H5VLwrap_register(nullptr, H5I_FILE), 0);
    for (H5I_type_t valid_type : {H5I_FILE, H5I_GROUP, H5I_DATATYPE,
                                  H5I_DATASET, H5I_MAP, H5I_ATTR}) {
      EXPECT_LT(H5VLwrap_register(native_obj, valid_type), 0);
    }
  }

  // Trigger an active wrap context via H5Ovisit3 on our custom VOL connector.
  const H5VL_class_t cls = MakeCustomVolClass("google.hdf5.custom_wrap", 421);
  hid_t custom_vol = H5VLregister_connector(&cls, H5P_DEFAULT);
  ASSERT_GE(custom_vol, 0);
  absl::Cleanup unreg_custom = [&custom_vol] {
    H5VLunregister_connector(custom_vol);
  };

  ScopedPlist custom_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(custom_fapl.valid());
  ASSERT_GE(H5Pset_vol(custom_fapl.get(), custom_vol, nullptr), 0);
  ScopedFile custom_file(
      H5Fcreate("custom_wrap", H5F_ACC_TRUNC, H5P_DEFAULT, custom_fapl.get()));
  ASSERT_TRUE(custom_file.valid());

  struct VisitCtx {
    int cb_hits = 0;
    std::string last_name;
    bool valid_wrapped_obj = false;
  } vctx;

  H5O_iterate2_t visit_op = [](hid_t obj, const char* name,
                               const H5O_info2_t* /*info*/, void* op_data) {
    auto* ctx = static_cast<VisitCtx*>(op_data);
    ++ctx->cb_hits;
    ctx->last_name = (name != nullptr) ? name : "";
    ctx->valid_wrapped_obj =
        (obj >= 0 && H5Iget_type(obj) == H5I_GROUP && H5VLobject(obj) != nullptr);
    return H5_ITER_CONT;
  };

  ASSERT_GE(H5Ovisit3(custom_file.get(), H5_INDEX_NAME, H5_ITER_INC, visit_op,
                      &vctx, H5O_INFO_BASIC),
            0);
  EXPECT_EQ(state.visit_calls, 1);
  EXPECT_TRUE(state.wrap_register_ok_in_visit);
  EXPECT_EQ(state.wrapped_group_closes, 1);
  EXPECT_EQ(vctx.cb_hits, 1);
  EXPECT_EQ(vctx.last_name, "wrapped_group");
  EXPECT_TRUE(vctx.valid_wrapped_obj);
}

TEST(H5VLTest, GetFileTypeFixedVlenAndFailureCleanup) {
  ScopedFile native_file = CreateInMemoryFile();
  ASSERT_TRUE(native_file.valid());
  void* native_file_obj = H5VLobject(native_file.get());
  ASSERT_NE(native_file_obj, nullptr);

  ScopedType vlen_seq(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vlen_seq.valid());
  ScopedType vlen_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vlen_str.valid());
  ASSERT_GE(H5Tset_size(vlen_str.get(), H5T_VARIABLE), 0);

  // Error checks.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5VLget_file_type(nullptr, H5VL_NATIVE, H5T_NATIVE_INT32), 0);
    EXPECT_LT(
        H5VLget_file_type(native_file_obj, H5VL_NATIVE, H5I_INVALID_HID), 0);
    EXPECT_LT(
        H5VLget_file_type(native_file_obj, H5VL_NATIVE, native_file.get()), 0);
    EXPECT_LT(
        H5VLget_file_type(native_file_obj, H5I_INVALID_HID, H5T_NATIVE_INT32),
        0);
    EXPECT_LT(
        H5VLget_file_type(native_file_obj, native_file.get(), H5T_NATIVE_INT32),
        0);
  }

  // Fixed-size type (H5T_GET_FORCE_CONV == false).
  ScopedType fixed_ft(
      H5VLget_file_type(native_file_obj, H5VL_NATIVE, H5T_NATIVE_INT32));
  ASSERT_TRUE(fixed_ft.valid());
  EXPECT_GT(H5Tequal(fixed_ft.get(), H5T_NATIVE_INT32), 0);

  // Variable-length sequence & string (H5T_GET_FORCE_CONV == true, sets disk
  // loc via H5VL_file_get and owns/frees file_vol_obj).
  ScopedType vlen_seq_ft(
      H5VLget_file_type(native_file_obj, H5VL_NATIVE, vlen_seq.get()));
  ASSERT_TRUE(vlen_seq_ft.valid());
  EXPECT_GT(H5Tget_size(vlen_seq_ft.get()), 0);

  ScopedType vlen_str_ft(
      H5VLget_file_type(native_file_obj, H5VL_NATIVE, vlen_str.get()));
  ASSERT_TRUE(vlen_str_ft.valid());
  EXPECT_GT(H5Tget_size(vlen_str_ft.get()), 0);

  // Failing file_cls.get triggers H5T_set_loc < 0 and error cleanup of both
  // file_vol_obj and file_type_id.
  CustomVolState state;
  state.fail_file_get = true;
  g_active_state = &state;
  absl::Cleanup clear_state = [] { g_active_state = nullptr; };

  const H5VL_class_t cls = MakeCustomVolClass("google.hdf5.fail_get", 422);
  hid_t custom_vol = H5VLregister_connector(&cls, H5P_DEFAULT);
  ASSERT_GE(custom_vol, 0);
  absl::Cleanup unreg_custom = [&custom_vol] {
    H5VLunregister_connector(custom_vol);
  };

  CustomObj dummy_file{H5I_FILE, false};
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5VLget_file_type(&dummy_file, custom_vol, vlen_seq.get()), 0);
  }
}

TEST(H5VLTest, LibContextAndStateLifecycleAndNullErrors) {
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5VLretrieve_lib_state(nullptr), 0);
    EXPECT_LT(H5VLopen_lib_context(nullptr), 0);
    EXPECT_LT(H5VLrestore_lib_state(nullptr), 0);
    EXPECT_LT(H5VLclose_lib_context(nullptr), 0);
    EXPECT_LT(H5VLfree_lib_state(nullptr), 0);
  }

  void* outer_ctx = nullptr;
  ASSERT_GE(H5VLopen_lib_context(&outer_ctx), 0);
  ASSERT_NE(outer_ctx, nullptr);
  absl::Cleanup close_outer = [&outer_ctx] {
    if (outer_ctx != nullptr) H5VLclose_lib_context(outer_ctx);
  };

  void* saved_state = nullptr;
  ASSERT_GE(H5VLretrieve_lib_state(&saved_state), 0);
  ASSERT_NE(saved_state, nullptr);
  absl::Cleanup free_state = [&saved_state] {
    if (saved_state != nullptr) H5VLfree_lib_state(saved_state);
  };

  void* inner_ctx = nullptr;
  ASSERT_GE(H5VLopen_lib_context(&inner_ctx), 0);
  ASSERT_NE(inner_ctx, nullptr);
  absl::Cleanup close_inner = [&inner_ctx] {
    if (inner_ctx != nullptr) H5VLclose_lib_context(inner_ctx);
  };

  EXPECT_GE(H5VLrestore_lib_state(saved_state), 0);

  EXPECT_GE(H5VLclose_lib_context(inner_ctx), 0);
  inner_ctx = nullptr;

  EXPECT_GE(H5VLfree_lib_state(saved_state), 0);
  saved_state = nullptr;

  EXPECT_GE(H5VLclose_lib_context(outer_ctx), 0);
  outer_ctx = nullptr;
}

TEST(H5VLTest, QueryOptionalSupportsNativeAndPassthruAndRejectsInvalid) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "/d", H5T_NATIVE_INT32, space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  uint64_t flags = 0;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5VLquery_optional(file.get(), H5VL_SUBCLS_FILE,
                                 H5VL_NATIVE_FILE_GET_SIZE, nullptr),
              0);
    EXPECT_LT(H5VLquery_optional(H5I_INVALID_HID, H5VL_SUBCLS_FILE,
                                 H5VL_NATIVE_FILE_GET_SIZE, &flags),
              0);
    EXPECT_LT(H5VLquery_optional(space.get(), H5VL_SUBCLS_FILE,
                                 H5VL_NATIVE_FILE_GET_SIZE, &flags),
              0);
    EXPECT_LT(
        H5VLquery_optional(file.get(), H5VL_SUBCLS_NONE, 0, &flags), 0);
    EXPECT_LT(
        H5VLquery_optional(file.get(), H5VL_SUBCLS_FILE, 99999, &flags), 0);
  }

  ASSERT_GE(H5VLquery_optional(file.get(), H5VL_SUBCLS_FILE,
                               H5VL_NATIVE_FILE_GET_SIZE, &flags),
            0);
  EXPECT_NE(flags & H5VL_OPT_QUERY_SUPPORTED, 0u);

  flags = 0;
  ASSERT_GE(H5VLquery_optional(dset.get(), H5VL_SUBCLS_DATASET,
                               H5VL_NATIVE_DATASET_GET_OFFSET, &flags),
            0);
  EXPECT_NE(flags & H5VL_OPT_QUERY_SUPPORTED, 0u);

  // Also test query through pass-through VOL connector.
  ScopedVol under_vol(H5VLget_connector_id_by_name(H5VL_NATIVE_NAME));
  ASSERT_TRUE(under_vol.valid());
  ScopedPlist pt_fapl = CreateCustomFaplWithPassthru(under_vol.get());
  ASSERT_TRUE(pt_fapl.valid());
  ScopedFile pt_file(H5Fcreate("passthru_opt_query.h5", H5F_ACC_TRUNC,
                               H5P_DEFAULT, pt_fapl.get()));
  ASSERT_TRUE(pt_file.valid());
  flags = 0;
  ASSERT_GE(H5VLquery_optional(pt_file.get(), H5VL_SUBCLS_FILE,
                               H5VL_NATIVE_FILE_GET_SIZE, &flags),
            0);
  EXPECT_NE(flags & H5VL_OPT_QUERY_SUPPORTED, 0u);
}

TEST(H5VLTest, DynamicOptionalOperationsAllSubclassesAndValidation) {
  int op_val = -1;
  {
    ScopedErrorSilence silence;
    // Register invalid args.
    EXPECT_LT(
        H5VLregister_opt_operation(H5VL_SUBCLS_FILE, "google.hdf5.op", nullptr),
        0);
    EXPECT_LT(H5VLregister_opt_operation(H5VL_SUBCLS_FILE, nullptr, &op_val),
              0);
    EXPECT_LT(H5VLregister_opt_operation(H5VL_SUBCLS_FILE, "", &op_val), 0);

    // Find invalid args.
    EXPECT_LT(
        H5VLfind_opt_operation(H5VL_SUBCLS_FILE, "google.hdf5.op", nullptr), 0);
    EXPECT_LT(H5VLfind_opt_operation(H5VL_SUBCLS_FILE, nullptr, &op_val), 0);
    EXPECT_LT(H5VLfind_opt_operation(H5VL_SUBCLS_FILE, "", &op_val), 0);
    EXPECT_LT(H5VLfind_opt_operation(H5VL_SUBCLS_FILE,
                                     "google.hdf5.never_registered", &op_val),
              0);

    // Unregister invalid args.
    EXPECT_LT(H5VLunregister_opt_operation(H5VL_SUBCLS_FILE, nullptr), 0);
    EXPECT_LT(H5VLunregister_opt_operation(H5VL_SUBCLS_FILE, ""), 0);
    EXPECT_LT(H5VLunregister_opt_operation(H5VL_SUBCLS_FILE,
                                           "google.hdf5.never_registered"),
              0);

    for (H5VL_subclass_t bad_subcls : {H5VL_SUBCLS_NONE, H5VL_SUBCLS_INFO,
                                       H5VL_SUBCLS_WRAP, H5VL_SUBCLS_BLOB,
                                       H5VL_SUBCLS_TOKEN}) {
      EXPECT_LT(
          H5VLregister_opt_operation(bad_subcls, "google.hdf5.bad", &op_val),
          0);
      EXPECT_LT(H5VLfind_opt_operation(bad_subcls, "google.hdf5.bad", &op_val),
                0);
      EXPECT_LT(H5VLunregister_opt_operation(bad_subcls, "google.hdf5.bad"), 0);
    }
  }

  constexpr H5VL_subclass_t kValidSubclasses[] = {
      H5VL_SUBCLS_ATTR,     H5VL_SUBCLS_DATASET, H5VL_SUBCLS_DATATYPE,
      H5VL_SUBCLS_FILE,     H5VL_SUBCLS_GROUP,   H5VL_SUBCLS_OBJECT,
      H5VL_SUBCLS_LINK,     H5VL_SUBCLS_REQUEST,
  };

  struct RegisteredOp {
    H5VL_subclass_t subcls;
    std::string name;
    int val = -1;
  };
  std::vector<RegisteredOp> registered;
  absl::Cleanup cleanup_ops = [&registered] {
    ScopedErrorSilence silence;
    for (const RegisteredOp& entry : registered) {
      H5VLunregister_opt_operation(entry.subcls, entry.name.c_str());
    }
  };

  for (size_t i = 0; i < sizeof(kValidSubclasses) / sizeof(kValidSubclasses[0]);
       ++i) {
    const H5VL_subclass_t subcls = kValidSubclasses[i];
    const std::string name =
        "google.hdf5.test.dyn_op_" + std::to_string(i);
    int assigned_val = -1;
    ASSERT_GE(H5VLregister_opt_operation(subcls, name.c_str(), &assigned_val),
              0);
    EXPECT_GE(assigned_val, H5VL_RESERVED_NATIVE_OPTIONAL);
    registered.push_back({subcls, name, assigned_val});

    // Duplicate registration in same subclass fails.
    {
      ScopedErrorSilence silence;
      int dup_val = -1;
      EXPECT_LT(H5VLregister_opt_operation(subcls, name.c_str(), &dup_val), 0);
    }

    int found_val = -1;
    ASSERT_GE(H5VLfind_opt_operation(subcls, name.c_str(), &found_val), 0);
    EXPECT_EQ(found_val, assigned_val);
  }

  // Unregister each op explicitly and verify lookup and second unregister fail.
  for (const RegisteredOp& entry : registered) {
    ASSERT_GE(H5VLunregister_opt_operation(entry.subcls, entry.name.c_str()),
              0);
    ScopedErrorSilence silence;
    int missing_val = -1;
    EXPECT_LT(H5VLfind_opt_operation(entry.subcls, entry.name.c_str(),
                                     &missing_val),
              0);
    EXPECT_LT(H5VLunregister_opt_operation(entry.subcls, entry.name.c_str()),
              0);
  }
  registered.clear();
}

}  // namespace
}  // namespace hdf5
