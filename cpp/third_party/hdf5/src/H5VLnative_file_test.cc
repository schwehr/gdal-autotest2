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
// Tests native VOL connector file callbacks (`H5VL__native_file_*`),
// public H5F* metadata/cache/logging/SWMR/page-buffering/elink/EOA APIs,
// and direct negative branches (third_party/hdf5/src/H5VLnative_file.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLnative_file.c

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5VLNATIVE_FILE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5F_FRIEND
#pragma push_macro("try")
#undef try
#define try h5_try_param
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FLprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Pprivate.h"
#include "third_party/hdf5/src/H5VLnative_private.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#pragma pop_macro("try")
}  // extern "C"
#ifdef H5VLNATIVE_FILE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5VLNATIVE_FILE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  ScopedApiContext() { pushed_ = (H5CX_push(&node_) >= 0); }
  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }
  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

TEST(H5VLNativeFileTest, GetSpecificOptionalAndReopenClose) {
  TempDir dir("vl_file_dir");
  const std::string main_path = (dir.path() / "main_long_name.h5").string();
  const std::string ext_path = (dir.path() / "external_target.h5").string();
  const std::string child_path = (dir.path() / "mounted_child.h5").string();
  const std::string mdc_log_path = (dir.path() / "mdc.log").string();

  // Create external target and mounted child files.
  {
    ScopedFile ext_f(
        H5Fcreate(ext_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ext_f.valid());
    ScopedGroup eg(H5Gcreate2(ext_f.get(), "/target_grp", H5P_DEFAULT,
                              H5P_DEFAULT, H5P_DEFAULT));
    EXPECT_TRUE(eg.valid());
  }
  {
    ScopedFile child_f(
        H5Fcreate(child_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(child_f.valid());
  }

  // Build FCPL + FAPL covering MDC logging, MDC image, dset no-attrs hint, and v110.
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110), 0);
  ASSERT_GE(H5Pset_elink_file_cache_size(fapl.get(), 8), 0);
  ASSERT_GE(H5Pset_mdc_log_options(fapl.get(), true, mdc_log_path.c_str(),
                                   /*start_on_access=*/false),
            0);
  H5AC_cache_image_config_t img_cfg{};
  img_cfg.version = H5AC__CURR_CACHE_IMAGE_CONFIG_VERSION;
  img_cfg.generate_image = true;
  img_cfg.save_resize_status = false;
  img_cfg.entry_ageout = H5AC__CACHE_IMAGE__ENTRY_AGEOUT__NONE;
  ASSERT_GE(H5Pset_mdc_image_config(fapl.get(), &img_cfg), 0);

  {
    ScopedFile file(H5Fcreate(main_path.c_str(), H5F_ACC_TRUNC, fcpl.get(),
                              fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedGroup mnt_pt(
        H5Gcreate2(file.get(), "/mnt", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(mnt_pt.valid());
    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ScopedDataset ds(H5Dcreate2(file.get(), "/dset", H5T_NATIVE_INT32,
                                scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());
    const int32_t v = 123;
    ASSERT_GE(H5Dwrite(ds.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       &v),
              0);

    // File gets: FAPL, FCPL, intent, fileno, name (null/full/truncated), obj_count/ids.
    ScopedPlist got_fapl(H5Fget_access_plist(file.get()));
    EXPECT_TRUE(got_fapl.valid());
    ScopedPlist got_fcpl(H5Fget_create_plist(file.get()));
    EXPECT_TRUE(got_fcpl.valid());
    unsigned intent = 0;
    EXPECT_GE(H5Fget_intent(file.get(), &intent), 0);
    EXPECT_EQ(intent, H5F_ACC_RDWR);
    unsigned long fileno = 0;
    EXPECT_GE(H5Fget_fileno(file.get(), &fileno), 0);

    const ssize_t name_len = H5Fget_name(file.get(), nullptr, 0);
    ASSERT_GT(name_len, 4);
    std::string full_name(static_cast<size_t>(name_len) + 1, '\0');
    EXPECT_EQ(H5Fget_name(ds.get(), full_name.data(), full_name.size()),
              name_len);
    std::string trunc_name(4, 'x');
    EXPECT_EQ(H5Fget_name(file.get(), trunc_name.data(), trunc_name.size()),
              name_len);
    EXPECT_EQ(trunc_name.back(), '\0');

    EXPECT_GE(H5Fget_obj_count(file.get(), H5F_OBJ_ALL), 1);
    std::vector<hid_t> obj_ids(8, H5I_INVALID_HID);
    const ssize_t n_objs =
        H5Fget_obj_ids(file.get(), H5F_OBJ_ALL, obj_ids.size(), obj_ids.data());
    EXPECT_GE(n_objs, 1);
    // H5Fget_obj_ids returns existing handles without incrementing refcounts.
    for (ssize_t i = 0; i < n_objs && i < static_cast<ssize_t>(obj_ids.size());
         ++i) {
      EXPECT_GT(H5Iis_valid(obj_ids[i]), 0);
    }

    // Mount + global/local flush + unmount.
    {
      ScopedFile child(H5Fopen(child_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
      ASSERT_TRUE(child.valid());
      ASSERT_GE(H5Fmount(file.get(), "/mnt", child.get(), H5P_DEFAULT), 0);
      EXPECT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
      EXPECT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);
      EXPECT_GE(H5Funmount(file.get(), "/mnt"), 0);
    }

    // External link traversal creates shared->efc; then clear EFC.
    ASSERT_GE(H5Lcreate_external(ext_path.c_str(), "/target_grp", file.get(),
                                 "/ext_link", H5P_DEFAULT, H5P_DEFAULT),
              0);
    {
      ScopedGroup eg(H5Gopen2(file.get(), "/ext_link", H5P_DEFAULT));
      EXPECT_TRUE(eg.valid());
    }
    EXPECT_GE(H5Fclear_elink_file_cache(file.get()), 0);

    // Optional file queries and setters.
    hsize_t fsize = 0;
    EXPECT_GE(H5Fget_filesize(file.get(), &fsize), 0);
    EXPECT_GT(fsize, 0u);
    EXPECT_GE(H5Fget_freespace(file.get()), 0);
    EXPECT_GE(H5Fget_free_sections(file.get(), H5FD_MEM_DEFAULT, 0, nullptr), 0);

    H5F_info2_t finfo{};
    EXPECT_GE(H5Fget_info2(file.get(), &finfo), 0);

    H5AC_cache_config_t mdc_cfg{};
    mdc_cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
    EXPECT_GE(H5Fget_mdc_config(file.get(), &mdc_cfg), 0);
    EXPECT_GE(H5Fset_mdc_config(file.get(), &mdc_cfg), 0);

    double hit_rate = 0.0;
    EXPECT_GE(H5Fget_mdc_hit_rate(file.get(), &hit_rate), 0);
    EXPECT_GE(H5Freset_mdc_hit_rate_stats(file.get()), 0);

    size_t max_sz = 0, min_clean = 0, cur_sz = 0;
    int cur_entries = 0;
    EXPECT_GE(
        H5Fget_mdc_size(file.get(), &max_sz, &min_clean, &cur_sz, &cur_entries),
        0);

    void* vfd_handle = nullptr;
    EXPECT_GE(H5Fget_vfd_handle(file.get(), H5P_DEFAULT, &vfd_handle), 0);
    EXPECT_NE(vfd_handle, nullptr);

    H5F_retry_info_t retry_info{};
    EXPECT_GE(H5Fget_metadata_read_retry_info(file.get(), &retry_info), 0);
    for (uint32_t* p : retry_info.retries) {
      if (p) {
        H5free_memory(p);
      }
    }

    // MDC logging start/status/stop.
    bool log_enabled = false, currently_logging = false;
    EXPECT_GE(H5Fstart_mdc_logging(file.get()), 0);
    EXPECT_GE(H5Fget_mdc_logging_status(file.get(), &log_enabled,
                                        &currently_logging),
              0);
    EXPECT_TRUE(log_enabled);
    EXPECT_TRUE(currently_logging);
    EXPECT_GE(H5Fstop_mdc_logging(file.get()), 0);

    // EOA, increment_filesize, libver bounds, dset_no_attrs_hint, start_swmr_write.
    haddr_t eoa = 0;
    EXPECT_GE(H5Fget_eoa(file.get(), &eoa), 0);
    EXPECT_GE(H5Fincrement_filesize(file.get(), 4096), 0);
    EXPECT_GE(H5Fset_libver_bounds(file.get(), H5F_LIBVER_V110,
                                   H5F_LIBVER_LATEST),
              0);
    bool minimize = false;
    EXPECT_GE(H5Fset_dset_no_attrs_hint(file.get(), true), 0);
    EXPECT_GE(H5Fget_dset_no_attrs_hint(file.get(), &minimize), 0);
    EXPECT_TRUE(minimize);

    // Reopen and close first file ID while second ID stays open to cover the
    // H5F_NREFS > 1 && nref == 1 flush-on-close branch.
    ScopedFile dup(H5Freopen(file.get()));
    ASSERT_TRUE(dup.valid());
    unsigned dup_intent = 0;
    EXPECT_GE(H5Fget_intent(dup.get(), &dup_intent), 0);
    EXPECT_EQ(dup_intent, H5F_ACC_RDWR);
    file.reset();
    // dup goes out of scope cleanly afterwards.
  }

  // Reopen with sec2 (without image config) + inspect cache image.
  EXPECT_GT(H5Fis_accessible(main_path.c_str(), H5P_DEFAULT), 0);
  {
    ScopedFile ro_f(H5Fopen(main_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(ro_f.valid());
    unsigned intent = 0;
    EXPECT_GE(H5Fget_intent(ro_f.get(), &intent), 0);
    EXPECT_EQ(intent, H5F_ACC_RDONLY);
    // H5Fflush on RDONLY handle is a clean no-op.
    EXPECT_GE(H5Fflush(ro_f.get(), H5F_SCOPE_LOCAL), 0);
    haddr_t img_addr = HADDR_UNDEF;
    hsize_t img_len = 0;
    EXPECT_GE(H5Fget_mdc_image_info(ro_f.get(), &img_addr, &img_len), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Fstart_swmr_write(ro_f.get()), 0);
    }
    // Format convert on non-SWMR file tested below.
  }

  // Dedicated SWMR write & read test (SWMR cannot be combined with MDC image).
  const std::string swmr_path = (dir.path() / "swmr_file.h5").string();
  {
    ScopedPlist swmr_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(swmr_fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(swmr_fapl.get(), H5F_LIBVER_V110,
                                   H5F_LIBVER_LATEST),
              0);
    ScopedFile swmr_w(H5Fcreate(swmr_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                                swmr_fapl.get()));
    ASSERT_TRUE(swmr_w.valid());
    EXPECT_GE(H5Fstart_swmr_write(swmr_w.get()), 0);
    unsigned swmr_w_intent = 0;
    EXPECT_GE(H5Fget_intent(swmr_w.get(), &swmr_w_intent), 0);
    EXPECT_EQ(swmr_w_intent, H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE);
  }
  {
    ScopedFile swmr_r(H5Fopen(
        swmr_path.c_str(), H5F_ACC_RDONLY | H5F_ACC_SWMR_READ, H5P_DEFAULT));
    ASSERT_TRUE(swmr_r.valid());
    unsigned swmr_r_intent = 0;
    EXPECT_GE(H5Fget_intent(swmr_r.get(), &swmr_r_intent), 0);
    EXPECT_EQ(swmr_r_intent, H5F_ACC_RDONLY | H5F_ACC_SWMR_READ);
  }

  // Delete file via H5Fdelete.
  EXPECT_GE(H5Fdelete(main_path.c_str(), H5P_DEFAULT), 0);
}

TEST(H5VLNativeFileTest, PageBufferingFormatConvertFileImageAndErrors) {
  TempFile page_file("paged_file.h5");
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(
      H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE, true, 1),
      0);
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_page_buffer_size(fapl.get(), 4096 * 4, 25, 25), 0);
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110), 0);

  {
    ScopedFile pf(
        H5Fcreate(page_file.c_str(), H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(pf.valid());

    unsigned acc[2] = {}, hits[2] = {}, misses[2] = {}, evict[2] = {},
             byp[2] = {};
    EXPECT_GE(
        H5Fget_page_buffering_stats(pf.get(), acc, hits, misses, evict, byp),
        0);
    EXPECT_GE(H5Freset_page_buffering_stats(pf.get()), 0);
    EXPECT_GE(H5Fformat_convert(pf.get()), 0);

    const ssize_t img_sz = H5Fget_file_image(pf.get(), nullptr, 0);
    ASSERT_GT(img_sz, 0);
    std::vector<uint8_t> image(static_cast<size_t>(img_sz));
    EXPECT_EQ(H5Fget_file_image(pf.get(), image.data(), image.size()), img_sz);

    ScopedErrorSilence silence;
    std::vector<uint8_t> small_img(8);
    EXPECT_LT(H5Fget_file_image(pf.get(), small_img.data(), small_img.size()),
              0);
  }

  // Core VFD file: container info, unsupported EOA/increment, disabled page buffer & logs.
  {
    ScopedFile core = CreateInMemoryFile("core_errors.h5");
    ASSERT_TRUE(core.valid());
    void* core_obj = H5VL_object(core.get());
    ASSERT_NE(core_obj, nullptr);

    ScopedApiContext api_ctx;

    H5VL_file_get_args_t gargs{};
    H5VL_file_cont_info_t cont_info{};
    cont_info.version = H5VL_CONTAINER_INFO_VERSION;
    gargs.op_type = H5VL_FILE_GET_CONT_INFO;
    gargs.args.get_cont_info.info = &cont_info;
    EXPECT_GE(
        H5VL__native_file_get(core_obj, &gargs, H5P_DATASET_XFER_DEFAULT, nullptr),
        0);
    EXPECT_GT(cont_info.blob_id_size, 0u);

    H5VL_file_specific_args_t sargs{};
    bool same_file = true;
    sargs.op_type = H5VL_FILE_IS_EQUAL;
    sargs.args.is_equal.obj2 = nullptr;
    sargs.args.is_equal.same_file = &same_file;
    EXPECT_GE(H5VL__native_file_specific(core_obj, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
    EXPECT_FALSE(same_file);

    sargs.args.is_equal.obj2 = core_obj;
    EXPECT_GE(H5VL__native_file_specific(core_obj, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
    EXPECT_TRUE(same_file);

    // Direct default-flags file create (hits flags == 0 -> H5F_ACC_EXCL).
    ScopedPlist core_fapl = MakeCoreFapl();
    H5P_genplist_t* fapl_plist =
        static_cast<H5P_genplist_t*>(H5I_object(core_fapl.get()));
    ASSERT_NE(fapl_plist, nullptr);
    H5VL_connector_prop_t conn_prop{};
    ASSERT_GE(H5P_peek(fapl_plist, H5F_ACS_VOL_CONN_NAME, &conn_prop), 0);
    ASSERT_GE(H5CX_set_vol_connector_prop(&conn_prop), 0);
    void* created_raw = H5VL__native_file_create(
        "direct_default_flags.h5", /*flags=*/0, H5P_FILE_CREATE_DEFAULT,
        core_fapl.get(), H5P_DATASET_XFER_DEFAULT, nullptr);
    ASSERT_NE(created_raw, nullptr);
    EXPECT_GE(H5VL__native_file_close(created_raw, H5P_DATASET_XFER_DEFAULT,
                                      nullptr),
              0);

    // Disabled/invalid branches under ScopedErrorSilence.
    ScopedErrorSilence silence;
    EXPECT_EQ(H5VL__native_file_create(
                  "/nonexistent_dir_xyz/file.h5", H5F_ACC_TRUNC,
                  H5P_FILE_CREATE_DEFAULT, H5P_FILE_ACCESS_DEFAULT,
                  H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VL__native_file_open(
                  "/nonexistent_dir_xyz/missing.h5", H5F_ACC_RDONLY,
                  H5P_FILE_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);

    haddr_t dummy_eoa = 0;
    EXPECT_LT(H5Fget_eoa(core.get(), &dummy_eoa), 0);
    EXPECT_LT(H5Fincrement_filesize(core.get(), 1024), 0);
    unsigned acc[2] = {}, hits[2] = {}, misses[2] = {}, evict[2] = {},
             byp[2] = {};
    EXPECT_LT(
        H5Fget_page_buffering_stats(core.get(), acc, hits, misses, evict, byp),
        0);
    EXPECT_LT(H5Freset_page_buffering_stats(core.get()), 0);
    EXPECT_LT(H5Fstart_mdc_logging(core.get()), 0);
    EXPECT_LT(H5Fstop_mdc_logging(core.get()), 0);
    bool b1 = false, b2 = false;
    EXPECT_GE(H5Fget_mdc_logging_status(core.get(), &b1, &b2), 0);
    EXPECT_FALSE(b1);
    EXPECT_FALSE(b2);

    H5AC_cache_config_t bad_cfg{};
    EXPECT_LT(H5Fset_mdc_config(core.get(), &bad_cfg), 0);
    EXPECT_LT(H5Fset_libver_bounds(H5I_INVALID_HID, H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_LATEST),
              0);
    EXPECT_LT(H5Fdelete("/nonexistent_dir_xyz/missing.h5", H5P_DEFAULT), 0);
    EXPECT_LT(H5Fis_accessible("/nonexistent_dir_xyz/missing.h5", H5P_DEFAULT),
              0);

    // Direct invalid object / edge-case / invalid op_type branches.
    size_t fname_len = 0;
    gargs = {};
    gargs.op_type = H5VL_FILE_GET_NAME;
    gargs.args.get_name.type = H5I_BADID;
    gargs.args.get_name.file_name_len = &fname_len;
    EXPECT_LT(
        H5VL__native_file_get(core_obj, &gargs, H5P_DATASET_XFER_DEFAULT, nullptr),
        0);
    gargs = {};
    gargs.op_type = H5VL_FILE_GET_OBJ_COUNT;
    gargs.args.get_obj_count.types = 0;
    gargs.args.get_obj_count.count = &fname_len;
    EXPECT_GE(
        H5VL__native_file_get(core_obj, &gargs, H5P_DATASET_XFER_DEFAULT, nullptr),
        0);
    EXPECT_EQ(fname_len, 0u);
    hid_t oid = H5I_INVALID_HID;
    gargs = {};
    gargs.op_type = H5VL_FILE_GET_OBJ_IDS;
    gargs.args.get_obj_ids.types = 0;
    gargs.args.get_obj_ids.max_objs = 1;
    gargs.args.get_obj_ids.oid_list = &oid;
    gargs.args.get_obj_ids.count = &fname_len;
    EXPECT_GE(
        H5VL__native_file_get(core_obj, &gargs, H5P_DATASET_XFER_DEFAULT, nullptr),
        0);
    EXPECT_EQ(fname_len, 0u);
    gargs = {};
    gargs.op_type = static_cast<H5VL_file_get_t>(999);
    EXPECT_LT(
        H5VL__native_file_get(core_obj, &gargs, H5P_DATASET_XFER_DEFAULT, nullptr),
        0);

    sargs = {};
    sargs.op_type = H5VL_FILE_FLUSH;
    sargs.args.flush.obj_type = H5I_BADID;
    sargs.args.flush.scope = H5F_SCOPE_LOCAL;
    EXPECT_LT(H5VL__native_file_specific(core_obj, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
    sargs = {};
    sargs.op_type = static_cast<H5VL_file_specific_t>(999);
    EXPECT_LT(H5VL__native_file_specific(core_obj, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    H5VL_optional_args_t oargs{};
    H5VL_native_file_optional_args_t fopt{};
    oargs.args = &fopt;
    oargs.op_type = H5VL_NATIVE_FILE_GET_INFO;
    H5F_info2_t bad_finfo{};
    fopt.get_info.type = H5I_BADID;
    fopt.get_info.finfo = &bad_finfo;
    EXPECT_LT(H5VL__native_file_optional(core_obj, &oargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    oargs.op_type = H5VL_NATIVE_FILE_GET_VFD_HANDLE;
    void* vfd_h = nullptr;
    fopt.get_vfd_handle.fapl_id = H5I_INVALID_HID;
    fopt.get_vfd_handle.file_handle = &vfd_h;
    EXPECT_LT(H5VL__native_file_optional(core_obj, &oargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    oargs.op_type = H5VL_NATIVE_FILE_GET_FREE_SECTIONS;
    size_t sect_cnt = 0;
    fopt.get_free_sections.type = H5FD_MEM_DEFAULT;
    fopt.get_free_sections.nsects = 0;
    fopt.get_free_sections.sect_info = nullptr;
    fopt.get_free_sections.sect_count = &sect_cnt;
    EXPECT_GE(H5VL__native_file_optional(core_obj, &oargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
    H5F_sect_info_t dummy_sect{};
    EXPECT_LT(H5Fget_free_sections(core.get(), H5FD_MEM_DEFAULT, 0, &dummy_sect),
              0);

    oargs.op_type = 999;
    EXPECT_LT(H5VL__native_file_optional(core_obj, &oargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    // Invalid container info version triggers H5F__get_cont_info failure.
    gargs = {};
    cont_info.version = 999;
    gargs.op_type = H5VL_FILE_GET_CONT_INFO;
    gargs.args.get_cont_info.info = &cont_info;
    EXPECT_LT(
        H5VL__native_file_get(core_obj, &gargs, H5P_DATASET_XFER_DEFAULT, nullptr),
        0);

    // Stack-allocated synthetic file structs exercise internal error guards
    // cleanly without corrupting any open file state.
    H5F_shared_t fake_sh{};
    H5F_t fake_f{};
    fake_f.shared = &fake_sh;

    // Null/invalid fcpl_id hits H5VL_FILE_GET_FCPL error branch.
    fake_sh.fcpl_id = H5I_INVALID_HID;
    gargs = {};
    gargs.op_type = H5VL_FILE_GET_FCPL;
    EXPECT_LT(
        H5VL__native_file_get(&fake_f, &gargs, H5P_DATASET_XFER_DEFAULT, nullptr),
        0);

    // Null metadata cache triggers H5AC_* failure paths cleanly.
    fake_sh.cache = nullptr;
    oargs.args = &fopt;
    oargs.op_type = H5VL_NATIVE_FILE_GET_MDC_CONF;
    H5AC_cache_config_t tmp_mdc_cfg{};
    tmp_mdc_cfg.version = H5AC__CURR_CACHE_CONFIG_VERSION;
    fopt.get_mdc_config.config = &tmp_mdc_cfg;
    EXPECT_LT(H5VL__native_file_optional(&fake_f, &oargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    double tmp_hr = 0.0;
    oargs.op_type = H5VL_NATIVE_FILE_GET_MDC_HR;
    fopt.get_mdc_hit_rate.hit_rate = &tmp_hr;
    EXPECT_LT(H5VL__native_file_optional(&fake_f, &oargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    size_t max_s = 0, min_s = 0, cur_s = 0;
    uint32_t cur_n = 0;
    oargs.op_type = H5VL_NATIVE_FILE_GET_MDC_SIZE;
    fopt.get_mdc_size.max_size = &max_s;
    fopt.get_mdc_size.min_clean_size = &min_s;
    fopt.get_mdc_size.cur_size = &cur_s;
    fopt.get_mdc_size.cur_num_entries = &cur_n;
    EXPECT_LT(H5VL__native_file_optional(&fake_f, &oargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    oargs.op_type = H5VL_NATIVE_FILE_RESET_MDC_HIT_RATE;
    EXPECT_LT(H5VL__native_file_optional(&fake_f, &oargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    haddr_t img_a = HADDR_UNDEF;
    hsize_t img_l = 0;
    oargs.op_type = H5VL_NATIVE_FILE_GET_MDC_IMAGE_INFO;
    fopt.get_mdc_image_info.addr = &img_a;
    fopt.get_mdc_image_info.len = &img_l;
    EXPECT_LT(H5VL__native_file_optional(&fake_f, &oargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    // Fake VFD exercises H5F__get_max_eof_eoa / H5F_get_eoa / H5F__set_eoa errors.
    H5FD_class_t fake_cls{};
    fake_cls.get_eoa = [](const H5FD_t*, H5FD_mem_t) -> haddr_t {
      return HADDR_UNDEF;
    };
    fake_cls.get_eof = [](const H5FD_t*, H5FD_mem_t) -> haddr_t {
      return HADDR_UNDEF;
    };
    H5FD_t fake_lf{};
    fake_lf.cls = &fake_cls;
    fake_lf.maxaddr = 1048576;
    fake_lf.feature_flags = H5FD_FEAT_SUPPORTS_SWMR_IO;
    H5F_super_t fake_sblock{};
    fake_sblock.ext_addr = HADDR_UNDEF;
    fake_sh.lf = &fake_lf;
    fake_sh.sblock = &fake_sblock;

    // Direct H5VL_FILE_DELETE failure branch.
    sargs = {};
    sargs.op_type = H5VL_FILE_DELETE;
    sargs.args.del.filename = "/nonexistent_dir_xyz/missing.h5";
    sargs.args.del.fapl_id = H5P_FILE_ACCESS_DEFAULT;
    EXPECT_LT(H5VL__native_file_specific(nullptr, &sargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    hsize_t out_sz = 0;
    oargs.op_type = H5VL_NATIVE_FILE_GET_FREE_SPACE;
    fopt.get_freespace.size = &out_sz;
    EXPECT_LT(H5VL__native_file_optional(&fake_f, &oargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    oargs.op_type = H5VL_NATIVE_FILE_GET_INFO;
    fopt.get_info.type = H5I_FILE;
    fopt.get_info.finfo = &bad_finfo;
    EXPECT_LT(H5VL__native_file_optional(&fake_f, &oargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    oargs.op_type = H5VL_NATIVE_FILE_GET_SIZE;
    fopt.get_size.size = &out_sz;
    EXPECT_LT(H5VL__native_file_optional(&fake_f, &oargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    haddr_t out_eoa = HADDR_UNDEF;
    oargs.op_type = H5VL_NATIVE_FILE_GET_EOA;
    fopt.get_eoa.eoa = &out_eoa;
    EXPECT_LT(H5VL__native_file_optional(&fake_f, &oargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    oargs.op_type = H5VL_NATIVE_FILE_INCR_FILESIZE;
    fopt.increment_filesize.increment = 64;
    EXPECT_LT(H5VL__native_file_optional(&fake_f, &oargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    fake_cls.get_eoa = [](const H5FD_t*, H5FD_mem_t) -> haddr_t { return 64; };
    fake_cls.get_eof = [](const H5FD_t*, H5FD_mem_t) -> haddr_t { return 64; };
    fake_cls.set_eoa = [](H5FD_t*, H5FD_mem_t, haddr_t) -> herr_t {
      return -1;
    };
    EXPECT_LT(H5VL__native_file_optional(&fake_f, &oargs,
                                         H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    // Close branches: (1) missing H5I ID on multi-ref RDWR file,
    // (2) H5F__close failure via H5F_CLOSE_DEFAULT, and (3) shared == NULL.
    fake_sh.nrefs = 2;
    fake_sh.flags = H5F_ACC_RDWR;
    fake_f.id_exists = true;
    EXPECT_LT(
        H5VL__native_file_close(&fake_f, H5P_DATASET_XFER_DEFAULT, nullptr), 0);

    fake_sh.nrefs = 1;
    fake_sh.fc_degree = H5F_CLOSE_DEFAULT;
    fake_f.closing = false;
    EXPECT_LT(
        H5VL__native_file_close(&fake_f, H5P_DATASET_XFER_DEFAULT, nullptr), 0);

    H5F_t* freeable_f = H5FL_CALLOC(H5F_t);
    ASSERT_NE(freeable_f, nullptr);
    EXPECT_GE(
        H5VL__native_file_close(freeable_f, H5P_DATASET_XFER_DEFAULT, nullptr),
        0);
  }
}

}  // namespace
}  // namespace hdf5
