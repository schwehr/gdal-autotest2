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
// Tests internal API context stack management, state capture/restoration,
// VOL wrapper/connector tracking, cached DXPL/LCPL/LAPL/DCPL/DAPL/FAPL
// getters/setters, selection I/O write-back, and error paths
// (third_party/hdf5/src/H5CX.c). Note: built in google3 non-MPI,
// uninstrumented configuration (-DNDEBUG, H5_HAVE_THREADSAFE_API).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5CX.c

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG; match internal struct layouts across
// H5private.h -> H5CXprivate.h -> H5ACprivate.h -> H5Cprivate.h.
#ifndef NDEBUG
#define H5CX_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5VL_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Pprivate.h"
#include "third_party/hdf5/src/H5VLnative_private.h"
#include "third_party/hdf5/src/H5VLpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
#include "third_party/hdf5/src/H5Zprivate.h"
}  // extern "C"
#ifdef H5CX_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5CX_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::DoubleEq;
using ::testing::Eq;
using ::testing::StrEq;

// RAII wrapper for pushing/popping a zero-initialized H5CX_node_t onto the
// current thread's API context stack. If H5CX_pop(true) fails (which leaves
// the node on the stack in upstream H5CX.c), the destructor falls back to
// H5CX_pop(false).
class ScopedApiContext {
 public:
  ScopedApiContext() { pushed_ = (H5CX_push(&node_) >= 0); }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool pushed() const { return pushed_; }

  herr_t Pop(bool update_dxpl_props) {
    if (!pushed_) return 0;
    const herr_t rc = H5CX_pop(update_dxpl_props);
    if (rc >= 0) {
      pushed_ = false;
    }
    return rc;
  }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

struct DummyVolInfo {
  int magic = 0;
};

extern "C" {

static H5Z_cb_return_t DummyFilterCallback(H5Z_filter_t /*filter*/,
                                           void* /*buf*/, size_t /*buf_size*/,
                                           void* /*op_data*/) {
  return H5Z_CB_CONT;
}

static H5T_conv_ret_t DummyConvExceptCallback(
    H5T_conv_except_t /*except_type*/, hid_t /*src_id*/, hid_t /*dst_id*/,
    void* /*src_buf*/, void* /*dst_buf*/, void* /*user_data*/) {
  return H5T_CONV_HANDLED;
}

static void* DummyVlenAlloc(size_t size, void* /*alloc_info*/) {
  return std::malloc(size);
}

static void DummyVlenFree(void* mem, void* /*free_info*/) { std::free(mem); }

static void* CopyDummyVolInfo(const void* info) {
  const auto* src = static_cast<const DummyVolInfo*>(info);
  return new DummyVolInfo{*src};
}

static herr_t FreeDummyVolInfo(void* info) {
  delete static_cast<DummyVolInfo*>(info);
  return 0;
}

}  // extern "C"

TEST(H5CXTest, PackageTerminateAndReinitialize) {
  ASSERT_GE(H5open(), 0);
  absl::Cleanup reopen = [] { H5open(); };

  // Ensure package is initialized by pushing and popping a context first.
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.pushed());
  }
  EXPECT_FALSE(H5CX_pushed());

  // Shut down HDF5 (invoking H5CX_term_package while H5_PKG_INIT_VAR == true),
  // then call H5CX_term_package again while already terminated (false branch).
  ASSERT_GE(H5close(), 0);
  EXPECT_EQ(H5CX_term_package(), 0);

  // Reopen HDF5 and push a fresh context to trigger H5CX__init_package().
  ASSERT_GE(H5open(), 0);
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());
  EXPECT_TRUE(H5CX_is_def_dxpl());
}

TEST(H5CXTest, PushPopNestingAndThreadIsolation) {
  EXPECT_FALSE(H5CX_pushed());

  ScopedPlist custom_dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(custom_dxpl.valid());
  ScopedPlist custom_lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(custom_lapl.valid());

  ScopedApiContext outer;
  ASSERT_TRUE(outer.pushed());
  EXPECT_TRUE(H5CX_pushed());
  EXPECT_TRUE(H5CX_is_def_dxpl());
  EXPECT_EQ(H5CX_get_dxpl(), H5P_DATASET_XFER_DEFAULT);
  EXPECT_EQ(H5CX_get_lapl(), H5P_LINK_ACCESS_DEFAULT);
  EXPECT_EQ(H5CX_get_tag(), H5AC__INVALID_TAG);
  EXPECT_EQ(H5CX_get_ring(), H5AC_RING_USER);

  H5CX_set_dxpl(custom_dxpl.get());
  H5CX_set_lapl(custom_lapl.get());
  H5CX_set_tag(0x12345);
  H5CX_set_ring(H5AC_RING_SBE);
  EXPECT_FALSE(H5CX_is_def_dxpl());
  EXPECT_EQ(H5CX_get_dxpl(), custom_dxpl.get());
  EXPECT_EQ(H5CX_get_lapl(), custom_lapl.get());
  EXPECT_EQ(H5CX_get_tag(), 0x12345);
  EXPECT_EQ(H5CX_get_ring(), H5AC_RING_SBE);

  // Worker thread starts with an empty context stack and does not perturb
  // outer.
  std::thread worker([] {
    EXPECT_FALSE(H5CX_pushed());
    ScopedApiContext worker_ctx;
    ASSERT_TRUE(worker_ctx.pushed());
    EXPECT_TRUE(H5CX_pushed());
    EXPECT_TRUE(H5CX_is_def_dxpl());
    H5CX_set_tag(0x99999);
    H5CX_set_ring(H5AC_RING_SB);
    EXPECT_EQ(H5CX_get_tag(), 0x99999);
    EXPECT_EQ(H5CX_get_ring(), H5AC_RING_SB);
    EXPECT_GE(worker_ctx.Pop(false), 0);
    EXPECT_FALSE(H5CX_pushed());
  });
  worker.join();

  // Nested push on main thread starts with clean defaults and restores outer.
  {
    ScopedApiContext inner;
    ASSERT_TRUE(inner.pushed());
    EXPECT_TRUE(H5CX_is_def_dxpl());
    EXPECT_EQ(H5CX_get_dxpl(), H5P_DATASET_XFER_DEFAULT);
    EXPECT_EQ(H5CX_get_lapl(), H5P_LINK_ACCESS_DEFAULT);
    EXPECT_EQ(H5CX_get_tag(), H5AC__INVALID_TAG);
    EXPECT_EQ(H5CX_get_ring(), H5AC_RING_USER);
    EXPECT_GE(inner.Pop(false), 0);
  }

  EXPECT_TRUE(H5CX_pushed());
  EXPECT_EQ(H5CX_get_dxpl(), custom_dxpl.get());
  EXPECT_EQ(H5CX_get_lapl(), custom_lapl.get());
  EXPECT_EQ(H5CX_get_tag(), 0x12345);
  EXPECT_EQ(H5CX_get_ring(), H5AC_RING_SBE);
  EXPECT_GE(outer.Pop(false), 0);
  EXPECT_FALSE(H5CX_pushed());
}

