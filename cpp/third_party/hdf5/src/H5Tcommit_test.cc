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
// Tests committed datatype lifecycle (named/anonymous/async commit and open,
// creation property lists, flush/refresh, multi-open shared handles, RFIC and
// version bounds validation, and internal VOL helpers)
// (third_party/hdf5/src/H5Tcommit.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tcommit.c

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"
#include "third_party/hdf5/src/H5VLconnector.h"
#include "third_party/hdf5/src/H5Zdevelop.h"

#ifndef NDEBUG
#define H5TCOMMIT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5T_FRIEND
#if defined(H5_HAVE__FLOAT16) && defined(__cplusplus)
using H5__Float16 = uint16_t;
#endif
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FOprivate.h"
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Lprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5Tpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5TCOMMIT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TCOMMIT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

class ScopedApiContext {
 public:
  explicit ScopedApiContext(haddr_t tag = 0x10000) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5AC_tag(tag, &prev_tag_);
      tagged_ = true;
    }
  }

  ~ScopedApiContext() {
    if (tagged_) {
      H5AC_tag(prev_tag_, nullptr);
    }
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

TEST(H5TcommitTest, NamedCommitOpenPlistAndLinkInMemory) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedPlist lcpl(H5Pcreate(H5P_LINK_CREATE));
  ASSERT_TRUE(lcpl.valid());
  ASSERT_GE(H5Pset_create_intermediate_group(lcpl.get(), 1), 0);

  ScopedPlist tcpl(H5Pcreate(H5P_DATATYPE_CREATE));
  ASSERT_TRUE(tcpl.valid());
  ASSERT_GE(H5Pset_obj_track_times(tcpl.get(), false), 0);

  ScopedPlist tapl(H5Pcreate(H5P_DATATYPE_ACCESS));
  ASSERT_TRUE(tapl.valid());

  ScopedType dt(H5Tcreate(H5T_COMPOUND, sizeof(int32_t) + sizeof(double)));
  ASSERT_TRUE(dt.valid());
  ASSERT_GE(H5Tinsert(dt.get(), "id", 0, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(dt.get(), "val", sizeof(int32_t), H5T_NATIVE_DOUBLE), 0);

  EXPECT_EQ(H5Tcommitted(dt.get()), 0);
  EXPECT_EQ(H5Tcommitted(H5T_NATIVE_INT32), 0);

  // Check get_create_plist on uncommitted transient and immutable types.
  {
    ScopedPlist transient_tcpl(H5Tget_create_plist(dt.get()));
    ASSERT_TRUE(transient_tcpl.valid());
    ScopedPlist immutable_tcpl(H5Tget_create_plist(H5T_NATIVE_INT32));
    ASSERT_TRUE(immutable_tcpl.valid());
  }

  ASSERT_GE(H5Tcommit2(file.get(), "/types/rec", dt.get(), lcpl.get(), tcpl.get(),
                       tapl.get()),
            0);
  EXPECT_GT(H5Tcommitted(dt.get()), 0);

  // Verify custom TCPL properties persisted via H5Tget_create_plist.
  {
    ScopedPlist got_tcpl(H5Tget_create_plist(dt.get()));
    ASSERT_TRUE(got_tcpl.valid());
    bool track_times = true;
    ASSERT_GE(H5Pget_obj_track_times(got_tcpl.get(), &track_times), 0);
    EXPECT_FALSE(track_times);
  }

  // Create an attribute using the committed datatype (exercises H5T_link).
  ScopedSpace attr_space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(attr_space.valid());
  ScopedAttr attr(H5Acreate2(file.get(), "rec_attr", dt.get(), attr_space.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());

  // Flush the named datatype.
  EXPECT_GE(H5Tflush(dt.get()), 0);

  // Reopen synchronously via file ID and via group ID.
  ScopedGroup types_grp(H5Gopen2(file.get(), "/types", H5P_DEFAULT));
  ASSERT_TRUE(types_grp.valid());
  ScopedType opened1(H5Topen2(file.get(), "/types/rec", tapl.get()));
  ASSERT_TRUE(opened1.valid());
  EXPECT_GT(H5Tcommitted(opened1.get()), 0);
  EXPECT_GT(H5Tequal(opened1.get(), dt.get()), 0);

  // Simultaneous open hits shared_fo != NULL reuse path in H5T_open.
  ScopedType opened2(H5Topen2(types_grp.get(), "rec", H5P_DEFAULT));
  ASSERT_TRUE(opened2.valid());
  EXPECT_GT(H5Tequal(opened2.get(), dt.get()), 0);
}

TEST(H5TcommitTest, AsyncCommitAndOpenWithEventSet) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedType dt_none(H5Tcopy(H5T_NATIVE_INT64));
  ASSERT_TRUE(dt_none.valid());
  ASSERT_GE(H5Tcommit_async(file.get(), "/t_none", dt_none.get(), H5P_DEFAULT,
                            H5P_DEFAULT, H5P_DEFAULT, H5ES_NONE),
            0);
  EXPECT_GT(H5Tcommitted(dt_none.get()), 0);

  ScopedType open_none(
      H5Topen_async(file.get(), "/t_none", H5P_DEFAULT, H5ES_NONE));
  ASSERT_TRUE(open_none.valid());
  EXPECT_GT(H5Tequal(open_none.get(), dt_none.get()), 0);

  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());
  ScopedType dt_es(H5Tcopy(H5T_NATIVE_DOUBLE));
  ASSERT_TRUE(dt_es.valid());
  ASSERT_GE(H5Tcommit_async(file.get(), "/t_es", dt_es.get(), H5P_DEFAULT,
                            H5P_DEFAULT, H5P_DEFAULT, es.get()),
            0);

  ScopedType open_es(
      H5Topen_async(file.get(), "/t_es", H5P_DEFAULT, es.get()));
  ASSERT_TRUE(open_es.valid());
  EXPECT_GT(H5Tequal(open_es.get(), dt_es.get()), 0);
}

