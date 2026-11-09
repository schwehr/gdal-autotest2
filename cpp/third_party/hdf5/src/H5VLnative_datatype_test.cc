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
// Tests native VOL connector datatype callbacks (`H5VL__native_datatype_*`),
// public named-datatype APIs, stacked pass-through VOL binary serialization,
// flush/refresh, and error branches (third_party/hdf5/src/H5VLnative_datatype.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5VLnative_datatype.c

#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/H5VLpassthru.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5VLNATIVE_DATATYPE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#if defined(H5_HAVE__FLOAT16) && defined(__cplusplus)
typedef uint16_t H5__Float16;
#endif
#define H5T_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Tpkg.h"
#include "third_party/hdf5/src/H5VLnative_private.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5VLNATIVE_DATATYPE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5VLNATIVE_DATATYPE_TEST_RESTORE_UNDEF_NDEBUG
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

herr_t FailingFlushCb(hid_t /*object_id*/, void* /*udata*/) { return -1; }

TEST(H5VLNativeDatatypeTest, PublicCommitOpenTcplFlushRefreshAndPassthru) {
  TempFile tmp_file("dt_lifecycle.h5");

  // 1. Named & anonymous commits, get_create_plist, flush, refresh.
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V110),
              0);

    ScopedFile file(H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedType named_dt(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_TRUE(named_dt.valid());
    ScopedPlist tcpl(H5Pcreate(H5P_DATATYPE_CREATE));
    ASSERT_TRUE(tcpl.valid());
    ASSERT_GE(H5Tcommit2(file.get(), "named_int", named_dt.get(), H5P_DEFAULT,
                         tcpl.get(), H5P_DEFAULT),
              0);

    ScopedPlist got_tcpl(H5Tget_create_plist(named_dt.get()));
    EXPECT_TRUE(got_tcpl.valid());
    EXPECT_GE(H5Tflush(named_dt.get()), 0);
    EXPECT_GE(H5Trefresh(named_dt.get()), 0);

    // Anonymous commit exercises H5T__commit_anon path cleanly.
    ScopedType anon_dt(H5Tcopy(H5T_NATIVE_DOUBLE));
    ASSERT_TRUE(anon_dt.valid());
    ASSERT_GE(H5Tcommit_anon(file.get(), anon_dt.get(), H5P_DEFAULT,
                             H5P_DEFAULT),
              0);
    EXPECT_GT(H5Tcommitted(anon_dt.get()), 0);
    anon_dt.reset();

    // Reopen named datatype directly.
    ScopedType reopened(H5Topen2(file.get(), "named_int", H5P_DEFAULT));
    EXPECT_TRUE(reopened.valid());
    EXPECT_EQ(H5Tget_class(reopened.get()), H5T_INTEGER);
  }

  // 2. Stacked Pass-through VOL connector over native VOL exercises
  //    H5VL_DATATYPE_GET_BINARY_SIZE & H5VL_DATATYPE_GET_BINARY in H5Topen2.
  {
    hid_t native_vol_id = H5VLget_connector_id_by_name("native");
    ASSERT_NE(native_vol_id, H5I_INVALID_HID);
    absl::Cleanup close_native_vol = [&] { H5VLclose(native_vol_id); };

    ScopedPlist pt_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(pt_fapl.valid());
    H5VL_pass_through_info_t pt_info{native_vol_id, nullptr};
    ASSERT_GE(H5Pset_vol(pt_fapl.get(), H5VL_PASSTHRU, &pt_info), 0);

    ScopedFile pt_file(
        H5Fopen(tmp_file.c_str(), H5F_ACC_RDONLY, pt_fapl.get()));
    ASSERT_TRUE(pt_file.valid());

    ScopedType opened_pt(H5Topen2(pt_file.get(), "named_int", H5P_DEFAULT));
    ASSERT_TRUE(opened_pt.valid());
    EXPECT_EQ(H5Tget_size(opened_pt.get()), sizeof(int32_t));
  }
}