TEST(H5CXTest, SetAplSetLocAndSetLibverBounds) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_V110),
            0);

  ScopedFile file(
      H5Fcreate("libver.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());
  H5F_t* internal_file =
      static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
  ASSERT_NE(internal_file, nullptr);

  ScopedPlist custom_lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(custom_lapl.valid());
  ASSERT_GE(H5Pset_nlinks(custom_lapl.get(), 42), 0);

  ScopedPlist custom_dapl(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_TRUE(custom_dapl.valid());
  ASSERT_GE(H5Pset_efile_prefix(custom_dapl.get(), "/tmp/efile"), 0);
  ASSERT_GE(H5Pset_virtual_prefix(custom_dapl.get(), "/tmp/vds"), 0);

  ScopedPlist custom_gapl(H5Pcreate(H5P_GROUP_ACCESS));
  ASSERT_TRUE(custom_gapl.valid());

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());

  // H5P_DEFAULT resolves to the libclass default plist ID.
  hid_t id = H5P_DEFAULT;
  ASSERT_GE(H5CX_set_apl(&id, H5P_CLS_LACC, H5I_INVALID_HID, false), 0);
  EXPECT_EQ(id, H5P_LINK_ACCESS_DEFAULT);

  // Non-default LACC, DACC, FACC, and unrelated GACC access classes.
  id = custom_lapl.get();
  ASSERT_GE(H5CX_set_apl(&id, H5P_CLS_LACC, H5I_INVALID_HID, false), 0);
  EXPECT_EQ(H5CX_get_lapl(), custom_lapl.get());

  id = custom_dapl.get();
  ASSERT_GE(H5CX_set_apl(&id, H5P_CLS_DACC, H5I_INVALID_HID, false), 0);

  id = fapl.get();
  ASSERT_GE(H5CX_set_apl(&id, H5P_CLS_FACC, H5I_INVALID_HID, false), 0);

  id = custom_gapl.get();
  ASSERT_GE(H5CX_set_apl(&id, H5P_CLS_GACC, H5I_INVALID_HID, false), 0);

  // Verify DAPL and FAPL values populated via H5CX_set_apl.
  const char* efile_prefix = nullptr;
  const char* vds_prefix = nullptr;
  ASSERT_GE(H5CX_get_ext_file_prefix(&efile_prefix), 0);
  ASSERT_GE(H5CX_get_vds_prefix(&vds_prefix), 0);
  EXPECT_THAT(efile_prefix, StrEq("/tmp/efile"));
  EXPECT_THAT(vds_prefix, StrEq("/tmp/vds"));

  H5F_libver_t low = H5F_LIBVER_ERROR;
  H5F_libver_t high = H5F_LIBVER_ERROR;
  ASSERT_GE(H5CX_get_libver_bounds(&low, &high), 0);
  EXPECT_EQ(low, H5F_LIBVER_V18);
  EXPECT_EQ(high, H5F_LIBVER_V110);

  // Overriding libver bounds directly with H5F_t* vs nullptr.
  ASSERT_GE(H5CX_set_libver_bounds(nullptr), 0);
  ASSERT_GE(H5CX_get_libver_bounds(&low, &high), 0);
  EXPECT_EQ(low, H5F_LIBVER_LATEST);
  EXPECT_EQ(high, H5F_LIBVER_LATEST);

  ASSERT_GE(H5CX_set_libver_bounds(internal_file), 0);
  ASSERT_GE(H5CX_get_libver_bounds(&low, &high), 0);
  EXPECT_EQ(low, H5F_LIBVER_V18);
  EXPECT_EQ(high, H5F_LIBVER_V110);

  EXPECT_GE(H5CX_set_loc(file.get()), 0);
}

TEST(H5CXTest, DefaultPropertyListGettersAndCaching) {
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());

  // Exercise both the first lookup (default cache copy) and second lookup
  // (_valid cache hit) for every default property getter.
  for (int pass = 0; pass < 2; ++pass) {
    double ratios[3] = {0.0, 0.0, 0.0};
    ASSERT_GE(H5CX_get_btree_split_ratios(ratios), 0);
    EXPECT_GT(ratios[1], 0.0);

    size_t max_temp_buf = 0;
    ASSERT_GE(H5CX_get_max_temp_buf(&max_temp_buf), 0);
    EXPECT_EQ(max_temp_buf, 1024u * 1024u);

    void* tconv_buf = reinterpret_cast<void*>(0x1);
    ASSERT_GE(H5CX_get_tconv_buf(&tconv_buf), 0);
    EXPECT_EQ(tconv_buf, nullptr);

    void* bkgr_buf = reinterpret_cast<void*>(0x1);
    ASSERT_GE(H5CX_get_bkgr_buf(&bkgr_buf), 0);
    EXPECT_EQ(bkgr_buf, nullptr);

    H5T_bkg_t bkgr_type = H5T_BKG_YES;
    ASSERT_GE(H5CX_get_bkgr_buf_type(&bkgr_type), 0);
    EXPECT_EQ(bkgr_type, H5T_BKG_NO);

    size_t vec_size = 0;
    ASSERT_GE(H5CX_get_vec_size(&vec_size), 0);
    EXPECT_EQ(vec_size, 1024u);

    H5Z_EDC_t edc = H5Z_ERROR_EDC;
    ASSERT_GE(H5CX_get_err_detect(&edc), 0);
    EXPECT_EQ(edc, H5Z_ENABLE_EDC);

    H5Z_cb_t filter_cb{};
    ASSERT_GE(H5CX_get_filter_cb(&filter_cb), 0);
    EXPECT_EQ(filter_cb.func, nullptr);

    H5Z_data_xform_t* xform = reinterpret_cast<H5Z_data_xform_t*>(0x1);
    ASSERT_GE(H5CX_get_data_transform(&xform), 0);
    EXPECT_EQ(xform, nullptr);

    H5T_vlen_alloc_info_t vl_info{};
    ASSERT_GE(H5CX_get_vlen_alloc_info(&vl_info), 0);
    EXPECT_EQ(vl_info.alloc_func, nullptr);
    EXPECT_EQ(vl_info.free_func, nullptr);

    H5T_conv_cb_t dt_conv_cb{};
    ASSERT_GE(H5CX_get_dt_conv_cb(&dt_conv_cb), 0);
    EXPECT_EQ(dt_conv_cb.func, nullptr);

    H5D_selection_io_mode_t sel_mode = H5D_SELECTION_IO_MODE_OFF;
    ASSERT_GE(H5CX_get_selection_io_mode(&sel_mode), 0);
    EXPECT_EQ(sel_mode, H5D_SELECTION_IO_MODE_DEFAULT);

    uint32_t no_sel_cause = 999;
    ASSERT_GE(H5CX_get_no_selection_io_cause(&no_sel_cause), 0);
    EXPECT_EQ(no_sel_cause, 0u);

    uint32_t actual_sel_mode = 999;
    ASSERT_GE(H5CX_get_actual_selection_io_mode(&actual_sel_mode), 0);
    EXPECT_EQ(actual_sel_mode, 0u);

    bool modify_write_buf = true;
    ASSERT_GE(H5CX_get_modify_write_buf(&modify_write_buf), 0);
    EXPECT_FALSE(modify_write_buf);

    H5T_cset_t encoding = H5T_CSET_ERROR;
    ASSERT_GE(H5CX_get_encoding(&encoding), 0);
    EXPECT_EQ(encoding, H5T_CSET_ASCII);

    unsigned intermediate = 99;
    ASSERT_GE(H5CX_get_intermediate_group(&intermediate), 0);
    EXPECT_EQ(intermediate, 0u);

    size_t nlinks = 0;
    ASSERT_GE(H5CX_get_nlinks(&nlinks), 0);
    EXPECT_GT(nlinks, 0u);

    bool min_ohdr = true;
    ASSERT_GE(H5CX_get_dset_min_ohdr_flag(&min_ohdr), 0);
    EXPECT_FALSE(min_ohdr);

    uint8_t ohdr_flags = 0xff;
    ASSERT_GE(H5CX_get_ohdr_flags(&ohdr_flags), 0);
    EXPECT_EQ(ohdr_flags, H5O_HDR_STORE_TIMES);

    const char* extfile_prefix = reinterpret_cast<const char*>(0x1);
    ASSERT_GE(H5CX_get_ext_file_prefix(&extfile_prefix), 0);
    EXPECT_EQ(extfile_prefix, nullptr);

    const char* vds_prefix = reinterpret_cast<const char*>(0x1);
    ASSERT_GE(H5CX_get_vds_prefix(&vds_prefix), 0);
    EXPECT_EQ(vds_prefix, nullptr);

    H5F_libver_t low = H5F_LIBVER_ERROR;
    H5F_libver_t high = H5F_LIBVER_ERROR;
    ASSERT_GE(H5CX_get_libver_bounds(&low, &high), 0);
    EXPECT_EQ(low, H5F_LIBVER_V18);
    EXPECT_EQ(high, H5F_LIBVER_LATEST);
  }
}