TEST(H5TcommitTest, AnonymousCommitLinkAndVlenDiskLocConversion) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  // VLEN datatype exercises H5T_LOC_DISK <-> H5T_LOC_MEMORY inside H5T__commit.
  ScopedType vlen_dt(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vlen_dt.valid());

  ScopedPlist tcpl(H5Pcreate(H5P_DATATYPE_CREATE));
  ASSERT_TRUE(tcpl.valid());
  ScopedPlist tapl(H5Pcreate(H5P_DATATYPE_ACCESS));
  ASSERT_TRUE(tapl.valid());

  ASSERT_GE(H5Tcommit_anon(file.get(), vlen_dt.get(), tcpl.get(), tapl.get()),
            0);
  EXPECT_GT(H5Tcommitted(vlen_dt.get()), 0);

  ASSERT_GE(H5Olink(vlen_dt.get(), file.get(), "/linked_vlen", H5P_DEFAULT,
                    H5P_DEFAULT),
            0);

  ScopedType reopened(H5Topen2(file.get(), "/linked_vlen", H5P_DEFAULT));
  ASSERT_TRUE(reopened.valid());
  EXPECT_EQ(H5Tget_class(reopened.get()), H5T_VLEN);

  // Unlinked anonymous commit cleans up cleanly on close.
  ScopedType anon_unlinked(H5Tcopy(H5T_NATIVE_INT16));
  ASSERT_TRUE(anon_unlinked.valid());
  ASSERT_GE(H5Tcommit_anon(file.get(), anon_unlinked.get(), H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  EXPECT_GT(H5Tcommitted(anon_unlinked.get()), 0);
}

TEST(H5TcommitTest, RefreshAndMultiHandleReopenOnTempFile) {
  TempFile temp("tcommit_refresh.h5");

  // Create OS file containing a committed compound datatype with an attribute.
  {
    ScopedFile rw_file(
        H5Fcreate(temp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(rw_file.valid());

    ScopedType dt(H5Tcreate(H5T_COMPOUND, sizeof(int32_t) + sizeof(float)));
    ASSERT_TRUE(dt.valid());
    ASSERT_GE(H5Tinsert(dt.get(), "a", 0, H5T_NATIVE_INT32), 0);
    ASSERT_GE(H5Tinsert(dt.get(), "b", sizeof(int32_t), H5T_NATIVE_FLOAT), 0);
    ASSERT_GE(H5Tcommit2(rw_file.get(), "/comp", dt.get(), H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT),
              0);
    // H5Trefresh is valid on RW files too (though H5O_refresh_metadata is a no-op).
    EXPECT_GE(H5Trefresh(dt.get()), 0);
  }

  // Open two read-only handles to the same file to hit:
  //   1) H5Trefresh -> H5T_save_refresh_state + H5T_construct_datatype +
  //      H5T_restore_refresh_state (only triggered on non-RDWR files), and
  //   2) H5FO_top_count == 0 inside H5T_open when opening /comp from f2 while f1
  //      already has shared_fo in open_objs.
  ScopedFile f1(H5Fopen(temp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(f1.valid());
  ScopedFile f2(H5Fopen(temp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(f2.valid());

  ScopedType dt1(H5Topen2(f1.get(), "/comp", H5P_DEFAULT));
  ASSERT_TRUE(dt1.valid());

  ScopedType dt2(H5Topen2(f2.get(), "/comp", H5P_DEFAULT));
  ASSERT_TRUE(dt2.valid());
  EXPECT_EQ(H5Tget_nmembers(dt2.get()), 2);

  ASSERT_GE(H5Trefresh(dt1.get()), 0);
  EXPECT_EQ(H5Tget_class(dt1.get()), H5T_COMPOUND);
  EXPECT_EQ(H5Tget_nmembers(dt1.get()), 2);

  // Committing onto a read-only file fails cleanly via both commit2 and anon.
  ScopedErrorSilence silence;
  ScopedType transient(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(transient.valid());
  EXPECT_LT(H5Tcommit2(f1.get(), "/ro_fail", transient.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(
      H5Tcommit_anon(f1.get(), transient.get(), H5P_DEFAULT, H5P_DEFAULT), 0);
}

TEST(H5TcommitTest, VersionBoundsAndUnusualUnusedBitsChecks) {
  // H5T_set_version failure when committing v5 complex type into v1.8-bounded file.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_V18),
              0);
    ScopedFile bounded_file(
        H5Fcreate("bounded.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(bounded_file.valid());

    ScopedType cfloat(H5Tcomplex_create(H5T_NATIVE_FLOAT));
    ASSERT_TRUE(cfloat.valid());

    ScopedErrorSilence silence;
    EXPECT_LT(H5Tcommit2(bounded_file.get(), "/bad_ver", cfloat.get(),
                         H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_EQ(H5Tcommitted(cfloat.get()), 0);
  }

  // Unusual unused numeric bits: fails on default v1-ohdr file (cleaning up created ohdr),
  // succeeds with H5F_RFIC_UNUSUAL_NUM_UNUSED_NUMERIC_BITS or v2 checksummed ohdr.
  ScopedType unusual_i32(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(unusual_i32.valid());
  ASSERT_GE(H5Tset_precision(unusual_i32.get(), 4), 0);

  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_LATEST),
              0);
    ScopedFile v1_file(
        H5Fcreate("unusual_fail.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(v1_file.valid());
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tcommit2(v1_file.get(), "/unusual", unusual_i32.get(),
                         H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_EQ(H5Lexists(v1_file.get(), "/unusual", H5P_DEFAULT), 0);
  }

  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_relax_file_integrity_checks(
                  fapl.get(), H5F_RFIC_UNUSUAL_NUM_UNUSED_NUMERIC_BITS),
              0);
    ScopedFile rfic_file(
        H5Fcreate("unusual_ok.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(rfic_file.valid());
    ScopedType copy_ok(H5Tcopy(unusual_i32.get()));
    ASSERT_TRUE(copy_ok.valid());
    EXPECT_GE(H5Tcommit2(rfic_file.get(), "/unusual_ok", copy_ok.get(),
                         H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_LATEST, H5F_LIBVER_LATEST),
        0);
    ScopedFile latest_file(
        H5Fcreate("unusual_chksum.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(latest_file.valid());
    ScopedType copy_chksum(H5Tcopy(unusual_i32.get()));
    ASSERT_TRUE(copy_chksum.valid());
    EXPECT_GE(H5Tcommit2(latest_file.get(), "/unusual_v2", copy_chksum.get(),
                         H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
              0);
  }
}

TEST(H5TcommitTest, PublicApiErrorPaths) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());
  ScopedGroup grp(H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());

  ScopedType transient(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(transient.valid());
  ScopedType committed(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(committed.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/already", committed.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedErrorSilence silence;

  // H5Tcommit2 & async invalid arguments.
  EXPECT_LT(H5Tcommit2(file.get(), nullptr, transient.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Tcommit2(file.get(), "", transient.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Tcommit2(file.get(), "/bad1", H5I_INVALID_HID, H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Tcommit2(file.get(), "/bad2", space.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Tcommit2(file.get(), "/bad3", committed.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Tcommit2(file.get(), "/bad4", transient.get(), dcpl.get(),
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Tcommit2(file.get(), "/bad5", transient.get(), H5P_DEFAULT,
                       dcpl.get(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Tcommit2(file.get(), "/bad6", transient.get(), H5P_DEFAULT,
                       H5P_DEFAULT, dcpl.get()),
            0);
  EXPECT_LT(H5Tcommit2(H5I_INVALID_HID, "/bad7", transient.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Tcommit2(file.get(), "/bad8", H5T_NATIVE_INT32, H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Non-sensible empty compound fails.
  ScopedType empty_comp(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(empty_comp.valid());
  EXPECT_LT(H5Tcommit2(file.get(), "/empty_comp", empty_comp.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Committing to an existing link or missing intermediate group fails H5T__commit_named.
  EXPECT_LT(H5Tcommit2(file.get(), "/grp", transient.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_LT(H5Tcommit2(file.get(), "/missing/dt", transient.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  EXPECT_LT(H5Tcommit_async(file.get(), nullptr, transient.get(), H5P_DEFAULT,
                            H5P_DEFAULT, H5P_DEFAULT, H5ES_NONE),
            0);

  // H5Tcommit_anon invalid arguments.
  EXPECT_LT(
      H5Tcommit_anon(file.get(), H5I_INVALID_HID, H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_LT(H5Tcommit_anon(file.get(), space.get(), H5P_DEFAULT, H5P_DEFAULT), 0);
  EXPECT_LT(
      H5Tcommit_anon(file.get(), committed.get(), H5P_DEFAULT, H5P_DEFAULT), 0);
  EXPECT_LT(H5Tcommit_anon(file.get(), transient.get(), dcpl.get(), H5P_DEFAULT),
            0);
  EXPECT_LT(H5Tcommit_anon(file.get(), transient.get(), H5P_DEFAULT, dcpl.get()),
            0);
  EXPECT_LT(
      H5Tcommit_anon(H5I_INVALID_HID, transient.get(), H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_LT(
      H5Tcommit_anon(space.get(), transient.get(), H5P_DEFAULT, H5P_DEFAULT), 0);

  // H5Tcommitted, H5Tget_create_plist, H5Tflush, H5Trefresh invalid IDs.
  EXPECT_LT(H5Tcommitted(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Tcommitted(space.get()), 0);

  EXPECT_EQ(H5Tget_create_plist(H5I_INVALID_HID), H5I_INVALID_HID);
  EXPECT_EQ(H5Tget_create_plist(space.get()), H5I_INVALID_HID);

  EXPECT_LT(H5Tflush(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Tflush(space.get()), 0);
  EXPECT_LT(H5Tflush(transient.get()), 0);

  EXPECT_LT(H5Trefresh(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Trefresh(space.get()), 0);
  EXPECT_LT(H5Trefresh(transient.get()), 0);

  // H5Topen2 & H5Topen_async errors (missing link and wrong object type).
  EXPECT_EQ(H5Topen2(file.get(), nullptr, H5P_DEFAULT), H5I_INVALID_HID);
  EXPECT_EQ(H5Topen2(file.get(), "", H5P_DEFAULT), H5I_INVALID_HID);
  EXPECT_EQ(H5Topen2(file.get(), "/already", dcpl.get()), H5I_INVALID_HID);
  EXPECT_EQ(H5Topen2(file.get(), "/nonexistent", H5P_DEFAULT), H5I_INVALID_HID);
  EXPECT_EQ(H5Topen2(file.get(), "/grp", H5P_DEFAULT), H5I_INVALID_HID);
  EXPECT_EQ(
      H5Topen_async(file.get(), "/grp", H5P_DEFAULT, H5ES_NONE),
      H5I_INVALID_HID);
}

struct MockFileObj {
  int id = 1;
};

struct MockDtObj {
  int id = 1;
};

struct MockReqObj {
  int id = 1;
};

struct MockVolState {
  std::vector<uint8_t> encoded_i32;
  bool fail_get_binary_size = false;
  bool fail_get_binary = false;
  bool corrupt_binary = false;
  bool fail_get_tcpl = false;
  bool fail_flush = false;
  bool fail_refresh = false;
  bool fail_close_once = false;
  bool return_null_get_object = false;
  int open_files = 0;
  int open_dts = 0;
  std::vector<MockReqObj*> active_reqs;
};

MockVolState* g_mock_vol_state = nullptr;
const H5VL_class_t* g_mock_vol_cls_ptr = nullptr;

void* MockWrapGetObject(const void* obj) {
  if (g_mock_vol_state != nullptr && g_mock_vol_state->return_null_get_object) {
    return nullptr;
  }
  return const_cast<void*>(obj);
}

void* MockFileCreate(const char* /*name*/, unsigned /*flags*/,
                     hid_t /*fcpl_id*/, hid_t /*fapl_id*/,
                     hid_t /*dxpl_id*/, void** /*req*/) {
  ++g_mock_vol_state->open_files;
  return new MockFileObj{};
}

herr_t MockFileClose(void* file, hid_t /*dxpl_id*/, void** /*req*/) {
  --g_mock_vol_state->open_files;
  delete static_cast<MockFileObj*>(file);
  return 0;
}

herr_t MockIntrospectGetConnCls(void* /*obj*/, H5VL_get_conn_lvl_t /*lvl*/,
                                const H5VL_class_t** conn_cls) {
  *conn_cls = g_mock_vol_cls_ptr;
  return 0;
}

herr_t MockIntrospectGetCapFlags(const void* /*info*/, uint64_t* cap_flags) {
  *cap_flags = g_mock_vol_cls_ptr->cap_flags;
  return 0;
}

herr_t MockIntrospectOptQuery(void* /*obj*/, H5VL_subclass_t /*cls*/,
                              int /*opt_type*/, uint64_t* flags) {
  *flags = 0;
  return 0;
}

herr_t MockReqWait(void* /*req*/, uint64_t /*timeout*/,
                   H5VL_request_status_t* status) {
  *status = H5VL_REQUEST_STATUS_SUCCEED;
  return 0;
}

herr_t MockReqFree(void* req) {
  auto* r = static_cast<MockReqObj*>(req);
  if (g_mock_vol_state != nullptr && r != nullptr) {
    auto& vec = g_mock_vol_state->active_reqs;
    vec.erase(std::remove(vec.begin(), vec.end(), r), vec.end());
  }
  delete r;
  return 0;
}

void* MockDatatypeCommit(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                         const char* /*name*/, hid_t /*type_id*/,
                         hid_t /*lcpl_id*/, hid_t /*tcpl_id*/,
                         hid_t /*tapl_id*/, hid_t /*dxpl_id*/, void** req) {
  if (req != nullptr && req != H5_REQUEST_NULL) {
    auto* r = new MockReqObj{};
    g_mock_vol_state->active_reqs.push_back(r);
    *req = r;
  }
  ++g_mock_vol_state->open_dts;
  return new MockDtObj{};
}

void* MockDatatypeOpen(void* /*obj*/, const H5VL_loc_params_t* /*loc_params*/,
                       const char* /*name*/, hid_t /*tapl_id*/,
                       hid_t /*dxpl_id*/, void** req) {
  if (req != nullptr && req != H5_REQUEST_NULL) {
    auto* r = new MockReqObj{};
    g_mock_vol_state->active_reqs.push_back(r);
    *req = r;
  }
  ++g_mock_vol_state->open_dts;
  return new MockDtObj{};
}

herr_t MockDatatypeGet(void* /*obj*/, H5VL_datatype_get_args_t* args,
                       hid_t /*dxpl_id*/, void** /*req*/) {
  switch (args->op_type) {
    case H5VL_DATATYPE_GET_BINARY_SIZE:
      if (g_mock_vol_state->fail_get_binary_size) {
        return -1;
      }
      *args->args.get_binary_size.size =
          g_mock_vol_state->corrupt_binary
              ? 4
              : g_mock_vol_state->encoded_i32.size();
      return 0;
    case H5VL_DATATYPE_GET_BINARY:
      if (g_mock_vol_state->fail_get_binary) {
        return -1;
      }
      if (g_mock_vol_state->corrupt_binary) {
        static constexpr uint8_t kBadBytes[4] = {0xff, 0xff, 0x00, 0x00};
        if (args->args.get_binary.buf_size < sizeof(kBadBytes)) {
          return -1;
        }
        std::memcpy(args->args.get_binary.buf, kBadBytes, sizeof(kBadBytes));
        return 0;
      }
      if (args->args.get_binary.buf_size <
          g_mock_vol_state->encoded_i32.size()) {
        return -1;
      }
      std::memcpy(args->args.get_binary.buf,
                  g_mock_vol_state->encoded_i32.data(),
                  g_mock_vol_state->encoded_i32.size());
      return 0;
    case H5VL_DATATYPE_GET_TCPL:
      if (g_mock_vol_state->fail_get_tcpl) {
        return -1;
      }
      args->args.get_tcpl.tcpl_id = H5Pcreate(H5P_DATATYPE_CREATE);
      return args->args.get_tcpl.tcpl_id >= 0 ? 0 : -1;
  }
  return -1;
}

herr_t MockDatatypeSpecific(void* /*obj*/,
                            H5VL_datatype_specific_args_t* args,
                            hid_t /*dxpl_id*/, void** /*req*/) {
  switch (args->op_type) {
    case H5VL_DATATYPE_FLUSH:
      return g_mock_vol_state->fail_flush ? -1 : 0;
    case H5VL_DATATYPE_REFRESH:
      return g_mock_vol_state->fail_refresh ? -1 : 0;
  }
  return -1;
}

herr_t MockDatatypeClose(void* dt, hid_t /*dxpl_id*/, void** /*req*/) {
  --g_mock_vol_state->open_dts;
  delete static_cast<MockDtObj*>(dt);
  if (g_mock_vol_state->fail_close_once) {
    g_mock_vol_state->fail_close_once = false;
    return -1;
  }
  return 0;
}

const H5VL_class_t kMockVolClass = {
    .version = H5VL_VERSION,
    .value = static_cast<H5VL_class_value_t>(501),
    .name = "mock_h5tcommit_vol",
    .conn_version = 1,
    .cap_flags = H5VL_CAP_FLAG_ASYNC | H5VL_CAP_FLAG_STORED_DATATYPES |
                 H5VL_CAP_FLAG_GET_PLIST | H5VL_CAP_FLAG_FLUSH_REFRESH,
    .wrap_cls =
        {
            .get_object = MockWrapGetObject,
        },
    .datatype_cls =
        {
            .commit = MockDatatypeCommit,
            .open = MockDatatypeOpen,
            .get = MockDatatypeGet,
            .specific = MockDatatypeSpecific,
            .close = MockDatatypeClose,
        },
    .file_cls =
        {
            .create = MockFileCreate,
            .close = MockFileClose,
        },
    .introspect_cls =
        {
            .get_conn_cls = MockIntrospectGetConnCls,
            .get_cap_flags = MockIntrospectGetCapFlags,
            .opt_query = MockIntrospectOptQuery,
        },
    .request_cls =
        {
            .wait = MockReqWait,
            .free = MockReqFree,
        },
};

int FailingEsInsertCb(const H5ES_op_info_t* /*op_info*/, void* /*ctx*/) {
  return -1;
}

constexpr H5Z_filter_t kFailFilterId = 310;

size_t FailFilterFunc(unsigned int flags, size_t /*cd_nelmts*/,
                      const unsigned int /*cd_values*/[], size_t /*nbytes*/,
                      size_t* /*buf_size*/, void** buf) {
  if ((flags & H5Z_FLAG_REVERSE) == 0 && buf != nullptr && *buf != nullptr) {
    // H5HF__huge_insert() allocates write_buf via H5MM_malloc() (malloc)
    // and leaves it allocated if H5Z_pipeline() fails without a filter_cb.
    std::free(*buf);
    *buf = nullptr;
  }
  return 0;
}

const H5Z_class2_t kFailFilterClass = {
    .version = H5Z_CLASS_T_VERS,
    .id = kFailFilterId,
    .encoder_present = 1,
    .decoder_present = 1,
    .name = "h5tcommit_fail_filter",
    .can_apply = nullptr,
    .set_local = nullptr,
    .filter = FailFilterFunc,
};

TEST(H5TcommitTest, MockVolConnectorLifecycleAndErrorPaths) {
  MockVolState state;
  size_t encoded_len = 0;
  ASSERT_GE(H5Tencode(H5T_NATIVE_INT32, nullptr, &encoded_len), 0);
  state.encoded_i32.resize(encoded_len);
  ASSERT_GE(
      H5Tencode(H5T_NATIVE_INT32, state.encoded_i32.data(), &encoded_len), 0);

  g_mock_vol_state = &state;
  g_mock_vol_cls_ptr = &kMockVolClass;

  hid_t vol_id = H5VLregister_connector(&kMockVolClass, H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  absl::Cleanup cleanup_vol = [&state, vol_id]() {
    for (MockReqObj* req : state.active_reqs) {
      delete req;
    }
    state.active_reqs.clear();
    EXPECT_GE(H5VLunregister_connector(vol_id), 0);
    EXPECT_EQ(state.open_files, 0);
    EXPECT_EQ(state.open_dts, 0);
    g_mock_vol_state = nullptr;
    g_mock_vol_cls_ptr = nullptr;
  };

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_vol(fapl.get(), vol_id, nullptr), 0);

  ScopedFile mock_file(H5Fcreate("mock.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                 fapl.get()));
  ASSERT_TRUE(mock_file.valid());

  // Successful open via mock VOL (validates H5T_construct_datatype success).
  ScopedType opened_ok(H5Topen2(mock_file.get(), "/mock_ok", H5P_DEFAULT));
  ASSERT_TRUE(opened_ok.valid());
  EXPECT_EQ(H5Tget_class(opened_ok.get()), H5T_INTEGER);

  ScopedErrorSilence silence;

  // Fail H5T_construct_datatype in three ways, exercising H5VL_register failure
  // and H5T_destruct_datatype cleanup (including close failure on last one).
  state.fail_get_binary_size = true;
  EXPECT_EQ(H5Topen2(mock_file.get(), "/fail_sz", H5P_DEFAULT),
            H5I_INVALID_HID);
  state.fail_get_binary_size = false;

  state.fail_get_binary = true;
  EXPECT_EQ(H5Topen2(mock_file.get(), "/fail_bin", H5P_DEFAULT),
            H5I_INVALID_HID);
  state.fail_get_binary = false;

  state.corrupt_binary = true;
  state.fail_close_once = true;
  EXPECT_EQ(H5Topen2(mock_file.get(), "/corrupt", H5P_DEFAULT),
            H5I_INVALID_HID);
  state.corrupt_binary = false;
  state.fail_close_once = false;

  // VOL failure paths for H5Tget_create_plist, H5Tflush, and H5Trefresh.
  state.fail_get_tcpl = true;
  EXPECT_EQ(H5Tget_create_plist(opened_ok.get()), H5I_INVALID_HID);
  state.fail_get_tcpl = false;

  state.fail_flush = true;
  EXPECT_LT(H5Tflush(opened_ok.get()), 0);
  state.fail_flush = false;

  state.fail_refresh = true;
  EXPECT_LT(H5Trefresh(opened_ok.get()), 0);
  state.fail_refresh = false;

  // H5Tcommit_async & H5Topen_async fail when H5ES_insert fails.
  ScopedEventSet fail_es(H5EScreate());
  ASSERT_TRUE(fail_es.valid());
  ASSERT_GE(H5ESregister_insert_func(fail_es.get(), FailingEsInsertCb, nullptr),
            0);

  ScopedType async_commit_dt(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(async_commit_dt.valid());
  EXPECT_LT(H5Tcommit_async(mock_file.get(), "/es_fail_c",
                            async_commit_dt.get(), H5P_DEFAULT, H5P_DEFAULT,
                            H5P_DEFAULT, fail_es.get()),
            0);

  EXPECT_EQ(H5Topen_async(mock_file.get(), "/es_fail_o", H5P_DEFAULT,
                          fail_es.get()),
            H5I_INVALID_HID);

  // H5T_save_refresh_state & H5T_restore_refresh_state fail if
  // wrap_cls.get_object returns nullptr (H5T_get_actual_type == NULL).
  H5O_shared_t dummy_sh{};
  state.return_null_get_object = true;
  EXPECT_LT(H5T_save_refresh_state(opened_ok.get(), &dummy_sh), 0);
  EXPECT_LT(H5T_restore_refresh_state(opened_ok.get(), &dummy_sh), 0);
  state.return_null_get_object = false;
}

TEST(H5TcommitTest, LargeDatatypeAndCommitNamedRevertErrorPaths) {
  ASSERT_GE(H5Zregister(&kFailFilterClass), 0);
  absl::Cleanup unregister_filter = []() {
    (void)H5Zunregister(kFailFilterId);
  };

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  // 1) Over-64KB datatype exceeds max object header message size in H5T__commit.
  constexpr int kNumMembers = 3500;
  ScopedType huge_comp(H5Tcreate(H5T_COMPOUND, kNumMembers * sizeof(int32_t)));
  ASSERT_TRUE(huge_comp.valid());
  for (int i = 0; i < kNumMembers; ++i) {
    std::string field = absl::StrFormat("f%04d", i);
    ASSERT_GE(H5Tinsert(huge_comp.get(), field.c_str(), i * sizeof(int32_t),
                        H5T_NATIVE_INT32),
              0);
  }

  // 2) Dense group with mandatory failing filter triggers H5G__dense_insert
  //    failure on huge (>4KB) link name after H5O_obj_create() succeeds,
  //    hitting H5T__commit_named() full revert cleanup.
  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(H5Pset_link_phase_change(gcpl.get(), 0, 0), 0);
  ASSERT_GE(
      H5Pset_filter(gcpl.get(), kFailFilterId, H5Z_FLAG_MANDATORY, 0, nullptr),
      0);

  ScopedGroup filtered_grp(H5Gcreate2(file.get(), "/filtered_grp", H5P_DEFAULT,
                                      gcpl.get(), H5P_DEFAULT));
  ASSERT_TRUE(filtered_grp.valid());

  ScopedType vlen_dt(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vlen_dt.valid());
  std::string huge_link_name = "/filtered_grp/" + std::string(5000, 'x');

  ScopedErrorSilence silence;
  EXPECT_LT(H5Tcommit2(file.get(), "/too_large_dt", huge_comp.get(),
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_EQ(H5Tcommitted(huge_comp.get()), 0);
  EXPECT_EQ(H5Lexists(file.get(), "/too_large_dt", H5P_DEFAULT), 0);

  EXPECT_LT(H5Tcommit2(file.get(), huge_link_name.c_str(), vlen_dt.get(),
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);
  EXPECT_EQ(H5Tcommitted(vlen_dt.get()), 0);
}

TEST(H5TcommitTest, InternalVolAndCommitHelpers) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  auto* f = static_cast<H5F_t*>(H5VL_object_verify(file.get(), H5I_FILE));
  ASSERT_NE(f, nullptr);

  ScopedGroup grp(H5Gcreate2(file.get(), "/grp", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedType transient_tid(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(transient_tid.valid());
  auto* transient_dt =
      static_cast<H5T_t*>(H5I_object_verify(transient_tid.get(), H5I_DATATYPE));
  ASSERT_NE(transient_dt, nullptr);

  ScopedType committed_tid(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(committed_tid.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/comm_i32", committed_tid.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);
  auto* committed_dt =
      static_cast<H5T_t*>(H5I_object_verify(committed_tid.get(), H5I_DATATYPE));
  ASSERT_NE(committed_dt, nullptr);

  // VOL introspection helpers on transient vs committed datatypes.
  EXPECT_EQ(H5T_get_named_type(transient_dt), nullptr);
  EXPECT_NE(H5T_get_named_type(committed_dt), nullptr);
  EXPECT_EQ(H5T_get_actual_type(transient_dt), transient_dt);
  H5T_t* actual_committed = H5T_get_actual_type(committed_dt);
  ASSERT_NE(actual_committed, nullptr);
  EXPECT_NE(actual_committed, committed_dt);
  EXPECT_FALSE(H5T_already_vol_managed(transient_dt));
  EXPECT_TRUE(H5T_already_vol_managed(committed_dt));

  ScopedErrorSilence silence;
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());

  // H5T_invoke_vol_optional rejects uncommitted and native-unsupported types cleanly.
  H5VL_optional_args_t opt_args{0, nullptr};
  EXPECT_LT(H5T_invoke_vol_optional(transient_dt, &opt_args, H5P_DEFAULT,
                                    nullptr, nullptr),
            0);
  EXPECT_LT(H5T_invoke_vol_optional(committed_dt, &opt_args, H5P_DEFAULT,
                                    nullptr, nullptr),
            0);

  // H5T__commit direct guard checks bypassed by native VOL wrapper.
  ScopedPlist tcpl(H5Pcreate(H5P_DATATYPE_CREATE));
  ASSERT_TRUE(tcpl.valid());
  EXPECT_LT(H5T__commit(f, actual_committed, tcpl.get()), 0);

  auto* immutable_dt = static_cast<H5T_t*>(
      H5I_object_verify(H5T_NATIVE_INT32, H5I_DATATYPE));
  ASSERT_NE(immutable_dt, nullptr);
  EXPECT_LT(H5T__commit(f, immutable_dt, tcpl.get()), 0);

  ScopedType empty_comp_tid(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(empty_comp_tid.valid());
  auto* empty_comp_dt = static_cast<H5T_t*>(
      H5I_object_verify(empty_comp_tid.get(), H5I_DATATYPE));
  ASSERT_NE(empty_comp_dt, nullptr);
  EXPECT_LT(H5T__commit(f, empty_comp_dt, tcpl.get()), 0);

  // H5T__commit H5T_set_loc failure: zero child VLEN size inside compound.
  {
    ScopedType vlen_tid(H5Tvlen_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(vlen_tid.valid());
    ScopedType comp_vlen_tid(H5Tcreate(H5T_COMPOUND, sizeof(hvl_t)));
    ASSERT_TRUE(comp_vlen_tid.valid());
    ASSERT_GE(
        H5Tinsert(comp_vlen_tid.get(), "v", 0, vlen_tid.get()), 0);
    auto* comp_vlen_dt = static_cast<H5T_t*>(
        H5I_object_verify(comp_vlen_tid.get(), H5I_DATATYPE));
    ASSERT_NE(comp_vlen_dt, nullptr);
    H5T_t* memb_dt = comp_vlen_dt->shared->u.compnd.memb[0].type;
    const size_t saved_memb_size = memb_dt->shared->size;
    memb_dt->shared->size = 0;
    EXPECT_LT(H5T__commit(f, comp_vlen_dt, tcpl.get()), 0);
    memb_dt->shared->size = saved_memb_size;
    EXPECT_GE(H5T_set_loc(comp_vlen_dt, nullptr, H5T_LOC_MEMORY), 0);
  }

  // H5T__commit H5O_create failure with non-OCPL plist (under -DNDEBUG).
  EXPECT_LT(H5T__commit(f, transient_dt, H5P_LINK_ACCESS_DEFAULT), 0);

  // H5T__commit error cleanup resets stale sh_loc.type when ohdr creation fails.
  {
    ScopedType unusual_i32(H5Tcopy(H5T_STD_I32LE));
    ASSERT_TRUE(unusual_i32.valid());
    ASSERT_GE(H5Tset_precision(unusual_i32.get(), 4), 0);
    auto* unusual_dt = static_cast<H5T_t*>(
        H5I_object_verify(unusual_i32.get(), H5I_DATATYPE));
    ASSERT_NE(unusual_dt, nullptr);
    unusual_dt->sh_loc.type = H5O_SHARE_TYPE_COMMITTED;
    EXPECT_LT(H5T__commit(f, unusual_dt, tcpl.get()), 0);
    EXPECT_EQ(unusual_dt->sh_loc.type, H5O_SHARE_TYPE_UNSHARED);
  }

  // H5T_link and H5T__get_create_plist failure with bogus oloc addr.
  {
    H5T_t fake_dt = *actual_committed;
    fake_dt.oloc.addr = 0xDEADBEEFULL;
    fake_dt.oloc.holding_file = false;
    EXPECT_LT(H5T_link(&fake_dt, 1), 0);
    EXPECT_EQ(H5T__get_create_plist(&fake_dt), H5I_INVALID_HID);
  }

  // H5Topen2 with invalid loc_id hits H5VL_setup_acc_args failure.
  EXPECT_EQ(H5Topen2(H5I_INVALID_HID, "/comm_i32", H5P_DEFAULT),
            H5I_INVALID_HID);

  // H5T_open failure on non-datatype object header (H5O_msg_read NULL).
  {
    H5G_loc_t grp_loc{};
    ASSERT_GE(H5G_loc(grp.get(), &grp_loc), 0);
    H5O_loc_t oloc_copy{};
    H5G_name_t path_copy{};
    ASSERT_GE(H5O_loc_reset(&oloc_copy), 0);
    ASSERT_GE(H5G_name_reset(&path_copy), 0);
    ASSERT_GE(H5O_loc_copy_deep(&oloc_copy, grp_loc.oloc), 0);
    ASSERT_GE(H5G_name_copy(&path_copy, grp_loc.path, H5_COPY_DEEP), 0);
    grp.reset();
    absl::Cleanup free_copy = [&]() {
      H5O_loc_free(&oloc_copy);
      H5G_name_free(&path_copy);
    };
    H5G_loc_t loc_copy{&oloc_copy, &path_copy};
    EXPECT_EQ(H5T_open(&loc_copy), nullptr);
  }

  // H5T_open shared_fo reuse with H5FO_top_count == 0 opens through top file
  // and closes cleanly afterward.
  {
    constexpr haddr_t kSharedAddr = 0x12345678ULL;
    ASSERT_GE(H5FO_insert(f, kSharedAddr, actual_committed->shared, false), 0);
    absl::Cleanup remove_shared_fo = [f]() {
      (void)H5FO_delete(f, kSharedAddr);
    };
    H5O_loc_t shared_oloc{};
    H5G_name_t shared_path{};
    ASSERT_GE(H5O_loc_reset(&shared_oloc), 0);
    ASSERT_GE(H5G_name_reset(&shared_path), 0);
    shared_oloc.file = f;
    shared_oloc.addr = kSharedAddr;
    H5G_loc_t shared_loc{&shared_oloc, &shared_path};
    H5T_t* opened_shared = H5T_open(&shared_loc);
    ASSERT_NE(opened_shared, nullptr);
    EXPECT_GE(H5T_close(opened_shared), 0);
  }

  // H5T_save_refresh_state and H5T_restore_refresh_state direct coverage.
  H5O_shared_t cached_sh{};
  EXPECT_LT(H5T_save_refresh_state(H5I_INVALID_HID, &cached_sh), 0);
  EXPECT_LT(H5T_restore_refresh_state(H5I_INVALID_HID, &cached_sh), 0);
  EXPECT_GE(H5T_save_refresh_state(committed_tid.get(), &cached_sh), 0);
  EXPECT_GE(H5T_restore_refresh_state(committed_tid.get(), &cached_sh), 0);

  // H5T_restore_refresh_state H5FO_top_decr failure, then restore sh_loc.
  {
    H5O_shared_t orig_sh = actual_committed->sh_loc;
    H5O_shared_t bad_sh = orig_sh;
    bad_sh.u.loc.oh_addr = 0xDEADBEEFULL;
    EXPECT_LT(H5T_restore_refresh_state(committed_tid.get(), &bad_sh), 0);
    std::memcpy(&actual_committed->sh_loc, &orig_sh, sizeof(orig_sh));
  }
}

TEST(H5TcommitTest, OpenSharedVlenFailureRefCommitAndUntypedObjErrors) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
  ASSERT_NE(f, nullptr);

  // H5T_open shared_fo path: H5T_set_loc(MEMORY) fails on zero-sized child VLEN
  // inside an already-open committed compound, covering cleanup of dt and
  // shared_fo->fo_count-- (H5Tcommit.c:996, 1084, 1112-1119).
  ScopedType vlen_tid(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vlen_tid.valid());
  ScopedType comp_vlen_tid(H5Tcreate(H5T_COMPOUND, sizeof(hvl_t)));
  ASSERT_TRUE(comp_vlen_tid.valid());
  ASSERT_GE(H5Tinsert(comp_vlen_tid.get(), "v", 0, vlen_tid.get()), 0);
  ASSERT_GE(H5Tcommit2(file.get(), "/comm_vlen_comp", comp_vlen_tid.get(),
                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);

  auto* comm_dt = static_cast<H5T_t*>(
      H5I_object_verify(comp_vlen_tid.get(), H5I_DATATYPE));
  ASSERT_NE(comm_dt, nullptr);
  H5T_t* actual_dt = H5T_get_actual_type(comm_dt);
  ASSERT_NE(actual_dt, nullptr);
  ASSERT_EQ(actual_dt->shared->fo_count, 1u);
  ASSERT_EQ(actual_dt->shared->type, H5T_COMPOUND);
  ASSERT_EQ(actual_dt->shared->u.compnd.nmembs, 1u);
  H5T_t* memb_dt = actual_dt->shared->u.compnd.memb[0].type;
  ASSERT_EQ(memb_dt->shared->type, H5T_VLEN);
  {
    ScopedErrorSilence silence;
    const size_t saved_size = memb_dt->shared->size;
    const H5T_loc_t saved_loc = memb_dt->shared->u.vlen.loc;
    absl::Cleanup restore_vlen = [&]() {
      memb_dt->shared->size = saved_size;
      memb_dt->shared->u.vlen.loc = saved_loc;
      if (actual_dt->shared->fo_count > 1) {
        actual_dt->shared->fo_count = 1;
      }
    };
    memb_dt->shared->u.vlen.loc = H5T_LOC_BADLOC;
    memb_dt->shared->size = 0;
    // H5T_open increments fo_count after H5T_set_loc succeeds, but its error
    // path unconditionally decrements shared_fo->fo_count.
    actual_dt->shared->fo_count = 2;
    EXPECT_EQ(H5Topen2(file.get(), "/comm_vlen_comp", H5P_DEFAULT),
              H5I_INVALID_HID);
    EXPECT_EQ(memb_dt->shared->size, sizeof(hvl_t));
    EXPECT_EQ(memb_dt->shared->u.vlen.loc, H5T_LOC_MEMORY);
    EXPECT_EQ(actual_dt->shared->fo_count, 1u);
    std::move(restore_vlen).Cancel();
  }

  // H5T__commit H5T_set_loc(MEMORY) failure after ohdr ownership transfer
  // (loc_init == false, H5Tcommit.c:517, 532) on non-opaque H5R_OBJECT2.
  ScopedPlist tcpl(H5Pcreate(H5P_DATATYPE_CREATE));
  ASSERT_TRUE(tcpl.valid());
  {
    ScopedType ref_tid(H5Tcopy(H5T_STD_REF));
    ASSERT_TRUE(ref_tid.valid());
    auto* ref_dt = static_cast<H5T_t*>(
        H5I_object_verify(ref_tid.get(), H5I_DATATYPE));
    ASSERT_NE(ref_dt, nullptr);
    ASSERT_EQ(ref_dt->shared->type, H5T_REFERENCE);
    ASSERT_TRUE(ref_dt->shared->u.atomic.u.r.opaque);

    ScopedErrorSilence silence;
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    ref_dt->shared->u.atomic.u.r.opaque = false;
    EXPECT_LT(H5T__commit(f, ref_dt, tcpl.get()), 0);

    // Clean up H5FO state (H5T__commit already deleted the ohdr from disk).
    const haddr_t ref_oh_addr = ref_dt->sh_loc.u.loc.oh_addr;
    if (H5_addr_defined(ref_oh_addr)) {
      (void)H5FO_mark(f, ref_oh_addr, false);
      (void)H5FO_top_decr(f, ref_oh_addr);
      (void)H5FO_delete(f, ref_oh_addr);
    }
    H5O_loc_reset(&ref_dt->oloc);
    H5G_name_reset(&ref_dt->path);
    ref_dt->shared->state = H5T_STATE_TRANSIENT;
    ref_dt->shared->fo_count = 0;
    ref_dt->sh_loc.type = H5O_SHARE_TYPE_UNSHARED;
    ref_dt->sh_loc.file = nullptr;
    ref_dt->shared->u.atomic.u.r.opaque = true;
    ref_dt->shared->u.atomic.u.r.loc = H5T_LOC_BADLOC;
    EXPECT_GE(H5T_set_loc(ref_dt, nullptr, H5T_LOC_MEMORY), 0);
  }

  // H5T__open_name H5O_obj_type failure on a linked untyped object header
  // (H5Tcommit.c:990).
  {
    ScopedFile untyped_file = CreateInMemoryFile("untyped.h5");
    ASSERT_TRUE(untyped_file.valid());
    auto* uf = static_cast<H5F_t*>(H5VL_object(untyped_file.get()));
    ASSERT_NE(uf, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5G_loc_t file_loc{};
    ASSERT_GE(H5G_loc(untyped_file.get(), &file_loc), 0);
    H5O_loc_t empty_oloc{};
    H5G_name_t empty_path{};
    ASSERT_GE(H5O_loc_reset(&empty_oloc), 0);
    ASSERT_GE(H5G_name_reset(&empty_path), 0);
    ASSERT_GE(H5O_create(uf, 64, 0, tcpl.get(), &empty_oloc), 0);
    H5G_loc_t empty_loc{&empty_oloc, &empty_path};
    const herr_t link_rc =
        H5L_link(&file_loc, "/untyped_obj", &empty_loc, H5P_DEFAULT);
    EXPECT_GE(H5O_close(&empty_oloc, nullptr), 0);
    H5G_name_free(&empty_path);
    ASSERT_GE(link_rc, 0);
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5Topen2(untyped_file.get(), "/untyped_obj", H5P_DEFAULT),
                H5I_INVALID_HID);
    }
  }
}

}  // namespace
}  // namespace hdf5
