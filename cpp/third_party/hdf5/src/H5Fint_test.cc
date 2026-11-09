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
// Tests internal file initialization, open/close/reopen/flush, access property
// list reflection, object enumeration, prefix resolution, symlink resolution,
// address coding, file images, retry tracking, object flush callbacks,
// SWMR write transition, and format conversion (third_party/hdf5/src/H5Fint.c).
//
// Google3 build notes: H5_HAVE_PARALLEL, H5_HAVE_DIRECT, H5_HAVE_WINDOW_PATH,
// and H5_USE_FILE_LOCKING are disabled by default; H5_HAVE_SYMLINK,
// H5_IGNORE_DISABLED_FILE_LOCKS, H5VL_PASSTHRU, and Onion VFD are enabled.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Fint.c

// `:hdf5` compiles with `-DNDEBUG`, which omits `#ifndef NDEBUG` fields in
// `H5C_cache_entry_t` (`H5AC_info_t`), embedded in `H5F_super_t`.
#ifndef NDEBUG
#define H5FINT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5AC_FRIEND
#define H5C_FRIEND
#define H5D_FRIEND
#define H5F_FRIEND
#define H5VL_FRIEND
#pragma push_macro("try")
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#undef try
#define try h5fint_try_param
#include "third_party/hdf5/src/H5ACpkg.h"
#include "third_party/hdf5/src/H5Cprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dpkg.h"
#include "third_party/hdf5/src/H5FDdevelop.h"
#include "third_party/hdf5/src/H5FDonion.h"
#include "third_party/hdf5/src/H5FDprivate.h"
#include "third_party/hdf5/src/H5FOprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5MMprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5Pprivate.h"
#include "third_party/hdf5/src/H5VLnative_private.h"
#include "third_party/hdf5/src/H5VLpassthru.h"
#include "third_party/hdf5/src/H5VLpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#undef try
#pragma clang diagnostic pop
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5FINT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FINT_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

// RAII API context push/pop for calling H5F package-internal routines directly.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t fapl_id = H5P_DEFAULT) {
    H5open();
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      hid_t plist_id =
          (fapl_id == H5P_DEFAULT) ? H5P_FILE_ACCESS_DEFAULT : fapl_id;
      if (auto* plist = static_cast<H5P_genplist_t*>(H5I_object(plist_id))) {
        H5VL_connector_prop_t connector_prop{};
        if (H5P_peek(plist, H5F_ACS_VOL_CONN_NAME, &connector_prop) >= 0) {
          H5CX_set_vol_connector_prop(&connector_prop);
        }
      }
    }
  }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

// Saves and restores an environment variable for hermetic test cleanup.
class ScopedEnvVar {
 public:
  explicit ScopedEnvVar(const char* name) : name_(name) {
    const char* prev = std::getenv(name);
    if (prev != nullptr) {
      saved_ = std::string(prev);
    }
  }