TEST(H5CXTest, CustomPropertyListGettersSettersAndCaching) {
  std::vector<uint8_t> tconv_storage(2048, 0);
  std::vector<uint8_t> bkg_storage(2048, 0);
  int filter_op_data = 11;
  int conv_op_data = 22;
  int alloc_info_tag = 33;
  int free_info_tag = 44;

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(H5Pset_btree_ratios(dxpl.get(), 0.2, 0.4, 0.8), 0);
  ASSERT_GE(H5Pset_buffer(dxpl.get(), tconv_storage.size(),
                          tconv_storage.data(), bkg_storage.data()),
            0);
  ASSERT_GE(H5Pset_preserve(dxpl.get(), true), 0);
  ASSERT_GE(H5Pset_hyper_vector_size(dxpl.get(), 64), 0);
  ASSERT_GE(H5Pset_edc_check(dxpl.get(), H5Z_DISABLE_EDC), 0);
  ASSERT_GE(
      H5Pset_filter_callback(dxpl.get(), DummyFilterCallback, &filter_op_data),
      0);
  ASSERT_GE(H5Pset_data_transform(dxpl.get(), "x*2+1"), 0);
  ASSERT_GE(H5Pset_vlen_mem_manager(dxpl.get(), DummyVlenAlloc, &alloc_info_tag,
                                    DummyVlenFree, &free_info_tag),
            0);
  ASSERT_GE(
      H5Pset_type_conv_cb(dxpl.get(), DummyConvExceptCallback, &conv_op_data),
      0);
  ASSERT_GE(H5Pset_selection_io(dxpl.get(), H5D_SELECTION_IO_MODE_ON), 0);
  ASSERT_GE(H5Pset_modify_write_buf(dxpl.get(), true), 0);

  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());
  ASSERT_GE(H5Pset_char_encoding(lcpl.get(), H5T_CSET_UTF8), 0);
  ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 1), 0);

  ScopedPlist lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(lapl.valid());
  ASSERT_GE(H5Pset_nlinks(lapl.get(), 77), 0);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_dset_no_attrs_hint(dcpl.get(), true), 0);
  ASSERT_GE(H5Pset_attr_creation_order(
                dcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);
  ASSERT_GE(H5Pset_obj_track_times(dcpl.get(), true), 0);

  ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_TRUE(dapl.valid());
  ASSERT_GE(H5Pset_efile_prefix(dapl.get(), "${ORIGIN}/ext"), 0);
  ASSERT_GE(H5Pset_virtual_prefix(dapl.get(), "${ORIGIN}/vds"), 0);

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_V112),
            0);

  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.pushed());
  H5CX_set_dxpl(dxpl.get());
  H5CX_set_lcpl(lcpl.get());
  H5CX_set_lapl(lapl.get());
  H5CX_set_dcpl(dcpl.get());

  hid_t apl_id = dapl.get();
  ASSERT_GE(H5CX_set_apl(&apl_id, H5P_CLS_DACC, H5I_INVALID_HID, false), 0);
  // DACC inherits from LACC, so H5CX_set_apl also sets lapl_id; restore our
  // distinct custom LAPL to verify H5CX_set_lapl/H5CX_get_nlinks independently.
  H5CX_set_lapl(lapl.get());
  apl_id = fapl.get();
  ASSERT_GE(H5CX_set_apl(&apl_id, H5P_CLS_FACC, H5I_INVALID_HID, false), 0);

  // Two passes exercise uncached lookup followed by cached lookup.
  for (int pass = 0; pass < 2; ++pass) {
    double ratios[3] = {};
    ASSERT_GE(H5CX_get_btree_split_ratios(ratios), 0);
    EXPECT_THAT(ratios[0], DoubleEq(0.2));
    EXPECT_THAT(ratios[1], DoubleEq(0.4));
    EXPECT_THAT(ratios[2], DoubleEq(0.8));

    size_t max_temp_buf = 0;
    ASSERT_GE(H5CX_get_max_temp_buf(&max_temp_buf), 0);
    EXPECT_EQ(max_temp_buf, tconv_storage.size());

    void* tconv_ptr = nullptr;
    ASSERT_GE(H5CX_get_tconv_buf(&tconv_ptr), 0);
    EXPECT_EQ(tconv_ptr, tconv_storage.data());

    void* bkg_ptr = nullptr;
    ASSERT_GE(H5CX_get_bkgr_buf(&bkg_ptr), 0);
    EXPECT_EQ(bkg_ptr, bkg_storage.data());

    H5T_bkg_t bkgr_type = H5T_BKG_NO;
    ASSERT_GE(H5CX_get_bkgr_buf_type(&bkgr_type), 0);
    EXPECT_EQ(bkgr_type, H5T_BKG_YES);

    size_t vec_size = 0;
    ASSERT_GE(H5CX_get_vec_size(&vec_size), 0);
    EXPECT_EQ(vec_size, 64u);

    H5Z_EDC_t edc = H5Z_ENABLE_EDC;
    ASSERT_GE(H5CX_get_err_detect(&edc), 0);
    EXPECT_EQ(edc, H5Z_DISABLE_EDC);

    H5Z_cb_t filter_cb{};
    ASSERT_GE(H5CX_get_filter_cb(&filter_cb), 0);
    EXPECT_EQ(filter_cb.func, &DummyFilterCallback);
    EXPECT_EQ(filter_cb.op_data, &filter_op_data);

    H5Z_data_xform_t* xform = nullptr;
    ASSERT_GE(H5CX_get_data_transform(&xform), 0);
    ASSERT_NE(xform, nullptr);
    EXPECT_THAT(H5Z_xform_extract_xform_str(xform), StrEq("x*2+1"));

    H5T_vlen_alloc_info_t vl_info{};
    ASSERT_GE(H5CX_get_vlen_alloc_info(&vl_info), 0);
    EXPECT_EQ(vl_info.alloc_func, &DummyVlenAlloc);
    EXPECT_EQ(vl_info.alloc_info, &alloc_info_tag);
    EXPECT_EQ(vl_info.free_func, &DummyVlenFree);
    EXPECT_EQ(vl_info.free_info, &free_info_tag);

    H5T_conv_cb_t dt_conv_cb{};
    ASSERT_GE(H5CX_get_dt_conv_cb(&dt_conv_cb), 0);
    EXPECT_EQ(dt_conv_cb.func, &DummyConvExceptCallback);
    EXPECT_EQ(dt_conv_cb.user_data, &conv_op_data);

    H5D_selection_io_mode_t sel_mode = H5D_SELECTION_IO_MODE_DEFAULT;
    ASSERT_GE(H5CX_get_selection_io_mode(&sel_mode), 0);
    EXPECT_EQ(sel_mode, H5D_SELECTION_IO_MODE_ON);

    bool modify_write_buf = false;
    ASSERT_GE(H5CX_get_modify_write_buf(&modify_write_buf), 0);
    EXPECT_TRUE(modify_write_buf);

    H5T_cset_t encoding = H5T_CSET_ASCII;
    ASSERT_GE(H5CX_get_encoding(&encoding), 0);
    EXPECT_EQ(encoding, H5T_CSET_UTF8);

    unsigned intermediate = 0;
    ASSERT_GE(H5CX_get_intermediate_group(&intermediate), 0);
    EXPECT_EQ(intermediate, 1u);

    size_t nlinks = 0;
    ASSERT_GE(H5CX_get_nlinks(&nlinks), 0);
    EXPECT_EQ(nlinks, 77u);

    bool min_ohdr = false;
    ASSERT_GE(H5CX_get_dset_min_ohdr_flag(&min_ohdr), 0);
    EXPECT_TRUE(min_ohdr);

    uint8_t ohdr_flags = 0;
    ASSERT_GE(H5CX_get_ohdr_flags(&ohdr_flags), 0);
    EXPECT_NE(ohdr_flags & H5O_HDR_ATTR_CRT_ORDER_TRACKED, 0);
    EXPECT_NE(ohdr_flags & H5O_HDR_ATTR_CRT_ORDER_INDEXED, 0);
    EXPECT_NE(ohdr_flags & H5O_HDR_STORE_TIMES, 0);

    const char* extfile_prefix = nullptr;
    ASSERT_GE(H5CX_get_ext_file_prefix(&extfile_prefix), 0);
    EXPECT_THAT(extfile_prefix, StrEq("${ORIGIN}/ext"));

    const char* vds_prefix = nullptr;
    ASSERT_GE(H5CX_get_vds_prefix(&vds_prefix), 0);
    EXPECT_THAT(vds_prefix, StrEq("${ORIGIN}/vds"));

    H5F_libver_t low = H5F_LIBVER_ERROR;
    H5F_libver_t high = H5F_LIBVER_ERROR;
    ASSERT_GE(H5CX_get_libver_bounds(&low, &high), 0);
    EXPECT_EQ(low, H5F_LIBVER_V110);
    EXPECT_EQ(high, H5F_LIBVER_V112);
  }

  // Exercise direct context overrides (H5CX_set_vlen_alloc_info,
  // H5CX_set_nlinks).
  int custom_info_a = 1;
  int custom_info_b = 2;
  ASSERT_GE(H5CX_set_vlen_alloc_info(nullptr, &custom_info_a, nullptr,
                                     &custom_info_b),
            0);
  H5T_vlen_alloc_info_t vl_override{};
  ASSERT_GE(H5CX_get_vlen_alloc_info(&vl_override), 0);
  EXPECT_EQ(vl_override.alloc_func, nullptr);
  EXPECT_EQ(vl_override.alloc_info, &custom_info_a);
  EXPECT_EQ(vl_override.free_func, nullptr);
  EXPECT_EQ(vl_override.free_info, &custom_info_b);

  ASSERT_GE(H5CX_set_nlinks(123), 0);
  size_t overridden_nlinks = 0;
  ASSERT_GE(H5CX_get_nlinks(&overridden_nlinks), 0);
  EXPECT_EQ(overridden_nlinks, 123u);
}