TEST(H5VLNativeDatatypeTest, DirectCallbacksAndErrorPaths) {
  TempFile tmp_file("dt_direct.h5");

  {
    ScopedPlist flush_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(flush_fapl.valid());
    ASSERT_GE(
        H5Pset_object_flush_cb(flush_fapl.get(), FailingFlushCb, nullptr), 0);
    ScopedFile file(H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                              flush_fapl.get()));
    ASSERT_TRUE(file.valid());
    void* file_obj = H5VL_object(file.get());
    ASSERT_NE(file_obj, nullptr);

    ScopedType valid_dt(H5Tcopy(H5T_NATIVE_INT));
    ASSERT_TRUE(valid_dt.valid());
    ScopedType empty_compound(H5Tcreate(H5T_COMPOUND, 8));
    ASSERT_TRUE(empty_compound.valid());

    H5VL_loc_params_t self_loc{};
    self_loc.type = H5VL_OBJECT_BY_SELF;
    self_loc.obj_type = H5I_FILE;

    ScopedApiContext api_ctx;

    // 1. Direct commit (named) + get binary size/data + get tcpl + specific + close.
    void* dt_obj = H5VL__native_datatype_commit(
        file_obj, &self_loc, "direct_dt", valid_dt.get(),
        H5P_LINK_CREATE_DEFAULT, H5P_DATATYPE_CREATE_DEFAULT,
        H5P_DATATYPE_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT, nullptr);
    ASSERT_NE(dt_obj, nullptr);

    H5VL_datatype_get_args_t get_args{};
    size_t bin_size = 0;
    get_args.op_type = H5VL_DATATYPE_GET_BINARY_SIZE;
    get_args.args.get_binary_size.size = &bin_size;
    ASSERT_GE(H5VL__native_datatype_get(dt_obj, &get_args,
                                        H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
    EXPECT_GT(bin_size, 0u);

    std::vector<uint8_t> bin_buf(bin_size);
    get_args = {};
    get_args.op_type = H5VL_DATATYPE_GET_BINARY;
    get_args.args.get_binary.buf = bin_buf.data();
    get_args.args.get_binary.buf_size = bin_buf.size();
    EXPECT_GE(H5VL__native_datatype_get(dt_obj, &get_args,
                                        H5P_DATASET_XFER_DEFAULT, nullptr),
              0);

    // Unencodable atomic byte order triggers H5T_encode failure in GET_BINARY.
    {
      ScopedErrorSilence silence;
      auto* dt_ptr = static_cast<H5T_t*>(dt_obj);
      H5T_shared_t bad_shared = *dt_ptr->shared;
      bad_shared.type = H5T_INTEGER;
      bad_shared.u.atomic.order = H5T_ORDER_NONE;
      H5T_t bad_dt = *dt_ptr;
      bad_dt.shared = &bad_shared;
      std::vector<uint8_t> enc_buf(64, 0);
      get_args = {};
      get_args.op_type = H5VL_DATATYPE_GET_BINARY;
      get_args.args.get_binary.buf = enc_buf.data();
      get_args.args.get_binary.buf_size = enc_buf.size();
      EXPECT_LT(H5VL__native_datatype_get(&bad_dt, &get_args,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
                0);

      get_args = {};
      get_args.op_type = static_cast<H5VL_datatype_get_t>(999);
      EXPECT_LT(H5VL__native_datatype_get(dt_obj, &get_args,
                                          H5P_DATASET_XFER_DEFAULT, nullptr),
                0);
    }

    // Exercise error branches inside H5VL__native_datatype_specific.
    {
      ScopedErrorSilence silence;
      H5VL_datatype_specific_args_t spec_args{};
      spec_args.op_type = H5VL_DATATYPE_FLUSH;
      spec_args.args.flush.type_id = H5I_INVALID_HID;
      EXPECT_LT(H5VL__native_datatype_specific(
                    dt_obj, &spec_args, H5P_DATASET_XFER_DEFAULT, nullptr),
                0);

      spec_args = {};
      spec_args.op_type = static_cast<H5VL_datatype_specific_t>(999);
      EXPECT_LT(H5VL__native_datatype_specific(
                    dt_obj, &spec_args, H5P_DATASET_XFER_DEFAULT, nullptr),
                0);
    }

    EXPECT_GE(
        H5VL__native_datatype_close(dt_obj, H5P_DATASET_XFER_DEFAULT, nullptr),
        0);

    // 2. Direct open + close, and commit/open failure branches.
    void* opened_obj = H5VL__native_datatype_open(
        file_obj, &self_loc, "direct_dt", H5P_DATATYPE_ACCESS_DEFAULT,
        H5P_DATASET_XFER_DEFAULT, nullptr);
    ASSERT_NE(opened_obj, nullptr);
    EXPECT_GE(H5VL__native_datatype_close(opened_obj, H5P_DATASET_XFER_DEFAULT,
                                          nullptr),
              0);

    // Register stack wrappers marked NAMED/OPEN to exercise already-committed
    // guard without leaking committed H5T_t* objects.
    auto* valid_ptr = static_cast<H5T_t*>(H5I_object(valid_dt.get()));
    ASSERT_NE(valid_ptr, nullptr);
    H5T_shared_t fake_shared = *valid_ptr->shared;
    H5T_t fake_dt = *valid_ptr;
    fake_dt.shared = &fake_shared;
    fake_shared.state = H5T_STATE_NAMED;
    hid_t fake_committed_id = H5I_register(H5I_DATATYPE, &fake_dt, false);
    ASSERT_NE(fake_committed_id, H5I_INVALID_HID);
    absl::Cleanup remove_fake_committed = [&] {
      H5I_remove(fake_committed_id);
    };

    ScopedErrorSilence silence;
    H5VL_loc_params_t bad_loc{};
    bad_loc.obj_type = H5I_BADID;
    EXPECT_EQ(H5VL__native_datatype_commit(
                  file_obj, &bad_loc, "bad_loc_dt", valid_dt.get(),
                  H5P_LINK_CREATE_DEFAULT, H5P_DATATYPE_CREATE_DEFAULT,
                  H5P_DATATYPE_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT,
                  nullptr),
              nullptr);
    EXPECT_EQ(H5VL__native_datatype_commit(
                  file_obj, &self_loc, "bad_type_dt", H5I_INVALID_HID,
                  H5P_LINK_CREATE_DEFAULT, H5P_DATATYPE_CREATE_DEFAULT,
                  H5P_DATATYPE_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT,
                  nullptr),
              nullptr);
    EXPECT_EQ(H5VL__native_datatype_commit(
                  file_obj, &self_loc, "already_committed_named",
                  fake_committed_id, H5P_LINK_CREATE_DEFAULT,
                  H5P_DATATYPE_CREATE_DEFAULT, H5P_DATATYPE_ACCESS_DEFAULT,
                  H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);
    fake_shared.state = H5T_STATE_OPEN;
    EXPECT_EQ(H5VL__native_datatype_commit(
                  file_obj, &self_loc, "already_committed_open",
                  fake_committed_id, H5P_LINK_CREATE_DEFAULT,
                  H5P_DATATYPE_CREATE_DEFAULT, H5P_DATATYPE_ACCESS_DEFAULT,
                  H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VL__native_datatype_commit(
                  file_obj, &self_loc, "immutable_dt", H5T_NATIVE_INT,
                  H5P_LINK_CREATE_DEFAULT, H5P_DATATYPE_CREATE_DEFAULT,
                  H5P_DATATYPE_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT,
                  nullptr),
              nullptr);
    EXPECT_EQ(H5VL__native_datatype_commit(
                  file_obj, &self_loc, "empty_compound_dt",
                  empty_compound.get(), H5P_LINK_CREATE_DEFAULT,
                  H5P_DATATYPE_CREATE_DEFAULT, H5P_DATATYPE_ACCESS_DEFAULT,
                  H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(H5VL__native_datatype_open(
                  file_obj, &bad_loc, "direct_dt", H5P_DATATYPE_ACCESS_DEFAULT,
                  H5P_DATASET_XFER_DEFAULT, nullptr),
              nullptr);
    EXPECT_EQ(
        H5VL__native_datatype_open(file_obj, &self_loc, "missing_dt",
                                   H5P_DATATYPE_ACCESS_DEFAULT,
                                   H5P_DATASET_XFER_DEFAULT, nullptr),
        nullptr);
  }

  // Reopen read-only to test H5T__commit_named / H5T__commit_anon failure +
  // done cleanup path.
  {
    ScopedFile ro_file(
        H5Fopen(tmp_file.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(ro_file.valid());
    void* ro_obj = H5VL_object(ro_file.get());
    ASSERT_NE(ro_obj, nullptr);

    ScopedType valid_dt(H5Tcopy(H5T_NATIVE_FLOAT));
    ASSERT_TRUE(valid_dt.valid());

    H5VL_loc_params_t self_loc{};
    self_loc.type = H5VL_OBJECT_BY_SELF;
    self_loc.obj_type = H5I_FILE;

    ScopedApiContext api_ctx;
    ScopedErrorSilence silence;
    EXPECT_EQ(H5VL__native_datatype_commit(
                  ro_obj, &self_loc, "ro_named", valid_dt.get(),
                  H5P_LINK_CREATE_DEFAULT, H5P_DATATYPE_CREATE_DEFAULT,
                  H5P_DATATYPE_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT,
                  nullptr),
              nullptr);
    EXPECT_EQ(H5VL__native_datatype_commit(
                  ro_obj, &self_loc, nullptr, valid_dt.get(),
                  H5P_LINK_CREATE_DEFAULT, H5P_DATATYPE_CREATE_DEFAULT,
                  H5P_DATATYPE_ACCESS_DEFAULT, H5P_DATASET_XFER_DEFAULT,
                  nullptr),
              nullptr);

    // Exercise H5VL_DATATYPE_REFRESH failure branch on read-only object.
    void* ro_dt = H5VL__native_datatype_open(
        ro_obj, &self_loc, "direct_dt", H5P_DATATYPE_ACCESS_DEFAULT,
        H5P_DATASET_XFER_DEFAULT, nullptr);
    ASSERT_NE(ro_dt, nullptr);
    H5VL_datatype_specific_args_t spec_args{};
    spec_args.op_type = H5VL_DATATYPE_REFRESH;
    spec_args.args.refresh.type_id = H5I_INVALID_HID;
    EXPECT_LT(H5VL__native_datatype_specific(ro_dt, &spec_args,
                                             H5P_DATASET_XFER_DEFAULT, nullptr),
              0);
    EXPECT_GE(
        H5VL__native_datatype_close(ro_dt, H5P_DATASET_XFER_DEFAULT, nullptr),
        0);
  }
}

}  // namespace
}  // namespace hdf5
