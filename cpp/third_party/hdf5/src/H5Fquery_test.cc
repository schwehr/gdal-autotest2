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
// Tests internal H5F_t / H5F_shared_t file query accessors, FCPL/FAPL property
// queries, VFD query wrappers, SOHM fields, container info, and mounted query
// fields (third_party/hdf5/src/H5Fquery.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Fquery.c

// `:hdf5` is built with -DNDEBUG; match internal struct layout in C++ tests.
#ifndef NDEBUG
#define H5FQUERY_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5F_FRIEND
#include "third_party/hdf5/src/H5Bprivate.h"
#include "third_party/hdf5/src/H5FDcore.h"
#include "third_party/hdf5/src/H5FDsec2.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#include "third_party/hdf5/src/H5Opublic.h"
#include "third_party/hdf5/src/H5VLconnector.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"

#ifdef H5FQUERY_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FQUERY_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <cstdint>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5FqueryTest, FileStructureSharedReopenAndMountAccessors) {
  ScopedFile f1 = CreateInMemoryFile("query_file_1.h5");
  ScopedFile f2(H5Freopen(f1.get()));
  ScopedFile f3 = CreateInMemoryFile("query_file_3.h5");
  ASSERT_TRUE(f1.valid());
  ASSERT_TRUE(f2.valid());
  ASSERT_TRUE(f3.valid());

  auto* h1 = static_cast<H5F_t*>(H5VL_object_verify(f1.get(), H5I_FILE));
  auto* h2 = static_cast<H5F_t*>(H5VL_object_verify(f2.get(), H5I_FILE));
  auto* h3 = static_cast<H5F_t*>(H5VL_object_verify(f3.get(), H5I_FILE));
  ASSERT_NE(h1, nullptr);
  ASSERT_NE(h2, nullptr);
  ASSERT_NE(h3, nullptr);

  H5F_shared_t* sh1 = H5F_get_shared(h1);
  ASSERT_NE(sh1, nullptr);
  EXPECT_NE(H5F_shared_get_intent(sh1) & H5F_ACC_RDWR, 0U);
  EXPECT_NE(H5F_get_intent(h1) & H5F_ACC_RDWR, 0U);
  EXPECT_TRUE(H5F_same_shared(h1, h2));
  EXPECT_FALSE(H5F_same_shared(h1, h3));
  EXPECT_EQ(H5F_get_nrefs(h1), 2U);

  // Opening a sec2 file read-only verifies exact H5F_ACC_RDONLY intent.
  TempFile ro_tf("query_ro.h5");
  {
    ScopedFile create_ro(
        H5Fcreate(ro_tf.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(create_ro.valid());
  }
  ScopedFile ro_file(H5Fopen(ro_tf.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(ro_file.valid());
  auto* ro_h = static_cast<H5F_t*>(H5VL_object_verify(ro_file.get(), H5I_FILE));
  ASSERT_NE(ro_h, nullptr);
  EXPECT_EQ(H5F_get_intent(ro_h), H5F_ACC_RDONLY);
  EXPECT_EQ(H5F_shared_get_intent(H5F_get_shared(ro_h)), H5F_ACC_RDONLY);

  EXPECT_STREQ(H5F_get_open_name(h1), "query_file_1.h5");
  EXPECT_NE(H5F_get_actual_name(h1), nullptr);
  EXPECT_NE(H5F_get_extpath(h1), nullptr);
  EXPECT_TRUE(H5F_file_id_exists(h1));
  EXPECT_NE(H5F_get_vol_obj(h1), nullptr);

  ScopedGroup grp(
      H5Gcreate2(f1.get(), "/mnt", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  EXPECT_EQ(H5F_get_nopen_objs(h1), 1U);
  grp.reset();
  EXPECT_EQ(H5F_get_nopen_objs(h1), 0U);

  // Mounting f3 onto f1 updates parent and nmounts accessors.
  EXPECT_EQ(H5F_get_parent(h3), nullptr);
  EXPECT_EQ(H5F_get_nmounts(h1), 0U);
  ASSERT_GE(H5Fmount(f1.get(), "/mnt", f3.get(), H5P_DEFAULT), 0);
  EXPECT_EQ(H5F_get_parent(h3), h1);
  EXPECT_EQ(H5F_get_nmounts(h1), 1U);
  ASSERT_GE(H5Funmount(f1.get(), "/mnt"), 0);
  EXPECT_EQ(H5F_get_parent(h3), nullptr);
  EXPECT_EQ(H5F_get_nmounts(h1), 0U);
}

TEST(H5FqueryTest, CustomFcplFaplAndSohmQueriesReflectConfiguredSettings) {
  TempFile sec2_file("query_custom_sec2.h5");
  TempFile mdc_log("query_mdc.log");

  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_TRUE(fapl.valid());

  // Configure FCPL: sizes, k-values, SOHM indexes, paged free-space strategy.
  ASSERT_GE(H5Pset_sizes(fcpl.get(), 8, 8), 0);
  ASSERT_GE(H5Pset_sym_k(fcpl.get(), 12, 8), 0);
  ASSERT_GE(H5Pset_istore_k(fcpl.get(), 24), 0);
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
  ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_ATTR_FLAG, 16), 0);
  ASSERT_GE(
      H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_PAGE, true, 16),
      0);
  ASSERT_GE(H5Pset_file_space_page_size(fcpl.get(), 1024), 0);

  // Configure FAPL: libver, cache, alignment, sieve, gc_ref, fclose_degree,
  // evict_on_close, read_attempts, mdc_logging, file_locking, rfic_flags.
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_V114), 0);
  ASSERT_GE(H5Pset_cache(fapl.get(), 0, 127, 65536, 0.25), 0);
  ASSERT_GE(H5Pset_alignment(fapl.get(), 64, 128), 0);
  ASSERT_GE(H5Pset_sieve_buf_size(fapl.get(), 16384), 0);
  ASSERT_GE(H5Pset_gc_references(fapl.get(), 1), 0);
  ASSERT_GE(H5Pset_fclose_degree(fapl.get(), H5F_CLOSE_STRONG), 0);
  ASSERT_GE(H5Pset_evict_on_close(fapl.get(), true), 0);
  ASSERT_GE(H5Pset_metadata_read_attempts(fapl.get(), 3), 0);
  ASSERT_GE(H5Pset_mdc_log_options(fapl.get(), true, mdc_log.c_str(), false), 0);
  ASSERT_GE(H5Pset_file_locking(fapl.get(), false, true), 0);
  ASSERT_GE(
      H5Pset_relax_file_integrity_checks(fapl.get(), H5F_RFIC_UNUSUAL_NUM_UNUSED_NUMERIC_BITS),
      0);

  ScopedFile file(
      H5Fcreate(sec2_file.c_str(), H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
  ASSERT_TRUE(file.valid());
  ASSERT_GE(H5Fset_dset_no_attrs_hint(file.get(), true), 0);

  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
  ASSERT_NE(f, nullptr);

  // Bounds and superblock/FCPL fields.
  EXPECT_EQ(H5F_get_low_bound(f), H5F_LIBVER_V18);
  EXPECT_EQ(H5F_get_high_bound(f), H5F_LIBVER_V114);
  EXPECT_EQ(H5F_sizeof_addr(f), uint8_t{8});
  EXPECT_EQ(H5F_sizeof_size(f), uint8_t{8});
  EXPECT_EQ(H5F_get_base_addr(f), haddr_t{0});
  EXPECT_EQ(H5F_sym_leaf_k(f), 8U);

  H5B_class_t snode_type{};
  snode_type.id = H5B_SNODE_ID;
  EXPECT_EQ(H5F_kvalue(f, &snode_type), 12U);
  H5B_class_t chunk_type{};
  chunk_type.id = H5B_CHUNK_ID;
  EXPECT_EQ(H5F_kvalue(f, &chunk_type), 24U);

  EXPECT_NE(H5F_get_fcpl(f), H5I_INVALID_HID);
  EXPECT_EQ(H5F_get_fc_degree(f), H5F_CLOSE_STRONG);
  EXPECT_TRUE(H5F_get_evict_on_close(f));
  EXPECT_EQ(H5F_rdcc_nslots(f), 127U);
  EXPECT_EQ(H5F_rdcc_nbytes(f), 65536U);
  EXPECT_DOUBLE_EQ(H5F_rdcc_w0(f), 0.25);
  EXPECT_EQ(H5F_sieve_buf_size(f), 16384U);
  EXPECT_EQ(H5F_gc_ref(f), 1U);
  EXPECT_GE(H5F_get_threshold(f), hsize_t{64});
  EXPECT_GE(H5F_get_alignment(f), hsize_t{128});
  EXPECT_EQ(H5F_get_read_attempts(f), 3U);
  EXPECT_TRUE(H5F_get_min_dset_ohdr(f));
  EXPECT_FALSE(H5F_get_use_file_locking(f));
  EXPECT_EQ(H5F_get_rfic_flags(f),
            static_cast<uint64_t>(H5F_RFIC_UNUSUAL_NUM_UNUSED_NUMERIC_BITS));

  // MDC logging properties.
  EXPECT_TRUE(H5F_use_mdc_logging(f));
  EXPECT_FALSE(H5F_start_mdc_log_on_access(f));
  EXPECT_STREQ(H5F_mdc_log_location(f), mdc_log.c_str());

  // SOHM, msg_crt_idx, grp_btree_shared, and FSM metadata queries.
  EXPECT_GE(H5F_get_sohm_nindexes(f), 1U);
  (void)H5F_get_sohm_addr(f);
  (void)H5F_get_sohm_vers(f);
  EXPECT_TRUE(H5F_store_msg_crt_idx(f));
  EXPECT_EQ(H5F_grp_btree_shared(f), f->shared->grp_btree_shared);
  EXPECT_EQ(H5F_get_pgend_meta_thres(f), f->shared->pgend_meta_thres);
  EXPECT_FALSE(H5F_get_point_of_no_return(f));
  EXPECT_FALSE(H5F_get_null_fsm_addr(f));

  // Temporary address queries.
  (void)H5F_use_tmp_space(f);
  EXPECT_FALSE(H5F_is_tmp_addr(f, 0));
  EXPECT_TRUE(H5F_is_tmp_addr(f, f->shared->tmp_addr));

  // Container info success and invalid-version failure.
  H5VL_file_cont_info_t cinfo{};
  cinfo.version = H5VL_CONTAINER_INFO_VERSION;
  ASSERT_GE(H5F__get_cont_info(f, &cinfo), 0);
  EXPECT_EQ(cinfo.feature_flags, 0U);
  EXPECT_EQ(cinfo.token_size, 8U);
  EXPECT_EQ(cinfo.blob_id_size, 12U);

  cinfo.version = 999;
  ScopedErrorSilence silence;
  EXPECT_LT(H5F__get_cont_info(f, &cinfo), 0);
}

TEST(H5FqueryTest, VfdLayerQueriesAndHandleRetrievalForCoreAndSec2) {
  // 1. Core VFD (no backing store) queries.
  ScopedFile core_file = CreateInMemoryFile("query_core_vfd.h5");
  ASSERT_TRUE(core_file.valid());
  auto* cf = static_cast<H5F_t*>(H5VL_object_verify(core_file.get(), H5I_FILE));
  ASSERT_NE(cf, nullptr);
  H5F_shared_t* csh = H5F_get_shared(cf);
  ASSERT_NE(csh, nullptr);

  EXPECT_EQ(H5F_get_driver_id(cf), H5FD_CORE);
  unsigned long fileno = 0;
  EXPECT_GE(H5F_get_fileno(cf, &fileno), 0);
  EXPECT_TRUE(H5F_shared_has_feature(csh, H5FD_FEAT_AGGREGATE_METADATA));
  EXPECT_TRUE(H5F_has_feature(cf, H5FD_FEAT_ACCUMULATE_METADATA));
  EXPECT_FALSE(H5F_has_feature(cf, H5FD_FEAT_HAS_MPI));

  const haddr_t eoa_sh = H5F_shared_get_eoa(csh, H5FD_MEM_DEFAULT);
  const haddr_t eoa_f = H5F_get_eoa(cf, H5FD_MEM_DEFAULT);
  EXPECT_NE(eoa_sh, HADDR_UNDEF);
  EXPECT_EQ(eoa_sh, eoa_f);

  H5FD_t* lf = nullptr;
  ASSERT_GE(H5F_shared_get_file_driver(csh, &lf), 0);
  EXPECT_EQ(lf, csh->lf);

  void* core_handle = nullptr;
  ASSERT_GE(H5F_get_vfd_handle(cf, H5P_DEFAULT, &core_handle), 0);
  EXPECT_NE(core_handle, nullptr);

  // Invalid FAPL ID causes H5FD_get_vfd_handle -> H5F_get_vfd_handle error.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5F_get_vfd_handle(cf, H5I_INVALID_HID, &core_handle), 0);
  }

  // Exercise vector/selection I/O support query for both read and write.
  (void)H5F_has_vector_select_io(cf, false);
  (void)H5F_has_vector_select_io(cf, true);

  // 2. Default sec2 VFD file handle and extpath on filesystem path.
  TempFile tf("query_sec2_vfd.h5");
  ScopedFile sec2_file(
      H5Fcreate(tf.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sec2_file.valid());
  auto* sf = static_cast<H5F_t*>(H5VL_object_verify(sec2_file.get(), H5I_FILE));
  ASSERT_NE(sf, nullptr);

  EXPECT_EQ(H5F_get_driver_id(sf), H5FD_SEC2);
  EXPECT_FALSE(std::string_view(H5F_get_extpath(sf)).empty());
  void* fd_ptr = nullptr;
  ASSERT_GE(H5F_get_vfd_handle(sf, H5P_DEFAULT, &fd_ptr), 0);
  ASSERT_NE(fd_ptr, nullptr);
  EXPECT_GE(*static_cast<int*>(fd_ptr), 0);
  EXPECT_FALSE(H5F_has_vector_select_io(sf, false));
  EXPECT_FALSE(H5F_has_vector_select_io(sf, true));
}

}  // namespace
}  // namespace hdf5