TEST(H5CXTest, SelectionIoWriteBackAndResetOnPop) {
  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());

  // 1. Default DXPL: setters ignore writes and keep default property state.
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.pushed());
    H5CX_set_no_selection_io_cause(H5D_SEL_IO_DISABLE_BY_API);
    H5CX_set_actual_selection_io_mode(H5D_VECTOR_IO);

    uint32_t cause = 999;
    uint32_t mode = 999;
    ASSERT_GE(H5CX_get_no_selection_io_cause(&cause), 0);
    ASSERT_GE(H5CX_get_actual_selection_io_mode(&mode), 0);
    EXPECT_EQ(cause, 0u);
    EXPECT_EQ(mode, 0u);
    EXPECT_GE(ctx.Pop(true), 0);
  }

  // 2. Custom DXPL: set values propagate back into DXPL on H5CX_pop(true).
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.pushed());
    H5CX_set_dxpl(dxpl.get());
    H5CX_set_no_selection_io_cause(H5D_SEL_IO_DISABLE_BY_API);
    H5CX_set_actual_selection_io_mode(H5D_SELECTION_IO | H5D_SCALAR_IO);

    uint32_t cause = 0;
    uint32_t mode = 0;
    ASSERT_GE(H5CX_get_no_selection_io_cause(&cause), 0);
    ASSERT_GE(H5CX_get_actual_selection_io_mode(&mode), 0);
    EXPECT_EQ(cause, H5D_SEL_IO_DISABLE_BY_API);
    EXPECT_EQ(mode, H5D_SELECTION_IO | H5D_SCALAR_IO);
    EXPECT_GE(ctx.Pop(true), 0);

    uint32_t persisted_cause = 0;
    uint32_t persisted_mode = 0;
    ASSERT_GE(H5Pget_no_selection_io_cause(dxpl.get(), &persisted_cause), 0);
    ASSERT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), &persisted_mode), 0);
    EXPECT_EQ(persisted_cause, H5D_SEL_IO_DISABLE_BY_API);
    EXPECT_EQ(persisted_mode, H5D_SELECTION_IO | H5D_SCALAR_IO);
  }

  // 3. Subsequent API call that does not set actual_selection_io_mode wipes the
  // stale DXPL value when reading via H5CX_get_actual_selection_io_mode() and
  // when popping with update_dxpl_props=true.
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.pushed());
    H5CX_set_dxpl(dxpl.get());

    uint32_t wiped_mode = 999;
    ASSERT_GE(H5CX_get_actual_selection_io_mode(&wiped_mode), 0);
    EXPECT_EQ(wiped_mode, 0u);

    uint32_t preserved_cause = 0;
    ASSERT_GE(H5CX_get_no_selection_io_cause(&preserved_cause), 0);
    EXPECT_EQ(preserved_cause, H5D_SEL_IO_DISABLE_BY_API);

    H5CX_set_no_selection_io_cause(0);
    EXPECT_GE(ctx.Pop(true), 0);

    uint32_t persisted_cause = 999;
    uint32_t persisted_mode = 999;
    ASSERT_GE(H5Pget_no_selection_io_cause(dxpl.get(), &persisted_cause), 0);
    ASSERT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), &persisted_mode), 0);
    EXPECT_EQ(persisted_cause, 0u);
    EXPECT_EQ(persisted_mode, 0u);
  }

  // 4. H5CX_pop(true) when actual_selection_io_mode was never set and never
  // queried also resets a previously non-zero DXPL mode back to 0.
  {
    ScopedApiContext ctx1;
    ASSERT_TRUE(ctx1.pushed());
    H5CX_set_dxpl(dxpl.get());
    H5CX_set_actual_selection_io_mode(H5D_VECTOR_IO);
    EXPECT_GE(ctx1.Pop(true), 0);

    uint32_t persisted_mode = 0;
    ASSERT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), &persisted_mode), 0);
    EXPECT_EQ(persisted_mode, H5D_VECTOR_IO);

    ScopedApiContext ctx2;
    ASSERT_TRUE(ctx2.pushed());
    H5CX_set_dxpl(dxpl.get());
    EXPECT_GE(ctx2.Pop(true), 0);

    ASSERT_GE(H5Pget_actual_selection_io_mode(dxpl.get(), &persisted_mode), 0);
    EXPECT_EQ(persisted_mode, 0u);
  }
}