  ~ScopedEnvVar() {
    if (saved_.has_value()) {
      setenv(name_, saved_->c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

 private:
  const char* name_;
  std::optional<std::string> saved_;
};

ScopedPlist MakeSec2Fapl() {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  if (fapl.valid() && H5Pset_fapl_sec2(fapl.get()) < 0) {
    fapl.reset();
  }
  return fapl;
}

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object(file_id));
}

TEST(H5FintTest, InitTermPackageAndFileLockEnvParsing) {
  ASSERT_GE(H5open(), 0);
  ASSERT_GE(H5F_init(), 0);

  const htri_t orig_use_locks = use_locks_env_g;
  const htri_t orig_ignore_locks = ignore_disabled_locks_g;
  ScopedEnvVar env_guard(HDF5_USE_FILE_LOCKING);
  absl::Cleanup restore_globals = [&] {
    use_locks_env_g = orig_use_locks;
    ignore_disabled_locks_g = orig_ignore_locks;
    H5open();
  };

  struct LockCase {
    const char* env_val;
    htri_t expected_use;
    htri_t expected_ignore;
  };
  constexpr LockCase kCases[] = {
      {"FALSE", false, -1}, {"0", false, -1},    {"BEST_EFFORT", true, true},
      {"TRUE", true, false}, {"1", true, false}, {"UNRECOGNIZED", -1, -1},
      {nullptr, -1, -1},
  };

  for (const auto& tc : kCases) {
    if (tc.env_val != nullptr) {
      ASSERT_EQ(setenv(HDF5_USE_FILE_LOCKING, tc.env_val, 1), 0);
    } else {
      unsetenv(HDF5_USE_FILE_LOCKING);
    }
    htri_t use_locks = 99;
    htri_t ignore_disabled = 99;
    ASSERT_GE(H5F__parse_file_lock_env_var(&use_locks, &ignore_disabled), 0);
    EXPECT_EQ(use_locks, tc.expected_use);
    EXPECT_EQ(ignore_disabled, tc.expected_ignore);
  }

  // Test H5F_term_package with open file IDs (clears H5I_FILE and retries)
  // and with zero file IDs (shuts down H5F package), then re-open library.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    // Intentionally leave raw hid_t open across H5close() so H5F_term_package
    // exercises H5I_nmembers(H5I_FILE) > 0 -> H5I_clear_type().
    const hid_t raw_fid =
        H5Fcreate("term_open.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get());
    ASSERT_NE(H5I_INVALID_HID, raw_fid);
  }
  ASSERT_GE(H5close(), 0);
  ASSERT_GE(H5open(), 0);
  EXPECT_GE(H5F_init(), 0);
}

TEST(H5FintTest, GetAccessPlistReflectsPropertiesAndPassthruVol) {
  TempDir dir("fapl_roundtrip");
  const auto paged_path = dir.child("paged.h5");
  const auto mdc_log_path = dir.child("mdc.log");

  // 1. Paged allocation + page buffer + EFC + object flush cb + RFIC + mdc log
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE,
                                         true, 1),
              0);
    ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), 4096), 0);

    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_elink_file_cache_size(fapl.get(), 3), 0);
    ASSERT_GE(H5Pset_fclose_degree(fapl.get(), H5F_CLOSE_SEMI), 0);
    ASSERT_GE(H5Pset_page_buffer_size(fapl.get(), 8192, 20, 30), 0);
    auto flush_cb = +[](hid_t, void*) -> herr_t { return 0; };
    int udata = 7;
    ASSERT_GE(H5Pset_object_flush_cb(fapl.get(), flush_cb, &udata), 0);
    ASSERT_GE(H5Pset_relax_file_integrity_checks(fapl.get(), H5F_RFIC_ALL), 0);
    ASSERT_GE(
        H5Pset_mdc_log_options(fapl.get(), true, mdc_log_path.c_str(), false),
        0);
    ASSERT_GE(H5Pset_alignment(fapl.get(), 64, 512), 0);
    ASSERT_GE(H5Pset_sieve_buf_size(fapl.get(), 16384), 0);
    ASSERT_GE(H5Pset_gc_references(fapl.get(), 1), 0);
    ASSERT_GE(H5Pset_meta_block_size(fapl.get(), 4096), 0);
    ASSERT_GE(H5Pset_small_data_block_size(fapl.get(), 4096), 0);

    ScopedFile file(H5Fcreate(paged_path.c_str(), H5F_ACC_TRUNC, fcpl.get(),
                              fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedPlist got_fapl(H5Fget_access_plist(file.get()));
    ASSERT_TRUE(got_fapl.valid());

    unsigned efc_size = 0;
    EXPECT_GE(H5Pget_elink_file_cache_size(got_fapl.get(), &efc_size), 0);
    EXPECT_EQ(efc_size, 3u);

    H5F_close_degree_t deg = H5F_CLOSE_DEFAULT;
    EXPECT_GE(H5Pget_fclose_degree(got_fapl.get(), &deg), 0);
    EXPECT_EQ(deg, H5F_CLOSE_SEMI);

    size_t pb_size = 0;
    unsigned min_meta = 0;
    unsigned min_raw = 0;
    EXPECT_GE(
        H5Pget_page_buffer_size(got_fapl.get(), &pb_size, &min_meta, &min_raw),
        0);
    EXPECT_EQ(pb_size, 8192u);
    EXPECT_EQ(min_meta, 20u);
    EXPECT_EQ(min_raw, 30u);

    uint64_t rfic = 0;
    EXPECT_GE(H5Pget_relax_file_integrity_checks(got_fapl.get(), &rfic), 0);
    EXPECT_EQ(rfic, H5F_RFIC_ALL);

    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);
    EXPECT_STREQ(H5F_mdc_log_location(f), mdc_log_path.c_str());

    // Internal H5F_get_access_plist(app_ref = false) coverage.
    {
      ScopedApiContext ctx;
      hid_t internal_fapl = H5F_get_access_plist(f, false);
      ASSERT_NE(H5I_INVALID_HID, internal_fapl);
      EXPECT_GE(H5I_dec_ref(internal_fapl), 0);
    }
  }

  // 2. Pass-through VOL connector caches and frees non-null connector_info.
  {
    const hid_t native_vol_id = H5VLget_connector_id_by_name(H5VL_NATIVE_NAME);
    ASSERT_NE(H5I_INVALID_HID, native_vol_id);
    absl::Cleanup close_native_vol = [&] { H5VLclose(native_vol_id); };

    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    H5VL_pass_through_info_t pt_info{};
    pt_info.under_vol_id = native_vol_id;
    pt_info.under_vol_info = nullptr;
    ASSERT_GE(H5Pset_vol(fapl.get(), H5VL_PASSTHRU, &pt_info), 0);

    ScopedFile file(
        H5Fcreate("passthru.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(
        H5Gcreate2(file.get(), "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    ScopedPlist got_fapl(H5Fget_access_plist(file.get()));
    ASSERT_TRUE(got_fapl.valid());
    hid_t vol_id = H5I_INVALID_HID;
    ASSERT_GE(H5Pget_vol_id(got_fapl.get(), &vol_id), 0);
    ASSERT_NE(H5I_INVALID_HID, vol_id);
    absl::Cleanup close_vol = [&vol_id] {
      if (vol_id != H5I_INVALID_HID) {
        H5VLclose(vol_id);
      }
    };
    char vol_name[64]{};
    EXPECT_GT(H5VLget_connector_name(file.get(), vol_name, sizeof(vol_name)), 0);
    EXPECT_STREQ(vol_name, H5VL_PASSTHRU_NAME);

    // Get existing file ID while file is open, then resurrect file ID after
    // weak close to exercise both branches of H5F_get_file_id().
    grp.reset();
    ScopedGroup grp2(H5Gopen2(file.get(), "g", H5P_DEFAULT));
    ASSERT_TRUE(grp2.valid());
    {
      ScopedFile existing_fid(H5Iget_file_id(grp2.get()));
      EXPECT_TRUE(existing_fid.valid());
    }
    file.reset();  // Weak close leaves file alive while grp2 stays open.
    ScopedFile resurrected(H5Iget_file_id(grp2.get()));
    EXPECT_TRUE(resurrected.valid());
  }
}

TEST(H5FintTest, ObjCountAndIdsCoverTypesFiltersAndTruncation) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  ScopedFile f1(H5Fcreate("obj1.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(f1.valid());
  ScopedFile f1_reopen(H5Freopen(f1.get()));
  ASSERT_TRUE(f1_reopen.valid());
  ScopedFile f2(H5Fcreate("obj2.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(f2.valid());

  ScopedGroup g1(
      H5Gcreate2(f1.get(), "grp1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g1.valid());
  ScopedGroup g2(
      H5Gcreate2(f1_reopen.get(), "grp2", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g2.valid());

  ScopedSpace sp(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(sp.valid());
  ScopedDataset ds(H5Dcreate2(f1.get(), "dset", H5T_NATIVE_INT32, sp.get(),
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds.valid());
  ScopedAttr attr(
      H5Acreate2(ds.get(), "a", H5T_NATIVE_INT32, sp.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());

  ScopedType named_dt(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(named_dt.valid());
  ASSERT_GE(
      H5Tcommit2(f1.get(), "dt", named_dt.get(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
      0);

  // Transient mutable datatype exercises !H5T_is_named() and !H5T_is_immutable().
  ScopedType transient_dt(H5Tcopy(H5T_NATIVE_INT64));
  ASSERT_TRUE(transient_dt.valid());

  // Global across all files (includes both file handles, objects, and transient dt).
  const ssize_t all_cnt = H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_ALL);
  EXPECT_GE(all_cnt, 7);

  // Shared count vs H5F_OBJ_LOCAL on f1.
  const ssize_t f1_shared_files = H5Fget_obj_count(f1.get(), H5F_OBJ_FILE);
  const ssize_t f1_local_files =
      H5Fget_obj_count(f1.get(), H5F_OBJ_FILE | H5F_OBJ_LOCAL);
  EXPECT_EQ(f1_shared_files, 2);
  EXPECT_EQ(f1_local_files, 1);

  const ssize_t f1_all = H5Fget_obj_count(f1.get(), H5F_OBJ_ALL);
  EXPECT_GE(f1_all, 6);

  // Truncated H5Fget_obj_ids stops iteration early at H5_ITER_STOP and skips
  // subsequent object type loops cleanly.
  std::vector<hid_t> partial(3, H5I_INVALID_HID);
  const ssize_t got_partial =
      H5Fget_obj_ids(f1.get(), H5F_OBJ_ALL, partial.size(), partial.data());
  EXPECT_EQ(got_partial, 3);
  for (hid_t id : partial) {
    EXPECT_NE(id, H5I_INVALID_HID);
  }

  // Internal direct calls with f==nullptr for both local and non-local modes,
  // max_objs == 0, and app_ref == false.
  {
    ScopedApiContext ctx;
    size_t cnt = 0;
    EXPECT_GE(H5F_get_obj_count(nullptr, H5F_OBJ_ALL | H5F_OBJ_LOCAL, true, &cnt),
              0);
    EXPECT_GE(cnt, 7u);
    hid_t one_id = H5I_INVALID_HID;
    EXPECT_GE(H5F_get_obj_ids(UnwrapFile(f1.get()), H5F_OBJ_ALL, 1, &one_id,
                              false, &cnt),
              0);
    EXPECT_EQ(cnt, 1u);
    EXPECT_GE(H5F_get_obj_ids(nullptr, H5F_OBJ_ALL, 0, nullptr, true, &cnt), 0);
    EXPECT_GE(cnt, 7u);
  }
}

TEST(H5FintTest, PrefixOpenFileSearchesEnvPropExtpathRelativeAndActualName) {
  TempDir root("prefix_open");
  const auto env_bad = root.child("env_missing");
  const auto env_dir = root.child("env_dir");
  const auto prop_dir = root.child("prop_dir");
  const auto ext_dir = root.child("ext_dir");
  const auto real_dir = root.child("real_dir");
  std::error_code ec;
  std::filesystem::create_directories(env_dir, ec);
  std::filesystem::create_directories(prop_dir, ec);
  std::filesystem::create_directories(ext_dir, ec);
  std::filesystem::create_directories(real_dir, ec);

  auto create_sec2_file = [](const std::filesystem::path& path,
                             const char* grp_name = nullptr) {
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_TRUE(fapl.valid());
    ScopedFile f(
        H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
    if (grp_name != nullptr) {
      ScopedGroup g(
          H5Gcreate2(f.get(), grp_name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(g.valid());
    }
  };

  const auto abs_file = root.child("abs_target.h5");
  const auto env_file = env_dir / "in_env.h5";
  const auto env_file_vds = env_dir / "in_vds.h5";
  const auto prop_file = prop_dir / "in_prop.h5";
  const auto ext_file = ext_dir / "in_ext.h5";
  const auto actual_file = real_dir / "in_actual.h5";
  const auto real_primary = real_dir / "primary_real.h5";
  const auto symlink_primary = ext_dir / "primary_link.h5";

  create_sec2_file(abs_file, "target");
  create_sec2_file(env_file, "target");
  create_sec2_file(env_file_vds);
  create_sec2_file(prop_file);
  create_sec2_file(ext_file);
  create_sec2_file(actual_file);
  create_sec2_file(real_primary);
  std::filesystem::create_symlink(real_primary, symlink_primary, ec);
  ASSERT_FALSE(ec) << ec.message();

  // Exercise H5Lcreate_external + EFC cycle + H5Pset_elink_prefix via public API.
  const auto cyc_a = ext_dir / "cyc_a.h5";
  const auto cyc_b = ext_dir / "cyc_b.h5";
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_elink_file_cache_size(fapl.get(), 4), 0);
    {
      ScopedFile fa(
          H5Fcreate(cyc_a.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(fa.valid());
      ScopedFile fb(
          H5Fcreate(cyc_b.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(fb.valid());
      EXPECT_GE(H5Lcreate_external(cyc_b.filename().c_str(), "/", fa.get(),
                                   "to_b", H5P_DEFAULT, H5P_DEFAULT),
                0);
      EXPECT_GE(H5Lcreate_external(cyc_a.filename().c_str(), "/", fb.get(),
                                   "to_a", H5P_DEFAULT, H5P_DEFAULT),
                0);
    }
    ScopedFile fa(
        H5Fopen(cyc_a.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(fa.valid());
    ScopedObject obj_b(H5Oopen(fa.get(), "to_b/to_a", H5P_DEFAULT));
    EXPECT_TRUE(obj_b.valid());
  }

  ScopedEnvVar guard_ext("HDF5_EXT_PREFIX");
  ScopedEnvVar guard_vds("HDF5_VDS_PREFIX");
  unsetenv("HDF5_EXT_PREFIX");
  unsetenv("HDF5_VDS_PREFIX");

  ScopedPlist fapl = MakeSec2Fapl();
  ASSERT_TRUE(fapl.valid());
  ScopedFile primary(
      H5Fopen(symlink_primary.c_str(), H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(primary.valid());
  H5F_t* primary_f = UnwrapFile(primary.get());
  ASSERT_NE(primary_f, nullptr);
  EXPECT_STREQ(H5F_get_open_name(primary_f), symlink_primary.c_str());
  std::string expected_real =
      std::filesystem::canonical(real_primary, ec).native();
  EXPECT_EQ(std::string(H5F_get_actual_name(primary_f)), expected_real);

  ScopedApiContext ctx(fapl.get());
  H5F_t* found = nullptr;

  // 1. Existing absolute path opens directly.
  ASSERT_GE(H5F_prefix_open_file(false, &found, primary_f, H5F_PREFIX_ELINK,
                                 nullptr, abs_file.c_str(), H5F_ACC_RDONLY,
                                 fapl.get()),
            0);
  ASSERT_NE(found, nullptr);
  EXPECT_GE(H5F_efc_close(primary_f, found), 0);
  found = nullptr;

  // 2. Multiple HDF5_EXT_PREFIX components with and without trailing '/'.
  std::string ext_env =
      env_bad.native() + ":" + prop_dir.native() + "/:" + env_dir.native();
  ASSERT_EQ(setenv("HDF5_EXT_PREFIX", ext_env.c_str(), 1), 0);
  ASSERT_GE(H5F_prefix_open_file(false, &found, primary_f, H5F_PREFIX_ELINK,
                                 nullptr, "in_env.h5", H5F_ACC_RDONLY,
                                 fapl.get()),
            0);
  ASSERT_NE(found, nullptr);
  EXPECT_GE(H5F_efc_close(primary_f, found), 0);
  found = nullptr;

  // 3. Missing absolute path strips directory and finds basename via prop_prefix.
  unsetenv("HDF5_EXT_PREFIX");
  ASSERT_GE(H5F_prefix_open_file(false, &found, primary_f, H5F_PREFIX_ELINK,
                                 prop_dir.c_str(), "/no/such/dir/in_prop.h5",
                                 H5F_ACC_RDONLY, fapl.get()),
            0);
  ASSERT_NE(found, nullptr);
  EXPECT_GE(H5F_efc_close(primary_f, found), 0);
  found = nullptr;

  // 4. HDF5_VDS_PREFIX branch.
  ASSERT_EQ(setenv("HDF5_VDS_PREFIX", env_dir.c_str(), 1), 0);
  ASSERT_GE(H5F_prefix_open_file(false, &found, primary_f, H5F_PREFIX_VDS,
                                 nullptr, "in_vds.h5", H5F_ACC_RDONLY,
                                 fapl.get()),
            0);
  ASSERT_NE(found, nullptr);
  EXPECT_GE(H5F_efc_close(primary_f, found), 0);
  found = nullptr;
  unsetenv("HDF5_VDS_PREFIX");

  // 5. H5F_EXTPATH(primary_f) resolves sibling in symlink directory.
  ASSERT_GE(H5F_prefix_open_file(false, &found, primary_f, H5F_PREFIX_ELINK,
                                 nullptr, "in_ext.h5", H5F_ACC_RDONLY,
                                 fapl.get()),
            0);
  ASSERT_NE(found, nullptr);
  EXPECT_GE(H5F_efc_close(primary_f, found), 0);
  found = nullptr;

  // 6. H5F_ACTUAL_NAME fallback resolves sibling in resolved symlink target dir.
  ASSERT_GE(H5F_prefix_open_file(false, &found, primary_f, H5F_PREFIX_ELINK,
                                 nullptr, "in_actual.h5", H5F_ACC_RDONLY,
                                 fapl.get()),
            0);
  ASSERT_NE(found, nullptr);
  EXPECT_GE(H5F_efc_close(primary_f, found), 0);
  found = nullptr;

  // 7. Invalid prefix_type and missing target (try=true vs try=false).
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5F_prefix_open_file(false, &found, primary_f,
                                   static_cast<H5F_prefix_open_t>(99), nullptr,
                                   "missing.h5", H5F_ACC_RDONLY, fapl.get()),
              0);
    EXPECT_EQ(found, nullptr);

    EXPECT_LT(H5F_prefix_open_file(false, &found, primary_f, H5F_PREFIX_ELINK,
                                   nullptr, "definitely_missing.h5",
                                   H5F_ACC_RDONLY, fapl.get()),
              0);
    EXPECT_EQ(found, nullptr);
  }
  EXPECT_GE(H5F_prefix_open_file(true, &found, primary_f, H5F_PREFIX_ELINK,
                                 nullptr, "definitely_missing.h5",
                                 H5F_ACC_RDONLY, fapl.get()),
            0);
  EXPECT_EQ(found, nullptr);
}

TEST(H5FintTest, IsHdf5AndOpenFailureAndDriverEdgeCases) {
  TempDir dir("open_edges");
  const auto sec2_path = dir.child("valid.h5");
  const auto non_h5_path = dir.child("plain.txt");
  const auto split_base = dir.child("split_file");
  const auto family_path = dir.child("family_%05d.h5");
  const auto dirty_copy = dir.child("dirty_copy.h5");

  {
    std::ofstream ofs(non_h5_path);
    ofs << "not an hdf5 signature\n";
  }

  // Create a valid sec2 file with libver LATEST (v3 superblock).
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST,
                                   H5F_LIBVER_LATEST),
              0);
    ScopedFile f(H5Fcreate(sec2_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                           fapl.get()));
    ASSERT_TRUE(f.valid());

    // Check accessibility while already open (hits H5F__sfile_search != NULL).
    EXPECT_GT(H5Fis_accessible(sec2_path.c_str(), fapl.get()), 0);

    // Verify internal NULL is_hdf5 output returns FAIL.
    ScopedApiContext ctx;
    ScopedErrorSilence silence;
    EXPECT_LT(H5F__is_hdf5(sec2_path.c_str(), fapl.get(), nullptr), 0);
  }

  // H5Fis_accessible on closed valid, non-HDF5, and missing file.
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_TRUE(fapl.valid());
    EXPECT_GT(H5Fis_accessible(sec2_path.c_str(), fapl.get()), 0);
    EXPECT_EQ(H5Fis_accessible(non_h5_path.c_str(), fapl.get()), 0);
    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Fis_accessible(dir.child("missing.h5").c_str(), fapl.get()), 0);
  }

  // Reopen conflicts on already-open sec2 file + lock/fc_degree/evict/flags.
  {
    ScopedPlist fapl_rw = MakeSec2Fapl();
    ASSERT_TRUE(fapl_rw.valid());
    ASSERT_GE(H5Pset_file_locking(fapl_rw.get(), true, false), 0);
    ASSERT_GE(H5Pset_fclose_degree(fapl_rw.get(), H5F_CLOSE_SEMI), 0);
    ASSERT_GE(H5Pset_evict_on_close(fapl_rw.get(), true), 0);

    ScopedFile open_rw(
        H5Fopen(sec2_path.c_str(), H5F_ACC_RDWR, fapl_rw.get()));
    ASSERT_TRUE(open_rw.valid());

    ScopedErrorSilence silence;
    ScopedApiContext ctx(fapl_rw.get());
    H5F_t* err_f = nullptr;

    // H5F_ACC_TRUNC and H5F_ACC_EXCL on already-open file fail.
    EXPECT_LT(H5F_open(false, &err_f, sec2_path.c_str(),
                       H5F_ACC_RDWR | H5F_ACC_TRUNC, H5P_FILE_CREATE_DEFAULT,
                       fapl_rw.get()),
              0);
    EXPECT_LT(H5F_open(false, &err_f, sec2_path.c_str(),
                       H5F_ACC_RDWR | H5F_ACC_EXCL, H5P_FILE_CREATE_DEFAULT,
                       fapl_rw.get()),
              0);

    // Try opening non-existent file with try=true succeeds with nullptr.
    H5F_t* try_missing = nullptr;
    EXPECT_GE(H5F_open(true, &try_missing, dir.child("nope.h5").c_str(),
                       H5F_ACC_RDONLY, H5P_FILE_CREATE_DEFAULT, fapl_rw.get()),
              0);
    EXPECT_EQ(try_missing, nullptr);

    // SWMR_WRITE over non-SWMR_WRITE fails.
    ScopedPlist fapl_def = MakeSec2Fapl();
    EXPECT_EQ(
        H5Fopen(sec2_path.c_str(), H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE,
                fapl_def.get()),
        H5I_INVALID_HID);

    // Mismatched file locking flags.
    {
      ScopedPlist fapl_nolock = MakeSec2Fapl();
      ASSERT_GE(H5Pset_file_locking(fapl_nolock.get(), false, false), 0);
      EXPECT_EQ(H5Fopen(sec2_path.c_str(), H5F_ACC_RDONLY, fapl_nolock.get()),
                H5I_INVALID_HID);
    }
    // Mismatched ignore_disabled_locks.
    {
      ScopedPlist fapl_ign = MakeSec2Fapl();
      ASSERT_GE(H5Pset_file_locking(fapl_ign.get(), true, true), 0);
      EXPECT_EQ(H5Fopen(sec2_path.c_str(), H5F_ACC_RDONLY, fapl_ign.get()),
                H5I_INVALID_HID);
    }
    // Mismatched fc_degree (default WEAK vs SEMI, and STRONG vs SEMI).
    {
      ScopedPlist fapl_weak = MakeSec2Fapl();
      EXPECT_EQ(H5Fopen(sec2_path.c_str(), H5F_ACC_RDONLY, fapl_weak.get()),
                H5I_INVALID_HID);
      ScopedPlist fapl_strong = MakeSec2Fapl();
      ASSERT_GE(H5Pset_fclose_degree(fapl_strong.get(), H5F_CLOSE_STRONG), 0);
      EXPECT_EQ(H5Fopen(sec2_path.c_str(), H5F_ACC_RDONLY, fapl_strong.get()),
                H5I_INVALID_HID);
    }
    // Mismatched evict_on_close.
    {
      ScopedPlist fapl_ev = MakeSec2Fapl();
      ASSERT_GE(H5Pset_file_locking(fapl_ev.get(), true, false), 0);
      ASSERT_GE(H5Pset_fclose_degree(fapl_ev.get(), H5F_CLOSE_SEMI), 0);
      ASSERT_GE(H5Pset_evict_on_close(fapl_ev.get(), false), 0);
      EXPECT_EQ(H5Fopen(sec2_path.c_str(), H5F_ACC_RDONLY, fapl_ev.get()),
                H5I_INVALID_HID);
    }

    // Copy the still-open v3 file bytes while WRITE_ACCESS is flushed on disk!
    // Copying preserves checksum + non-zero status_flags for dirty-open tests.
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::copy_file(sec2_path, dirty_copy, ec))
        << ec.message();
  }

  // Dirty-flag v3 superblock open rejection & H5F_ACS_CLEAR_STATUS_FLAGS_NAME.
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_TRUE(fapl.valid());
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5Fopen(dirty_copy.c_str(), H5F_ACC_RDWR, fapl.get()),
                H5I_INVALID_HID);
      EXPECT_EQ(H5Fopen(dirty_copy.c_str(), H5F_ACC_RDONLY, fapl.get()),
                H5I_INVALID_HID);
      EXPECT_EQ(
          H5Fopen(dirty_copy.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ,
                  fapl.get()),
          H5I_INVALID_HID);
    }

    // Open with private clear_status_flags property succeeds and clears flags.
    {
      ScopedPlist fapl_clear = MakeSec2Fapl();
      ASSERT_TRUE(fapl_clear.valid());
      ScopedApiContext ctx;
      auto* pl = static_cast<H5P_genplist_t*>(H5I_object(fapl_clear.get()));
      ASSERT_NE(pl, nullptr);
      bool clear_flag = true;
      ASSERT_GE(H5P_set(pl, H5F_ACS_CLEAR_STATUS_FLAGS_NAME, &clear_flag), 0);
      ScopedFile cleared(
          H5Fopen(dirty_copy.c_str(), H5F_ACC_RDWR, fapl_clear.get()));
      EXPECT_TRUE(cleared.valid());
    }
    // Reopening normally now succeeds!
    ScopedFile clean_reopen(
        H5Fopen(dirty_copy.c_str(), H5F_ACC_RDONLY, fapl.get()));
    EXPECT_TRUE(clean_reopen.valid());
  }

  // Already-open RDONLY conflicts: RDWR over RDONLY and SWMR_READ over non-SWMR RDONLY.
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_TRUE(fapl.valid());
    ScopedFile ro(H5Fopen(sec2_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(ro.valid());

    ScopedErrorSilence silence;
    EXPECT_EQ(H5Fopen(sec2_path.c_str(), H5F_ACC_RDWR, fapl.get()),
              H5I_INVALID_HID);
    EXPECT_EQ(
        H5Fopen(sec2_path.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ,
                fapl.get()),
        H5I_INVALID_HID);
  }

  // Split VFD + paged strategy error, Core + SWMR_WRITE error, SWMR + cache image error,
  // and Onion VFD (drvr->cmp == NULL).
  {
    ScopedErrorSilence silence;

    // Split + paged strategy fails in H5F__new.
    ScopedPlist fcpl_paged(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl_paged.valid());
    ASSERT_GE(H5Pset_file_space_strategy(fcpl_paged.get(),
                                         H5F_FSPACE_STRATEGY_PAGE, true, 1),
              0);
    ScopedPlist fapl_split(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl_split.valid());
    ASSERT_GE(H5Pset_fapl_split(fapl_split.get(), "-m.h5", H5P_DEFAULT,
                                "-r.h5", H5P_DEFAULT),
              0);
    EXPECT_EQ(H5Fcreate(split_base.c_str(), H5F_ACC_TRUNC, fcpl_paged.get(),
                        fapl_split.get()),
              H5I_INVALID_HID);

    // Core VFD + SWMR_WRITE fails in H5F__new.
    ScopedPlist fapl_core = MakeCoreFapl();
    EXPECT_EQ(H5Fcreate("core_swmr.h5", H5F_ACC_TRUNC | H5F_ACC_SWMR_WRITE,
                        H5P_DEFAULT, fapl_core.get()),
              H5I_INVALID_HID);

    // Sec2 + SWMR_WRITE + cache image fails in H5F_open.
    ScopedPlist fapl_ci = MakeSec2Fapl();
    H5AC_cache_image_config_t ci_cfg{};
    ci_cfg.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
    ci_cfg.generate_image = true;
    ci_cfg.save_resize_status = false;
    ci_cfg.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE;
    ASSERT_GE(H5Pset_mdc_image_config(fapl_ci.get(), &ci_cfg), 0);
    EXPECT_EQ(H5Fcreate(dir.child("swmr_ci.h5").c_str(),
                        H5F_ACC_TRUNC | H5F_ACC_SWMR_WRITE, H5P_DEFAULT,
                        fapl_ci.get()),
              H5I_INVALID_HID);

    // Onion VFD creation & open hit drvr->cmp == NULL branch in H5F_open.
    ScopedPlist fapl_sec2_child = MakeSec2Fapl();
    ScopedPlist fapl_onion(H5Pcreate(H5P_FILE_ACCESS));
    H5FD_onion_fapl_info_t onion_info{};
    onion_info.version = H5FD_ONION_FAPL_INFO_VERSION_CURR;
    onion_info.backing_fapl_id = fapl_sec2_child.get();
    onion_info.page_size = 4096;
    onion_info.store_target = H5FD_ONION_STORE_TARGET_ONION;
    onion_info.revision_num = H5FD_ONION_FAPL_INFO_REVISION_ID_LATEST;
    ASSERT_GE(H5Pset_fapl_onion(fapl_onion.get(), &onion_info), 0);
    const auto onion_path = dir.child("onion_test.h5");
    ScopedFile onion_f(H5Fcreate(onion_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                                 fapl_onion.get()));
    EXPECT_TRUE(onion_f.valid());
  }

  // Family VFD creates drvinfo entry (unpinned in H5F__dest) + fails get_file_image.
  {
    ScopedPlist fapl_fam(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl_fam.valid());
    ASSERT_GE(H5Pset_fapl_family(fapl_fam.get(), 65536, H5P_DEFAULT), 0);
    ScopedFile fam_f(H5Fcreate(family_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                               fapl_fam.get()));
    ASSERT_TRUE(fam_f.valid());
    ScopedErrorSilence silence;
    EXPECT_LT(H5Fget_file_image(fam_f.get(), nullptr, 0), 0);
  }
}

TEST(H5FintTest, PageBufferSizeAdjustmentOnOpenAndDelete) {
  TempDir dir("pb_and_del");
  const auto paged_path = dir.child("paged_small.h5");
  const auto unpaged_path = dir.child("unpaged.h5");
  const auto del_path = dir.child("to_delete.h5");

  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_GE(
        H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE, true, 1),
        0);
    ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), 4096), 0);
    ScopedPlist fapl = MakeSec2Fapl();
    ScopedFile f(H5Fcreate(paged_path.c_str(), H5F_ACC_TRUNC, fcpl.get(),
                           fapl.get()));
    ASSERT_TRUE(f.valid());
  }
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ScopedFile f(H5Fcreate(unpaged_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                           fapl.get()));
    ASSERT_TRUE(f.valid());
  }

  // Open paged file (page_size 4096) with smaller page_buf_size (1024): bumps to 4096.
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_GE(H5Pset_page_buffer_size(fapl.get(), 1024, 0, 0), 0);
    ScopedFile f(H5Fopen(paged_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(f.valid());
    ScopedPlist got(H5Fget_access_plist(f.get()));
    size_t pb = 0;
    EXPECT_GE(H5Pget_page_buffer_size(got.get(), &pb, nullptr, nullptr), 0);
    EXPECT_EQ(pb, 4096u);
  }

  // Open unpaged file with page_buf_size == H5F_PAGE_BUFFER_SIZE_DEFAULT: resets to 0.
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ScopedApiContext ctx;
    auto* pl = static_cast<H5P_genplist_t*>(H5I_object(fapl.get()));
    ASSERT_NE(pl, nullptr);
    size_t def_sz = H5F_PAGE_BUFFER_SIZE_DEFAULT;
    ASSERT_GE(H5P_set(pl, H5F_ACS_PAGE_BUFFER_SIZE_NAME, &def_sz), 0);
    ScopedFile f(H5Fopen(unpaged_path.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(f.valid());
    ScopedPlist got(H5Fget_access_plist(f.get()));
    size_t pb = 999;
    EXPECT_GE(H5Pget_page_buffer_size(got.get(), &pb, nullptr, nullptr), 0);
    EXPECT_EQ(pb, 0u);
  }

  // Create & delete via H5Fdelete (covers H5F__delete).
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ScopedFile f(
        H5Fcreate(del_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
  }
  {
    ScopedPlist fapl = MakeSec2Fapl();
    EXPECT_GE(H5Fdelete(del_path.c_str(), fapl.get()), 0);
    EXPECT_FALSE(std::filesystem::exists(del_path));
  }
}

TEST(H5FintTest, FlushCloseDegreesMountsReopenAndFileIdResurrection) {
  // 1. SEMI close degree: fails when nopen_files == 1 && nopen_objs > 0;
  // succeeds when another mounted file keeps nopen_files > 0 in H5F_try_close.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_GE(H5Pset_fclose_degree(fapl.get(), H5F_CLOSE_SEMI), 0);
    ScopedFile f1(
        H5Fcreate("semi.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f1.valid());
    ScopedGroup g(
        H5Gcreate2(f1.get(), "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());

    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Fclose(f1.get()), 0);
    }
    EXPECT_EQ(H5F_get_nopen_objs(UnwrapFile(f1.get())), 1u);

    // Reopen + flush coverage (reopen creates an independent H5F_t sharing
    // H5F_shared_t).
    ScopedFile f2(H5Freopen(f1.get()));
    ASSERT_TRUE(f2.valid());
    EXPECT_EQ(H5F_get_nrefs(UnwrapFile(f1.get())), 2u);
    EXPECT_GE(H5Fflush(f2.get(), H5F_SCOPE_LOCAL), 0);
    EXPECT_GE(H5Fflush(f2.get(), H5F_SCOPE_GLOBAL), 0);

    g.reset();
    f1.reset();
    f2.reset();

    // Mounted SEMI pair: nopen_files == 2 allows child close while parent &
    // parent's group stay open, hitting H5F_CLOSE_SEMI nopen_files > 0 branch.
    ScopedFile semi_parent(
        H5Fcreate("semi_parent.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ScopedFile semi_child(
        H5Fcreate("semi_child.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(semi_parent.valid());
    ASSERT_TRUE(semi_child.valid());
    ScopedGroup semi_mnt(H5Gcreate2(semi_parent.get(), "mnt", H5P_DEFAULT,
                                    H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(semi_mnt.valid());
    ASSERT_GE(H5Fmount(semi_parent.get(), "/mnt", semi_child.get(), H5P_DEFAULT),
              0);
    EXPECT_GE(H5Fclose(semi_child.release()), 0);
    semi_mnt.reset();
    semi_parent.reset();
  }

  // 2. STRONG close degree forcibly closes group, dataset, attr, and committed dt.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_GE(H5Pset_fclose_degree(fapl.get(), H5F_CLOSE_STRONG), 0);
    const hid_t fid =
        H5Fcreate("strong.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get());
    ASSERT_NE(H5I_INVALID_HID, fid);
    const hid_t gid =
        H5Gcreate2(fid, "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    const hid_t sp = H5Screate(H5S_SCALAR);
    const hid_t dt = H5Tcopy(H5T_NATIVE_INT32);
    ASSERT_GE(H5Tcommit2(fid, "dt", dt, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              0);
    const hid_t ds = H5Dcreate2(fid, "ds", dt, sp, H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT);
    const hid_t attr =
        H5Acreate2(ds, "a", dt, sp, H5P_DEFAULT, H5P_DEFAULT);
    ASSERT_NE(H5I_INVALID_HID, gid);
    ASSERT_NE(H5I_INVALID_HID, ds);
    ASSERT_NE(H5I_INVALID_HID, attr);

    // Closing fid triggers H5F_try_close H5F_CLOSE_STRONG loop closing children.
    EXPECT_GE(H5Fclose(fid), 0);
    EXPECT_GE(H5Sclose(sp), 0);
    // Child handles are still open as app refs? H5I_dec_ref(false) leaves app ref=1
    // unless closed here; close remaining app handles cleanly if valid.
    ScopedErrorSilence silence;
    (void)H5Aclose(attr);
    (void)H5Dclose(ds);
    (void)H5Tclose(dt);
    (void)H5Gclose(gid);
  }

  // 3. WEAK close + Mount hierarchy + H5F_get_id / H5F_get_file_id resurrection.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_GE(H5Pset_fclose_degree(fapl.get(), H5F_CLOSE_WEAK), 0);
    ScopedFile parent(
        H5Fcreate("parent.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ScopedFile child(
        H5Fcreate("child.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(parent.valid());
    ASSERT_TRUE(child.valid());
    ScopedGroup mnt_pt(
        H5Gcreate2(parent.get(), "mnt", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(mnt_pt.valid());
    ASSERT_GE(H5Fmount(parent.get(), "/mnt", child.get(), H5P_DEFAULT), 0);

    H5F_t* parent_f = UnwrapFile(parent.get());
    EXPECT_EQ(H5F_get_nmounts(parent_f), 1u);

    // Existing file ID branch in H5F_get_id.
    {
      ScopedApiContext ctx;
      const hid_t inc_id = H5F_get_id(parent_f);
      ASSERT_NE(H5I_INVALID_HID, inc_id);
      EXPECT_GE(H5I_dec_ref(inc_id), 0);
    }

    // Close parent weakly while child stays open, then resurrect parent ID via
    // H5F_get_id with VOL wrapper context set, then close both.
    mnt_pt.reset();
    ScopedGroup reopen_mnt(H5Gopen2(parent.get(), "/mnt", H5P_DEFAULT));
    ASSERT_TRUE(reopen_mnt.valid());
    parent.reset();
    EXPECT_FALSE(H5F_file_id_exists(parent_f));

    {
      ScopedApiContext ctx;
      H5VL_object_t* mnt_vol_obj = H5VL_vol_object(reopen_mnt.get());
      ASSERT_NE(mnt_vol_obj, nullptr);
      ASSERT_GE(H5VL_set_vol_wrapper(mnt_vol_obj), 0);
      absl::Cleanup reset_wrapper = [] { H5VL_reset_vol_wrapper(); };
      const hid_t res_id = H5F_get_id(parent_f);
      ASSERT_NE(H5I_INVALID_HID, res_id);
      EXPECT_TRUE(H5F_file_id_exists(parent_f));
      EXPECT_GE(H5I_dec_ref(res_id), 0);
    }

    reopen_mnt.reset();
    // Closing child triggers H5F_try_close(f->parent) up mount hierarchy!
    child.reset();
  }
}

TEST(H5FintTest, AddrEncodeDecodeMetadataSettersImageInfoRetriesAndFlushCb) {
  // 1. Address encode/decode across lengths and HADDR_UNDEF.
  for (size_t addr_len : {size_t{2}, size_t{4}, size_t{8}}) {
    uint8_t buf[16]{};
    uint8_t* enc_p = buf;
    constexpr haddr_t kVal = 0x1234;
    H5F_addr_encode_len(addr_len, &enc_p, kVal);
    H5F_addr_encode_len(addr_len, &enc_p, HADDR_UNDEF);
    EXPECT_EQ(static_cast<size_t>(enc_p - buf), addr_len * 2);

    const uint8_t* dec_p = buf;
    haddr_t out1 = 0;
    haddr_t out2 = 0;
    H5F_addr_decode_len(addr_len, &dec_p, &out1);
    H5F_addr_decode_len(addr_len, &dec_p, &out2);
    EXPECT_EQ(out1, kVal);
    EXPECT_EQ(out2, HADDR_UNDEF);
  }

  // 2. Superblock v0 vs v2 file images, error checks, info2 (with SOHM), setters,
  // metadata read retries, object flush cb, and VFD helpers.
  TempDir dir("img_retries");
  const auto sec2_path = dir.child("swmr_retries.h5");

  // Core v0 & v2 images.
  {
    ScopedFile v0 = CreateInMemoryFile("v0.h5");
    ASSERT_TRUE(v0.valid());
    H5F_t* f0 = UnwrapFile(v0.get());

    uint8_t fbuf[16]{};
    uint8_t* ep = fbuf;
    H5F_addr_encode(f0, &ep, haddr_t{0xdeadbeefULL});
    const uint8_t* dp = fbuf;
    haddr_t dec = 0;
    H5F_addr_decode(f0, &dp, &dec);
    EXPECT_EQ(dec, 0xdeadbeefULL);

    const ssize_t sz0 = H5Fget_file_image(v0.get(), nullptr, 0);
    ASSERT_GT(sz0, 0);
    std::vector<uint8_t> img0(static_cast<size_t>(sz0));
    EXPECT_EQ(H5Fget_file_image(v0.get(), img0.data(), img0.size()), sz0);

    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Fget_file_image(v0.get(), img0.data(), 8), 0);
    }
  }

  // Split driver fails H5Fget_file_image.
  {
    ScopedPlist fapl_split(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(H5Pset_fapl_split(fapl_split.get(), "-m.h5", H5P_DEFAULT,
                                "-r.h5", H5P_DEFAULT),
              0);
    const auto sp_base = dir.child("split_img");
    ScopedFile sf(H5Fcreate(sp_base.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                            fapl_split.get()));
    ASSERT_TRUE(sf.valid());
    ScopedErrorSilence silence;
    EXPECT_LT(H5Fget_file_image(sf.get(), nullptr, 0), 0);
  }

  // v3 file with SOHM, flush cb (success and failure), retries, VFD setters.
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
    ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_ATTR_FLAG, 1),
              0);

    struct FlushState {
      int calls = 0;
      herr_t rc = 0;
      hid_t last_id = H5I_INVALID_HID;
    } flush_state;
    auto cb = +[](hid_t obj_id, void* ctx) -> herr_t {
      auto* st = static_cast<FlushState*>(ctx);
      ++st->calls;
      st->last_id = obj_id;
      return st->rc;
    };

    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110,
                                   H5F_LIBVER_LATEST),
              0);
    ASSERT_GE(H5Pset_object_flush_cb(fapl.get(), cb, &flush_state), 0);

    ScopedFile f(H5Fcreate(sec2_path.c_str(),
                           H5F_ACC_TRUNC | H5F_ACC_SWMR_WRITE, fcpl.get(),
                           fapl.get()));
    ASSERT_TRUE(f.valid());
    H5F_t* hf = UnwrapFile(f.get());

    // Add attribute big enough to initialize SOHM table so H5SM_ih_size branch runs.
    ScopedSpace sp(H5Screate(H5S_SCALAR));
    ScopedAttr attr(H5Acreate2(f.get(), "shared_attr", H5T_NATIVE_INT64,
                               sp.get(), H5P_DEFAULT, H5P_DEFAULT));
    int64_t aval = 42;
    ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT64, &aval), 0);
    attr.reset();

    // Test H5Fobject_flush_cb success and error return.
    ScopedGroup g(
        H5Gcreate2(f.get(), "flush_g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
    EXPECT_GE(H5Oflush(g.get()), 0);
    EXPECT_EQ(flush_state.calls, 1);
    EXPECT_EQ(flush_state.last_id, g.get());
    flush_state.rc = -1;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Oflush(g.get()), 0);
    }
    flush_state.rc = 0;
    g.reset();

    // v3 file image recalculates superblock checksum.
    const ssize_t sz3 = H5Fget_file_image(f.get(), nullptr, 0);
    ASSERT_GT(sz3, 0);
    std::vector<uint8_t> img3(static_cast<size_t>(sz3));
    EXPECT_EQ(H5Fget_file_image(f.get(), img3.data(), img3.size()), sz3);

    H5F_info2_t finfo{};
    ASSERT_GE(H5Fget_info2(f.get(), &finfo), 0);
    EXPECT_EQ(finfo.super.version, 3u);

    // Test read retry tracking & retrieval.
    {
      ScopedApiContext ctx;
      EXPECT_GE(H5F_track_metadata_read_retries(hf, H5AC_OHDR_ID, 1), 0);
      EXPECT_GE(H5F_track_metadata_read_retries(hf, H5AC_SUPERBLOCK_ID, 12), 0);
      EXPECT_GE(H5F_track_metadata_read_retries(hf, H5AC_BT_ID, 2), 0);
    }
    H5F_retry_info_t rinfo{};
    ASSERT_GE(H5Fget_metadata_read_retry_info(f.get(), &rinfo), 0);
    absl::Cleanup free_retries = [&] {
      for (auto* p : rinfo.retries) {
        if (p != nullptr) {
          H5free_memory(p);
        }
      }
    };
    EXPECT_EQ(rinfo.nbins, 2u);
    ASSERT_NE(rinfo.retries[0], nullptr);  // H5AC_OHDR_ID maps to index 0
    EXPECT_EQ(rinfo.retries[0][0], 1u);    // log10(1) == 0

    // Test internal metadata/VFD setters & getters.
    haddr_t saved_base = H5F_get_base_addr(hf);
    H5UC_t* saved_bt = H5F_grp_btree_shared(hf);
    const haddr_t saved_sohm = H5F_get_sohm_addr(hf);
    const unsigned saved_svers = H5F_get_sohm_vers(hf);
    const unsigned saved_snidx = H5F_get_sohm_nindexes(hf);
    const bool saved_crt = H5F_store_msg_crt_idx(hf);
    const bool saved_min = H5F_get_min_dset_ohdr(hf);
    haddr_t max_eof_eoa = 0;

    {
      ScopedApiContext ctx;
      EXPECT_GE(H5F_set_grp_btree_shared(hf, saved_bt), 0);
      EXPECT_GE(H5F_set_sohm_addr(hf, saved_sohm), 0);
      EXPECT_GE(H5F_set_sohm_vers(hf, saved_svers), 0);
      EXPECT_GE(H5F_set_sohm_nindexes(hf, saved_snidx), 0);
      EXPECT_GE(H5F_set_store_msg_crt_idx(hf, !saved_crt), 0);
      EXPECT_EQ(H5F_store_msg_crt_idx(hf), !saved_crt);
      EXPECT_GE(H5F_set_store_msg_crt_idx(hf, saved_crt), 0);
      EXPECT_GE(H5F_set_min_dset_ohdr(hf, !saved_min), 0);
      EXPECT_EQ(H5F_get_min_dset_ohdr(hf), !saved_min);
      EXPECT_GE(H5F_set_min_dset_ohdr(hf, saved_min), 0);
      EXPECT_GE(H5F__get_max_eof_eoa(hf, &max_eof_eoa), 0);
      EXPECT_GT(max_eof_eoa, 0u);
      EXPECT_GE(H5F__set_paged_aggr(hf, true), 0);
      EXPECT_GE(H5F__set_paged_aggr(hf, false), 0);
      EXPECT_GE(H5F__set_base_addr(hf, saved_base), 0);
      EXPECT_GE(H5F__set_eoa(hf, H5FD_MEM_DEFAULT, max_eof_eoa + 64), 0);
    }

    // No-retries case (nbins == 0).
    ScopedFile simple = CreateInMemoryFile("simple_retry.h5");
    H5F_retry_info_t empty_rinfo{};
    ASSERT_GE(H5Fget_metadata_read_retry_info(simple.get(), &empty_rinfo), 0);
    EXPECT_EQ(empty_rinfo.nbins, 0u);
  }
}

TEST(H5FintTest, StartSwmrWriteValidationAndSuccessAndFormatConvert) {
  TempDir dir("swmr_and_conv");

  // 1. H5Fformat_convert downgrades v3 paged superblock & removes FSINFO message.
  {
    const auto conv_path = dir.child("conv.h5");
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_GE(
        H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE, true, 1),
        0);
    ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), 4096), 0);
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST,
                                   H5F_LIBVER_LATEST),
              0);

    {
      ScopedFile f(H5Fcreate(conv_path.c_str(), H5F_ACC_TRUNC, fcpl.get(),
                             fapl.get()));
      ASSERT_TRUE(f.valid());
      ScopedGroup g(
          H5Gcreate2(f.get(), "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(g.valid());
      g.reset();
      ASSERT_GE(H5Fformat_convert(f.get()), 0);
      H5F_info2_t finfo{};
      ASSERT_GE(H5Fget_info2(f.get(), &finfo), 0);
      EXPECT_EQ(finfo.super.version, 2u);
    }

    // Converting an already v2 non-paged file is a clean no-op.
    ScopedFile v2_f(H5Fopen(conv_path.c_str(), H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(v2_f.valid());
    EXPECT_GE(H5Fformat_convert(v2_f.get()), 0);
  }

  // 2. H5Fstart_swmr_write error paths.
  {
    ScopedErrorSilence silence;
    const auto p_v18 = dir.child("swmr_v18.h5");
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_V18),
              0);
    {
      ScopedFile f(
          H5Fcreate(p_v18.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
      EXPECT_LT(H5Fstart_swmr_write(f.get()), 0);  // super_vers < 3
    }
    {
      ScopedFile f_ro(H5Fopen(p_v18.c_str(), H5F_ACC_RDONLY, fapl.get()));
      ASSERT_TRUE(f_ro.valid());
      EXPECT_LT(H5Fstart_swmr_write(f_ro.get()), 0);  // RDONLY
    }

    // MDC cache image enabled fails H5Fstart_swmr_write.
    const auto p_ci = dir.child("swmr_ci_start.h5");
    ScopedPlist fapl_ci = MakeSec2Fapl();
    ASSERT_GE(H5Pset_libver_bounds(fapl_ci.get(), H5F_LIBVER_V110,
                                   H5F_LIBVER_LATEST),
              0);
    H5AC_cache_image_config_t ci_cfg{};
    ci_cfg.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
    ci_cfg.generate_image = true;
    ci_cfg.save_resize_status = false;
    ci_cfg.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE;
    ASSERT_GE(H5Pset_mdc_image_config(fapl_ci.get(), &ci_cfg), 0);
    {
      ScopedFile f(H5Fcreate(p_ci.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                             fapl_ci.get()));
      ASSERT_TRUE(f.valid());
      EXPECT_LT(H5Fstart_swmr_write(f.get()), 0);
    }
  }

  // 3. Object/bound failure cases & full success with open group and dataset.
  {
    const auto p_ok = dir.child("swmr_ok.h5");
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110,
                                   H5F_LIBVER_LATEST),
              0);
    ScopedFile f(
        H5Fcreate(p_ok.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());

    // Open attribute blocks start_swmr_write; closing it unblocks.
    {
      ScopedSpace sp(H5Screate(H5S_SCALAR));
      ScopedAttr a(H5Acreate2(f.get(), "tmp_attr", H5T_NATIVE_INT32, sp.get(),
                              H5P_DEFAULT, H5P_DEFAULT));
      ScopedErrorSilence silence;
      EXPECT_LT(H5Fstart_swmr_write(f.get()), 0);
    }

    // Lowering libver_bounds below 1.10 blocks, creating an old object also blocks.
    {
      ScopedErrorSilence silence;
      ASSERT_GE(H5Fset_libver_bounds(f.get(), H5F_LIBVER_EARLIEST,
                                     H5F_LIBVER_V110),
                0);
      EXPECT_LT(H5Fstart_swmr_write(f.get()), 0);
      ScopedGroup old_grp(H5Gcreate2(f.get(), "old_grp", H5P_DEFAULT,
                                     H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_GE(H5Fset_libver_bounds(f.get(), H5F_LIBVER_V110,
                                     H5F_LIBVER_LATEST),
                0);
      EXPECT_LT(H5Fstart_swmr_write(f.get()), 0);  // open < 1.10 object
    }
    // Identical bounds no-op check for H5Fset_libver_bounds.
    EXPECT_GE(H5Fset_libver_bounds(f.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST),
              0);

    // Open group + chunked dataset survive H5Fstart_swmr_write() refresh.
    ScopedGroup grp(
        H5Gcreate2(f.get(), "live_grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    constexpr hsize_t kDim = 4;
    constexpr hsize_t kMaxDim = H5S_UNLIMITED;
    ScopedSpace dsp(H5Screate_simple(1, &kDim, &kMaxDim));
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &kDim), 0);
    ScopedDataset ds(H5Dcreate2(f.get(), "live_ds", H5T_NATIVE_INT32, dsp.get(),
                                H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());
    constexpr int32_t kData[4] = {10, 20, 30, 40};
    ASSERT_GE(
        H5Dwrite(ds.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, kData),
        0);

    ASSERT_GE(H5Fstart_swmr_write(f.get()), 0);

    // Calling H5Fstart_swmr_write again fails (already in SWMR_WRITE), and
    // H5Fset_libver_bounds is skipped in SWMR_WRITE mode.
    EXPECT_GE(H5Fset_libver_bounds(f.get(), H5F_LIBVER_V18, H5F_LIBVER_V110),
              0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Fstart_swmr_write(f.get()), 0);
    }

    int32_t read_back[4]{};
    EXPECT_GE(H5Dread(ds.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      read_back),
              0);
    EXPECT_THAT(read_back, ElementsAre(10, 20, 30, 40));
  }
}

// ---------- Custom Fault-Injecting VFD & VOL Infrastructure ----------

struct FaultTrigger {
  int skip = -1;
  int count = 1;

  void Reset() {
    skip = -1;
    count = 1;
  }

  void FailAfter(int n_ok, int n_fail = 1) {
    skip = n_ok;
    count = n_fail;
  }

  bool Consume() {
    if (skip < 0) return false;
    if (skip > 0) {
      --skip;
      return false;
    }
    if (count == 0) return false;
    if (count > 0) --count;
    return true;
  }
};

struct FaultVfdState {
  FaultTrigger open;
  FaultTrigger close;
  FaultTrigger read;
  FaultTrigger write;
  FaultTrigger lock;
  FaultTrigger unlock;
  FaultTrigger flush;
  FaultTrigger truncate;
  FaultTrigger get_eoa;
  FaultTrigger set_eoa;
  FaultTrigger get_eof;
  FaultTrigger get_handle;
  FaultTrigger get_type_map;
  FaultTrigger del;
  bool fail_fapl_copy = false;
  bool fail_fapl_free = false;
  bool corrupt_maxaddr = false;
  bool override_handle = false;
  int override_fd = -1;
  std::string redirect_from;
  std::string redirect_to;

  void Reset() {
    open.Reset();
    close.Reset();
    read.Reset();
    write.Reset();
    lock.Reset();
    unlock.Reset();
    flush.Reset();
    truncate.Reset();
    get_eoa.Reset();
    set_eoa.Reset();
    get_eof.Reset();
    get_handle.Reset();
    get_type_map.Reset();
    del.Reset();
    fail_fapl_copy = false;
    fail_fapl_free = false;
    corrupt_maxaddr = false;
    override_handle = false;
    override_fd = -1;
    redirect_from.clear();
    redirect_to.clear();
  }
};

FaultVfdState& GetFaultVfdState() {
  static auto* state = new FaultVfdState();
  return *state;
}

const H5FD_class_t* g_sec2_cls = nullptr;

extern "C" {

void* FaultVfdFaplGet(H5FD_t*) {
  return std::calloc(1, sizeof(int));
}

void* FaultVfdFaplCopy(const void* old_fa) {
  if (GetFaultVfdState().fail_fapl_copy) {
    return nullptr;
  }
  void* copy = std::calloc(1, sizeof(int));
  if (copy != nullptr && old_fa != nullptr) {
    std::memcpy(copy, old_fa, sizeof(int));
  }
  return copy;
}

herr_t FaultVfdFaplFree(void* fa) {
  std::free(fa);
  return GetFaultVfdState().fail_fapl_free ? FAIL : SUCCEED;
}

H5FD_t* FaultVfdOpen(const char* name, unsigned flags, hid_t fapl_id,
                     haddr_t maxaddr) {
  auto& st = GetFaultVfdState();
  if (st.open.Consume()) {
    return nullptr;
  }
  const char* target = name;
  if (!st.redirect_from.empty() && name != nullptr &&
      st.redirect_from == name) {
    target = st.redirect_to.c_str();
  }
  return g_sec2_cls->open(target, flags, fapl_id, maxaddr);
}

herr_t FaultVfdClose(H5FD_t* file) {
  // Always delegate to sec2 close first so OS fd and VFD memory never leak.
  herr_t rc = g_sec2_cls->close(file);
  if (GetFaultVfdState().close.Consume()) {
    return FAIL;
  }
  return rc;
}

int FaultVfdCmp(const H5FD_t* f1, const H5FD_t* f2) {
  return g_sec2_cls->cmp(f1, f2);
}

herr_t FaultVfdQuery(const H5FD_t* f, unsigned long* flags) {
  auto& st = GetFaultVfdState();
  if (f != nullptr && st.corrupt_maxaddr) {
    const_cast<H5FD_t*>(f)->maxaddr = HADDR_UNDEF;
  }
  return g_sec2_cls->query(f, flags);
}

herr_t FaultVfdGetTypeMap(const H5FD_t*, H5FD_mem_t* type_map) {
  if (GetFaultVfdState().get_type_map.Consume()) {
    return FAIL;
  }
  std::memcpy(type_map, g_sec2_cls->fl_map, sizeof(g_sec2_cls->fl_map));
  return SUCCEED;
}

haddr_t FaultVfdGetEoa(const H5FD_t* f, H5FD_mem_t type) {
  if (GetFaultVfdState().get_eoa.Consume()) {
    return HADDR_UNDEF;
  }
  return g_sec2_cls->get_eoa(f, type);
}

herr_t FaultVfdSetEoa(H5FD_t* f, H5FD_mem_t type, haddr_t addr) {
  if (GetFaultVfdState().set_eoa.Consume()) {
    return FAIL;
  }
  return g_sec2_cls->set_eoa(f, type, addr);
}

haddr_t FaultVfdGetEof(const H5FD_t* f, H5FD_mem_t type) {
  if (GetFaultVfdState().get_eof.Consume()) {
    return HADDR_UNDEF;
  }
  return g_sec2_cls->get_eof(f, type);
}

herr_t FaultVfdGetHandle(H5FD_t* f, hid_t fapl_id, void** file_handle) {
  auto& st = GetFaultVfdState();
  if (st.get_handle.Consume()) {
    return FAIL;
  }
  if (st.override_handle) {
    *file_handle = &st.override_fd;
    return SUCCEED;
  }
  return g_sec2_cls->get_handle(f, fapl_id, file_handle);
}

herr_t FaultVfdRead(H5FD_t* f, H5FD_mem_t type, hid_t dxpl, haddr_t addr,
                    size_t size, void* buf) {
  if (GetFaultVfdState().read.Consume()) {
    return FAIL;
  }
  return g_sec2_cls->read(f, type, dxpl, addr, size, buf);
}

herr_t FaultVfdWrite(H5FD_t* f, H5FD_mem_t type, hid_t dxpl, haddr_t addr,
                     size_t size, const void* buf) {
  if (GetFaultVfdState().write.Consume()) {
    return FAIL;
  }
  return g_sec2_cls->write(f, type, dxpl, addr, size, buf);
}

herr_t FaultVfdFlush(H5FD_t* f, hid_t dxpl_id, bool closing) {
  if (GetFaultVfdState().flush.Consume()) {
    return FAIL;
  }
  return (g_sec2_cls->flush != nullptr) ? g_sec2_cls->flush(f, dxpl_id, closing)
                                        : SUCCEED;
}

herr_t FaultVfdTruncate(H5FD_t* f, hid_t dxpl_id, bool closing) {
  if (GetFaultVfdState().truncate.Consume()) {
    return FAIL;
  }
  return g_sec2_cls->truncate(f, dxpl_id, closing);
}

herr_t FaultVfdLock(H5FD_t* f, bool rw) {
  if (GetFaultVfdState().lock.Consume()) {
    return FAIL;
  }
  return g_sec2_cls->lock(f, rw);
}

herr_t FaultVfdUnlock(H5FD_t* f) {
  if (GetFaultVfdState().unlock.Consume()) {
    return FAIL;
  }
  return g_sec2_cls->unlock(f);
}

herr_t FaultVfdDelete(const char* name, hid_t fapl_id) {
  if (GetFaultVfdState().del.Consume()) {
    return FAIL;
  }
  return g_sec2_cls->del(name, fapl_id);
}

herr_t FaultVfdCtl(H5FD_t* f, uint64_t op_code, uint64_t flags,
                   const void* input, void** output) {
  return g_sec2_cls->ctl(f, op_code, flags, input, output);
}

}  // extern "C"

H5FD_class_t BuildFaultVfdClass(const H5FD_class_t* sec2_cls) {
  H5FD_class_t cls = *sec2_cls;
  cls.value = static_cast<H5FD_class_value_t>(512);
  cls.name = "fault_sec2";
  cls.fapl_size = sizeof(int);
  cls.fapl_get = FaultVfdFaplGet;
  cls.fapl_copy = FaultVfdFaplCopy;
  cls.fapl_free = FaultVfdFaplFree;
  cls.open = FaultVfdOpen;
  cls.close = FaultVfdClose;
  cls.cmp = FaultVfdCmp;
  cls.query = FaultVfdQuery;
  cls.get_type_map = FaultVfdGetTypeMap;
  cls.get_eoa = FaultVfdGetEoa;
  cls.set_eoa = FaultVfdSetEoa;
  cls.get_eof = FaultVfdGetEof;
  cls.get_handle = FaultVfdGetHandle;
  cls.read = FaultVfdRead;
  cls.write = FaultVfdWrite;
  cls.flush = FaultVfdFlush;
  cls.truncate = FaultVfdTruncate;
  cls.lock = FaultVfdLock;
  cls.unlock = FaultVfdUnlock;
  cls.del = FaultVfdDelete;
  cls.ctl = FaultVfdCtl;
  return cls;
}

ScopedPlist MakeFaultVfdFapl(hid_t vfd_id) {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  int cfg = 7;
  if (fapl.valid() && H5Pset_driver(fapl.get(), vfd_id, &cfg) < 0) {
    fapl.reset();
  }
  return fapl;
}

struct FaultVolState {
  bool fail_info_copy = false;
  bool fail_info_free = false;
  FaultTrigger fail_get_wrap_ctx;
  bool fail_wrap_object = false;
  bool fail_unwrap_object = false;
  bool fail_free_wrap_ctx = false;
  bool fail_object_get_file = false;
  bool null_object_get_file = false;

  void Reset() {
    fail_info_copy = false;
    fail_info_free = false;
    fail_get_wrap_ctx.Reset();
    fail_wrap_object = false;
    fail_unwrap_object = false;
    fail_free_wrap_ctx = false;
    fail_object_get_file = false;
    null_object_get_file = false;
  }
};

FaultVolState& GetFaultVolState() {
  static auto* state = new FaultVolState();
  return *state;
}

extern "C" {

void* FaultVolInfoCopy(const void* info) {
  if (GetFaultVolState().fail_info_copy) {
    return nullptr;
  }
  void* copy = std::calloc(1, sizeof(int));
  if (copy != nullptr && info != nullptr) {
    std::memcpy(copy, info, sizeof(int));
  }
  return copy;
}

herr_t FaultVolInfoCmp(int* cmp_value, const void* info1, const void* info2) {
  const int a = info1 ? *static_cast<const int*>(info1) : 0;
  const int b = info2 ? *static_cast<const int*>(info2) : 0;
  *cmp_value = (a > b) - (a < b);
  return SUCCEED;
}

herr_t FaultVolInfoFree(void* info) {
  std::free(info);
  return GetFaultVolState().fail_info_free ? FAIL : SUCCEED;
}

void* FaultVolGetObject(const void* obj) {
  return const_cast<void*>(obj);
}

herr_t FaultVolGetWrapCtx(const void*, void** wrap_ctx) {
  if (GetFaultVolState().fail_get_wrap_ctx.Consume()) {
    return FAIL;
  }
  *wrap_ctx = std::calloc(1, sizeof(int));
  return (*wrap_ctx != nullptr) ? SUCCEED : FAIL;
}

void* FaultVolWrapObject(void* obj, H5I_type_t, void*) {
  if (GetFaultVolState().fail_wrap_object) {
    return nullptr;
  }
  return obj;
}

void* FaultVolUnwrapObject(void* obj) {
  if (GetFaultVolState().fail_unwrap_object) {
    return nullptr;
  }
  return obj;
}

herr_t FaultVolFreeWrapCtx(void* wrap_ctx) {
  std::free(wrap_ctx);
  return GetFaultVolState().fail_free_wrap_ctx ? FAIL : SUCCEED;
}

herr_t FaultVolObjectGet(void* obj, const H5VL_loc_params_t* loc_params,
                         H5VL_object_get_args_t* args, hid_t dxpl_id,
                         void** req) {
  auto& st = GetFaultVolState();
  if (args != nullptr && args->op_type == H5VL_OBJECT_GET_FILE) {
    if (st.fail_object_get_file) {
      return FAIL;
    }
    if (st.null_object_get_file) {
      *args->args.get_file.file = nullptr;
      return SUCCEED;
    }
  }
  return H5VL__native_object_get(obj, loc_params, args, dxpl_id, req);
}

herr_t FailingClearFlagsGet(hid_t, const char*, size_t, void*) { return FAIL; }

herr_t FailingCopyPropCb(const char*, size_t, void*) { return FAIL; }

hid_t g_failed_set_plist_id = H5I_INVALID_HID;

herr_t FailingSetPropCb(hid_t prop_id, const char*, size_t, void*) {
  g_failed_set_plist_id = prop_id;
  return FAIL;
}

int g_fail_get_skip = 0;

herr_t CountdownFailGet(hid_t, const char*, size_t, void*) {
  if (g_fail_get_skip > 0) {
    --g_fail_get_skip;
    return SUCCEED;
  }
  return FAIL;
}

}  // extern "C"

TEST(H5FintTest, EdgeCasesSuperblockLocksAndMountClose) {
  ASSERT_GE(H5open(), 0);
  TempDir dir("edge_cases");

  const htri_t orig_use = use_locks_env_g;
  const htri_t orig_ign = ignore_disabled_locks_g;
  absl::Cleanup restore_lock_globals = [&] {
    use_locks_env_g = orig_use;
    ignore_disabled_locks_g = orig_ign;
  };

  // 1. Force global file lock overrides on create + close.
  {
    use_locks_env_g = true;
    ignore_disabled_locks_g = true;
    const auto p_lock = dir.child("env_lock.h5");
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_TRUE(fapl.valid());
    ScopedFile f(
        H5Fcreate(p_lock.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
  }
  use_locks_env_g = orig_use;
  ignore_disabled_locks_g = orig_ign;

  // 2. Truncate existing closed file (tentative open -> close -> truncate).
  const auto p_trunc = dir.child("retruncate.h5");
  {
    ScopedPlist fapl = MakeSec2Fapl();
    {
      ScopedFile f1(
          H5Fcreate(p_trunc.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f1.valid());
    }
    ScopedFile f2(
        H5Fcreate(p_trunc.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f2.valid());
  }

  // 3. Direct H5F_open try=true missing file, H5P_DEFAULT fapl, H5P_DEFAULT fcpl.
  {
    ScopedPlist sec2_fapl = MakeSec2Fapl();
    ScopedApiContext ctx(sec2_fapl.get());
    H5F_t* hf = nullptr;
    EXPECT_GE(H5F_open(true, &hf, dir.child("does_not_exist.h5").c_str(),
                       H5F_ACC_RDONLY, H5P_FILE_CREATE_DEFAULT,
                       sec2_fapl.get()),
              0);
    EXPECT_EQ(hf, nullptr);

    ScopedErrorSilence silence;
    EXPECT_LT(H5F_open(false, &hf, "bad_fapl", H5F_ACC_RDONLY,
                       H5P_FILE_CREATE_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_LT(H5F_open(false, &hf, dir.child("bad_fcpl.h5").c_str(),
                       H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                       H5P_DEFAULT, sec2_fapl.get()),
              0);
  }

  // 4. Close-degree mismatch (DEFAULT vs STRONG and SEMI vs STRONG).
  {
    const auto p_deg = dir.child("deg_mismatch.h5");
    ScopedPlist fapl_strong = MakeSec2Fapl();
    ASSERT_GE(H5Pset_fclose_degree(fapl_strong.get(), H5F_CLOSE_STRONG), 0);
    ScopedFile f_strong(H5Fcreate(p_deg.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                                  fapl_strong.get()));
    ASSERT_TRUE(f_strong.valid());

    ScopedErrorSilence silence;
    ScopedPlist fapl_def = MakeSec2Fapl();
    EXPECT_LT(H5Fopen(p_deg.c_str(), H5F_ACC_RDWR, fapl_def.get()), 0);
    ScopedPlist fapl_semi = MakeSec2Fapl();
    ASSERT_GE(H5Pset_fclose_degree(fapl_semi.get(), H5F_CLOSE_SEMI), 0);
    EXPECT_LT(H5Fopen(p_deg.c_str(), H5F_ACC_RDWR, fapl_semi.get()), 0);
  }

  // 5. Garbage file fails H5F__super_read (2117).
  {
    const auto p_garbage = dir.child("garbage.h5");
    {
      std::ofstream ofs(p_garbage, std::ios::binary);
      const std::string zeros(64, '\0');
      ofs.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    ScopedPlist fapl = MakeSec2Fapl();
    ScopedErrorSilence silence;
    EXPECT_LT(H5Fopen(p_garbage.c_str(), H5F_ACC_RDONLY, fapl.get()), 0);
  }

  // 6. SWMR write unlock on open and inconsistent v3 SWMR status_flags on read.
  {
    const auto p_swmr = dir.child("swmr_open_flags.h5");
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST,
                                   H5F_LIBVER_LATEST),
              0);
    ASSERT_GE(H5Pset_file_locking(fapl.get(), true, true), 0);
    {
      ScopedFile f(
          H5Fcreate(p_swmr.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
    }
    {
      ScopedFile f_sw(
          H5Fopen(p_swmr.c_str(), H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE,
                  fapl.get()));
      ASSERT_TRUE(f_sw.valid());
    }

    // Patch superblock status_flags = H5F_SUPER_SWMR_WRITE_ACCESS (0x04) only
    // (without WRITE_ACCESS 0x01) and update v3 superblock checksum.
    std::ifstream ifs(p_swmr, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());
    ifs.close();
    ASSERT_GE(bytes.size(), 48u);
    ASSERT_EQ(bytes[8], HDF5_SUPERBLOCK_VERSION_3);
    bytes[H5F_SUPER_STATUS_FLAGS_OFF(HDF5_SUPERBLOCK_VERSION_3)] =
        H5F_SUPER_SWMR_WRITE_ACCESS;
    const uint32_t csum = H5_checksum_metadata(bytes.data(), 48 - 4, 0);
    uint8_t* p = bytes.data() + 44;
    UINT32ENCODE(p, csum);
    std::ofstream ofs(p_swmr, std::ios::binary | std::ios::trunc);
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    ofs.close();

    ScopedErrorSilence silence;
    EXPECT_LT(
        H5Fopen(p_swmr.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ, fapl.get()),
        0);
  }

  // 7. H5F_try_close closing/mount/parent/unknown-degree paths.
  {
    const auto p_close = dir.child("try_close_edge.h5");
    ScopedPlist fapl = MakeSec2Fapl();
    ScopedFile f(
        H5Fcreate(p_close.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
    ScopedApiContext ctx(fapl.get());
    H5F_t* hf = UnwrapFile(f.get());
    ASSERT_NE(hf, nullptr);

    hf->closing = true;
    bool was_closed = false;
    EXPECT_GE(H5F_try_close(hf, &was_closed), 0);
    EXPECT_TRUE(was_closed);
    hf->closing = false;

    const auto orig_deg = hf->shared->fc_degree;
    hf->shared->fc_degree = H5F_CLOSE_DEFAULT;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5F_try_close(hf, nullptr), 0);
    }
    hf->shared->fc_degree = orig_deg;
  }

  {
    ScopedPlist fapl_strong = MakeSec2Fapl();
    ASSERT_GE(H5Pset_fclose_degree(fapl_strong.get(), H5F_CLOSE_STRONG), 0);
    const auto p_par = dir.child("mount_par.h5");
    const auto p_chd = dir.child("mount_chd.h5");
    ScopedFile par(H5Fcreate(p_par.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                             fapl_strong.get()));
    ASSERT_TRUE(par.valid());
    ScopedGroup mnt(
        H5Gcreate2(par.get(), "mnt", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(mnt.valid());
    mnt.reset();

    ScopedFile chd(H5Fcreate(p_chd.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                             fapl_strong.get()));
    ASSERT_TRUE(chd.valid());
    ASSERT_GE(H5Fmount(par.get(), "/mnt", chd.get(), H5P_DEFAULT), 0);

    // Trigger H5F_try_close with parent failing on child close, then restore.
    {
      ScopedApiContext ctx(fapl_strong.get());
      H5F_t* par_hf = UnwrapFile(par.get());
      H5F_t* chd_hf = UnwrapFile(chd.get());
      ASSERT_NE(par_hf, nullptr);
      ASSERT_NE(chd_hf, nullptr);
      const auto orig_par_deg = par_hf->shared->fc_degree;
      const auto orig_chd_deg = chd_hf->shared->fc_degree;

      // Closing child ID while mounted keeps child open in H5F_CLOSE_STRONG.
      chd.reset();
      EXPECT_EQ(par_hf->nmounts, 1u);

      // Fault child via fc_degree while unmounting/closing parent.
      chd_hf = nullptr;
      EXPECT_GE(H5Funmount(par.get(), "/mnt"), 0);
      // Re-mount another child and exercise child->parent fail on H5F_try_close.
      ScopedFile chd2(
          H5Fopen(p_chd.c_str(), H5F_ACC_RDWR, fapl_strong.get()));
      ASSERT_TRUE(chd2.valid());
      ASSERT_GE(H5Fmount(par.get(), "/mnt", chd2.get(), H5P_DEFAULT), 0);
      H5F_t* chd2_hf = UnwrapFile(chd2.get());
      ASSERT_NE(chd2_hf, nullptr);

      // First drop parent's file ID cleanly while child keeps hierarchy alive,
      // then fault parent's fc_degree so closing child fails on parent unwind.
      par.reset();
      par_hf->shared->fc_degree = H5F_CLOSE_DEFAULT;
      chd2_hf->shared->fc_degree = H5F_CLOSE_SEMI;
      {
        ScopedErrorSilence silence;
        chd2.reset();  // triggers H5F_try_close on chd2 -> parent -> fails
      }
      par_hf->shared->fc_degree = orig_par_deg;
      chd2_hf->shared->fc_degree = orig_chd_deg;
      chd2_hf->closing = false;
      EXPECT_GE(H5F_try_close(chd2_hf, nullptr), 0);
      // Also exercise parent -> H5F__close_mounts failure (line 2671).
      {
        const auto p_par2 = dir.child("mount_par2.h5");
        const auto p_chd2 = dir.child("mount_chd2.h5");
        ScopedFile par2(H5Fcreate(p_par2.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                                  fapl_strong.get()));
        ASSERT_TRUE(par2.valid());
        ScopedGroup mnt2(
            H5Gcreate2(par2.get(), "mnt", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
        ASSERT_TRUE(mnt2.valid());
        mnt2.reset();

        ScopedFile chd3(H5Fcreate(p_chd2.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                                  fapl_strong.get()));
        ASSERT_TRUE(chd3.valid());
        ASSERT_GE(H5Fmount(par2.get(), "/mnt", chd3.get(), H5P_DEFAULT), 0);

        H5F_t* par2_hf = UnwrapFile(par2.get());
        H5F_t* chd3_hf = UnwrapFile(chd3.get());
        ASSERT_NE(par2_hf, nullptr);
        ASSERT_NE(chd3_hf, nullptr);
        const auto orig_chd3_deg = chd3_hf->shared->fc_degree;

        chd3.reset();  // stays mounted in par2
        chd3_hf->shared->fc_degree = H5F_CLOSE_DEFAULT;
        {
          ScopedErrorSilence silence;
          par2.reset();  // triggers H5F_try_close(par2) -> H5F__close_mounts -> fails
        }
        chd3_hf->shared->fc_degree = orig_chd3_deg;
        EXPECT_GE(H5F_try_close(chd3_hf, nullptr), 0);
        // Group was already closed by H5F__close_mounts before child close failed.
        par2_hf->shared->mtab.nmounts = 0;
        par2_hf->nmounts = 0;
        par2_hf->closing = false;
        EXPECT_GE(H5F_try_close(par2_hf, nullptr), 0);
      }
    }
  }

  // 8. Oversize addr_len decode, invalid file_image args, family drvinfo unpin,
  //    and H5Fstart_swmr_write with file locking enabled.
  {
    std::array<uint8_t, 13> addr_buf{};
    addr_buf[0] = 0x34;
    addr_buf[1] = 0x12;
    const uint8_t* ptr = addr_buf.data();
    haddr_t decoded = 0;
    H5F_addr_decode_len(12, &ptr, &decoded);
    EXPECT_EQ(decoded, haddr_t{0x1234});

    ScopedApiContext ctx;
    size_t ilen = 0;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5F__get_file_image(nullptr, nullptr, 0, &ilen), 0);
      H5F_t dummy_f{};
      H5F_shared_t dummy_sh{};
      H5FD_t dummy_lf{};
      dummy_sh.lf = &dummy_lf;
      dummy_f.shared = &dummy_sh;
      EXPECT_LT(H5F__get_file_image(&dummy_f, nullptr, 0, &ilen), 0);
    }

    // Family VFD creates a drvinfo block entry when super_vers < 2 (v0/v1),
    // which H5F__dest unpins on close (line 1537).
    const auto fam_pat = dir.child("fam_drv_%d.h5");
    ScopedPlist fam_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(H5Pset_fapl_family(fam_fapl.get(), 65536, H5P_DEFAULT), 0);
    ASSERT_GE(
        H5Pset_libver_bounds(fam_fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
        0);
    ScopedFile fam(
        H5Fcreate(fam_pat.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fam_fapl.get()));
    ASSERT_TRUE(fam.valid());
  }

  {
    const auto p_swmr_lock = dir.child("swmr_with_lock.h5");
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110,
                                   H5F_LIBVER_LATEST),
              0);
    ASSERT_GE(H5Pset_file_locking(fapl.get(), true, true), 0);
    ScopedFile f(
        H5Fcreate(p_swmr_lock.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
    EXPECT_GE(H5Fstart_swmr_write(f.get()), 0);
  }
}

TEST(H5FintTest, MissingPropertyListEntriesFailCleanly) {
  ASSERT_GE(H5open(), 0);
  TempDir dir("missing_props");
  const auto existing_path = dir.child("existing.h5");
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ScopedFile f(H5Fcreate(existing_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                           fapl.get()));
    ASSERT_TRUE(f.valid());
  }

  const htri_t orig_use = use_locks_env_g;
  const htri_t orig_ign = ignore_disabled_locks_g;
  absl::Cleanup restore_lock_globals = [&] {
    use_locks_env_g = orig_use;
    ignore_disabled_locks_g = orig_ign;
  };
  use_locks_env_g = FAIL;
  ignore_disabled_locks_g = FAIL;

  ScopedErrorSilence silence;
  ScopedApiContext ctx;

  // 1. Missing lock properties in FAPL.
  for (const char* prop : {H5F_ACS_USE_FILE_LOCKING_NAME,
                           H5F_ACS_IGNORE_DISABLED_FILE_LOCKS_NAME}) {
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_GE(H5Premove(fapl.get(), prop), 0);
    H5F_t* hf = nullptr;
    EXPECT_LT(H5F_open(false, &hf, existing_path.c_str(), H5F_ACC_RDONLY,
                       H5P_FILE_CREATE_DEFAULT, fapl.get()),
              0);
  }

  // 2. Missing FCPL properties in H5F__new.
  {
    ScopedPlist sec2_fapl = MakeSec2Fapl();
    constexpr const char* kFcplProps[] = {
        H5F_CRT_ADDR_BYTE_NUM_NAME,
        H5F_CRT_OBJ_BYTE_NUM_NAME,
        H5F_CRT_SHMSG_NINDEXES_NAME,
        H5F_CRT_FILE_SPACE_STRATEGY_NAME,
        H5F_CRT_FREE_SPACE_PERSIST_NAME,
        H5F_CRT_FREE_SPACE_THRESHOLD_NAME,
        H5F_CRT_FILE_SPACE_PAGE_SIZE_NAME,
    };
    for (const char* prop : kFcplProps) {
      ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
      ASSERT_GE(H5Premove(fcpl.get(), prop), 0);
      H5F_t* hf = nullptr;
      EXPECT_LT(H5F_open(false, &hf, existing_path.c_str(), H5F_ACC_RDONLY,
                         fcpl.get(), sec2_fapl.get()),
                0);
    }
  }

  // 3. Missing FAPL properties in H5F__new (including EFC cleanup on late fail).
  {
    constexpr const char* kFaplNewProps[] = {
        H5F_ACS_META_CACHE_INIT_CONFIG_NAME,
        H5F_ACS_DATA_CACHE_NUM_SLOTS_NAME,
        H5F_ACS_DATA_CACHE_BYTE_SIZE_NAME,
        H5F_ACS_PREEMPT_READ_CHUNKS_NAME,
        H5F_ACS_ALIGN_THRHD_NAME,
        H5F_ACS_ALIGN_NAME,
        H5F_ACS_GARBG_COLCT_REF_NAME,
        H5F_ACS_SIEVE_BUF_SIZE_NAME,
        H5F_ACS_LIBVER_LOW_BOUND_NAME,
        H5F_ACS_LIBVER_HIGH_BOUND_NAME,
        H5F_ACS_USE_MDC_LOGGING_NAME,
        H5F_ACS_START_MDC_LOG_ON_ACCESS_NAME,
        H5F_ACS_META_BLOCK_SIZE_NAME,
        H5F_ACS_SDATA_BLOCK_SIZE_NAME,
        H5F_ACS_EFC_SIZE_NAME,
        H5F_ACS_META_CACHE_INIT_IMAGE_CONFIG_NAME,
        H5F_ACS_RFIC_FLAGS_NAME,
        H5F_ACS_METADATA_READ_ATTEMPTS_NAME,
        H5F_ACS_MDC_LOG_LOCATION_NAME,
        H5F_ACS_OBJECT_FLUSH_CB_NAME,
    };
    for (const char* prop : kFaplNewProps) {
      ScopedPlist fapl = MakeSec2Fapl();
      if (std::strcmp(prop, H5F_ACS_RFIC_FLAGS_NAME) == 0) {
        ASSERT_GE(H5Pset_elink_file_cache_size(fapl.get(), 2), 0);
      }
      ASSERT_GE(H5Premove(fapl.get(), prop), 0);
      H5F_t* hf = nullptr;
      EXPECT_LT(H5F_open(false, &hf, existing_path.c_str(), H5F_ACC_RDONLY,
                         H5P_FILE_CREATE_DEFAULT, fapl.get()),
                0);
    }
  }

  // 4. Missing page buffer / evict-on-close / close-degree props + bad PB create
  //    + failing CLEAR_STATUS_FLAGS property in H5F_open.
  for (const char* prop :
       {H5F_ACS_PAGE_BUFFER_SIZE_NAME, H5F_ACS_EVICT_ON_CLOSE_FLAG_NAME,
        H5F_ACS_CLOSE_DEGREE_NAME}) {
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_GE(H5Premove(fapl.get(), prop), 0);
    H5F_t* hf = nullptr;
    EXPECT_LT(H5F_open(false, &hf, existing_path.c_str(), H5F_ACC_RDONLY,
                       H5P_FILE_CREATE_DEFAULT, fapl.get()),
              0);
  }
  for (const char* prop : {H5F_ACS_PAGE_BUFFER_MIN_META_PERC_NAME,
                           H5F_ACS_PAGE_BUFFER_MIN_RAW_PERC_NAME}) {
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_GE(H5Pset_page_buffer_size(fapl.get(), 4096, 50, 50), 0);
    ASSERT_GE(H5Premove(fapl.get(), prop), 0);
    H5F_t* hf = nullptr;
    EXPECT_LT(H5F_open(false, &hf, existing_path.c_str(), H5F_ACC_RDONLY,
                       H5P_FILE_CREATE_DEFAULT, fapl.get()),
              0);
  }
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_GE(H5Pset_page_buffer_size(fapl.get(), 512, 0, 0), 0);
    H5F_t* hf = nullptr;
    EXPECT_LT(H5F_open(false, &hf, dir.child("bad_pb_create.h5").c_str(),
                       H5F_ACC_RDWR | H5F_ACC_CREAT | H5F_ACC_TRUNC,
                       H5P_FILE_CREATE_DEFAULT, fapl.get()),
              0);
  }
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ASSERT_GE(H5Premove(fapl.get(), H5F_ACS_CLEAR_STATUS_FLAGS_NAME), 0);
    bool val = false;
    ASSERT_GE(H5Pinsert2(fapl.get(), H5F_ACS_CLEAR_STATUS_FLAGS_NAME,
                         sizeof(bool), &val, nullptr, FailingClearFlagsGet,
                         nullptr, nullptr, nullptr, nullptr),
              0);
    H5F_t* hf = nullptr;
    EXPECT_LT(H5F_open(false, &hf, existing_path.c_str(), H5F_ACC_RDONLY,
                       H5P_FILE_CREATE_DEFAULT, fapl.get()),
              0);
  }
}

TEST(H5FintTest, CustomFaultInjectingVfdErrors) {
  ASSERT_GE(H5open(), 0);
  g_sec2_cls =
      static_cast<const H5FD_class_t*>(H5I_object_verify(H5FD_SEC2, H5I_VFL));
  ASSERT_NE(g_sec2_cls, nullptr);

  const H5FD_class_t fault_cls = BuildFaultVfdClass(g_sec2_cls);
  const hid_t vfd_id = H5FDregister(&fault_cls);
  ASSERT_NE(vfd_id, H5I_INVALID_HID);
  auto& st = GetFaultVfdState();
  st.Reset();
  absl::Cleanup cleanup = [&] {
    st.Reset();
    H5FDunregister(vfd_id);
  };

  TempDir dir("fault_vfd");
  ScopedErrorSilence silence;

  // Create clean baseline FaultVfd files.
  const auto p_base = dir.child("vfd_base.h5");
  {
    ScopedPlist fapl = MakeFaultVfdFapl(vfd_id);
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110,
                                   H5F_LIBVER_LATEST),
              0);
    ScopedFile f(
        H5Fcreate(p_base.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
  }

  // 1. fapl_copy and fapl_free failure paths in H5F_get_access_plist.
  {
    ScopedPlist fapl = MakeFaultVfdFapl(vfd_id);
    ScopedFile f(H5Fopen(p_base.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(f.valid());

    st.fail_fapl_free = true;
    EXPECT_LT(H5Fget_access_plist(f.get()), 0);
    st.fail_fapl_free = false;
    st.fail_fapl_copy = true;
    EXPECT_LT(H5Fget_access_plist(f.get()), 0);
    st.fail_fapl_copy = false;
  }

  // 2. H5F__is_hdf5 read fail and close-after-sig-found fail.
  {
    ScopedPlist fapl = MakeFaultVfdFapl(vfd_id);
    st.read.FailAfter(0, 1);
    EXPECT_LT(H5Fis_accessible(p_base.c_str(), fapl.get()), 0);
    st.Reset();

    st.close.FailAfter(0, 1);
    EXPECT_LT(H5Fis_accessible(p_base.c_str(), fapl.get()), 0);
    st.Reset();
  }

  // 3. H5F__new failures: HADDR_UNDEF maxaddr & get_type_map fail.
  {
    ScopedPlist fapl = MakeFaultVfdFapl(vfd_id);
    st.corrupt_maxaddr = true;
    EXPECT_LT(H5Fopen(p_base.c_str(), H5F_ACC_RDONLY, fapl.get()), 0);
    st.Reset();

    st.get_type_map.FailAfter(0, 1);
    st.close.FailAfter(0, 1);  // also covers H5F_open line 1999-2000
    EXPECT_LT(H5Fopen(p_base.c_str(), H5F_ACC_RDONLY, fapl.get()), 0);
    st.Reset();
  }

  // 4. H5F_open VFD failure matrix: tentative open, real open, already-open
  //    close, tentative->reopen close/open fail, lock+close fail, super_init,
  //    mkroot, superblock/ext status flush + SWMR unlock fail, destruct close.
  {
    ScopedPlist fapl = MakeFaultVfdFapl(vfd_id);
    st.open.FailAfter(0, 2);  // fail tentative and create open on new file
    EXPECT_LT(H5Fcreate(dir.child("tent_fail.h5").c_str(), H5F_ACC_TRUNC,
                        H5P_DEFAULT, fapl.get()),
              0);
    st.Reset();

    st.open.FailAfter(1, 1);  // tentative misses new file, H5FD_open(CREAT) fails
    EXPECT_LT(H5Fcreate(dir.child("creat_fail.h5").c_str(), H5F_ACC_TRUNC,
                        H5P_DEFAULT, fapl.get()),
              0);
    st.Reset();

    st.open.FailAfter(0, 1);
    EXPECT_LT(H5Fopen(p_base.c_str(), H5F_ACC_RDONLY, fapl.get()), 0);
    st.Reset();

    st.open.FailAfter(1, 1);  // tentative open succeeds, truncate open fails
    EXPECT_LT(H5Fcreate(p_base.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()),
              0);
    st.Reset();

    // Tentative open succeeds on existing disposable file, tentative close fails
    // before truncate open (line 1976).
    {
      const auto p_retrunc = dir.child("retrunc_close_fail.h5");
      {
        ScopedFile f(H5Fcreate(p_retrunc.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                               fapl.get()));
        ASSERT_TRUE(f.valid());
      }
      st.close.FailAfter(0, 1);
      EXPECT_LT(
          H5Fcreate(p_retrunc.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()),
          0);
      st.Reset();
    }

    // Open p_base, reopen while failing VFD close on duplicate lookup.
    {
      ScopedFile f1(H5Fopen(p_base.c_str(), H5F_ACC_RDONLY, fapl.get()));
      ASSERT_TRUE(f1.valid());
      st.close.FailAfter(0, 1);
      EXPECT_LT(H5Fopen(p_base.c_str(), H5F_ACC_RDONLY, fapl.get()), 0);
      st.Reset();
    }

    // Lock fail + close fail during open.
    {
      ScopedPlist lock_fapl = MakeFaultVfdFapl(vfd_id);
      ASSERT_GE(H5Pset_file_locking(lock_fapl.get(), true, true), 0);
      st.lock.FailAfter(0, 1);
      st.close.FailAfter(0, 1);
      EXPECT_LT(H5Fopen(p_base.c_str(), H5F_ACC_RDONLY, lock_fapl.get()), 0);
      st.Reset();

      // SWMR write open fails unlock, and H5F_open cleanup fails close.
      ASSERT_GE(H5Pset_libver_bounds(lock_fapl.get(), H5F_LIBVER_V110,
                                     H5F_LIBVER_LATEST),
                0);
      st.unlock.FailAfter(0, 1);
      st.close.FailAfter(0, 1);
      EXPECT_LT(H5Fopen(p_base.c_str(), H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE,
                        lock_fapl.get()),
                0);
      st.Reset();
    }

    // Status-flag flush fail on RDWR open (superblock line 2215 & ext line 2217).
    {
      st.write.FailAfter(0, 1);
      EXPECT_LT(H5Fopen(p_base.c_str(), H5F_ACC_RDWR, fapl.get()), 0);
      st.Reset();

      const auto p_v3_ext = dir.child("v3_ext_flush_fail.h5");
      const auto p_v3_ext_2 = dir.child("v3_ext_flush_fail_2.h5");
      ScopedPlist fcpl_ext(H5Pcreate(H5P_FILE_CREATE));
      ASSERT_GE(H5Pset_file_space_strategy(fcpl_ext.get(),
                                           H5F_FSPACE_STRATEGY_PAGE, true, 1),
                0);
      ScopedPlist fapl_v3 = MakeFaultVfdFapl(vfd_id);
      ASSERT_GE(H5Pset_libver_bounds(fapl_v3.get(), H5F_LIBVER_V110,
                                     H5F_LIBVER_LATEST),
                0);
      for (const auto& p : {p_v3_ext, p_v3_ext_2}) {
        ScopedFile f(H5Fcreate(p.c_str(), H5F_ACC_TRUNC, fcpl_ext.get(),
                               fapl_v3.get()));
        ASSERT_TRUE(f.valid());
      }
      st.flush.FailAfter(0, 1);
      EXPECT_LT(H5Fopen(p_v3_ext.c_str(), H5F_ACC_RDWR, fapl_v3.get()), 0);
      st.Reset();

      // Use a fresh unflagged file so line 2215 succeeds and line 2217 triggers.
      st.flush.FailAfter(1, 1);
      EXPECT_LT(H5Fopen(p_v3_ext_2.c_str(), H5F_ACC_RDWR, fapl_v3.get()), 0);
      st.Reset();
    }

    // Fail H5F__super_init (2105) cleanly via set_eoa (note: failing later in
    // H5G_mkroot leaves a dirty sblock without root_grp/root_ent, causing
    // H5C_dest in H5F__dest to fail flushing and leak H5C_t under LSan).
    {
      ScopedPlist fapl_no_aggr = MakeFaultVfdFapl(vfd_id);
      ASSERT_GE(H5Pset_meta_block_size(fapl_no_aggr.get(), 0), 0);
      int create_failures = 0;
      for (int ok_eoa = 0; ok_eoa <= 1; ++ok_eoa) {
        st.set_eoa.FailAfter(ok_eoa, 1);
        const hid_t fid =
            H5Fcreate(dir.child("super_init_fail.h5").c_str(),
                      H5F_ACC_TRUNC, H5P_DEFAULT, fapl_no_aggr.get());
        st.Reset();
        EXPECT_LT(fid, 0);
        if (fid >= 0) {
          EXPECT_GE(H5Fclose(fid), 0);
        } else {
          ++create_failures;
        }
      }
      EXPECT_EQ(create_failures, 2);
    }

    // Fail reads incrementally on a v0/v1 file with a subgroup opened RDWR so
    // both H5F__super_read (2117) and H5G_mkroot(false) (2140) fail cleanly.
    {
      const auto p_v0 = dir.child("v0_mkroot_fault.h5");
      ScopedPlist fapl_v0 = MakeFaultVfdFapl(vfd_id);
      ASSERT_GE(
          H5Pset_libver_bounds(fapl_v0.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
          0);
      {
        ScopedFile f(
            H5Fcreate(p_v0.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl_v0.get()));
        ASSERT_TRUE(f.valid());
        ScopedGroup g(
            H5Gcreate2(f.get(), "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
        ASSERT_TRUE(g.valid());
      }
      int open_failures = 0;
      for (int ok_reads = 0; ok_reads <= 10; ++ok_reads) {
        st.read.FailAfter(ok_reads, -1);
        const hid_t fid = H5Fopen(p_v0.c_str(), H5F_ACC_RDWR, fapl_v0.get());
        st.Reset();
        if (fid >= 0) {
          EXPECT_GE(H5Fclose(fid), 0);
          break;
        }
        ++open_failures;
      }
      EXPECT_GE(open_failures, 2);
    }
  }

  // 5. H5F_prefix_open_file failure across env/prop/extpath/rel/actual/abs.
  {
    const auto bad_probe = dir.child("bad_probe.h5");
    {
      std::ofstream ofs(bad_probe, std::ios::binary);
      const std::string zeros(64, '\0');
      ofs.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    const auto prim_path = dir.child("prefix_primary.h5");
    ScopedPlist fapl = MakeFaultVfdFapl(vfd_id);
    ASSERT_GE(H5Pset_elink_file_cache_size(fapl.get(), 4), 0);
    ScopedFile prim(
        H5Fcreate(prim_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(prim.valid());

    ScopedApiContext ctx(fapl.get());
    H5F_t* prim_hf = UnwrapFile(prim.get());
    ASSERT_NE(prim_hf, nullptr);
    H5F_t* target_hf = nullptr;

    // 5a: Absolute path fails superblock read -> 886.
    EXPECT_LT(H5F_prefix_open_file(true, &target_hf, prim_hf, H5F_PREFIX_ELINK,
                                   nullptr, bad_probe.c_str(), H5F_ACC_RDONLY,
                                   fapl.get()),
              0);

    // 5b: Env prefix misses nonexistent file -> exercises env loop without
    //     upstream saved_env leak on early error, and hits 1039 (!try).
    {
      ScopedEnvVar env_guard("HDF5_EXT_PREFIX");
      ASSERT_EQ(setenv("HDF5_EXT_PREFIX", dir.path().c_str(), 1), 0);
      EXPECT_LT(H5F_prefix_open_file(false, &target_hf, prim_hf,
                                     H5F_PREFIX_ELINK, nullptr,
                                     "nonexistent_in_env.h5", H5F_ACC_RDONLY,
                                     fapl.get()),
                0);
    }

    // 5c: Prop prefix fails -> 974 + 1046.
    {
      ScopedEnvVar env_guard("HDF5_EXT_PREFIX");
      unsetenv("HDF5_EXT_PREFIX");
      EXPECT_LT(H5F_prefix_open_file(
                    true, &target_hf, prim_hf, H5F_PREFIX_ELINK,
                    dir.path().c_str(), "bad_probe.h5", H5F_ACC_RDONLY,
                    fapl.get()),
                0);

      // 5d: extpath fails -> 992 + 1046.
      EXPECT_LT(H5F_prefix_open_file(true, &target_hf, prim_hf,
                                     H5F_PREFIX_ELINK, nullptr, "bad_probe.h5",
                                     H5F_ACC_RDONLY, fapl.get()),
                0);

      // 5e: relative name via VFD redirect (without changing CWD) -> 1004.
      char* saved_ext = prim_hf->shared->extpath;
      prim_hf->shared->extpath = nullptr;
      absl::Cleanup restore_ext = [&] { prim_hf->shared->extpath = saved_ext; };

      st.redirect_from = "bad_probe.h5";
      st.redirect_to = bad_probe;
      EXPECT_LT(H5F_prefix_open_file(true, &target_hf, prim_hf,
                                     H5F_PREFIX_ELINK, nullptr, "bad_probe.h5",
                                     H5F_ACC_RDONLY, fapl.get()),
                0);
      st.redirect_from.clear();
      st.redirect_to.clear();

      // 5f: actual_name directory probe fails -> 1028 + 1046.
      EXPECT_LT(H5F_prefix_open_file(true, &target_hf, prim_hf,
                                     H5F_PREFIX_ELINK, nullptr, "bad_probe.h5",
                                     H5F_ACC_RDONLY, fapl.get()),
                0);
    }
  }

  // 6. H5F__build_actual_name errors (lstat fail, get_handle fail, stat fail on
  //    dangling symlink, fstat fail on bad fd, st_ino mismatch).
  {
    const auto p_real = dir.child("symlink_target.h5");
    ScopedPlist fapl = MakeFaultVfdFapl(vfd_id);
    {
      ScopedFile f(
          H5Fcreate(p_real.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
    }

    // 6a: Virtual path -> lstat fails (2849).
    st.redirect_from = dir.child("virtual_no_lstat.h5");
    st.redirect_to = p_real;
    EXPECT_LT(H5Fopen(st.redirect_from.c_str(), H5F_ACC_RDONLY, fapl.get()), 0);
    st.Reset();

    // 6b: Dangling symlink redirected in open() -> lstat sees S_IFLNK, stat fails
    // (2892).
    const auto p_dangle = dir.child("dangle_link.h5");
    ASSERT_EQ(symlink(dir.child("missing_target.h5").c_str(), p_dangle.c_str()),
              0);
    st.redirect_from = p_dangle;
    st.redirect_to = p_real;
    EXPECT_LT(H5Fopen(p_dangle.c_str(), H5F_ACC_RDONLY, fapl.get()), 0);
    st.Reset();

    // 6c: Valid symlink + handle fault cases (2887, 2897, 2901).
    const auto p_link = dir.child("valid_link.h5");
    ASSERT_EQ(symlink(p_real.c_str(), p_link.c_str()), 0);

    st.get_handle.FailAfter(0, 1);
    EXPECT_LT(H5Fopen(p_link.c_str(), H5F_ACC_RDONLY, fapl.get()), 0);
    st.Reset();

    st.override_handle = true;
    st.override_fd = -1;
    EXPECT_LT(H5Fopen(p_link.c_str(), H5F_ACC_RDONLY, fapl.get()), 0);
    st.Reset();

    const auto p_other = dir.child("other_inode.h5");
    {
      std::ofstream ofs(p_other, std::ios::binary);
      ofs << "other";
    }
    int other_fd = open(p_other.c_str(), O_RDONLY);
    ASSERT_GE(other_fd, 0);
    absl::Cleanup close_other = [&] { close(other_fd); };
    st.override_handle = true;
    st.override_fd = other_fd;
    EXPECT_LT(H5Fopen(p_link.c_str(), H5F_ACC_RDONLY, fapl.get()), 0);
    st.Reset();
  }

  // 7. Flush / accum / page buffer / truncate / delete / image / eoa / eof /
  //    get_info / format_convert / start_swmr_write VFD faults.
  {
    const auto p_flush = dir.child("flush_fault.h5");
    ScopedPlist fapl = MakeFaultVfdFapl(vfd_id);
    ScopedFile f(
        H5Fcreate(p_flush.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());

    // Create chunked dataset with cached chunks so H5D_flush_all writes.
    constexpr hsize_t kDim = 8;
    ScopedSpace dsp(H5Screate_simple(1, &kDim, nullptr));
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &kDim), 0);
    ScopedDataset ds(H5Dcreate2(f.get(), "cdset", H5T_NATIVE_INT32, dsp.get(),
                                H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());
    constexpr int32_t kVals[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_GE(H5Dwrite(ds.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, kVals),
              0);

    // 7a: Fail write in H5D_flush_all + truncate + flush during H5Fflush.
    st.write.FailAfter(0, -1);
    st.truncate.FailAfter(0, -1);
    st.flush.FailAfter(0, -1);
    EXPECT_LT(H5Fflush(f.get(), H5F_SCOPE_LOCAL), 0);
    st.Reset();

    // 7b: Dirty small group so H5AC_flush puts it in accum, then fail accum
    //     flush write (2385).
    {
      ScopedGroup g(
          H5Gcreate2(f.get(), "g_accum", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(g.valid());
    }
    st.write.FailAfter(0, 1);
    EXPECT_LT(H5Fflush(f.get(), H5F_SCOPE_LOCAL), 0);
    st.Reset();

    // 7c: Create enough groups (>4KB metadata) so H5AC_flush itself triggers
    //     VFD write and fails (2348) and on 2nd pass (2369).
    for (int i = 0; i < 64; ++i) {
      ScopedGroup g(H5Gcreate2(f.get(), ("g_many_" + std::to_string(i)).c_str(),
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(g.valid());
    }
    st.write.FailAfter(0, -1);
    EXPECT_LT(H5Fflush(f.get(), H5F_SCOPE_LOCAL), 0);
    st.Reset();

    // 7d: Fresh file so H5Fset_libver_bounds triggers H5F__flush -> fail write
    //     (3244), without leftover dirty cache errors from 7a-7c.
    {
      const auto p_libver = dir.child("libver_flush_fault.h5");
      ScopedFile flv(
          H5Fcreate(p_libver.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(flv.valid());
      ASSERT_GE(H5Fflush(flv.get(), H5F_SCOPE_LOCAL), 0);
      ScopedGroup dg(H5Gcreate2(flv.get(), "dirty_lv", H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
      ASSERT_TRUE(dg.valid());
      st.write.FailAfter(0, -1);
      EXPECT_LT(H5Fset_libver_bounds(flv.get(), H5F_LIBVER_LATEST,
                                     H5F_LIBVER_LATEST),
                0);
      st.Reset();
    }

    // 7e: get_eoa / read failure on H5Fget_file_image + set_eoa / get_eof fail.
    const ssize_t img_len = H5Fget_file_image(f.get(), nullptr, 0);
    ASSERT_GT(img_len, 0);
    std::vector<uint8_t> img_buf(static_cast<size_t>(img_len) + 64);

    st.get_eoa.FailAfter(0, 1);
    EXPECT_LT(H5Fget_file_image(f.get(), nullptr, 0), 0);
    st.Reset();

    st.read.FailAfter(0, 1);
    EXPECT_LT(H5Fget_file_image(f.get(), img_buf.data(), img_buf.size()), 0);
    st.Reset();

    {
      ScopedApiContext ctx(fapl.get());
      H5F_t* hf = UnwrapFile(f.get());
      st.set_eoa.FailAfter(0, 1);
      EXPECT_LT(H5F__set_eoa(hf, H5FD_MEM_DEFAULT, 128), 0);
      st.Reset();

      st.get_eof.FailAfter(0, 1);
      haddr_t max_e = 0;
      EXPECT_LT(H5F__get_max_eof_eoa(hf, &max_e), 0);
      st.Reset();
    }

    // 7f: Failing write during close (H5MF_close / H5F__flush_phase1/2 /
    // H5AC_dest) when file handle is closed via H5F_open -> H5F_try_close?
    // Test clean H5F_open error unwind with free_on_failure=true, or close.
    f.reset();

    // 7g: H5Fdelete fail (2513).
    st.del.FailAfter(0, 1);
    EXPECT_LT(H5Fdelete(p_flush.c_str(), fapl.get()), 0);
    st.Reset();
    EXPECT_GE(H5Fdelete(p_flush.c_str(), fapl.get()), 0);
  }

  // 8. Paged file: H5PB_flush fail (2390), H5PB_dest fail (1571),
  //    H5Fget_info2 read fail (3405, 3409, 3414), H5Fformat_convert fail (4120).
  {
    const auto p_paged = dir.child("paged_fault.h5");
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_GE(
        H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE, true, 1),
        0);
    ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), 4096), 0);
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
    ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_ATTR_FLAG, 1),
              0);

    ScopedPlist fapl = MakeFaultVfdFapl(vfd_id);
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST,
                                   H5F_LIBVER_LATEST),
              0);
    ScopedPlist fapl_pb = MakeFaultVfdFapl(vfd_id);
    ASSERT_GE(H5Pset_libver_bounds(fapl_pb.get(), H5F_LIBVER_LATEST,
                                   H5F_LIBVER_LATEST),
              0);
    ASSERT_GE(H5Pset_page_buffer_size(fapl_pb.get(), 8192, 50, 50), 0);

    {
      ScopedFile f(H5Fcreate(p_paged.c_str(), H5F_ACC_TRUNC, fcpl.get(),
                             fapl_pb.get()));
      ASSERT_TRUE(f.valid());
      {
        ScopedGroup g1(H5Gcreate2(f.get(), "pg", H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
        ASSERT_TRUE(g1.valid());
        ScopedSpace sp(H5Screate(H5S_SCALAR));
        int64_t val = 99;
        ScopedAttr a1(H5Acreate2(g1.get(), "a1", H5T_NATIVE_INT64, sp.get(),
                                 H5P_DEFAULT, H5P_DEFAULT));
        ASSERT_TRUE(a1.valid());
        ASSERT_GE(H5Awrite(a1.get(), H5T_NATIVE_INT64, &val), 0);
        ScopedAttr a2(H5Acreate2(g1.get(), "a2", H5T_NATIVE_INT64, sp.get(),
                                 H5P_DEFAULT, H5P_DEFAULT));
        ASSERT_TRUE(a2.valid());
        ASSERT_GE(H5Awrite(a2.get(), H5T_NATIVE_INT64, &val), 0);
      }
      {
        ScopedGroup gone(H5Gcreate2(f.get(), "gone", H5P_DEFAULT, H5P_DEFAULT,
                                    H5P_DEFAULT));
        ASSERT_TRUE(gone.valid());
      }
      ASSERT_GE(H5Ldelete(f.get(), "gone", H5P_DEFAULT), 0);
      ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_LOCAL), 0);

      // Fail a write during flush on a new dirty group using page buffering.
      {
        ScopedGroup g2(H5Gcreate2(f.get(), "pg2", H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
        ASSERT_TRUE(g2.valid());
      }
      st.write.FailAfter(0, -1);
      EXPECT_LT(H5Fflush(f.get(), H5F_SCOPE_LOCAL), 0);
      st.Reset();
    }

    // Reopen fresh without page buffer and evict cache so H5Fget_info2 fails
    // on superblock size (3405), FSM (3409), and SOHM (3414) reads.
    {
      ScopedFile f(H5Fopen(p_paged.c_str(), H5F_ACC_RDWR, fapl.get()));
      ASSERT_TRUE(f.valid());
      ScopedApiContext ctx(fapl.get());
      H5F_t* hf = UnwrapFile(f.get());
      ASSERT_NE(hf, nullptr);

      int info_failures = 0;
      for (int ok_reads = 0; ok_reads <= 8; ++ok_reads) {
        ASSERT_GE(H5AC_evict(hf), 0);
        H5F_info2_t info{};
        st.read.FailAfter(ok_reads, -1);
        const herr_t rc = H5Fget_info2(f.get(), &info);
        st.Reset();
        if (rc >= 0) {
          break;
        }
        ++info_failures;
      }
      EXPECT_GE(info_failures, 3);
    }

    // Reopen fresh without page buffer, evict cache, and fail
    // H5Fformat_convert on extension message remove (4120).
    {
      ScopedFile f(H5Fopen(p_paged.c_str(), H5F_ACC_RDWR, fapl.get()));
      ASSERT_TRUE(f.valid());
      ScopedApiContext ctx(fapl.get());
      H5F_t* hf = UnwrapFile(f.get());
      ASSERT_NE(hf, nullptr);
      ASSERT_GE(H5AC_evict(hf), 0);
      st.read.FailAfter(0, -1);
      EXPECT_LT(H5Fformat_convert(f.get()), 0);
      st.Reset();
    }

    // Fail H5Fformat_convert at H5F__super_ext_remove_msg (4120), H5MF_try_close
    // (4125), and H5F_super_dirty (4141).
    {
      int convert_failures = 0;
      for (int ok_reads = 0; ok_reads <= 5; ++ok_reads) {
        const auto p_fc =
            dir.child("paged_fc_" + std::to_string(ok_reads) + ".h5");
        {
          ScopedFile f(
              H5Fcreate(p_fc.c_str(), H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
          ASSERT_TRUE(f.valid());
          {
            ScopedGroup g1(H5Gcreate2(f.get(), "gone", H5P_DEFAULT, H5P_DEFAULT,
                                      H5P_DEFAULT));
            ASSERT_TRUE(g1.valid());
          }
          {
            ScopedGroup g2(H5Gcreate2(f.get(), "keep_tail", H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT));
            ASSERT_TRUE(g2.valid());
          }
          ASSERT_GE(H5Ldelete(f.get(), "gone", H5P_DEFAULT), 0);
        }
        ScopedFile f(H5Fopen(p_fc.c_str(), H5F_ACC_RDWR, fapl.get()));
        ASSERT_TRUE(f.valid());
        {
          ScopedApiContext ctx(fapl.get());
          H5F_t* hf = UnwrapFile(f.get());
          ASSERT_NE(hf, nullptr);
          ASSERT_GE(H5AC_evict(hf), 0);
        }
        st.read.FailAfter(ok_reads, -1);
        const herr_t rc = H5Fformat_convert(f.get());
        st.Reset();
        if (rc >= 0) {
          break;
        }
        ++convert_failures;
      }
      EXPECT_GE(convert_failures, 2);

      const auto p_conv_dirty = dir.child("conv_super_dirty.h5");
      ScopedFile f(
          H5Fcreate(p_conv_dirty.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
      ScopedApiContext ctx(fapl.get());
      H5F_t* hf = UnwrapFile(f.get());
      ASSERT_NE(hf, nullptr);
      ASSERT_NE(hf->shared->sblock, nullptr);
      hf->shared->sblock->cache_info.is_pinned = false;
      absl::Cleanup restore_pin = [&] {
        hf->shared->sblock->cache_info.is_pinned = true;
      };
      EXPECT_LT(H5Fformat_convert(f.get()), 0);
    }
  }

  // 9. H5F__dest flush/trunc/close/accum/phase1/cache-image/mf_close error
  //    paths + H5F__flush_phase1/2.
  {
    const auto p_dest = dir.child("dest_fault.h5");
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_GE(H5Pset_file_space_strategy(fcpl.get(),
                                         H5F_FSPACE_STRATEGY_FSM_AGGR, true, 1),
              0);
    ScopedPlist fapl = MakeFaultVfdFapl(vfd_id);
    ScopedFile f(
        H5Fcreate(p_dest.c_str(), H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(f.valid());

    // Dirty an attribute so H5F__flush_phase1/2 + H5MF_close + truncate +
    // accum_reset + H5AC_dest + H5FD_close are all exercised.
    {
      ScopedSpace sp(H5Screate(H5S_SCALAR));
      ScopedAttr a(H5Acreate2(f.get(), "dirty_a", H5T_NATIVE_INT32, sp.get(),
                              H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(a.valid());
    }
    st.write.FailAfter(0, -1);
    st.truncate.FailAfter(0, -1);
    st.flush.FailAfter(0, -1);
    st.close.FailAfter(0, -1);
    EXPECT_LT(H5Fclose(f.release()), 0);
    st.Reset();
  }
  {
    // 9b: Phase-1 H5MF_free_aggrs fail -> hits 2316 and 1426 cleanly.
    const auto p_aggr = dir.child("close_phase1_aggr_fail.h5");
    ScopedPlist fapl = MakeFaultVfdFapl(vfd_id);
    ASSERT_GE(H5Pset_meta_block_size(fapl.get(), 4096), 0);
    ScopedFile f(
        H5Fcreate(p_aggr.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
    ScopedGroup g(
        H5Gcreate2(f.get(), "g1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
    g.reset();
    st.set_eoa.FailAfter(0, 1);
    EXPECT_LT(H5Fclose(f.release()), 0);
    st.Reset();
  }
  {
    // 9c: H5AC_prep_for_file_close failure on unread cache image (1434).
    const auto p_ci = dir.child("close_ci_read_fail.h5");
    {
      ScopedPlist fapl_ci = MakeFaultVfdFapl(vfd_id);
      ASSERT_GE(H5Pset_libver_bounds(fapl_ci.get(), H5F_LIBVER_V110,
                                     H5F_LIBVER_LATEST),
                0);
      H5AC_cache_image_config_t cfg{};
      cfg.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
      cfg.generate_image = true;
      cfg.save_resize_status = false;
      cfg.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE;
      ASSERT_GE(H5Pset_mdc_image_config(fapl_ci.get(), &cfg), 0);
      ScopedFile f(
          H5Fcreate(p_ci.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl_ci.get()));
      ASSERT_TRUE(f.valid());
      ScopedGroup g(
          H5Gcreate2(f.get(), "cg", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(g.valid());
    }
    ScopedPlist fapl_rd = MakeFaultVfdFapl(vfd_id);
    ScopedFile f_rd(H5Fopen(p_ci.c_str(), H5F_ACC_RDONLY, fapl_rd.get()));
    ASSERT_TRUE(f_rd.valid());
    st.read.FailAfter(0, 1);
    EXPECT_LT(H5Fclose(f_rd.release()), 0);
    st.Reset();
  }
  {
    // 9d: H5MF_close failure on persistent-FSM ext-msg write / shrink (1490).
    {
      int close_failures = 0;
      for (int ok_reads = 0; ok_reads <= 1; ++ok_reads) {
        const auto p_mf =
            dir.child("close_mf_fail_" + std::to_string(ok_reads) + ".h5");
        ScopedPlist fcpl_mf(H5Pcreate(H5P_FILE_CREATE));
        ASSERT_GE(H5Pset_file_space_strategy(
                      fcpl_mf.get(), H5F_FSPACE_STRATEGY_FSM_AGGR, true, 1),
                  0);
        ScopedPlist fapl_mf = MakeFaultVfdFapl(vfd_id);
        ASSERT_GE(H5Pset_libver_bounds(fapl_mf.get(), H5F_LIBVER_V110,
                                       H5F_LIBVER_LATEST),
                  0);
        ScopedFile f(H5Fcreate(p_mf.c_str(), H5F_ACC_TRUNC, fcpl_mf.get(),
                               fapl_mf.get()));
        ASSERT_TRUE(f.valid());
        {
          ScopedGroup g(
              H5Gcreate2(f.get(), "mg", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
          ASSERT_TRUE(g.valid());
        }
        ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_LOCAL), 0);
        {
          ScopedApiContext ctx(fapl_mf.get());
          H5F_t* hf = UnwrapFile(f.get());
          ASSERT_NE(hf, nullptr);
          ASSERT_GE(H5AC_evict(hf), 0);
        }
        st.read.FailAfter(ok_reads, 1);
        if (H5Fclose(f.release()) < 0) {
          ++close_failures;
        }
        st.Reset();
      }
      EXPECT_GE(close_failures, 1);
    }
  }

  // 10. H5F__start_swmr_write error paths & rollback cleanup.
  {
    const auto p_swmr = dir.child("swmr_vfd_fault.h5");
    ScopedPlist fapl = MakeFaultVfdFapl(vfd_id);
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110,
                                   H5F_LIBVER_LATEST),
              0);
    ASSERT_GE(H5Pset_file_locking(fapl.get(), true, true), 0);

    // 10a: Flush failure at start_swmr_write line 3837 -> unlock fail line 4061.
    {
      ScopedFile f(
          H5Fcreate(p_swmr.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
      ScopedGroup g(
          H5Gcreate2(f.get(), "sg", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(g.valid());
      st.write.FailAfter(0, -1);
      st.unlock.FailAfter(0, 1);
      EXPECT_LT(H5Fstart_swmr_write(f.get()), 0);
      st.Reset();
    }

    // 10b: Fail unlock at 4006 -> enters 4007.
    {
      ScopedFile f(
          H5Fcreate(p_swmr.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
      st.unlock.FailAfter(0, 2);
      EXPECT_LT(H5Fstart_swmr_write(f.get()), 0);
      st.Reset();
    }

    // 10c: Succeed unlock, fail lock at 4009 -> enters 4010 and rollback 4034+!
    {
      ScopedFile f(
          H5Fcreate(p_swmr.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
      st.lock.FailAfter(0, 1);
      EXPECT_LT(H5Fstart_swmr_write(f.get()), 0);
      st.Reset();
    }

    // 10d: Succeed unlock + lock, fail superblock flush at 4020 AND rollback
    //      superblock flush at 4054!
    {
      ScopedFile f(
          H5Fcreate(p_swmr.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
      ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_LOCAL), 0);
      st.write.FailAfter(0, 2);
      EXPECT_LT(H5Fstart_swmr_write(f.get()), 0);
      st.Reset();
    }

    // 10e: H5F_flush_tagged_metadata(ext_addr) fail -> line 3833.
    {
      ScopedFile f(
          H5Fcreate(p_swmr.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
      st.flush.FailAfter(0, 1);
      EXPECT_LT(H5Fstart_swmr_write(f.get()), 0);
      st.Reset();
    }

    // 10f: H5O_get_native_info fail on evicted open group -> line 3869.
    {
      ScopedFile f(
          H5Fcreate(p_swmr.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
      ScopedGroup g(
          H5Gcreate2(f.get(), "sg", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(g.valid());
      ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_LOCAL), 0);
      ScopedApiContext ctx(fapl.get());
      H5F_t* hf = UnwrapFile(f.get());
      ASSERT_NE(hf, nullptr);
      ASSERT_GE(H5AC_evict(hf), 0);
      st.read.FailAfter(0, -1);
      EXPECT_LT(H5Fstart_swmr_write(f.get()), 0);
      st.Reset();
    }

    // 10g: H5D_get_access_plist fail on open dataset -> line 3934.
    {
      ScopedFile f(
          H5Fcreate(p_swmr.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
      constexpr hsize_t kOne = 1;
      ScopedSpace dsp(H5Screate_simple(1, &kOne, nullptr));
      ScopedDataset ds(H5Dcreate2(f.get(), "ds", H5T_NATIVE_INT32, dsp.get(),
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(ds.valid());
      ScopedApiContext ctx(fapl.get());
      auto* d = static_cast<H5D_t*>(H5VL_object(ds.get()));
      ASSERT_NE(d, nullptr);
      const hid_t orig_dapl = d->shared->dapl_id;
      d->shared->dapl_id = H5I_INVALID_HID;
      absl::Cleanup restore_dapl = [&] { d->shared->dapl_id = orig_dapl; };
      EXPECT_LT(H5Fstart_swmr_write(f.get()), 0);
    }

    // 10h: H5F_super_dirty fail at line 4016 AND rollback line 4051.
    {
      ScopedFile f(
          H5Fcreate(p_swmr.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(f.valid());
      ASSERT_GE(H5Fflush(f.get(), H5F_SCOPE_LOCAL), 0);
      ScopedApiContext ctx(fapl.get());
      H5F_t* hf = UnwrapFile(f.get());
      ASSERT_NE(hf, nullptr);
      ASSERT_NE(hf->shared->sblock, nullptr);
      hf->shared->sblock->cache_info.is_pinned = false;
      absl::Cleanup restore_pin = [&] {
        hf->shared->sblock->cache_info.is_pinned = true;
      };
      EXPECT_LT(H5Fstart_swmr_write(f.get()), 0);
    }
  }
}

TEST(H5FintTest, CustomFaultInjectingVolErrors) {
  ASSERT_GE(H5open(), 0);
  ASSERT_NE(H5VL_NATIVE_conn_g, nullptr);
  ASSERT_NE(H5VL_NATIVE_conn_g->cls, nullptr);

  H5VL_class_t vol_cls = *H5VL_NATIVE_conn_g->cls;
  vol_cls.value = static_cast<H5VL_class_value_t>(501);
  vol_cls.name = "fault_native_vol";
  vol_cls.info_cls.size = sizeof(int);
  vol_cls.info_cls.copy = FaultVolInfoCopy;
  vol_cls.info_cls.cmp = FaultVolInfoCmp;
  vol_cls.info_cls.free = FaultVolInfoFree;
  vol_cls.wrap_cls.get_object = FaultVolGetObject;
  vol_cls.wrap_cls.get_wrap_ctx = FaultVolGetWrapCtx;
  vol_cls.wrap_cls.wrap_object = FaultVolWrapObject;
  vol_cls.wrap_cls.unwrap_object = FaultVolUnwrapObject;
  vol_cls.wrap_cls.free_wrap_ctx = FaultVolFreeWrapCtx;
  vol_cls.object_cls.get = FaultVolObjectGet;

  const hid_t vol_id = H5VLregister_connector(&vol_cls, H5P_VOL_INITIALIZE_DEFAULT);
  ASSERT_NE(vol_id, H5I_INVALID_HID);
  auto& st = GetFaultVolState();
  st.Reset();
  absl::Cleanup cleanup = [&] {
    st.Reset();
    H5VLunregister_connector(vol_id);
  };

  ScopedErrorSilence silence;
  ScopedPlist fapl = MakeCoreFapl();
  int info_val = 42;
  ASSERT_GE(H5Pset_vol(fapl.get(), vol_id, &info_val), 0);

  // 1. Failing info_cls.copy in H5F__set_vol_conn (337, 1342).
  {
    st.fail_info_copy = true;
    EXPECT_LT(H5Fcreate("vol_copy_fail.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                        fapl.get()),
              0);
    st.fail_info_copy = false;
  }

  // 2. Failing info_cls.copy in H5F_get_access_plist (486) & info_cls.free in
  //    H5F__dest (1613).
  {
    ScopedFile f(
        H5Fcreate("vol_fapl_fail.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
    st.fail_info_copy = true;
    EXPECT_LT(H5Fget_access_plist(f.get()), 0);
    st.fail_info_copy = false;

    st.fail_info_free = true;
    f.reset();
    st.fail_info_free = false;
  }

  // 3. Failing wrap_cls.unwrap_object in H5F__dest (1659).
  {
    ScopedFile f(
        H5Fcreate("vol_unwrap_fail.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(f.valid());
    ScopedApiContext ctx(fapl.get());
    H5F_t* hf = UnwrapFile(f.get());
    ASSERT_NE(hf, nullptr);
    ASSERT_GE(H5VL_set_vol_wrapper(hf->vol_obj), 0);
    absl::Cleanup reset_wrap = [] { H5VL_reset_vol_wrapper(); };

    st.fail_unwrap_object = true;
    f.reset();
    st.fail_unwrap_object = false;
  }

  // 4. H5F_get_file_id error paths (4180, 4184, 4190, 4194, 4208) &
  //    H5F_get_id error path (2753).
  {
    ScopedGroup grp;
    {
      ScopedFile f(H5Fcreate("vol_get_fid.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                             fapl.get()));
      ASSERT_TRUE(f.valid());
      grp.reset(
          H5Gcreate2(f.get(), "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(grp.valid());
    }  // file ID dropped; grp keeps file alive via shared reference

    ScopedApiContext ctx(fapl.get());
    H5VL_object_t* grp_vol = H5VL_vol_object(grp.get());
    ASSERT_NE(grp_vol, nullptr);

    st.fail_object_get_file = true;
    EXPECT_LT(H5F_get_file_id(grp_vol, H5I_GROUP, true), 0);
    st.fail_object_get_file = false;

    st.null_object_get_file = true;
    EXPECT_LT(H5F_get_file_id(grp_vol, H5I_GROUP, true), 0);
    st.null_object_get_file = false;

    st.fail_get_wrap_ctx.FailAfter(0, 1);
    EXPECT_LT(H5F_get_file_id(grp_vol, H5I_GROUP, true), 0);
    st.fail_get_wrap_ctx.FailAfter(1, 1);
    EXPECT_LT(H5F_get_file_id(grp_vol, H5I_GROUP, true), 0);
    st.fail_get_wrap_ctx.Reset();

    st.fail_wrap_object = true;
    EXPECT_LT(H5F_get_file_id(grp_vol, H5I_GROUP, true), 0);
    st.fail_wrap_object = false;

    // Resurrect file ID via H5Iget_file_id and test H5F_get_id wrap failure.
    ScopedFile rf(H5Iget_file_id(grp.get()));
    ASSERT_TRUE(rf.valid());
    H5F_t* rf_hf = UnwrapFile(rf.get());
    ASSERT_NE(rf_hf, nullptr);
    rf.reset();  // drop file ID again; grp still keeps rf_hf valid

    // Without vol_wrap_ctx in H5CX, H5VL_wrap_register fails at 2753.
    EXPECT_LT(H5F_get_id(rf_hf), 0);
  }
}

TEST(H5FintTest, InternalEdgeErrorPathsAndLifecycleFaults) {
  ASSERT_GE(H5open(), 0);
  TempDir dir("internal_edge");
  const auto existing_path = dir.child("existing.h5");
  {
    ScopedPlist fapl = MakeSec2Fapl();
    ScopedFile f(H5Fcreate(existing_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                           fapl.get()));
    ASSERT_TRUE(f.valid());
  }

  ScopedErrorSilence silence;

  // 1. H5F_get_access_plist errors when default FAPL ID is invalid (383),
  //    H5P_copy_plist fails in a property copy callback (385), or any FAPL
  //    property set fails during population (lines 391..494).
  {
    ScopedPlist fcpl_pb(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_GE(
        H5Pset_file_space_strategy(fcpl_pb.get(), H5F_FSPACE_STRATEGY_PAGE,
                                   false, 1),
        0);
    ASSERT_GE(H5Pset_file_space_page_size(fcpl_pb.get(), 4096), 0);

    ScopedPlist fapl_pb = MakeSec2Fapl();
    ASSERT_GE(H5Pset_libver_bounds(fapl_pb.get(), H5F_LIBVER_LATEST,
                                   H5F_LIBVER_LATEST),
              0);
    ASSERT_GE(H5Pset_page_buffer_size(fapl_pb.get(), 4096, 50, 50), 0);
    ASSERT_GE(H5Pset_fclose_degree(fapl_pb.get(), H5F_CLOSE_STRONG), 0);

    ScopedFile f(H5Fcreate(dir.child("fapl_copy_edge.h5").c_str(),
                           H5F_ACC_TRUNC, fcpl_pb.get(), fapl_pb.get()));
    ASSERT_TRUE(f.valid());
    ScopedApiContext ctx(fapl_pb.get());
    H5F_t* hf = UnwrapFile(f.get());
    ASSERT_NE(hf, nullptr);

    const hid_t orig_default_fapl = H5P_LST_FILE_ACCESS_ID_g;
    absl::Cleanup restore_default_fapl = [&] {
      H5P_LST_FILE_ACCESS_ID_g = orig_default_fapl;
      g_failed_set_plist_id = H5I_INVALID_HID;
    };

    H5P_LST_FILE_ACCESS_ID_g = H5I_INVALID_HID;
    EXPECT_LT(H5F_get_access_plist(hf, false), 0);
    H5P_LST_FILE_ACCESS_ID_g = orig_default_fapl;

    ScopedPlist bad_copy_fapl = MakeCoreFapl();
    int prop_val = 1;
    ASSERT_GE(H5Pinsert2(bad_copy_fapl.get(), "fail_copy_prop", sizeof(int),
                         &prop_val, nullptr, nullptr, nullptr,
                         FailingCopyPropCb, nullptr, nullptr),
              0);
    H5P_LST_FILE_ACCESS_ID_g = bad_copy_fapl.get();
    EXPECT_LT(H5F_get_access_plist(hf, false), 0);
    H5P_LST_FILE_ACCESS_ID_g = orig_default_fapl;

    const char* const kSetFailProps[] = {
        H5F_ACS_META_CACHE_INIT_CONFIG_NAME,
        H5F_ACS_DATA_CACHE_NUM_SLOTS_NAME,
        H5F_ACS_DATA_CACHE_BYTE_SIZE_NAME,
        H5F_ACS_PREEMPT_READ_CHUNKS_NAME,
        H5F_ACS_ALIGN_THRHD_NAME,
        H5F_ACS_ALIGN_NAME,
        H5F_ACS_GARBG_COLCT_REF_NAME,
        H5F_ACS_META_BLOCK_SIZE_NAME,
        H5F_ACS_SIEVE_BUF_SIZE_NAME,
        H5F_ACS_SDATA_BLOCK_SIZE_NAME,
        H5F_ACS_LIBVER_LOW_BOUND_NAME,
        H5F_ACS_LIBVER_HIGH_BOUND_NAME,
        H5F_ACS_USE_FILE_LOCKING_NAME,
        H5F_ACS_IGNORE_DISABLED_FILE_LOCKS_NAME,
        H5F_ACS_METADATA_READ_ATTEMPTS_NAME,
        H5F_ACS_OBJECT_FLUSH_CB_NAME,
        H5F_ACS_EFC_SIZE_NAME,
        H5F_ACS_PAGE_BUFFER_SIZE_NAME,
        H5F_ACS_PAGE_BUFFER_MIN_META_PERC_NAME,
        H5F_ACS_PAGE_BUFFER_MIN_RAW_PERC_NAME,
        H5F_ACS_META_CACHE_INIT_IMAGE_CONFIG_NAME,
        H5F_ACS_RFIC_FLAGS_NAME,
        H5F_ACS_CLOSE_DEGREE_NAME,
    };
    auto inject_set_fail = [&](const char* prop_name) {
      ScopedPlist mod_fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_TRUE(mod_fapl.valid());
      size_t psize = 0;
      ASSERT_GE(H5Pget_size(mod_fapl.get(), prop_name, &psize), 0) << prop_name;
      std::vector<uint8_t> zeros(psize, 0);
      ASSERT_GE(H5Premove(mod_fapl.get(), prop_name), 0) << prop_name;
      ASSERT_GE(H5Pinsert2(mod_fapl.get(), prop_name, psize, zeros.data(),
                           FailingSetPropCb, nullptr, nullptr, nullptr, nullptr,
                           nullptr),
                0)
          << prop_name;
      g_failed_set_plist_id = H5I_INVALID_HID;
      H5P_LST_FILE_ACCESS_ID_g = mod_fapl.get();
      EXPECT_LT(H5F_get_access_plist(hf, false), 0) << prop_name;
      H5P_LST_FILE_ACCESS_ID_g = orig_default_fapl;
      if (g_failed_set_plist_id != H5I_INVALID_HID) {
        EXPECT_GE(H5I_dec_ref(g_failed_set_plist_id), 0);
        g_failed_set_plist_id = H5I_INVALID_HID;
      }
    };

    for (const char* prop_name : kSetFailProps) {
      inject_set_fail(prop_name);
    }

    const auto orig_deg = hf->shared->fc_degree;
    hf->shared->fc_degree = H5F_CLOSE_DEFAULT;
    absl::Cleanup restore_deg = [&] { hf->shared->fc_degree = orig_deg; };
    inject_set_fail(H5F_ACS_CLOSE_DEGREE_NAME);
  }

  // 2. H5F__new alignment threshold/alignment get failures after H5FD_open
  //    reads them once (lines 1222, 1224).
  {
    absl::Cleanup reset_skip = [] { g_fail_get_skip = 0; };
    ScopedApiContext ctx;
    for (const char* prop : {H5F_ACS_ALIGN_THRHD_NAME, H5F_ACS_ALIGN_NAME}) {
      ScopedPlist fapl = MakeSec2Fapl();
      ASSERT_GE(H5Premove(fapl.get(), prop), 0);
      hsize_t val = 0;
      ASSERT_GE(H5Pinsert2(fapl.get(), prop, sizeof(hsize_t), &val, nullptr,
                           CountdownFailGet, nullptr, nullptr, nullptr, nullptr),
                0);
      g_fail_get_skip = 1;
      H5F_t* hf = nullptr;
      EXPECT_LT(H5F_open(false, &hf, existing_path.c_str(), H5F_ACC_RDONLY,
                         H5P_FILE_CREATE_DEFAULT, fapl.get()),
                0);
      EXPECT_EQ(hf, nullptr);
    }
  }

  // 3. H5F__dest invalid fcpl_id (lines 1604, 1607).
  {
    ScopedPlist sec2_fapl = MakeSec2Fapl();
    ScopedApiContext ctx(sec2_fapl.get());
    H5F_t* hf = nullptr;
    ASSERT_GE(H5F_open(false, &hf, existing_path.c_str(), H5F_ACC_RDONLY,
                       H5P_FILE_CREATE_DEFAULT, sec2_fapl.get()),
              0);
    ASSERT_NE(hf, nullptr);
    ASSERT_GE(H5F__post_open(hf), 0);

    const hid_t orig_fcpl = hf->shared->fcpl_id;
    hf->shared->fcpl_id = H5I_INVALID_HID;
    EXPECT_LT(H5F_try_close(hf, nullptr), 0);
    EXPECT_GE(H5I_dec_ref(orig_fcpl), 0);
    hf = H5FL_FREE(H5F_t, hf);
  }

  // 4. H5F__dest without active H5CX triggers H5CX_get_vol_wrap_ctx error (1657).
  {
    ScopedPlist sec2_fapl = MakeSec2Fapl();
    ScopedFile hold(
        H5Fopen(existing_path.c_str(), H5F_ACC_RDONLY, sec2_fapl.get()));
    ASSERT_TRUE(hold.valid());

    H5F_t* hf2 = nullptr;
    {
      ScopedApiContext ctx(sec2_fapl.get());
      ASSERT_GE(H5F_open(false, &hf2, existing_path.c_str(), H5F_ACC_RDONLY,
                         H5P_FILE_CREATE_DEFAULT, sec2_fapl.get()),
                0);
      ASSERT_NE(hf2, nullptr);
      ASSERT_GE(H5F__post_open(hf2), 0);
    }  // H5CX stack is now empty -> H5CX_get_vol_wrap_ctx fails at 1657

    EXPECT_LT(H5F_try_close(hf2, nullptr), 0);
    hf2 = H5FL_FREE(H5F_t, hf2);
  }

  // 5. H5F__dest with non-empty obj_count fails at H5FO_top_dest (line 1678).
  {
    ScopedPlist sec2_fapl = MakeSec2Fapl();
    ScopedApiContext ctx(sec2_fapl.get());
    H5F_t* hf = nullptr;
    ASSERT_GE(H5F_open(false, &hf, existing_path.c_str(), H5F_ACC_RDONLY,
                       H5P_FILE_CREATE_DEFAULT, sec2_fapl.get()),
              0);
    ASSERT_NE(hf, nullptr);
    ASSERT_GE(H5F__post_open(hf), 0);
    ASSERT_GE(H5FO_top_incr(hf, 0x1000), 0);

    EXPECT_LT(H5F_try_close(hf, nullptr), 0);
    EXPECT_GE(H5FO_top_decr(hf, 0x1000), 0);
    EXPECT_GE(H5FO_top_dest(hf), 0);
    hf = H5FL_FREE(H5F_t, hf);
  }
}

}  // namespace
}  // namespace hdf5