TEST(H5CXTest, GetterAndPopErrorPaths) {
  ScopedErrorSilence silence;

  // H5CX_get_vol_wrap_ctx fails when no API context is pushed.
  void* wrap_ctx = reinterpret_cast<void*>(0x1);
  EXPECT_LT(H5CX_get_vol_wrap_ctx(&wrap_ctx), 0);

  // Wrong-class plist lacking DXPL/LCPL/LAPL/DCPL/DAPL/FAPL properties.
  ScopedPlist wrong_plist(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(wrong_plist.valid());
  ScopedPlist wrong_dxpl_plist(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(wrong_dxpl_plist.valid());

  // 1. Open-coded DXPL / DAPL / POP error paths with invalid HIDs and wrong
  // property list classes.
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.pushed());
    H5CX_set_dxpl(H5I_INVALID_HID);

    H5Z_data_xform_t* xform = nullptr;
    H5T_vlen_alloc_info_t vl_info{};
    EXPECT_LT(H5CX_get_data_transform(&xform), 0);
    EXPECT_LT(H5CX_get_vlen_alloc_info(&vl_info), 0);

    hid_t bad_dapl = H5I_INVALID_HID;
    ASSERT_GE(H5CX_set_apl(&bad_dapl, H5P_CLS_DACC, H5I_INVALID_HID, false), 0);
    const char* prefix = nullptr;
    EXPECT_LT(H5CX_get_ext_file_prefix(&prefix), 0);
    EXPECT_LT(H5CX_get_vds_prefix(&prefix), 0);

    H5CX_set_no_selection_io_cause(1);
    EXPECT_LT(ctx.Pop(true), 0);
  }

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.pushed());
    H5CX_set_dxpl(wrong_plist.get());

    H5Z_data_xform_t* xform = nullptr;
    H5T_vlen_alloc_info_t vl_info{};
    EXPECT_LT(H5CX_get_data_transform(&xform), 0);
    EXPECT_LT(H5CX_get_vlen_alloc_info(&vl_info), 0);

    hid_t wrong_dapl = wrong_dxpl_plist.get();
    ASSERT_GE(H5CX_set_apl(&wrong_dapl, H5P_CLS_DACC, H5I_INVALID_HID, false),
              0);
    const char* prefix = nullptr;
    EXPECT_LT(H5CX_get_ext_file_prefix(&prefix), 0);
    EXPECT_LT(H5CX_get_vds_prefix(&prefix), 0);

    H5CX_set_no_selection_io_cause(1);
    EXPECT_LT(ctx.Pop(true), 0);
  }

  // Trigger failure in H5CX_pop(true) on actual_selection_io_mode write-back.
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.pushed());
    H5CX_set_dxpl(wrong_plist.get());
    EXPECT_LT(ctx.Pop(true), 0);
  }

  // 2. Hit all 4 property-retrieval failure checks in H5CX_get_vlen_alloc_info
  // incrementally via a custom property class.
  const hid_t vlen_cls =
      H5Pcreate_class(H5P_ROOT, "h5cx_test_vlen_cls", nullptr, nullptr, nullptr,
                      nullptr, nullptr, nullptr);
  ASSERT_GE(vlen_cls, 0);
  absl::Cleanup close_vlen_cls = [vlen_cls] { H5Pclose_class(vlen_cls); };

  ScopedPlist partial_vlen(H5Pcreate(vlen_cls));
  ASSERT_TRUE(partial_vlen.valid());

  H5MM_allocate_t alloc_fn = nullptr;
  void* alloc_ptr = nullptr;
  H5MM_free_t free_fn = nullptr;
  H5T_vlen_alloc_info_t vl_out{};

  {
    ScopedApiContext ctx;
    H5CX_set_dxpl(partial_vlen.get());
    EXPECT_LT(H5CX_get_vlen_alloc_info(&vl_out), 0);
  }

  ASSERT_GE(H5Pinsert2(partial_vlen.get(), H5D_XFER_VLEN_ALLOC_NAME,
                       sizeof(alloc_fn), &alloc_fn, nullptr, nullptr, nullptr,
                       nullptr, nullptr, nullptr),
            0);
  {
    ScopedApiContext ctx;
    H5CX_set_dxpl(partial_vlen.get());
    EXPECT_LT(H5CX_get_vlen_alloc_info(&vl_out), 0);
  }

  ASSERT_GE(H5Pinsert2(partial_vlen.get(), H5D_XFER_VLEN_ALLOC_INFO_NAME,
                       sizeof(alloc_ptr), &alloc_ptr, nullptr, nullptr, nullptr,
                       nullptr, nullptr, nullptr),
            0);
  {
    ScopedApiContext ctx;
    H5CX_set_dxpl(partial_vlen.get());
    EXPECT_LT(H5CX_get_vlen_alloc_info(&vl_out), 0);
  }

  ASSERT_GE(H5Pinsert2(partial_vlen.get(), H5D_XFER_VLEN_FREE_NAME,
                       sizeof(free_fn), &free_fn, nullptr, nullptr, nullptr,
                       nullptr, nullptr, nullptr),
            0);
  {
    ScopedApiContext ctx;
    H5CX_set_dxpl(partial_vlen.get());
    EXPECT_LT(H5CX_get_vlen_alloc_info(&vl_out), 0);
  }

  // 3. Macro-based getter error branches with invalid HIDs and wrong classes.
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.pushed());
    H5CX_set_dxpl(wrong_plist.get());
    H5CX_set_lcpl(H5I_INVALID_HID);
    H5CX_set_lapl(H5I_INVALID_HID);
    H5CX_set_dcpl(H5I_INVALID_HID);
    hid_t wrong_fapl = wrong_dxpl_plist.get();
    ASSERT_GE(H5CX_set_apl(&wrong_fapl, H5P_CLS_FACC, H5I_INVALID_HID, false),
              0);

    double ratios[3] = {};
    size_t sz = 0;
    void* ptr = nullptr;
    H5T_bkg_t bkg = H5T_BKG_NO;
    H5Z_EDC_t edc = H5Z_ERROR_EDC;
    H5Z_cb_t fcb{};
    H5T_conv_cb_t dcb{};
    H5D_selection_io_mode_t smode = H5D_SELECTION_IO_MODE_DEFAULT;
    uint32_t u32 = 0;
    bool flag = false;
    H5T_cset_t cset = H5T_CSET_ERROR;
    unsigned uns = 0;
    uint8_t u8 = 0;
    H5F_libver_t low = H5F_LIBVER_ERROR;
    H5F_libver_t high = H5F_LIBVER_ERROR;

    EXPECT_LT(H5CX_get_btree_split_ratios(ratios), 0);
    EXPECT_LT(H5CX_get_max_temp_buf(&sz), 0);
    EXPECT_LT(H5CX_get_tconv_buf(&ptr), 0);
    EXPECT_LT(H5CX_get_bkgr_buf(&ptr), 0);
    EXPECT_LT(H5CX_get_bkgr_buf_type(&bkg), 0);
    EXPECT_LT(H5CX_get_vec_size(&sz), 0);
    EXPECT_LT(H5CX_get_err_detect(&edc), 0);
    EXPECT_LT(H5CX_get_filter_cb(&fcb), 0);
    EXPECT_LT(H5CX_get_dt_conv_cb(&dcb), 0);
    EXPECT_LT(H5CX_get_selection_io_mode(&smode), 0);
    EXPECT_LT(H5CX_get_no_selection_io_cause(&u32), 0);
    // H5CX_get_actual_selection_io_mode resets stale state from def cache
    // when neither _set nor _valid is set on a non-default DXPL, so it
    // succeeds with 0 without querying dxpl.
    u32 = 999;
    EXPECT_GE(H5CX_get_actual_selection_io_mode(&u32), 0);
    EXPECT_EQ(u32, 0u);
    EXPECT_LT(H5CX_get_modify_write_buf(&flag), 0);
    EXPECT_LT(H5CX_get_encoding(&cset), 0);
    EXPECT_LT(H5CX_get_intermediate_group(&uns), 0);
    EXPECT_LT(H5CX_get_nlinks(&sz), 0);
    EXPECT_LT(H5CX_get_dset_min_ohdr_flag(&flag), 0);
    EXPECT_LT(H5CX_get_ohdr_flags(&u8), 0);
    EXPECT_LT(H5CX_get_libver_bounds(&low, &high), 0);
  }

  // 4. Complement step 3: wrong-class plists for LCPL/LAPL/DCPL and invalid
  // HIDs for DXPL/FAPL, plus partial FAPL lacking high_bound.
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.pushed());
    H5CX_set_dxpl(H5I_INVALID_HID);
    H5CX_set_lcpl(wrong_plist.get());
    H5CX_set_lapl(wrong_plist.get());
    H5CX_set_dcpl(wrong_plist.get());
    hid_t bad_fapl = H5I_INVALID_HID;
    ASSERT_GE(H5CX_set_apl(&bad_fapl, H5P_CLS_FACC, H5I_INVALID_HID, false), 0);

    double ratios[3] = {};
    size_t sz = 0;
    void* ptr = nullptr;
    H5T_bkg_t bkg = H5T_BKG_NO;
    H5Z_EDC_t edc = H5Z_ERROR_EDC;
    H5Z_cb_t fcb{};
    H5T_conv_cb_t dcb{};
    H5D_selection_io_mode_t smode = H5D_SELECTION_IO_MODE_DEFAULT;
    uint32_t u32 = 0;
    bool flag = false;
    H5T_cset_t cset = H5T_CSET_ERROR;
    unsigned uns = 0;
    uint8_t u8 = 0;
    H5F_libver_t low = H5F_LIBVER_ERROR;
    H5F_libver_t high = H5F_LIBVER_ERROR;

    EXPECT_LT(H5CX_get_btree_split_ratios(ratios), 0);
    EXPECT_LT(H5CX_get_max_temp_buf(&sz), 0);
    EXPECT_LT(H5CX_get_tconv_buf(&ptr), 0);
    EXPECT_LT(H5CX_get_bkgr_buf(&ptr), 0);
    EXPECT_LT(H5CX_get_bkgr_buf_type(&bkg), 0);
    EXPECT_LT(H5CX_get_vec_size(&sz), 0);
    EXPECT_LT(H5CX_get_err_detect(&edc), 0);
    EXPECT_LT(H5CX_get_filter_cb(&fcb), 0);
    EXPECT_LT(H5CX_get_dt_conv_cb(&dcb), 0);
    EXPECT_LT(H5CX_get_selection_io_mode(&smode), 0);
    EXPECT_LT(H5CX_get_no_selection_io_cause(&u32), 0);
    EXPECT_LT(H5CX_get_modify_write_buf(&flag), 0);
    EXPECT_LT(H5CX_get_encoding(&cset), 0);
    EXPECT_LT(H5CX_get_intermediate_group(&uns), 0);
    EXPECT_LT(H5CX_get_nlinks(&sz), 0);
    EXPECT_LT(H5CX_get_dset_min_ohdr_flag(&flag), 0);
    EXPECT_LT(H5CX_get_ohdr_flags(&u8), 0);
    EXPECT_LT(H5CX_get_libver_bounds(&low, &high), 0);
  }

  // Partial FAPL with low_bound only triggers the second property failure in
  // H5CX_get_libver_bounds.
  ScopedPlist partial_fapl(H5Pcreate(vlen_cls));
  ASSERT_TRUE(partial_fapl.valid());
  H5F_libver_t init_low = H5F_LIBVER_V18;
  ASSERT_GE(H5Pinsert2(partial_fapl.get(), H5F_ACS_LIBVER_LOW_BOUND_NAME,
                       sizeof(init_low), &init_low, nullptr, nullptr, nullptr,
                       nullptr, nullptr, nullptr),
            0);
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.pushed());
    hid_t fapl_id = partial_fapl.get();
    ASSERT_GE(H5CX_set_apl(&fapl_id, H5P_CLS_FACC, H5I_INVALID_HID, false), 0);
    H5F_libver_t low = H5F_LIBVER_ERROR;
    H5F_libver_t high = H5F_LIBVER_ERROR;
    EXPECT_LT(H5CX_get_libver_bounds(&low, &high), 0);
  }
}

TEST(H5CXTest, RetrieveRestoreFreeStateAndVolContext) {
  // 1. Default state round-trip (all default plists, null VOL wrapper/prop).
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.pushed());

    void* wrap_ctx = reinterpret_cast<void*>(0x1);
    ASSERT_GE(H5CX_get_vol_wrap_ctx(&wrap_ctx), 0);
    EXPECT_EQ(wrap_ctx, nullptr);

    H5VL_connector_prop_t vol_prop{};
    vol_prop.connector = H5VL_NATIVE_conn_g;
    ASSERT_GE(H5CX_get_vol_connector_prop(&vol_prop), 0);
    EXPECT_EQ(vol_prop.connector, nullptr);
    EXPECT_EQ(vol_prop.connector_info, nullptr);

    H5CX_state_t* state = nullptr;
    ASSERT_GE(H5CX_retrieve_state(&state), 0);
    ASSERT_NE(state, nullptr);
    absl::Cleanup free_state = [state] { H5CX_free_state(state); };

    EXPECT_EQ(state->dcpl_id, H5P_DATASET_CREATE_DEFAULT);
    EXPECT_EQ(state->dxpl_id, H5P_DATASET_XFER_DEFAULT);
    EXPECT_EQ(state->lapl_id, H5P_LINK_ACCESS_DEFAULT);
    EXPECT_EQ(state->lcpl_id, H5P_LINK_CREATE_DEFAULT);
    EXPECT_EQ(state->vol_wrap_ctx, nullptr);
    EXPECT_EQ(state->vol_connector_prop.connector, nullptr);

    ASSERT_GE(H5CX_restore_state(state), 0);
    ASSERT_GE(H5CX_get_vol_wrap_ctx(&wrap_ctx), 0);
    EXPECT_EQ(wrap_ctx, nullptr);
  }

  // 2. Full custom state round-trip with custom plists, real H5VL wrapper
  // context from a Core VFD file, and custom registered VOL connector + info.
  ScopedFile file = CreateInMemoryFile("vol_wrap.h5");
  ASSERT_TRUE(file.valid());
  H5VL_object_t* vol_obj = H5VL_vol_object_verify(file.get(), H5I_FILE);
  ASSERT_NE(vol_obj, nullptr);

  H5VL_class_t dummy_cls{};
  dummy_cls.version = H5VL_VERSION;
  dummy_cls.value = H5_VOL_RESERVED + 77;
  dummy_cls.name = "h5cx_test_dummy_vol";
  dummy_cls.info_cls.size = sizeof(DummyVolInfo);
  dummy_cls.info_cls.copy = &CopyDummyVolInfo;
  dummy_cls.info_cls.free = &FreeDummyVolInfo;

  const hid_t vol_id = H5VLregister_connector(&dummy_cls, H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  absl::Cleanup unregister_vol = [vol_id] { H5VLunregister_connector(vol_id); };

  H5VL_connector_t* dummy_conn =
      static_cast<H5VL_connector_t*>(H5I_object_verify(vol_id, H5I_VOL));
  ASSERT_NE(dummy_conn, nullptr);

  DummyVolInfo raw_info{12345};
  H5VL_connector_prop_t in_prop{dummy_conn, &raw_info};

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_dset_no_attrs_hint(dcpl.get(), true), 0);

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(H5Pset_hyper_vector_size(dxpl.get(), 32), 0);

  ScopedPlist lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(lapl.valid());
  ASSERT_GE(H5Pset_nlinks(lapl.get(), 19), 0);

  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());
  ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 1), 0);

  H5CX_state_t* saved_state = nullptr;
  {
    ScopedApiContext outer;
    ASSERT_TRUE(outer.pushed());
    H5CX_set_dcpl(dcpl.get());
    H5CX_set_dxpl(dxpl.get());
    H5CX_set_lapl(lapl.get());
    H5CX_set_lcpl(lcpl.get());
    ASSERT_GE(H5VL_set_vol_wrapper(vol_obj), 0);
    ASSERT_GE(H5CX_set_vol_connector_prop(&in_prop), 0);

    void* created_wrap_ctx = nullptr;
    ASSERT_GE(H5CX_get_vol_wrap_ctx(&created_wrap_ctx), 0);
    ASSERT_NE(created_wrap_ctx, nullptr);

    ASSERT_GE(H5CX_retrieve_state(&saved_state), 0);
    ASSERT_NE(saved_state, nullptr);

    // Release outer wrapper ref (rc: 2 -> 1, still owned by saved_state).
    ASSERT_GE(H5VL_reset_vol_wrapper(), 0);
    EXPECT_GE(outer.Pop(false), 0);
  }
  absl::Cleanup free_saved_state = [saved_state] {
    H5CX_free_state(saved_state);
  };

  EXPECT_NE(saved_state->dcpl_id, H5P_DATASET_CREATE_DEFAULT);
  EXPECT_NE(saved_state->dxpl_id, H5P_DATASET_XFER_DEFAULT);
  EXPECT_NE(saved_state->lapl_id, H5P_LINK_ACCESS_DEFAULT);
  EXPECT_NE(saved_state->lcpl_id, H5P_LINK_CREATE_DEFAULT);
  EXPECT_NE(saved_state->vol_wrap_ctx, nullptr);
  EXPECT_EQ(saved_state->vol_connector_prop.connector, dummy_conn);
  ASSERT_NE(saved_state->vol_connector_prop.connector_info, nullptr);
  EXPECT_NE(saved_state->vol_connector_prop.connector_info, &raw_info);
  EXPECT_EQ(static_cast<const DummyVolInfo*>(
                saved_state->vol_connector_prop.connector_info)
                ->magic,
            12345);

  // Restore into a clean context and verify all restored fields.
  {
    ScopedApiContext restored_ctx;
    ASSERT_TRUE(restored_ctx.pushed());
    ASSERT_GE(H5CX_restore_state(saved_state), 0);

    bool min_ohdr = false;
    size_t vec_size = 0;
    size_t nlinks = 0;
    unsigned intermed = 0;
    void* wrap_ctx = nullptr;
    H5VL_connector_prop_t out_prop{};

    ASSERT_GE(H5CX_get_dset_min_ohdr_flag(&min_ohdr), 0);
    ASSERT_GE(H5CX_get_vec_size(&vec_size), 0);
    ASSERT_GE(H5CX_get_nlinks(&nlinks), 0);
    ASSERT_GE(H5CX_get_intermediate_group(&intermed), 0);
    ASSERT_GE(H5CX_get_vol_wrap_ctx(&wrap_ctx), 0);
    ASSERT_GE(H5CX_get_vol_connector_prop(&out_prop), 0);

    EXPECT_TRUE(min_ohdr);
    EXPECT_EQ(vec_size, 32u);
    EXPECT_EQ(nlinks, 19u);
    EXPECT_EQ(intermed, 1u);
    EXPECT_EQ(wrap_ctx, saved_state->vol_wrap_ctx);
    EXPECT_EQ(out_prop.connector, dummy_conn);
    ASSERT_NE(out_prop.connector_info, nullptr);
    EXPECT_EQ(static_cast<const DummyVolInfo*>(out_prop.connector_info)->magic,
              12345);
  }
}

TEST(H5CXTest, RetrieveStateErrorCleanupPaths) {
  ScopedErrorSilence silence;

  ScopedPlist ok_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ScopedPlist ok_dxpl(H5Pcreate(H5P_DATASET_XFER));
  ScopedPlist ok_lapl(H5Pcreate(H5P_LINK_ACCESS));
  ASSERT_TRUE(ok_dcpl.valid() && ok_dxpl.valid() && ok_lapl.valid());

  // 1. Invalid HID lookup failures at each plist slot in H5CX_retrieve_state.
  for (int stage = 0; stage < 4; ++stage) {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.pushed());
    H5CX_set_dcpl(stage == 0 ? H5I_INVALID_HID : ok_dcpl.get());
    if (stage > 0) {
      H5CX_set_dxpl(stage == 1 ? H5I_INVALID_HID : ok_dxpl.get());
    }
    if (stage > 1) {
      H5CX_set_lapl(stage == 2 ? H5I_INVALID_HID : ok_lapl.get());
    }
    if (stage > 2) {
      H5CX_set_lcpl(H5I_INVALID_HID);
    }

    H5CX_state_t* state = reinterpret_cast<H5CX_state_t*>(0x1);
    EXPECT_LT(H5CX_retrieve_state(&state), 0);
    EXPECT_THAT(state, Eq(nullptr));
  }
  // Note: upstream H5CX_retrieve_state stores H5P_copy_plist's -1 (FAIL)
  // directly into (*api_state)->*_id, causing H5CX_free_state's H5I_dec_ref(-1)
  // error return to skip H5FL_FREE and leak *api_state (flagged by
  // HeapLeakChecker / LSan), while H5P_copy_plist error cleanup corrupts
  // built-in class nplists in H5Pint.c.
}

}  // namespace
}  // namespace hdf5
