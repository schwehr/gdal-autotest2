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
// Tests core HDF5 datatype lifecycle, creation, copying, locking, inspection,
// comparison, resize, binary encode/decode, conversion path registry/execution,
// VL memory reclamation, and internal H5T helpers (third_party/hdf5/src/H5T.c).
//
// Google3 build notes (third_party/hdf5/src/H5pubconf.h):
//   - Compiled on Linux (LP64) with -DNDEBUG, H5_HAVE__FLOAT16=1,
//     H5_HAVE_COMPLEX_NUMBERS=1, and deprecated symbols enabled.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5T.c

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which elides debug-only fields in some
// internal structs and undefines H5T_DEBUG in H5Tpkg.h. Define NDEBUG around
// the internal prelude and private/package headers to guarantee ABI alignment.
#ifndef NDEBUG
#define H5T_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#if defined(H5_HAVE__FLOAT16) && defined(__cplusplus)
// H5private.h omits H5__Float16 in C++, but H5Tpkg.h declares two extern vars
// using H5__Float16 whenever H5_HAVE__FLOAT16 is defined.
typedef _Float16 H5__Float16;
#endif
#define H5T_FRIEND
#define H5F_FRIEND
#define H5VL_FRIEND
#include "third_party/hdf5/src/H5Tpkg.h"
#include "third_party/hdf5/src/H5Tconv.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FOprivate.h"
// H5Fpkg.h uses `try` as a parameter name in a prototype (`H5F__efc_open`).
#pragma push_macro("try")
#undef try
#define try h5_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#undef try
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5VLpkg.h"
}  // extern "C"
#ifdef H5T_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5T_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

struct ConvTracker {
  int init_calls = 0;
  int conv_calls = 0;
  int free_calls = 0;
  bool fail_init = false;
  bool saw_bkg = false;
  bool use_priv = false;
};

ConvTracker g_conv_tracker_a;
ConvTracker g_conv_tracker_b;
ConvTracker g_soft_tracker;

herr_t TrackedCustomConvA(hid_t /*src_id*/, hid_t /*dst_id*/, H5T_cdata_t* cdata,
                          size_t nelmts, size_t /*buf_stride*/,
                          size_t /*bkg_stride*/, void* buf, void* bkg,
                          hid_t /*dxpl_id*/) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      ++g_conv_tracker_a.init_calls;
      if (g_conv_tracker_a.fail_init) {
        return -1;
      }
      cdata->need_bkg = H5T_BKG_YES;
      if (g_conv_tracker_a.use_priv && cdata->priv == nullptr) {
        cdata->priv = std::malloc(sizeof(int));
        if (cdata->priv == nullptr) {
          return -1;
        }
        *static_cast<int*>(cdata->priv) = 42;
      }
      return 0;
    case H5T_CONV_CONV: {
      ++g_conv_tracker_a.conv_calls;
      if (bkg != nullptr) {
        g_conv_tracker_a.saw_bkg = true;
      }
      auto* bytes = static_cast<uint8_t*>(buf);
      for (size_t i = 0; i < nelmts; ++i) {
        bytes[i * 4] ^= 0x55;
      }
      return 0;
    }
    case H5T_CONV_FREE:
      ++g_conv_tracker_a.free_calls;
      if (cdata->priv != nullptr) {
        std::free(cdata->priv);
        cdata->priv = nullptr;
      }
      return 0;
    default:
      return -1;
  }
}

herr_t TrackedCustomConvB(hid_t /*src_id*/, hid_t /*dst_id*/, H5T_cdata_t* cdata,
                          size_t nelmts, size_t /*buf_stride*/,
                          size_t /*bkg_stride*/, void* buf, void* /*bkg*/,
                          hid_t /*dxpl_id*/) {
  switch (cdata->command) {
    case H5T_CONV_INIT:
      ++g_conv_tracker_b.init_calls;
      cdata->need_bkg = H5T_BKG_NO;
      return 0;
    case H5T_CONV_CONV: {
      ++g_conv_tracker_b.conv_calls;
      auto* bytes = static_cast<uint8_t*>(buf);
      for (size_t i = 0; i < nelmts; ++i) {
        bytes[i * 4] ^= 0xAA;
      }
      return 0;
    }
    case H5T_CONV_FREE:
      ++g_conv_tracker_b.free_calls;
      return 0;
    default:
      return -1;
  }
}

herr_t SelectiveSoftConv(hid_t src_id, hid_t dst_id, H5T_cdata_t* cdata,
                         size_t nelmts, size_t /*buf_stride*/,
                         size_t /*bkg_stride*/, void* buf, void* /*bkg*/,
                         hid_t /*dxpl_id*/) {
  switch (cdata->command) {
    case H5T_CONV_INIT: {
      ++g_soft_tracker.init_calls;
      // Only handle opaque types whose tag starts with "soft_accept", allowing
      // us to exercise both successful soft replacement and INIT rejection on
      // existing path table entries.
      char* stag = H5Tget_tag(src_id);
      char* dtag = H5Tget_tag(dst_id);
      absl::Cleanup free_tags = [&] {
        H5free_memory(stag);
        H5free_memory(dtag);
      };
      if (stag == nullptr || dtag == nullptr) {
        return -1;
      }
      if (std::string_view(stag).starts_with("soft_accept") &&
          std::string_view(dtag).starts_with("soft_accept")) {
        cdata->need_bkg = H5T_BKG_NO;
        return 0;
      }
      return -1;
    }
    case H5T_CONV_CONV: {
      ++g_soft_tracker.conv_calls;
      auto* bytes = static_cast<uint8_t*>(buf);
      for (size_t i = 0; i < nelmts; ++i) {
        bytes[i * 4] ^= 0x0F;
      }
      return 0;
    }
    case H5T_CONV_FREE:
      ++g_soft_tracker.free_calls;
      return 0;
    default:
      return -1;
  }
}

herr_t AlwaysFailSoftConv(hid_t /*src_id*/, hid_t /*dst_id*/,
                          H5T_cdata_t* cdata, size_t /*nelmts*/,
                          size_t /*buf_stride*/, size_t /*bkg_stride*/,
                          void* /*buf*/, void* /*bkg*/, hid_t /*dxpl_id*/) {
  if (cdata->command == H5T_CONV_INIT) {
    return -1;
  }
  return 0;
}

TEST(H5TTest, InternalInitAndCloseReopenCycle) {
  ASSERT_GE(H5open(), 0);
  EXPECT_GE(H5T_init(), 0);

  // Trigger at least one conversion and inspect the path table before closing.
  int32_t val = 7;
  uint8_t buf[sizeof(double)]{};
  std::memcpy(buf, &val, sizeof(val));
  ASSERT_GE(H5Tconvert(H5T_NATIVE_INT32, H5T_NATIVE_DOUBLE, 1, buf, nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_GT(H5T__get_path_table_npaths(), 1);

  // Close the library to exercise H5T_top_term_package(), H5T__unlock_cb(),
  // and H5T_term_package(), then immediately reopen it for subsequent tests.
  ASSERT_GE(H5close(), 0);
  EXPECT_EQ(H5T_NATIVE_INT_g, H5I_INVALID_HID);
  EXPECT_EQ(H5T_IEEE_F32BE_g, H5I_INVALID_HID);

  ASSERT_GE(H5open(), 0);
  EXPECT_NE(H5T_NATIVE_INT_g, H5I_INVALID_HID);
  EXPECT_NE(H5T_IEEE_F32BE_g, H5I_INVALID_HID);
  EXPECT_NE(H5T_NATIVE_FLOAT16_g, H5I_INVALID_HID);
  EXPECT_NE(H5T_NATIVE_FLOAT_COMPLEX_g, H5I_INVALID_HID);
  EXPECT_TRUE(std::isinf(H5T_NATIVE_FLOAT_POS_INF_g));
  EXPECT_TRUE(std::isinf(H5T_NATIVE_FLOAT_NEG_INF_g));
  EXPECT_TRUE(std::isinf(H5T_NATIVE_DOUBLE_POS_INF_g));
  EXPECT_TRUE(std::isinf(H5T_NATIVE_DOUBLE_NEG_INF_g));
  EXPECT_FALSE(std::signbit(H5T_NATIVE_LDOUBLE_POS_INF_g));
  EXPECT_TRUE(std::signbit(H5T_NATIVE_LDOUBLE_NEG_INF_g));
  EXPECT_FALSE(std::isfinite(H5T_NATIVE_LDOUBLE_POS_INF_g));
  EXPECT_FALSE(std::isfinite(H5T_NATIVE_LDOUBLE_NEG_INF_g));
  EXPECT_EQ(H5Tget_class(H5T_NATIVE_INT), H5T_INTEGER);

  // Re-initialize only the H5T package with H5P_LST_DATATYPE_CREATE_ID_g
  // cleared so H5T__init_package() exercises its fallback plist registration.
  while (H5T_top_term_package() > 0) {
  }
  while (H5T_term_package() > 0) {
  }
  ASSERT_GE(H5I_dec_ref(H5P_LST_DATATYPE_CREATE_ID_g), 0);
  H5P_LST_DATATYPE_CREATE_ID_g = H5I_INVALID_HID;
  ASSERT_GE(H5T_init(), 0);
  EXPECT_NE(H5P_LST_DATATYPE_CREATE_ID_g, H5I_INVALID_HID);
  EXPECT_EQ(H5Tget_class(H5T_NATIVE_INT), H5T_INTEGER);
}

TEST(H5TTest, CreateSupportedAndUnsupportedClasses) {
  struct CreateCase {
    H5T_class_t cls;
    size_t size;
  };
  const std::vector<CreateCase> kValid = {
      {H5T_INTEGER, 4},
      {H5T_FLOAT, 4},
      {H5T_TIME, 8},
      {H5T_STRING, 16},
      {H5T_STRING, H5T_VARIABLE},
      {H5T_OPAQUE, 8},
      {H5T_COMPOUND, 16},
      {H5T_ENUM, sizeof(char)},
      {H5T_ENUM, sizeof(short)},
      {H5T_ENUM, sizeof(int)},
      {H5T_ENUM, sizeof(long)},
  };

  for (const auto& tc : kValid) {
    ScopedType dt(H5Tcreate(tc.cls, tc.size));
    ASSERT_TRUE(dt.valid()) << "cls=" << tc.cls << " size=" << tc.size;
    EXPECT_EQ(H5Tget_class(dt.get()), tc.cls);
    if (tc.cls == H5T_STRING && tc.size == H5T_VARIABLE) {
      EXPECT_GT(H5Tis_variable_str(dt.get()), 0);
    } else {
      EXPECT_EQ(H5Tget_size(dt.get()), tc.size);
    }
    if (tc.cls == H5T_OPAQUE) {
      char* tag = H5Tget_tag(dt.get());
      ASSERT_NE(tag, nullptr);
      absl::Cleanup free_tag = [&] { H5free_memory(tag); };
      EXPECT_STREQ(tag, "");
    }
  }

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Tcreate(H5T_INTEGER, 0), H5I_INVALID_HID);
  EXPECT_EQ(H5Tcreate(H5T_ENUM, 3), H5I_INVALID_HID);
  EXPECT_EQ(H5Tcreate(H5T_BITFIELD, 4), H5I_INVALID_HID);
  EXPECT_EQ(H5Tcreate(H5T_VLEN, 4), H5I_INVALID_HID);
  EXPECT_EQ(H5Tcreate(H5T_ARRAY, 4), H5I_INVALID_HID);
  EXPECT_EQ(H5Tcreate(H5T_COMPLEX, 8), H5I_INVALID_HID);
  EXPECT_EQ(H5Tcreate(H5T_REFERENCE, 8), H5I_INVALID_HID);
  EXPECT_EQ(H5Tcreate(H5T_NO_CLASS, 4), H5I_INVALID_HID);
  EXPECT_EQ(H5Tcreate(H5T_NCLASSES, 4), H5I_INVALID_HID);
}

TEST(H5TTest, CopyDatatypeDatasetCommittedAndErrors) {
  // 1. Copy predefined and transient datatypes.
  ScopedType copy_int(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(copy_int.valid());
  EXPECT_GT(H5Tequal(copy_int.get(), H5T_NATIVE_INT), 0);
  EXPECT_GE(H5Tset_size(copy_int.get(), 8), 0);

  // 2. Copy datatype from a dataset (and from a committed datatype) in memory.
  ScopedFile file = CreateInMemoryFile("t_copy.h5");
  ASSERT_TRUE(file.valid());

  ScopedType committed(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(committed.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "committed_i32", committed.get(), H5P_DEFAULT,
                      H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedType copy_committed(H5Tcopy(committed.get()));
  ASSERT_TRUE(copy_committed.valid());
  EXPECT_EQ(H5Tcommitted(copy_committed.get()), 0);
  EXPECT_GT(H5Tequal(copy_committed.get(), H5T_STD_I32LE), 0);

  constexpr hsize_t kDim = 2;
  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "ds", committed.get(), space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  ScopedType from_dset(H5Tcopy(dset.get()));
  ASSERT_TRUE(from_dset.valid());
  EXPECT_GT(H5Tequal(from_dset.get(), H5T_STD_I32LE), 0);
  EXPECT_GE(H5Tset_size(from_dset.get(), 4), 0);

  // 3. Compound with committed member exercises H5T_copy_reopen via member query.
  ScopedType cmp(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(cmp.valid());
  ASSERT_GE(H5Tinsert(cmp.get(), "a", 0, committed.get()), 0);
  ASSERT_GE(H5Tinsert(cmp.get(), "b", 4, H5T_NATIVE_FLOAT), 0);
  ScopedType memb0(H5Tget_member_type(cmp.get(), 0));
  ASSERT_TRUE(memb0.valid());
  EXPECT_GT(H5Tequal(memb0.get(), H5T_STD_I32LE), 0);

  // 4. Error paths for H5Tcopy and internal H5T_copy / H5T_copy_reopen.
  ScopedPlist plist(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(plist.valid());
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Tcopy(H5I_INVALID_HID), H5I_INVALID_HID);
    EXPECT_EQ(H5Tcopy(file.get()), H5I_INVALID_HID);
    EXPECT_EQ(H5Tcopy(space.get()), H5I_INVALID_HID);
    EXPECT_EQ(H5Tcopy(plist.get()), H5I_INVALID_HID);

    auto* nat_dt = static_cast<H5T_t*>(H5I_object_verify(H5T_NATIVE_INT, H5I_DATATYPE));
    ASSERT_NE(nat_dt, nullptr);
    EXPECT_EQ(H5T_copy(nat_dt, static_cast<H5T_copy_t>(99)), nullptr);
    H5T_t* rdonly = H5T_copy(nat_dt, H5T_COPY_ALL);
    ASSERT_NE(rdonly, nullptr);
    EXPECT_EQ(H5T_is_immutable(rdonly), 0);
    EXPECT_GE(H5T_close(rdonly), 0);
  }
}

TEST(H5TTest, CopyReopenHandlesClosedAndAlreadyOpenNamedTypes) {
  ScopedFile file = CreateInMemoryFile("t_reopen.h5");
  ASSERT_TRUE(file.valid());

  ScopedType committed(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(committed.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "mytype", committed.get(), H5P_DEFAULT,
                      H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Build a dataset using the committed type, then close handles and reopen
  // file so the dataset references an unopened committed type in the file.
  constexpr hsize_t kDim = 1;
  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "d", committed.get(), space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  // Extract the VOL-wrapped H5T_t* (which holds H5T_STATE_OPEN and
  // H5O_SHARE_TYPE_COMMITTED) and exercise H5T_copy_reopen on an already-open
  // committed type (reopened_fo != NULL branch).
  auto* outer_dt =
      static_cast<H5T_t*>(H5I_object_verify(committed.get(), H5I_DATATYPE));
  ASSERT_NE(outer_dt, nullptr);
  ASSERT_NE(outer_dt->vol_obj, nullptr);
  auto* vol_dt = static_cast<H5T_t*>(H5VL_object_data(outer_dt->vol_obj));
  ASSERT_NE(vol_dt, nullptr);
  EXPECT_EQ(vol_dt->shared->state, H5T_STATE_OPEN);

  H5T_t* reopened1 = H5T_copy_reopen(vol_dt);
  ASSERT_NE(reopened1, nullptr);
  EXPECT_EQ(reopened1->shared->state, H5T_STATE_OPEN);
  EXPECT_GE(H5T_close(reopened1), 0);

  // Keep a H5T_COPY_ALL snapshot (which degrades OPEN -> NAMED), close the
  // open handles so H5FO no longer tracks the object, then reopen via
  // H5T_copy_reopen (reopened_fo == NULL branch).
  H5T_t* named_copy = H5T_copy(vol_dt, H5T_COPY_ALL);
  ASSERT_NE(named_copy, nullptr);
  EXPECT_EQ(named_copy->shared->state, H5T_STATE_NAMED);
  committed.reset();
  dset.reset();
  H5T_t* reopened2 = H5T_copy_reopen(named_copy);
  ASSERT_NE(reopened2, nullptr);
  EXPECT_EQ(reopened2->shared->state, H5T_STATE_OPEN);
  EXPECT_GE(H5T_close(reopened2), 0);
  EXPECT_GE(H5T_close_real(named_copy), 0);
}

TEST(H5TTest, CloseCloseAsyncAndLock) {
  ScopedFile file = CreateInMemoryFile("t_close_async.h5");
  ASSERT_TRUE(file.valid());

  // 1. H5Tclose_async with H5ES_NONE on committed datatype.
  {
    ScopedType t1(H5Tcopy(H5T_NATIVE_INT));
    ASSERT_TRUE(t1.valid());
    ASSERT_GE(H5Tcommit2(file.get(), "t1", t1.get(), H5P_DEFAULT, H5P_DEFAULT,
                        H5P_DEFAULT),
              0);
    hid_t raw_id = t1.release();
    EXPECT_GE(H5Tclose_async(raw_id, H5ES_NONE), 0);
  }

  // 2. H5Tclose_async with a valid event set on committed datatype.
  {
    ScopedEventSet es(H5EScreate());
    ASSERT_TRUE(es.valid());
    ScopedType t2(H5Tcopy(H5T_NATIVE_DOUBLE));
    ASSERT_TRUE(t2.valid());
    ASSERT_GE(H5Tcommit2(file.get(), "t2", t2.get(), H5P_DEFAULT, H5P_DEFAULT,
                        H5P_DEFAULT),
              0);
    hid_t raw_id = t2.release();
    EXPECT_GE(H5Tclose_async(raw_id, es.get()), 0);
    size_t num_in_progress = 0;
    bool err_occurred = false;
    EXPECT_GE(H5ESwait(es.get(), H5ES_WAIT_FOREVER, &num_in_progress,
                       &err_occurred),
              0);
    EXPECT_FALSE(err_occurred);
  }

  // 3. Locking transient datatype prevents modification and public close.
  ScopedType transient(H5Tcreate(H5T_INTEGER, 4));
  ASSERT_TRUE(transient.valid());
  auto* dt_ptr = static_cast<H5T_t*>(H5I_object_verify(transient.get(), H5I_DATATYPE));
  ASSERT_NE(dt_ptr, nullptr);

  // Restore state to TRANSIENT on scope exit so ScopedType can close it cleanly
  // without leaving immutable handles behind across randomized test runs.
  absl::Cleanup unlock_transient = [dt_ptr] {
    dt_ptr->shared->state = H5T_STATE_TRANSIENT;
  };

  // Internal H5T_lock(immutable=false) produces RDONLY, then immutable=true
  // upgrades RDONLY -> IMMUTABLE.
  EXPECT_GE(H5T_lock(dt_ptr, false), 0);
  EXPECT_EQ(H5T_is_immutable(dt_ptr), 0);
  EXPECT_GE(H5Tlock(transient.get()), 0);
  EXPECT_GT(H5T_is_immutable(dt_ptr), 0);
  // Locking an already immutable datatype is a no-op success.
  EXPECT_GE(H5Tlock(transient.get()), 0);

  // 4. Error paths for H5Tclose, H5Tclose_async, and H5Tlock.
  ScopedType named_state_dt(H5Tcreate(H5T_INTEGER, 4));
  ASSERT_TRUE(named_state_dt.valid());
  auto* named_state_ptr =
      static_cast<H5T_t*>(H5I_object_verify(named_state_dt.get(), H5I_DATATYPE));
  ASSERT_NE(named_state_ptr, nullptr);
  absl::Cleanup restore_named_state = [named_state_ptr] {
    named_state_ptr->shared->state = H5T_STATE_TRANSIENT;
  };
  ScopedType plain_transient(H5Tcreate(H5T_INTEGER, 4));
  ASSERT_TRUE(plain_transient.valid());

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tclose(H5I_INVALID_HID), 0);
    EXPECT_LT(H5Tclose(H5T_NATIVE_INT), 0);
    EXPECT_LT(H5Tclose(transient.get()), 0);
    EXPECT_LT(H5Tset_size(transient.get(), 8), 0);

    EXPECT_LT(H5Tclose_async(H5I_INVALID_HID, H5ES_NONE), 0);
    EXPECT_LT(H5Tclose_async(H5T_NATIVE_INT, H5ES_NONE), 0);
    // Transient type has no VOL object.
    EXPECT_LT(H5Tclose_async(plain_transient.get(), H5ES_NONE), 0);

    EXPECT_LT(H5Tlock(H5I_INVALID_HID), 0);
    named_state_ptr->shared->state = H5T_STATE_NAMED;
    EXPECT_LT(H5Tlock(named_state_dt.get()), 0);
    named_state_ptr->shared->state = H5T_STATE_OPEN;
    EXPECT_LT(H5Tlock(named_state_dt.get()), 0);
    // Internal H5T_lock() is a no-op on already NAMED/OPEN/IMMUTABLE types.
    EXPECT_GE(H5T_lock(named_state_ptr, true), 0);
  }
}

TEST(H5TTest, ClassDetectClassVariableStrAndSuper) {
  ScopedType fixed_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(fixed_str.valid());
  ASSERT_GE(H5Tset_size(fixed_str.get(), 10), 0);

  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vl_str.valid());
  ASSERT_GE(H5Tset_size(vl_str.get(), H5T_VARIABLE), 0);

  ScopedType vlen_int(H5Tvlen_create(H5T_NATIVE_INT));
  ASSERT_TRUE(vlen_int.valid());

  constexpr hsize_t kDim = 3;
  ScopedType arr_flt(H5Tarray_create2(H5T_NATIVE_FLOAT, 1, &kDim));
  ASSERT_TRUE(arr_flt.valid());

  ScopedType enm(H5Tenum_create(H5T_NATIVE_INT));
  ASSERT_TRUE(enm.valid());

  ScopedType cplx(H5Tcomplex_create(H5T_NATIVE_DOUBLE));
  ASSERT_TRUE(cplx.valid());

  ScopedType inner_cmp(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(inner_cmp.valid());
  ASSERT_GE(H5Tinsert(inner_cmp.get(), "x", 0, H5T_NATIVE_DOUBLE), 0);

  ScopedType outer_cmp(H5Tcreate(H5T_COMPOUND, 8 + 16));
  ASSERT_TRUE(outer_cmp.valid());
  ASSERT_GE(H5Tinsert(outer_cmp.get(), "c", 0, inner_cmp.get()), 0);
  ASSERT_GE(H5Tinsert(outer_cmp.get(), "s", 8, vl_str.get()), 0);

  // Public class & variable-string checks.
  EXPECT_EQ(H5Tget_class(vl_str.get()), H5T_STRING);
  EXPECT_EQ(H5Tget_class(fixed_str.get()), H5T_STRING);
  EXPECT_GT(H5Tis_variable_str(vl_str.get()), 0);
  EXPECT_EQ(H5Tis_variable_str(fixed_str.get()), 0);
  EXPECT_EQ(H5Tis_variable_str(vlen_int.get()), 0);
  EXPECT_EQ(H5Tis_variable_str(H5T_NATIVE_INT), 0);

  // Internal H5T_get_class exposes VL representation when internal == true.
  auto* vl_str_ptr = static_cast<H5T_t*>(H5I_object_verify(vl_str.get(), H5I_DATATYPE));
  ASSERT_NE(vl_str_ptr, nullptr);
  EXPECT_EQ(H5T_get_class(vl_str_ptr, true), H5T_VLEN);
  EXPECT_EQ(H5T_get_class(vl_str_ptr, false), H5T_STRING);
  EXPECT_EQ(H5T_detect_class(vl_str_ptr, H5T_VLEN, false), 1);

  // H5Tdetect_class across all composite hierarchies.
  EXPECT_GT(H5Tdetect_class(vl_str.get(), H5T_STRING), 0);
  EXPECT_EQ(H5Tdetect_class(vl_str.get(), H5T_VLEN), 0);
  EXPECT_GT(H5Tdetect_class(vlen_int.get(), H5T_INTEGER), 0);
  EXPECT_EQ(H5Tdetect_class(vlen_int.get(), H5T_FLOAT), 0);
  EXPECT_GT(H5Tdetect_class(arr_flt.get(), H5T_FLOAT), 0);
  EXPECT_GT(H5Tdetect_class(enm.get(), H5T_INTEGER), 0);
  EXPECT_GT(H5Tdetect_class(cplx.get(), H5T_FLOAT), 0);
  EXPECT_GT(H5Tdetect_class(outer_cmp.get(), H5T_COMPOUND), 0);
  EXPECT_GT(H5Tdetect_class(outer_cmp.get(), H5T_FLOAT), 0);
  EXPECT_GT(H5Tdetect_class(outer_cmp.get(), H5T_STRING), 0);
  EXPECT_EQ(H5Tdetect_class(outer_cmp.get(), H5T_OPAQUE), 0);

  // H5Tget_super on derived types.
  {
    ScopedType sup(H5Tget_super(enm.get()));
    ASSERT_TRUE(sup.valid());
    EXPECT_GT(H5Tequal(sup.get(), H5T_NATIVE_INT), 0);
  }
  {
    ScopedType sup(H5Tget_super(arr_flt.get()));
    ASSERT_TRUE(sup.valid());
    EXPECT_GT(H5Tequal(sup.get(), H5T_NATIVE_FLOAT), 0);
  }
  {
    ScopedType sup(H5Tget_super(vlen_int.get()));
    ASSERT_TRUE(sup.valid());
    EXPECT_GT(H5Tequal(sup.get(), H5T_NATIVE_INT), 0);
  }
  {
    ScopedType sup(H5Tget_super(cplx.get()));
    ASSERT_TRUE(sup.valid());
    EXPECT_GT(H5Tequal(sup.get(), H5T_NATIVE_DOUBLE), 0);
  }
  {
    ScopedType sup(H5Tget_super(vl_str.get()));
    ASSERT_TRUE(sup.valid());
    EXPECT_GT(H5Tequal(sup.get(), H5T_NATIVE_UCHAR), 0);
  }

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Tget_class(H5I_INVALID_HID), H5T_NO_CLASS);
  EXPECT_LT(H5Tis_variable_str(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Tdetect_class(H5I_INVALID_HID, H5T_INTEGER), 0);
  EXPECT_LT(H5Tdetect_class(H5T_NATIVE_INT, H5T_NO_CLASS), 0);
  EXPECT_LT(H5Tdetect_class(H5T_NATIVE_INT, H5T_NCLASSES), 0);
  EXPECT_EQ(H5Tget_super(H5T_NATIVE_INT), H5I_INVALID_HID);
  EXPECT_EQ(H5Tget_super(H5I_INVALID_HID), H5I_INVALID_HID);
}

TEST(H5TTest, GetAndSetSizeForAtomicCompoundFloatStringAndErrors) {
  // 1. Atomic integer offset/precision shrinkage branches.
  ScopedType int_dt(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(int_dt.valid());
  ASSERT_GE(H5Tset_precision(int_dt.get(), 16), 0);
  ASSERT_GE(H5Tset_offset(int_dt.get(), 12), 0);
  // Shift offset down when offset + prec > 8 * new_size (12 + 16 = 28 > 24).
  EXPECT_GE(H5Tset_size(int_dt.get(), 3), 0);
  EXPECT_EQ(H5Tget_offset(int_dt.get()), 8);
  EXPECT_EQ(H5Tget_precision(int_dt.get()), 16);
  // Shrink prec and zero offset when prec > 8 * new_size (16 > 8).
  EXPECT_GE(H5Tset_size(int_dt.get(), 1), 0);
  EXPECT_EQ(H5Tget_offset(int_dt.get()), 0);
  EXPECT_EQ(H5Tget_precision(int_dt.get()), 8);

  // 2. Float resize checks sign/epos/mpos bounds before shrinking.
  ScopedType flt_dt(H5Tcopy(H5T_IEEE_F64LE));
  ASSERT_TRUE(flt_dt.valid());
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tset_size(flt_dt.get(), 4), 0);
  }
  ASSERT_GE(H5Tset_fields(flt_dt.get(), 31, 23, 8, 0, 23), 0);
  EXPECT_GE(H5Tset_size(flt_dt.get(), 4), 0);
  EXPECT_EQ(H5Tget_size(flt_dt.get()), 4);

  // 3. String resize and conversion to variable length (+ parent resize).
  ScopedType str_dt(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(str_dt.valid());
  ASSERT_GE(H5Tset_cset(str_dt.get(), H5T_CSET_UTF8), 0);
  EXPECT_GE(H5Tset_size(str_dt.get(), 24), 0);
  EXPECT_EQ(H5Tget_size(str_dt.get()), 24);
  EXPECT_EQ(H5Tget_precision(str_dt.get()), 24 * 8);
  EXPECT_GE(H5Tset_size(str_dt.get(), H5T_VARIABLE), 0);
  EXPECT_GT(H5Tis_variable_str(str_dt.get()), 0);
  EXPECT_EQ(H5Tget_cset(str_dt.get()), H5T_CSET_UTF8);
  // Resizing a VL string hits the dt->shared->parent branch in H5T__set_size().
  EXPECT_GE(H5Tset_size(str_dt.get(), 1), 0);

  // 4. Time, bitfield, opaque, and empty + populated compound resizing.
  ScopedType time_dt(H5Tcreate(H5T_TIME, 8));
  ASSERT_TRUE(time_dt.valid());
  EXPECT_GE(H5Tset_size(time_dt.get(), 4), 0);

  ScopedType bit_dt(H5Tcopy(H5T_STD_B32LE));
  ASSERT_TRUE(bit_dt.valid());
  EXPECT_GE(H5Tset_size(bit_dt.get(), 2), 0);

  ScopedType opq_dt(H5Tcreate(H5T_OPAQUE, 8));
  ASSERT_TRUE(opq_dt.valid());
  EXPECT_GE(H5Tset_size(opq_dt.get(), 12), 0);

  ScopedType cmp_empty(H5Tcreate(H5T_COMPOUND, 16));
  ASSERT_TRUE(cmp_empty.valid());
  EXPECT_GE(H5Tset_size(cmp_empty.get(), 8), 0);

  // Unpacked compound: members at offset 0 (4B) and offset 8 (4B), total 16B.
  ScopedType cmp(H5Tcreate(H5T_COMPOUND, 16));
  ASSERT_TRUE(cmp.valid());
  ASSERT_GE(H5Tinsert(cmp.get(), "a", 0, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(cmp.get(), "b", 8, H5T_NATIVE_INT32), 0);
  EXPECT_GE(H5Tset_size(cmp.get(), 20), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tset_size(cmp.get(), 10), 0);  // Cuts off member "b".
  }
  EXPECT_GE(H5Tset_size(cmp.get(), 12), 0);  // Valid shrink.

  // 5. Enum with members, array, ref, complex, and sequence vlen cannot resize.
  ScopedType enm(H5Tenum_create(H5T_NATIVE_INT));
  ASSERT_TRUE(enm.valid());
  int32_t zero = 0;
  ASSERT_GE(H5Tenum_insert(enm.get(), "ZERO", &zero), 0);
  constexpr hsize_t kDim = 2;
  ScopedType arr(H5Tarray_create2(H5T_NATIVE_INT, 1, &kDim));
  ASSERT_TRUE(arr.valid());
  ScopedType vlen(H5Tvlen_create(H5T_NATIVE_INT));
  ASSERT_TRUE(vlen.valid());
  ScopedType cplx(H5Tcomplex_create(H5T_NATIVE_FLOAT));
  ASSERT_TRUE(cplx.valid());

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Tget_size(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Tset_size(H5I_INVALID_HID, 4), 0);
  EXPECT_LT(H5Tset_size(H5T_NATIVE_INT, 4), 0);
  EXPECT_LT(H5Tset_size(int_dt.get(), 0), 0);
  EXPECT_LT(H5Tset_size(int_dt.get(), H5T_VARIABLE), 0);
  EXPECT_LT(H5Tset_size(enm.get(), 8), 0);
  EXPECT_LT(H5Tset_size(arr.get(), 16), 0);
  EXPECT_LT(H5Tset_size(vlen.get(), 16), 0);
  EXPECT_LT(H5Tset_size(cplx.get(), 16), 0);
  EXPECT_LT(H5Tset_size(H5T_STD_REF_OBJ, 8), 0);
  ScopedType ref_copy(H5Tcopy(H5T_STD_REF));
  ASSERT_TRUE(ref_copy.valid());
  EXPECT_LT(H5Tset_size(ref_copy.get(), 32), 0);
}

TEST(H5TTest, CompareCoversAllClassesAndFields) {
  auto cmp_ids = [](hid_t a, hid_t b, bool superset = false) {
    auto* p1 = static_cast<const H5T_t*>(H5I_object_verify(a, H5I_DATATYPE));
    auto* p2 = static_cast<const H5T_t*>(H5I_object_verify(b, H5I_DATATYPE));
    return H5T_cmp(p1, p2, superset);
  };

  // Equal pointer & error inputs on H5Tequal.
  EXPECT_GT(H5Tequal(H5T_NATIVE_INT, H5T_NATIVE_INT), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tequal(H5I_INVALID_HID, H5T_NATIVE_INT), 0);
    EXPECT_LT(H5Tequal(H5T_NATIVE_INT, H5I_INVALID_HID), 0);
  }

  // Class, size, and asymmetric parent presence differences.
  EXPECT_NE(cmp_ids(H5T_NATIVE_INT32, H5T_NATIVE_FLOAT), 0);
  EXPECT_LT(cmp_ids(H5T_STD_I16LE, H5T_STD_I32LE), 0);
  EXPECT_GT(cmp_ids(H5T_STD_I32LE, H5T_STD_I16LE), 0);
  {
    ScopedType e_p1(H5Tenum_create(H5T_NATIVE_INT32));
    ScopedType e_p2(H5Tenum_create(H5T_NATIVE_INT32));
    ASSERT_TRUE(e_p1.valid() && e_p2.valid());
    auto* ep1 = static_cast<H5T_t*>(H5I_object_verify(e_p1.get(), H5I_DATATYPE));
    auto* ep2 = static_cast<H5T_t*>(H5I_object_verify(e_p2.get(), H5I_DATATYPE));
    ASSERT_NE(ep1, nullptr);
    ASSERT_NE(ep2, nullptr);
    H5T_t* orig_parent = ep2->shared->parent;
    ep2->shared->parent = nullptr;
    absl::Cleanup restore_parent = [&] { ep2->shared->parent = orig_parent; };
    EXPECT_LT(H5T_cmp(ep1, ep2, false), 0);
    EXPECT_GT(H5T_cmp(ep2, ep1, false), 0);
  }

  // Compound: empty vs empty, different member count, unsorted multi-member on
  // both dt1 and dt2, member name/offset/size/type differences in both signs.
  ScopedType c_empty1(H5Tcreate(H5T_COMPOUND, 8));
  ScopedType c_empty2(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(c_empty1.valid() && c_empty2.valid());
  EXPECT_EQ(cmp_ids(c_empty1.get(), c_empty2.get()), 0);

  ScopedType c1(H5Tcreate(H5T_COMPOUND, 12));
  ScopedType c2(H5Tcreate(H5T_COMPOUND, 12));
  ASSERT_TRUE(c1.valid() && c2.valid());
  ASSERT_GE(H5Tinsert(c1.get(), "z", 4, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(c1.get(), "a", 0, H5T_NATIVE_FLOAT), 0);
  ASSERT_GE(H5Tinsert(c2.get(), "a", 0, H5T_NATIVE_FLOAT), 0);
  ASSERT_GE(H5Tinsert(c2.get(), "z", 4, H5T_NATIVE_INT32), 0);
  EXPECT_GT(H5Tequal(c1.get(), c2.get()), 0);
  // Putting unsorted c1 as dt2 exercises dt2's idx2 bubble-sort swap path.
  EXPECT_EQ(cmp_ids(c2.get(), c1.get()), 0);
  EXPECT_LT(cmp_ids(c_empty1.get(), c1.get()), 0);
  ASSERT_GE(H5Tset_size(c_empty1.get(), 12), 0);
  EXPECT_LT(cmp_ids(c_empty1.get(), c1.get()), 0);
  EXPECT_GT(cmp_ids(c1.get(), c_empty1.get()), 0);

  ScopedType c_diff_name(H5Tcreate(H5T_COMPOUND, 12));
  ScopedType c_diff_off(H5Tcreate(H5T_COMPOUND, 12));
  ScopedType c_diff_type(H5Tcreate(H5T_COMPOUND, 12));
  ASSERT_TRUE(c_diff_name.valid() && c_diff_off.valid() && c_diff_type.valid());
  ASSERT_GE(H5Tinsert(c_diff_name.get(), "b", 0, H5T_NATIVE_FLOAT), 0);
  ASSERT_GE(H5Tinsert(c_diff_name.get(), "z", 4, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(c_diff_off.get(), "a", 0, H5T_NATIVE_FLOAT), 0);
  ASSERT_GE(H5Tinsert(c_diff_off.get(), "z", 8, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(c_diff_type.get(), "a", 0, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(c_diff_type.get(), "z", 4, H5T_NATIVE_INT32), 0);
  EXPECT_LT(cmp_ids(c1.get(), c_diff_name.get()), 0);
  EXPECT_GT(cmp_ids(c_diff_name.get(), c1.get()), 0);
  EXPECT_LT(cmp_ids(c1.get(), c_diff_off.get()), 0);
  EXPECT_GT(cmp_ids(c_diff_off.get(), c1.get()), 0);
  EXPECT_GT(cmp_ids(c1.get(), c_diff_type.get()), 0);
  EXPECT_LT(cmp_ids(c_diff_type.get(), c1.get()), 0);

  ScopedType c_msz4(H5Tcreate(H5T_COMPOUND, 8));
  ScopedType c_msz2(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(c_msz4.valid() && c_msz2.valid());
  ASSERT_GE(H5Tinsert(c_msz4.get(), "a", 0, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(c_msz2.get(), "a", 0, H5T_NATIVE_INT16), 0);
  EXPECT_LT(cmp_ids(c_msz2.get(), c_msz4.get()), 0);
  EXPECT_GT(cmp_ids(c_msz4.get(), c_msz2.get()), 0);

  // Enum: empty, multi-member sorting, superset=true/false, value diff.
  ScopedType e_empty1(H5Tenum_create(H5T_NATIVE_INT32));
  ScopedType e_empty2(H5Tenum_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(e_empty1.valid() && e_empty2.valid());
  EXPECT_EQ(cmp_ids(e_empty1.get(), e_empty2.get()), 0);

  ScopedType e_sub(H5Tenum_create(H5T_NATIVE_INT32));
  ScopedType e_full(H5Tenum_create(H5T_NATIVE_INT32));
  ScopedType e_valdiff(H5Tenum_create(H5T_NATIVE_INT32));
  ScopedType e_namediff(H5Tenum_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(e_sub.valid() && e_full.valid() && e_valdiff.valid() &&
              e_namediff.valid());
  int32_t v1 = 1, v2 = 2, v3 = 3, v9 = 9;
  ASSERT_GE(H5Tenum_insert(e_sub.get(), "Z", &v3), 0);
  ASSERT_GE(H5Tenum_insert(e_sub.get(), "A", &v1), 0);
  ASSERT_GE(H5Tenum_insert(e_full.get(), "M", &v2), 0);
  ASSERT_GE(H5Tenum_insert(e_full.get(), "Z", &v3), 0);
  ASSERT_GE(H5Tenum_insert(e_full.get(), "A", &v1), 0);
  ASSERT_GE(H5Tenum_insert(e_valdiff.get(), "A", &v1), 0);
  ASSERT_GE(H5Tenum_insert(e_valdiff.get(), "Z", &v9), 0);
  ASSERT_GE(H5Tenum_insert(e_namediff.get(), "A", &v1), 0);
  ASSERT_GE(H5Tenum_insert(e_namediff.get(), "B", &v3), 0);

  EXPECT_EQ(cmp_ids(e_sub.get(), e_full.get(), /*superset=*/true), 0);
  EXPECT_GT(cmp_ids(e_full.get(), e_sub.get(), /*superset=*/true), 0);
  EXPECT_LT(cmp_ids(e_sub.get(), e_full.get(), /*superset=*/false), 0);
  EXPECT_GT(cmp_ids(e_full.get(), e_sub.get(), /*superset=*/false), 0);
  EXPECT_LT(cmp_ids(e_sub.get(), e_valdiff.get(), /*superset=*/false), 0);
  EXPECT_GT(cmp_ids(e_valdiff.get(), e_sub.get(), /*superset=*/false), 0);
  EXPECT_GT(cmp_ids(e_sub.get(), e_namediff.get(), /*superset=*/false), 0);
  EXPECT_LT(cmp_ids(e_namediff.get(), e_sub.get(), /*superset=*/false), 0);
  EXPECT_NE(cmp_ids(e_namediff.get(), e_full.get(), /*superset=*/true), 0);

  // VLEN vs STRING class comparison, plus internal VLEN subfields
  // (SEQUENCE vs STRING, MEMORY vs DISK vs BADLOC, and distinct file pointers).
  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_GE(H5Tset_size(vl_str.get(), H5T_VARIABLE), 0);
  ScopedType vl_uchar(H5Tvlen_create(H5T_NATIVE_UCHAR));
  ScopedType vl_uchar2(H5Tvlen_create(H5T_NATIVE_UCHAR));
  ASSERT_TRUE(vl_str.valid() && vl_uchar.valid() && vl_uchar2.valid());
  EXPECT_GT(cmp_ids(vl_uchar.get(), vl_str.get()), 0);
  EXPECT_LT(cmp_ids(vl_str.get(), vl_uchar.get()), 0);

  auto* vl_p1 =
      static_cast<H5T_t*>(H5I_object_verify(vl_uchar.get(), H5I_DATATYPE));
  auto* vl_p2 =
      static_cast<H5T_t*>(H5I_object_verify(vl_uchar2.get(), H5I_DATATYPE));
  ASSERT_NE(vl_p1, nullptr);
  ASSERT_NE(vl_p2, nullptr);
  {
    const H5T_vlen_type_t orig_type = vl_p2->shared->u.vlen.type;
    const H5T_loc_t orig_loc = vl_p2->shared->u.vlen.loc;
    H5VL_object_t* const orig_file = vl_p2->shared->u.vlen.file;
    absl::Cleanup restore_vl = [&] {
      vl_p2->shared->u.vlen.type = orig_type;
      vl_p2->shared->u.vlen.loc = orig_loc;
      vl_p2->shared->u.vlen.file = orig_file;
    };

    vl_p2->shared->u.vlen.type = H5T_VLEN_STRING;
    EXPECT_LT(H5T_cmp(vl_p1, vl_p2, false), 0);
    EXPECT_GT(H5T_cmp(vl_p2, vl_p1, false), 0);
    vl_p2->shared->u.vlen.type = orig_type;

    vl_p2->shared->u.vlen.loc = H5T_LOC_DISK;
    EXPECT_LT(H5T_cmp(vl_p1, vl_p2, false), 0);
    EXPECT_GT(H5T_cmp(vl_p2, vl_p1, false), 0);
    vl_p2->shared->u.vlen.loc = H5T_LOC_BADLOC;
    EXPECT_GT(H5T_cmp(vl_p2, vl_p1, false), 0);
    vl_p2->shared->u.vlen.loc = orig_loc;

    ScopedFile cmp_file = CreateInMemoryFile("t_cmp_vlen.h5");
    ASSERT_TRUE(cmp_file.valid());
    H5VL_object_t* real_file_obj =
        H5VL_vol_object_verify(cmp_file.get(), H5I_FILE);
    ASSERT_NE(real_file_obj, nullptr);
    vl_p2->shared->u.vlen.file = real_file_obj;
    EXPECT_LT(H5T_cmp(vl_p1, vl_p2, false), 0);
    EXPECT_GT(H5T_cmp(vl_p2, vl_p1, false), 0);
  }

  // Array: dims & parents & ndims.
  constexpr std::array<hsize_t, 1> kD1 = {12};
  constexpr std::array<hsize_t, 2> kD2a = {3, 4};
  constexpr std::array<hsize_t, 2> kD2b = {2, 6};
  ScopedType a1(H5Tarray_create2(H5T_NATIVE_INT32, 1, kD1.data()));
  ScopedType a2(H5Tarray_create2(H5T_NATIVE_INT32, 2, kD2a.data()));
  ScopedType a3(H5Tarray_create2(H5T_NATIVE_INT32, 2, kD2b.data()));
  ScopedType a4(H5Tarray_create2(H5T_NATIVE_FLOAT, 2, kD2a.data()));
  ASSERT_TRUE(a1.valid() && a2.valid() && a3.valid() && a4.valid());
  EXPECT_LT(cmp_ids(a1.get(), a2.get()), 0);
  EXPECT_GT(cmp_ids(a2.get(), a1.get()), 0);
  EXPECT_GT(cmp_ids(a2.get(), a3.get()), 0);
  EXPECT_LT(cmp_ids(a3.get(), a2.get()), 0);
  EXPECT_NE(cmp_ids(a2.get(), a4.get()), 0);

  // Complex: differing parent and differing form.
  ScopedType cplx_le(H5Tcomplex_create(H5T_IEEE_F32LE));
  ScopedType cplx_be(H5Tcomplex_create(H5T_IEEE_F32BE));
  ScopedType cplx_polar(H5Tcomplex_create(H5T_IEEE_F32LE));
  ASSERT_TRUE(cplx_le.valid() && cplx_be.valid() && cplx_polar.valid());
  EXPECT_NE(cmp_ids(cplx_le.get(), cplx_be.get()), 0);
  auto* polar_ptr =
      static_cast<H5T_t*>(H5I_object_verify(cplx_polar.get(), H5I_DATATYPE));
  ASSERT_NE(polar_ptr, nullptr);
  polar_ptr->shared->u.cplx.form = H5T_COMPLEX_POLAR;
  EXPECT_LT(cmp_ids(cplx_le.get(), cplx_polar.get()), 0);
  EXPECT_GT(cmp_ids(cplx_polar.get(), cplx_le.get()), 0);

  // Opaque tags.
  ScopedType opq1(H5Tcreate(H5T_OPAQUE, 4));
  ScopedType opq2(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(opq1.valid() && opq2.valid());
  ASSERT_GE(H5Tset_tag(opq1.get(), "alpha"), 0);
  ASSERT_GE(H5Tset_tag(opq2.get(), "beta"), 0);
  EXPECT_LT(cmp_ids(opq1.get(), opq2.get()), 0);
  EXPECT_GT(cmp_ids(opq2.get(), opq1.get()), 0);

  // Atomic: order, prec, offset, lsb_pad, msb_pad, int sign, string cset/pad.
  EXPECT_NE(cmp_ids(H5T_STD_I32LE, H5T_STD_I32BE), 0);
  EXPECT_NE(cmp_ids(H5T_STD_I32LE, H5T_STD_U32LE), 0);
  ScopedType i_mod(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(i_mod.valid());
  ASSERT_GE(H5Tset_precision(i_mod.get(), 16), 0);
  EXPECT_LT(cmp_ids(i_mod.get(), H5T_STD_I32LE), 0);
  EXPECT_GT(cmp_ids(H5T_STD_I32LE, i_mod.get()), 0);
  ASSERT_GE(H5Tset_offset(i_mod.get(), 4), 0);
  ScopedType i_prec16(H5Tcopy(H5T_STD_I32LE));
  ASSERT_TRUE(i_prec16.valid());
  ASSERT_GE(H5Tset_precision(i_prec16.get(), 16), 0);
  EXPECT_GT(cmp_ids(i_mod.get(), i_prec16.get()), 0);
  EXPECT_LT(cmp_ids(i_prec16.get(), i_mod.get()), 0);
  ASSERT_GE(H5Tset_pad(i_mod.get(), H5T_PAD_ONE, H5T_PAD_ZERO), 0);
  ScopedType i_pad_msb(H5Tcopy(i_mod.get()));
  ASSERT_TRUE(i_pad_msb.valid());
  EXPECT_EQ(cmp_ids(i_mod.get(), i_pad_msb.get()), 0);
  ASSERT_GE(H5Tset_pad(i_pad_msb.get(), H5T_PAD_ONE, H5T_PAD_ONE), 0);
  EXPECT_LT(cmp_ids(i_mod.get(), i_pad_msb.get()), 0);
  EXPECT_GT(cmp_ids(i_pad_msb.get(), i_mod.get()), 0);
  ScopedType i_pad_lsb0(H5Tcopy(i_prec16.get()));
  ASSERT_TRUE(i_pad_lsb0.valid());
  ASSERT_GE(H5Tset_offset(i_pad_lsb0.get(), 4), 0);
  EXPECT_LT(cmp_ids(i_pad_lsb0.get(), i_mod.get()), 0);
  EXPECT_GT(cmp_ids(i_mod.get(), i_pad_lsb0.get()), 0);

  // Float fields: sign, epos, esize, ebias, mpos, msize, norm, inpad (both signs).
  ScopedType f_base(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(f_base.valid());
  ScopedType f_alt(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(f_alt.valid());
  ASSERT_GE(H5Tset_ebias(f_alt.get(), 128), 0);
  EXPECT_LT(cmp_ids(f_base.get(), f_alt.get()), 0);
  EXPECT_GT(cmp_ids(f_alt.get(), f_base.get()), 0);
  ASSERT_GE(H5Tset_ebias(f_alt.get(), 127), 0);
  ASSERT_GE(H5Tset_norm(f_alt.get(), H5T_NORM_NONE), 0);
  EXPECT_LT(cmp_ids(f_base.get(), f_alt.get()), 0);
  EXPECT_GT(cmp_ids(f_alt.get(), f_base.get()), 0);
  ASSERT_GE(H5Tset_norm(f_alt.get(), H5T_NORM_IMPLIED), 0);
  ASSERT_GE(H5Tset_inpad(f_alt.get(), H5T_PAD_ONE), 0);
  EXPECT_LT(cmp_ids(f_base.get(), f_alt.get()), 0);
  EXPECT_GT(cmp_ids(f_alt.get(), f_base.get()), 0);
  ASSERT_GE(H5Tset_inpad(f_alt.get(), H5T_PAD_ZERO), 0);
  ASSERT_GE(H5Tset_fields(f_alt.get(), 30, 22, 8, 0, 22), 0);
  EXPECT_GT(cmp_ids(f_base.get(), f_alt.get()), 0);
  EXPECT_LT(cmp_ids(f_alt.get(), f_base.get()), 0);
  // Vary epos, esize, mpos, msize individually with sign fixed at 31.
  ASSERT_GE(H5Tset_fields(f_alt.get(), 31, 24, 7, 0, 24), 0);
  EXPECT_LT(cmp_ids(f_base.get(), f_alt.get()), 0);
  EXPECT_GT(cmp_ids(f_alt.get(), f_base.get()), 0);
  ASSERT_GE(H5Tset_fields(f_alt.get(), 31, 23, 7, 0, 23), 0);
  EXPECT_GT(cmp_ids(f_base.get(), f_alt.get()), 0);
  EXPECT_LT(cmp_ids(f_alt.get(), f_base.get()), 0);
  ASSERT_GE(H5Tset_fields(f_alt.get(), 31, 23, 8, 1, 22), 0);
  EXPECT_LT(cmp_ids(f_base.get(), f_alt.get()), 0);
  EXPECT_GT(cmp_ids(f_alt.get(), f_base.get()), 0);
  ASSERT_GE(H5Tset_fields(f_alt.get(), 31, 23, 8, 0, 22), 0);
  EXPECT_GT(cmp_ids(f_base.get(), f_alt.get()), 0);
  EXPECT_LT(cmp_ids(f_alt.get(), f_base.get()), 0);

  // String cset/pad (both directions), Time, Bitfield, Reference subfields.
  ScopedType s1(H5Tcopy(H5T_C_S1));
  ScopedType s2(H5Tcopy(H5T_FORTRAN_S1));
  ASSERT_TRUE(s1.valid() && s2.valid());
  EXPECT_LT(cmp_ids(s1.get(), s2.get()), 0);
  EXPECT_GT(cmp_ids(s2.get(), s1.get()), 0);
  ASSERT_GE(H5Tset_cset(s2.get(), H5T_CSET_UTF8), 0);
  EXPECT_LT(cmp_ids(s1.get(), s2.get()), 0);
  EXPECT_GT(cmp_ids(s2.get(), s1.get()), 0);

  EXPECT_NE(cmp_ids(H5T_UNIX_D32LE, H5T_UNIX_D32BE), 0);
  EXPECT_NE(cmp_ids(H5T_STD_B32LE, H5T_STD_B32BE), 0);
  EXPECT_NE(cmp_ids(H5T_STD_REF_OBJ, H5T_STD_REF_DSETREG), 0);
  {
    ScopedType r1(H5Tcopy(H5T_STD_REF_OBJ));
    ScopedType r2(H5Tcopy(H5T_STD_REF_OBJ));
    ASSERT_TRUE(r1.valid() && r2.valid());
    auto* rp1 = static_cast<H5T_t*>(H5I_object_verify(r1.get(), H5I_DATATYPE));
    auto* rp2 = static_cast<H5T_t*>(H5I_object_verify(r2.get(), H5I_DATATYPE));
    ASSERT_NE(rp1, nullptr);
    ASSERT_NE(rp2, nullptr);
    const H5R_type_t orig_rtype = rp2->shared->u.atomic.u.r.rtype;
    const H5T_loc_t orig_loc = rp2->shared->u.atomic.u.r.loc;
    H5VL_object_t* const orig_rfile = rp2->shared->u.atomic.u.r.file;
    absl::Cleanup restore_ref = [&] {
      rp2->shared->u.atomic.u.r.rtype = orig_rtype;
      rp2->shared->u.atomic.u.r.loc = orig_loc;
      rp2->shared->u.atomic.u.r.file = orig_rfile;
    };

    rp2->shared->u.atomic.u.r.rtype = H5R_OBJECT2;
    EXPECT_LT(H5T_cmp(rp1, rp2, false), 0);
    EXPECT_GT(H5T_cmp(rp2, rp1, false), 0);
    rp2->shared->u.atomic.u.r.rtype = orig_rtype;

    rp2->shared->u.atomic.u.r.loc = H5T_LOC_DISK;
    EXPECT_LT(H5T_cmp(rp1, rp2, false), 0);
    EXPECT_GT(H5T_cmp(rp2, rp1, false), 0);
    rp2->shared->u.atomic.u.r.loc = orig_loc;

    ScopedFile rfile = CreateInMemoryFile("t_cmp_ref.h5");
    ASSERT_TRUE(rfile.valid());
    H5VL_object_t* rfile_vol = H5VL_vol_object_verify(rfile.get(), H5I_FILE);
    ASSERT_NE(rfile_vol, nullptr);
    rp2->shared->u.atomic.u.r.file = rfile_vol;
    EXPECT_LT(H5T_cmp(rp1, rp2, false), 0);
    EXPECT_GT(H5T_cmp(rp2, rp1, false), 0);
  }
}

TEST(H5TTest, EncodeAndDecodeRoundTripAndErrorHandling) {
  constexpr hsize_t kDim = 2;
  ScopedType arr(H5Tarray_create2(H5T_NATIVE_FLOAT, 1, &kDim));
  ScopedType cplx(H5Tcomplex_create(H5T_IEEE_F64LE));
  ScopedType opq(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(arr.valid() && cplx.valid() && opq.valid());
  ASSERT_GE(H5Tset_tag(opq.get(), "tag1"), 0);

  ScopedType cmp(H5Tcreate(H5T_COMPOUND, 32));
  ASSERT_TRUE(cmp.valid());
  ASSERT_GE(H5Tinsert(cmp.get(), "arr", 0, arr.get()), 0);
  ASSERT_GE(H5Tinsert(cmp.get(), "cplx", 8, cplx.get()), 0);
  ASSERT_GE(H5Tinsert(cmp.get(), "opq", 24, opq.get()), 0);

  const std::vector<hid_t> kTypes = {
      H5T_NATIVE_INT32, H5T_IEEE_F64BE, H5T_C_S1, cmp.get(), H5T_STD_REF,
  };
  for (hid_t tid : kTypes) {
    size_t nalloc = 0;
    ASSERT_GE(H5Tencode(tid, nullptr, &nalloc), 0);
    ASSERT_GT(nalloc, 2u);

    std::vector<uint8_t> small_buf(1);
    size_t small_len = 1;
    ASSERT_GE(H5Tencode(tid, small_buf.data(), &small_len), 0);
    EXPECT_EQ(small_len, nalloc);

    std::vector<uint8_t> buf(nalloc);
    ASSERT_GE(H5Tencode(tid, buf.data(), &nalloc), 0);

    ScopedType decoded(H5Tdecode(buf.data(), buf.size()));
    ASSERT_TRUE(decoded.valid());
    EXPECT_GT(H5Tequal(tid, decoded.get()), 0);

    ScopedType decoded2(H5Tdecode2(buf.data(), buf.size()));
    ASSERT_TRUE(decoded2.valid());
    EXPECT_GT(H5Tequal(tid, decoded2.get()), 0);
  }

  // Corrupt byte-stream and bad-argument decode/encode error cases.
  size_t valid_len = 0;
  ASSERT_GE(H5Tencode(H5T_NATIVE_INT, nullptr, &valid_len), 0);
  std::vector<uint8_t> encoded(valid_len);
  ASSERT_GE(H5Tencode(H5T_NATIVE_INT, encoded.data(), &valid_len), 0);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Tencode(H5I_INVALID_HID, encoded.data(), &valid_len), 0);
  EXPECT_LT(H5Tencode(H5T_NATIVE_INT, encoded.data(), nullptr), 0);

  EXPECT_EQ(H5Tdecode2(nullptr, valid_len), H5I_INVALID_HID);
  EXPECT_EQ(H5Tdecode2(encoded.data(), 0), H5I_INVALID_HID);
  EXPECT_EQ(H5Tdecode2(encoded.data(), 1), H5I_INVALID_HID);
  EXPECT_EQ(H5Tdecode2(encoded.data(), 2), H5I_INVALID_HID);

  std::vector<uint8_t> bad_msg = encoded;
  bad_msg[0] ^= 0xff;
  EXPECT_EQ(H5Tdecode2(bad_msg.data(), bad_msg.size()), H5I_INVALID_HID);

  std::vector<uint8_t> bad_ver = encoded;
  bad_ver[1] = 99;
  EXPECT_EQ(H5Tdecode2(bad_ver.data(), bad_ver.size()), H5I_INVALID_HID);
}

TEST(H5TTest, RegisterUnregisterFindCompilerConvAndSoftReplacement) {
  ScopedType opq_a1(H5Tcreate(H5T_OPAQUE, 4));
  ScopedType opq_a2(H5Tcreate(H5T_OPAQUE, 4));
  ScopedType opq_r1(H5Tcreate(H5T_OPAQUE, 4));
  ScopedType opq_r2(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(opq_a1.valid() && opq_a2.valid() && opq_r1.valid() &&
              opq_r2.valid());
  ASSERT_GE(H5Tset_tag(opq_a1.get(), "soft_accept_src"), 0);
  ASSERT_GE(H5Tset_tag(opq_a2.get(), "soft_accept_dst"), 0);
  ASSERT_GE(H5Tset_tag(opq_r1.get(), "reject_src"), 0);
  ASSERT_GE(H5Tset_tag(opq_r2.get(), "reject_dst"), 0);

  g_conv_tracker_a = {};
  g_conv_tracker_b = {};
  g_soft_tracker = {};

  absl::Cleanup cleanup_convs = [&] {
    H5Tunregister(H5T_PERS_DONTCARE, "custom_hard_a", H5I_INVALID_HID,
                  H5I_INVALID_HID, TrackedCustomConvA);
    H5Tunregister(H5T_PERS_DONTCARE, "custom_hard_b", H5I_INVALID_HID,
                  H5I_INVALID_HID, TrackedCustomConvB);
    H5Tunregister(H5T_PERS_DONTCARE, "custom_soft_sel", H5I_INVALID_HID,
                  H5I_INVALID_HID, SelectiveSoftConv);
    H5Tunregister(H5T_PERS_DONTCARE, "custom_soft_fail", H5I_INVALID_HID,
                  H5I_INVALID_HID, AlwaysFailSoftConv);
  };

  // 1. Register hard converter, verify H5Tfind & H5Tcompiler_conv, execute
  // H5Tconvert with background buffer, then replace with TrackedCustomConvB.
  g_conv_tracker_a.use_priv = true;
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "custom_hard_a", opq_a1.get(),
                        opq_a2.get(), TrackedCustomConvA),
            0);
  EXPECT_EQ(g_conv_tracker_a.init_calls, 1);

  H5T_cdata_t* pcdata = nullptr;
  EXPECT_EQ(H5Tfind(opq_a1.get(), opq_a2.get(), &pcdata), TrackedCustomConvA);
  ASSERT_NE(pcdata, nullptr);
  EXPECT_GT(H5Tcompiler_conv(opq_a1.get(), opq_a2.get()), 0);

  std::array<uint8_t, 8> data = {1, 2, 3, 4, 5, 6, 7, 8};
  std::array<uint8_t, 8> bkg = {0, 0, 0, 0, 0, 0, 0, 0};
  ASSERT_GE(H5Tconvert(opq_a1.get(), opq_a2.get(), 2, data.data(), bkg.data(),
                       H5P_DEFAULT),
            0);
  EXPECT_EQ(g_conv_tracker_a.conv_calls, 1);
  EXPECT_TRUE(g_conv_tracker_a.saw_bkg);
  EXPECT_EQ(data[0], 1 ^ 0x55);
  EXPECT_EQ(data[4], 5 ^ 0x55);

  // Replace hard function on the same path; verify TrackedCustomConvA receives
  // H5T_CONV_FREE.
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "custom_hard_b", opq_a1.get(),
                        opq_a2.get(), TrackedCustomConvB),
            0);
  EXPECT_EQ(g_conv_tracker_a.free_calls, 1);
  EXPECT_EQ(H5Tfind(opq_a1.get(), opq_a2.get(), &pcdata), TrackedCustomConvB);

  // Equal types skip hard registration (no-op optimization).
  EXPECT_GE(H5Tregister(H5T_PERS_HARD, "noop_self", opq_a1.get(), opq_a1.get(),
                        TrackedCustomConvB),
            0);

  // Unregister the hard path and confirm it is gone.
  ASSERT_GE(H5Tunregister(H5T_PERS_HARD, "custom_hard_b", opq_a1.get(),
                          opq_a2.get(), TrackedCustomConvB),
            0);
  EXPECT_EQ(g_conv_tracker_b.free_calls, 1);

  // 2. Soft converter registration: create two existing soft paths first (using
  // AlwaysFail + built-in or another soft function) so registering a new soft
  // converter iterates existing paths, replacing matching ones and skipping
  // failing INITs.
  ASSERT_GE(H5Tregister(H5T_PERS_SOFT, "custom_soft_sel", opq_a1.get(),
                        opq_a2.get(), SelectiveSoftConv),
            0);
  ASSERT_GE(H5Tconvert(opq_a1.get(), opq_a2.get(), 2, data.data(), nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_EQ(g_soft_tracker.conv_calls, 1);
  EXPECT_EQ(H5Tcompiler_conv(opq_a1.get(), opq_a2.get()), 0);

  // Now create a second soft path (opq_r1 -> opq_r2) using SelectiveSoftConv
  // (which rejects opq_r1 -> opq_r2? Actually make SelectiveSoftConv succeed on
  // opq_a* and AlwaysFailSoftConv fail on all). Because H5T__register()
  // returns the last matching soft-INIT result across existing paths, test
  // AlwaysFailSoftConv expecting < 0 while verifying it still skips failed INITs
  // and registers cleanly when no existing soft path fails LAST.
  EXPECT_LT(H5Tregister(H5T_PERS_SOFT, "custom_soft_fail", opq_r1.get(),
                        opq_r2.get(), AlwaysFailSoftConv),
            0);

  // Registering SelectiveSoftConv again replaces the existing soft path.
  EXPECT_GE(H5Tregister(H5T_PERS_SOFT, "custom_soft_sel", opq_a1.get(),
                        opq_a2.get(), SelectiveSoftConv),
            0);
  EXPECT_EQ(H5Tfind(opq_a1.get(), opq_a2.get(), &pcdata), SelectiveSoftConv);

  // Built-in hard & soft path query checks.
  EXPECT_GT(H5Tcompiler_conv(H5T_NATIVE_INT32, H5T_NATIVE_DOUBLE), 0);
  EXPECT_EQ(H5Tcompiler_conv(H5T_STD_I32BE, H5T_STD_I16LE), 0);

  // 3. Error paths for H5Tregister / H5Tunregister / H5Tfind / H5Tcompiler_conv.
  ScopedSpace bad_space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(bad_space.valid());
  ScopedErrorSilence silence;
  EXPECT_LT(H5Tregister(H5T_PERS_DONTCARE, "bad", H5T_NATIVE_INT,
                        H5T_NATIVE_DOUBLE, TrackedCustomConvB),
            0);
  EXPECT_LT(H5Tregister(H5T_PERS_HARD, nullptr, H5T_NATIVE_INT,
                        H5T_NATIVE_DOUBLE, TrackedCustomConvB),
            0);
  EXPECT_LT(H5Tregister(H5T_PERS_HARD, "", H5T_NATIVE_INT, H5T_NATIVE_DOUBLE,
                        TrackedCustomConvB),
            0);
  EXPECT_LT(H5Tregister(H5T_PERS_HARD, "bad", H5I_INVALID_HID,
                        H5T_NATIVE_DOUBLE, TrackedCustomConvB),
            0);
  EXPECT_LT(H5Tregister(H5T_PERS_HARD, "bad", H5T_NATIVE_INT, H5I_INVALID_HID,
                        TrackedCustomConvB),
            0);
  EXPECT_LT(H5Tregister(H5T_PERS_HARD, "bad", H5T_NATIVE_INT,
                        H5T_NATIVE_DOUBLE, nullptr),
            0);

  g_conv_tracker_a.fail_init = true;
  EXPECT_LT(H5Tregister(H5T_PERS_HARD, "init_fail", opq_r1.get(), opq_r2.get(),
                        TrackedCustomConvA),
            0);
  g_conv_tracker_a.fail_init = false;

  EXPECT_LT(H5Tunregister(H5T_PERS_HARD, nullptr, bad_space.get(),
                          H5I_INVALID_HID, nullptr),
            0);
  EXPECT_LT(H5Tunregister(H5T_PERS_HARD, nullptr, H5I_INVALID_HID,
                          bad_space.get(), nullptr),
            0);

  EXPECT_EQ(H5Tfind(H5I_INVALID_HID, H5T_NATIVE_INT, &pcdata), nullptr);
  EXPECT_EQ(H5Tfind(H5T_NATIVE_INT, H5I_INVALID_HID, &pcdata), nullptr);
  EXPECT_EQ(H5Tfind(H5T_NATIVE_INT, H5T_NATIVE_DOUBLE, nullptr), nullptr);
  EXPECT_EQ(H5Tfind(opq_r1.get(), H5T_NATIVE_INT, &pcdata), nullptr);

  EXPECT_LT(H5Tcompiler_conv(H5I_INVALID_HID, H5T_NATIVE_INT), 0);
  EXPECT_LT(H5Tcompiler_conv(opq_r1.get(), H5T_NATIVE_INT), 0);
}

TEST(H5TTest, PathTableGrowthViaManyHardConverters) {
  // Exceed H5T_DEF_CONV_TABLE_SLOTS (128) and default soft slots (32) cleanly,
  // then unregister every custom entry before exit.
  constexpr int kNumPairs = 140;
  std::vector<ScopedType> srcs;
  std::vector<ScopedType> dsts;
  srcs.reserve(kNumPairs);
  dsts.reserve(kNumPairs);

  absl::Cleanup cleanup = [&] {
    H5Tunregister(H5T_PERS_DONTCARE, "bulk_hard", H5I_INVALID_HID,
                  H5I_INVALID_HID, TrackedCustomConvB);
    H5Tunregister(H5T_PERS_DONTCARE, "bulk_soft", H5I_INVALID_HID,
                  H5I_INVALID_HID, AlwaysFailSoftConv);
  };

  for (int i = 0; i < kNumPairs; ++i) {
    ScopedType s(H5Tcreate(H5T_OPAQUE, 4));
    ScopedType d(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(s.valid() && d.valid());
    std::string stag = "src_" + std::to_string(i);
    std::string dtag = "dst_" + std::to_string(i);
    ASSERT_GE(H5Tset_tag(s.get(), stag.c_str()), 0);
    ASSERT_GE(H5Tset_tag(d.get(), dtag.c_str()), 0);
    ASSERT_GE(H5Tregister(H5T_PERS_HARD, "bulk_hard", s.get(), d.get(),
                          TrackedCustomConvB),
              0);
    srcs.push_back(std::move(s));
    dsts.push_back(std::move(d));
  }
  EXPECT_GE(H5T__get_path_table_npaths(), kNumPairs);

  for (int i = 0; i < 36; ++i) {
    ASSERT_GE(H5Tregister(H5T_PERS_SOFT, "bulk_soft", srcs[0].get(),
                          dsts[0].get(), AlwaysFailSoftConv),
              0);
  }
}

TEST(H5TTest, ConvertWithExceptionCallbackAndErrorCases) {
  struct ExcState {
    int count = 0;
  } exc_state;

  auto conv_cb = [](H5T_conv_except_t /*except_type*/, hid_t /*src_id*/,
                    hid_t /*dst_id*/, void* /*src_buf*/, void* dst_buf,
                    void* user_data) -> H5T_conv_ret_t {
    ++static_cast<ExcState*>(user_data)->count;
    *static_cast<int32_t*>(dst_buf) = -1;
    return H5T_CONV_HANDLED;
  };

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(H5Pset_type_conv_cb(dxpl.get(), conv_cb, &exc_state), 0);

  // Overflow conversion triggers H5T_convert() temporary ID registration when
  // cb_struct.func != nullptr.
  double big = 1.0e20;
  uint8_t buf[sizeof(double)]{};
  std::memcpy(buf, &big, sizeof(big));
  EXPECT_GE(H5Tconvert(H5T_NATIVE_DOUBLE, H5T_NATIVE_INT32, 1, buf, nullptr,
                       dxpl.get()),
            0);
  EXPECT_EQ(exc_state.count, 1);
  int32_t out = 0;
  std::memcpy(&out, buf, sizeof(out));
  EXPECT_EQ(out, -1);

  // long double input zero-initialized before assignment for MSan cleanliness.
  long double ld;
  std::memset(&ld, 0, sizeof(ld));
  ld = 123.5L;
  uint8_t ld_buf[sizeof(long double)]{};
  std::memcpy(ld_buf, &ld, sizeof(ld));
  EXPECT_GE(H5Tconvert(H5T_NATIVE_LDOUBLE, H5T_NATIVE_DOUBLE, 1, ld_buf,
                       nullptr, H5P_DEFAULT),
            0);
  double ld_out = 0.0;
  std::memcpy(&ld_out, ld_buf, sizeof(ld_out));
  EXPECT_DOUBLE_EQ(ld_out, 123.5);

  // Error cases.
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ScopedType opq1(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(fcpl.valid() && opq1.valid());
  ScopedErrorSilence silence;
  EXPECT_LT(H5Tconvert(H5I_INVALID_HID, H5T_NATIVE_INT, 1, buf, nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_LT(H5Tconvert(H5T_NATIVE_INT, H5I_INVALID_HID, 1, buf, nullptr,
                       H5P_DEFAULT),
            0);
  EXPECT_LT(H5Tconvert(H5T_NATIVE_INT, H5T_NATIVE_DOUBLE, 1, buf, nullptr,
                       fcpl.get()),
            0);
  EXPECT_LT(H5Tconvert(opq1.get(), H5T_NATIVE_INT, 1, buf, nullptr,
                       H5P_DEFAULT),
            0);
}

TEST(H5TTest, ReclaimVlenSequencesStringsCompoundsAndErrors) {
  struct AllocTracker {
    int allocs = 0;
    int frees = 0;
  } tracker;

  auto custom_alloc = [](size_t size, void* alloc_info) -> void* {
    ++static_cast<AllocTracker*>(alloc_info)->allocs;
    return std::malloc(size);
  };
  auto custom_free = [](void* mem, void* free_info) {
    ++static_cast<AllocTracker*>(free_info)->frees;
    std::free(mem);
  };

  ScopedPlist dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(dxpl.valid());
  ASSERT_GE(
      H5Pset_vlen_mem_manager(dxpl.get(), custom_alloc, &tracker, custom_free,
                              &tracker),
      0);

  ScopedType vl_seq(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vl_seq.valid());
  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vl_str.valid());
  ASSERT_GE(H5Tset_size(vl_str.get(), H5T_VARIABLE), 0);

  struct Elem {
    hvl_t seq;
    char* str;
  };
  ScopedType cmp(H5Tcreate(H5T_COMPOUND, sizeof(Elem)));
  ASSERT_TRUE(cmp.valid());
  ASSERT_GE(H5Tinsert(cmp.get(), "seq", offsetof(Elem, seq), vl_seq.get()), 0);
  ASSERT_GE(H5Tinsert(cmp.get(), "str", offsetof(Elem, str), vl_str.get()), 0);

  constexpr hsize_t kCount = 2;
  ScopedSpace space(H5Screate_simple(1, &kCount, nullptr));
  ASSERT_TRUE(space.valid());

  std::array<Elem, 2> elems{};
  for (size_t i = 0; i < elems.size(); ++i) {
    elems[i].seq.len = 2;
    elems[i].seq.p = custom_alloc(2 * sizeof(int32_t), &tracker);
    elems[i].str = static_cast<char*>(custom_alloc(8, &tracker));
    std::snprintf(elems[i].str, 8, "v%zu", i);
  }
  EXPECT_GE(H5Treclaim(cmp.get(), space.get(), dxpl.get(), elems.data()), 0);
  EXPECT_EQ(tracker.frees, tracker.allocs);

  // Reclaim with default DXPL on VL string scalar.
  char* s = static_cast<char*>(std::malloc(6));
  std::snprintf(s, 6, "hello");
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  EXPECT_GE(H5Treclaim(vl_str.get(), scalar.get(), H5P_DEFAULT, &s), 0);

  // Error cases: null buffer, invalid type, invalid space, space without
  // extent, non-DXPL plist.
  ScopedSpace no_ext(H5Screate(H5S_SIMPLE));
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(no_ext.valid() && fcpl.valid());
  ScopedErrorSilence silence;
  EXPECT_LT(H5Treclaim(vl_str.get(), scalar.get(), H5P_DEFAULT, nullptr), 0);
  EXPECT_LT(H5Treclaim(H5I_INVALID_HID, scalar.get(), H5P_DEFAULT, &s), 0);
  EXPECT_LT(H5Treclaim(vl_str.get(), H5I_INVALID_HID, H5P_DEFAULT, &s), 0);
  EXPECT_LT(H5Treclaim(vl_str.get(), no_ext.get(), H5P_DEFAULT, &s), 0);
  EXPECT_LT(H5Treclaim(vl_str.get(), scalar.get(), fcpl.get(), &s), 0);
}

TEST(H5TTest, InternalPredicatesVersionPatchLocAndVolObj) {
  ScopedPlist fapl_early = MakeCoreFapl();
  ASSERT_TRUE(fapl_early.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl_early.get(), H5F_LIBVER_EARLIEST,
                                 H5F_LIBVER_V18),
            0);
  ScopedFile file_early(
      H5Fcreate("t_early.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_early.get()));
  ASSERT_TRUE(file_early.valid());

  ScopedPlist fapl_latest = MakeCoreFapl();
  ASSERT_TRUE(fapl_latest.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl_latest.get(), H5F_LIBVER_V200, H5F_LIBVER_LATEST),
      0);
  ScopedFile file_latest(
      H5Fcreate("t_latest.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_latest.get()));
  ASSERT_TRUE(file_latest.valid());

  auto* f_early =
      static_cast<H5F_t*>(H5VL_object_verify(file_early.get(), H5I_FILE));
  auto* f_latest =
      static_cast<H5F_t*>(H5VL_object_verify(file_latest.get(), H5I_FILE));
  auto* f_latest_vol = H5VL_vol_object_verify(file_latest.get(), H5I_FILE);
  auto* f_early_vol = H5VL_vol_object_verify(file_early.get(), H5I_FILE);
  ASSERT_NE(f_early, nullptr);
  ASSERT_NE(f_latest, nullptr);
  ASSERT_NE(f_latest_vol, nullptr);
  ASSERT_NE(f_early_vol, nullptr);

  // 1. Committed datatype cross-file usage triggers
  // H5T_convert_committed_datatype(), H5T_oloc(), H5T_nameof(), H5T_is_named().
  ScopedType committed(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(committed.valid());
  ASSERT_GE(H5Tcommit2(file_latest.get(), "c_int", committed.get(), H5P_DEFAULT,
                      H5P_DEFAULT, H5P_DEFAULT),
            0);
  auto* committed_ptr =
      static_cast<H5T_t*>(H5I_object_verify(committed.get(), H5I_DATATYPE));
  ASSERT_NE(committed_ptr, nullptr);
  EXPECT_GT(H5T_is_named(committed_ptr), 0);
  auto* vol_dt = static_cast<H5T_t*>(H5VL_object_data(committed_ptr->vol_obj));
  ASSERT_NE(vol_dt, nullptr);
  EXPECT_GT(H5T_is_named(vol_dt), 0);
  EXPECT_NE(H5T_oloc(vol_dt), nullptr);
  EXPECT_NE(H5T_nameof(vol_dt), nullptr);

  constexpr hsize_t kDim = 1;
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ScopedSpace space1(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(scalar.valid() && space1.valid());

  // Same-file committed copy is a no-op; cross-file dataset & attr create
  // trigger H5T_convert_committed_datatype(dt, f_early).
  H5T_t* copy_same = H5T_copy(vol_dt, H5T_COPY_ALL);
  ASSERT_NE(copy_same, nullptr);
  EXPECT_GE(H5T_convert_committed_datatype(copy_same, f_latest), 0);
  EXPECT_GE(H5T_patch_file(copy_same, f_latest), 0);
  EXPECT_GE(H5T_close_real(copy_same), 0);

  ScopedDataset cross_dset(H5Dcreate2(file_early.get(), "cross_ds",
                                      committed.get(), space1.get(),
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(cross_dset.valid());
  ScopedAttr cross_attr(H5Acreate2(file_early.get(), "cross_at", committed.get(),
                                   scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(cross_attr.valid());

  // 2. Reference, VL storage, relocatable, sensible, and unused-bits helpers.
  auto as_ptr = [](hid_t id) {
    return static_cast<H5T_t*>(H5I_object_verify(id, H5I_DATATYPE));
  };
  EXPECT_EQ(H5T_get_ref_type(as_ptr(H5T_STD_REF_OBJ)), H5R_OBJECT1);
  EXPECT_EQ(H5T_get_ref_type(as_ptr(H5T_STD_REF_DSETREG)),
            H5R_DATASET_REGION1);
  EXPECT_EQ(H5T_get_ref_type(as_ptr(H5T_STD_REF)), H5R_OBJECT2);
  EXPECT_EQ(H5T_get_ref_type(as_ptr(H5T_NATIVE_INT)), H5R_BADTYPE);

  ScopedType cmp_empty(H5Tcreate(H5T_COMPOUND, 8));
  ScopedType cmp_ok(H5Tcreate(H5T_COMPOUND, 4));
  ScopedType enm_empty(H5Tenum_create(H5T_NATIVE_INT));
  ScopedType enm_ok(H5Tenum_create(H5T_NATIVE_INT));
  ASSERT_TRUE(cmp_empty.valid() && cmp_ok.valid() && enm_empty.valid() &&
              enm_ok.valid());
  int ev = 1;
  ASSERT_GE(H5Tinsert(cmp_ok.get(), "x", 0, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tenum_insert(enm_ok.get(), "ONE", &ev), 0);
  EXPECT_EQ(H5T_is_sensible(as_ptr(cmp_empty.get())), 0);
  EXPECT_GT(H5T_is_sensible(as_ptr(cmp_ok.get())), 0);
  EXPECT_EQ(H5T_is_sensible(as_ptr(enm_empty.get())), 0);
  EXPECT_GT(H5T_is_sensible(as_ptr(enm_ok.get())), 0);
  EXPECT_GT(H5T_is_sensible(as_ptr(H5T_NATIVE_INT)), 0);

  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_GE(H5Tset_size(vl_str.get(), H5T_VARIABLE), 0);
  ScopedType vl_seq(H5Tvlen_create(H5T_STD_REF_OBJ));
  ScopedType arr_ref(H5Tarray_create2(H5T_STD_REF_DSETREG, 1, &kDim));
  ScopedType cmp_ref(H5Tcreate(H5T_COMPOUND, sizeof(hobj_ref_t)));
  ASSERT_TRUE(vl_str.valid() && vl_seq.valid() && arr_ref.valid() &&
              cmp_ref.valid());
  ASSERT_GE(H5Tinsert(cmp_ref.get(), "r", 0, H5T_STD_REF_OBJ), 0);

  EXPECT_GT(H5T_is_relocatable(as_ptr(vl_str.get())), 0);
  EXPECT_GT(H5T_is_relocatable(as_ptr(H5T_STD_REF_OBJ)), 0);
  EXPECT_EQ(H5T_is_relocatable(as_ptr(H5T_NATIVE_INT)), 0);

  EXPECT_GT(H5T_is_vl_storage(as_ptr(vl_str.get())), 0);
  EXPECT_GT(H5T_is_vl_storage(as_ptr(H5T_STD_REF_OBJ)), 0);
  EXPECT_GT(H5T_is_vl_storage(as_ptr(H5T_STD_REF_DSETREG)), 0);
  EXPECT_GT(H5T_is_vl_storage(as_ptr(arr_ref.get())), 0);
  EXPECT_GT(H5T_is_vl_storage(as_ptr(cmp_ref.get())), 0);
  EXPECT_EQ(H5T_is_vl_storage(as_ptr(H5T_STD_REF)), 0);
  EXPECT_EQ(H5T_is_vl_storage(as_ptr(H5T_NATIVE_INT)), 0);

  // Unused bits check.
  ScopedType normal_pad(H5Tcopy(H5T_STD_I32LE));
  ScopedType unusual_pad(H5Tcopy(H5T_STD_I32LE));
  ScopedType odd_flt(H5Tcopy(H5T_IEEE_F32LE));
  ScopedType odd_bit(H5Tcopy(H5T_STD_B32LE));
  ASSERT_TRUE(normal_pad.valid() && unusual_pad.valid() && odd_flt.valid() &&
              odd_bit.valid());
  ASSERT_GE(H5Tset_precision(normal_pad.get(), 24), 0);
  ASSERT_GE(H5Tset_precision(unusual_pad.get(), 8), 0);
  ASSERT_GE(H5Tset_fields(odd_flt.get(), 7, 5, 2, 0, 5), 0);
  ASSERT_GE(H5Tset_precision(odd_flt.get(), 8), 0);
  ASSERT_GE(H5Tset_precision(odd_bit.get(), 8), 0);
  EXPECT_FALSE(
      H5T_is_numeric_with_unusual_unused_bits(as_ptr(H5T_STD_I8LE)));
  EXPECT_FALSE(
      H5T_is_numeric_with_unusual_unused_bits(as_ptr(H5T_NATIVE_INT32)));
  EXPECT_FALSE(
      H5T_is_numeric_with_unusual_unused_bits(as_ptr(normal_pad.get())));
  EXPECT_TRUE(
      H5T_is_numeric_with_unusual_unused_bits(as_ptr(unusual_pad.get())));
  EXPECT_TRUE(H5T_is_numeric_with_unusual_unused_bits(as_ptr(odd_flt.get())));
  EXPECT_TRUE(H5T_is_numeric_with_unusual_unused_bits(as_ptr(odd_bit.get())));
  EXPECT_FALSE(H5T_is_numeric_with_unusual_unused_bits(as_ptr(H5T_C_S1)));

  // 3. Path predicates, compound subset, and H5T_set_version / upgrade.
  EXPECT_TRUE(H5T_noop_conv(as_ptr(H5T_NATIVE_INT32), as_ptr(H5T_NATIVE_INT32)));
  EXPECT_FALSE(H5T_noop_conv(as_ptr(H5T_NATIVE_INT32), as_ptr(H5T_NATIVE_DOUBLE)));
  H5T_path_t* noop_p =
      H5T_path_find(as_ptr(H5T_NATIVE_INT32), as_ptr(H5T_NATIVE_INT32));
  ASSERT_NE(noop_p, nullptr);
  EXPECT_TRUE(H5T_path_noop(noop_p));
  EXPECT_EQ(H5T_path_compound_subset(noop_p), nullptr);

  // Construct compounds where `H5T__conv_struct_opt` rejects (field grows from
  // 1B to 8B inside a 2B src struct) so HDF5 falls back to H5T__conv_struct,
  // making H5T_path_compound_subset() return the subset struct info.
  ScopedType cmp_src(H5Tcreate(H5T_COMPOUND, 2));
  ScopedType cmp_dst(H5Tcreate(H5T_COMPOUND, 12));
  ASSERT_TRUE(cmp_src.valid() && cmp_dst.valid());
  ASSERT_GE(H5Tinsert(cmp_src.get(), "a", 0, H5T_NATIVE_INT8), 0);
  ASSERT_GE(H5Tinsert(cmp_src.get(), "b", 1, H5T_NATIVE_INT8), 0);
  ASSERT_GE(H5Tinsert(cmp_dst.get(), "a", 0, H5T_NATIVE_INT64), 0);
  ASSERT_GE(H5Tinsert(cmp_dst.get(), "c", 8, H5T_NATIVE_INT32), 0);
  H5T_path_t* sub_p = H5T_path_find(as_ptr(cmp_src.get()), as_ptr(cmp_dst.get()));
  ASSERT_NE(sub_p, nullptr);
  EXPECT_FALSE(H5T_path_noop(sub_p));
  EXPECT_NE(H5T_path_compound_subset(sub_p), nullptr);
  EXPECT_EQ(H5T_path_bkg(sub_p), H5T_BKG_YES);

  // Version upgrades across compound/array/enum/vlen/complex + out of bounds.
  ScopedType cplx(H5Tcomplex_create(H5T_NATIVE_FLOAT));
  ScopedType vlen_enm(H5Tvlen_create(enm_ok.get()));
  ScopedType arr_cmp(H5Tarray_create2(cmp_ok.get(), 1, &kDim));
  ASSERT_TRUE(cplx.valid() && vlen_enm.valid() && arr_cmp.valid());
  EXPECT_GE(H5T_set_version(f_latest, as_ptr(vlen_enm.get())), 0);
  EXPECT_GE(H5T_set_version(f_latest, as_ptr(arr_cmp.get())), 0);
  EXPECT_GE(H5T_set_version(f_latest, as_ptr(cplx.get())), 0);

  // 4. H5T_set_loc, H5T_patch_vlen_file, H5T_own_vol_obj, and VOL unregister.
  ScopedType vl_nested(H5Tvlen_create(vl_seq.get()));
  ScopedType arr_vl(H5Tarray_create2(vl_seq.get(), 1, &kDim));
  ScopedType cmp_vl(H5Tcreate(H5T_COMPOUND, sizeof(hvl_t) + sizeof(int32_t)));
  ASSERT_TRUE(vl_nested.valid() && arr_vl.valid() && cmp_vl.valid());
  ASSERT_GE(H5Tinsert(cmp_vl.get(), "seq", 0, vl_seq.get()), 0);
  ASSERT_GE(H5Tinsert(cmp_vl.get(), "i", sizeof(hvl_t), H5T_NATIVE_INT32), 0);

  {
    // H5T_set_loc(..., H5T_LOC_DISK) invokes H5VL_file_get(), which requires an
    // active H5CX API context.
    H5CX_node_t cx_node{};
    ASSERT_GE(H5CX_push(&cx_node), 0);
    absl::Cleanup pop_cx = [] { H5CX_pop(false); };

    EXPECT_GE(H5T_set_loc(as_ptr(arr_vl.get()), f_latest_vol, H5T_LOC_DISK), 0);
    EXPECT_GE(H5T_set_loc(as_ptr(cmp_vl.get()), f_latest_vol, H5T_LOC_DISK), 0);
    EXPECT_GE(H5T_set_loc(as_ptr(vl_nested.get()), f_latest_vol, H5T_LOC_DISK),
              0);
    EXPECT_GE(H5T_patch_vlen_file(as_ptr(vl_seq.get()), f_early_vol), 0);
    EXPECT_GE(H5T_set_loc(as_ptr(arr_vl.get()), nullptr, H5T_LOC_MEMORY), 0);
    EXPECT_GE(H5T_set_loc(as_ptr(cmp_vl.get()), nullptr, H5T_LOC_MEMORY), 0);
    EXPECT_GE(H5T_set_loc(as_ptr(vl_nested.get()), nullptr, H5T_LOC_MEMORY), 0);
  }

  // Own VOL object twice to exercise replacement and release on close.
  ScopedType tmp_owned(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(tmp_owned.valid());
  EXPECT_GE(H5T_own_vol_obj(as_ptr(tmp_owned.get()), f_early_vol), 0);
  EXPECT_GE(H5T_own_vol_obj(as_ptr(tmp_owned.get()), f_latest_vol), 0);

  // Error checks on internal helpers.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5T_oloc(as_ptr(tmp_owned.get())), nullptr);
    EXPECT_EQ(H5T_nameof(as_ptr(tmp_owned.get())), nullptr);
    EXPECT_EQ(H5T_oloc(as_ptr(H5T_NATIVE_INT)), nullptr);
    EXPECT_EQ(H5T_nameof(as_ptr(H5T_NATIVE_INT)), nullptr);
    EXPECT_LT(H5T_set_version(f_early, as_ptr(cplx.get())), 0);
    EXPECT_LT(H5T_set_version(f_early, as_ptr(H5T_STD_REF)), 0);
  }
}

TEST(H5TTest, FileIoWithVlenCompoundArrayAndRefExercisesVolPathMatch) {
  // Closing a file after performing VLEN, nested-VLEN, ARRAY-of-VLEN, COMPOUND
  // with VLEN, and REFERENCE dataset I/O exercises H5T_unregister(...,
  // f->vol_obj, ...) and all branches of H5T_path_match_find_type_with_volobj().
  ScopedFile file = CreateInMemoryFile("t_vol_paths.h5");
  ASSERT_TRUE(file.valid());

  ScopedType vl_i32(H5Tvlen_create(H5T_NATIVE_INT32));
  ScopedType vl_nested(H5Tvlen_create(vl_i32.get()));
  constexpr hsize_t kArrDim = 2;
  ScopedType arr_vl(H5Tarray_create2(vl_i32.get(), 1, &kArrDim));
  struct CmpVl {
    int32_t tag;
    hvl_t seq;
  };
  ScopedType cmp_vl(H5Tcreate(H5T_COMPOUND, sizeof(CmpVl)));
  ASSERT_TRUE(vl_i32.valid() && vl_nested.valid() && arr_vl.valid() &&
              cmp_vl.valid());
  ASSERT_GE(H5Tinsert(cmp_vl.get(), "tag", offsetof(CmpVl, tag), H5T_NATIVE_INT32),
            0);
  ASSERT_GE(H5Tinsert(cmp_vl.get(), "seq", offsetof(CmpVl, seq), vl_i32.get()),
            0);

  constexpr hsize_t kDim = 1;
  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());

  // 1. Plain VLEN & VLEN-of-VLEN datasets.
  int32_t raw_vals[2] = {10, 20};
  hvl_t inner{2, raw_vals};
  hvl_t outer{1, &inner};
  ScopedDataset ds_vl(H5Dcreate2(file.get(), "ds_vl", vl_i32.get(), space.get(),
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds_vl.valid());
  ASSERT_GE(H5Dwrite(ds_vl.get(), vl_i32.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     &inner),
            0);

  ScopedDataset ds_vln(H5Dcreate2(file.get(), "ds_vln", vl_nested.get(),
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
  ASSERT_TRUE(ds_vln.valid());
  ASSERT_GE(H5Dwrite(ds_vln.get(), vl_nested.get(), H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, &outer),
            0);

  // 2. ARRAY of VLEN & COMPOUND of VLEN datasets.
  std::array<hvl_t, 2> arr_buf = {{{1, &raw_vals[0]}, {1, &raw_vals[1]}}};
  ScopedDataset ds_arr(H5Dcreate2(file.get(), "ds_arr", arr_vl.get(),
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
  ASSERT_TRUE(ds_arr.valid());
  ASSERT_GE(H5Dwrite(ds_arr.get(), arr_vl.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     arr_buf.data()),
            0);

  CmpVl cmp_val{99, {2, raw_vals}};
  ScopedDataset ds_cmp(H5Dcreate2(file.get(), "ds_cmp", cmp_vl.get(),
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
  ASSERT_TRUE(ds_cmp.valid());
  ASSERT_GE(H5Dwrite(ds_cmp.get(), cmp_vl.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     &cmp_val),
            0);

  // 3. New opaque reference dataset.
  H5R_ref_t ref{};
  ASSERT_GE(H5Rcreate_object(file.get(), "/ds_vl", H5P_DEFAULT, &ref), 0);
  absl::Cleanup destroy_ref = [&] { H5Rdestroy(&ref); };
  ScopedDataset ds_ref(H5Dcreate2(file.get(), "ds_ref", H5T_STD_REF,
                                  space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
  ASSERT_TRUE(ds_ref.valid());
  ASSERT_GE(H5Dwrite(ds_ref.get(), H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     &ref),
            0);

  // Close datasets before closing file; file.reset() triggers
  // H5T_unregister(..., f->vol_obj, ...).
  ds_ref.reset();
  ds_cmp.reset();
  ds_arr.reset();
  ds_vln.reset();
  ds_vl.reset();

  // Compound with VLEN member first followed by non-VL member exercises the
  // early break in H5T_path_match_find_type_with_volobj() when ret_value is
  // already true from member 0.
  struct VlFirst {
    hvl_t seq;
    int32_t tag;
  };
  ScopedType cmp_vl_first(H5Tcreate(H5T_COMPOUND, sizeof(VlFirst)));
  ASSERT_TRUE(cmp_vl_first.valid());
  ASSERT_GE(H5Tinsert(cmp_vl_first.get(), "seq", offsetof(VlFirst, seq),
                      vl_i32.get()),
            0);
  ASSERT_GE(H5Tinsert(cmp_vl_first.get(), "tag", offsetof(VlFirst, tag),
                      H5T_NATIVE_INT32),
            0);
  VlFirst vl_first_val{{2, raw_vals}, 77};
  ScopedDataset ds_vl_first(H5Dcreate2(file.get(), "ds_vl_first",
                                       cmp_vl_first.get(), space.get(),
                                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds_vl_first.valid());
  ASSERT_GE(H5Dwrite(ds_vl_first.get(), cmp_vl_first.get(), H5S_ALL, H5S_ALL,
                     H5P_DEFAULT, &vl_first_val),
            0);
  ds_vl_first.reset();
  file.reset();
}

TEST(H5TTest, LocSizeChangeShrinkErrorsAndDetectVlenRef) {
  auto as_ptr = [](hid_t id) {
    return static_cast<H5T_t*>(H5I_object_verify(id, H5I_DATATYPE));
  };

  ScopedPlist fapl_latest = MakeCoreFapl();
  ASSERT_TRUE(fapl_latest.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl_latest.get(), H5F_LIBVER_V200, H5F_LIBVER_LATEST),
      0);
  ScopedFile file_latest(H5Fcreate("t_loc_sz.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                    fapl_latest.get()));
  ASSERT_TRUE(file_latest.valid());
  H5VL_object_t* f_latest_vol =
      H5VL_vol_object_verify(file_latest.get(), H5I_FILE);
  ASSERT_NE(f_latest_vol, nullptr);

  H5CX_node_t cx_node{};
  ASSERT_GE(H5CX_push(&cx_node), 0);
  absl::Cleanup pop_cx = [] { H5CX_pop(false); };

  // 1. ARRAY of H5T_STD_REF (64B mem <-> ~19B disk) changes size on DISK and
  // back to MEMORY, exercising H5T_set_loc array & reference branches.
  constexpr hsize_t kDim = 2;
  ScopedType arr_ref2(H5Tarray_create2(H5T_STD_REF, 1, &kDim));
  ASSERT_TRUE(arr_ref2.valid());
  EXPECT_GT(H5T_set_loc(as_ptr(arr_ref2.get()), f_latest_vol, H5T_LOC_DISK), 0);
  EXPECT_LT(H5Tget_size(arr_ref2.get()), kDim * sizeof(H5R_ref_t));
  EXPECT_GT(H5T_set_loc(as_ptr(arr_ref2.get()), nullptr, H5T_LOC_MEMORY), 0);
  EXPECT_EQ(H5Tget_size(arr_ref2.get()), kDim * sizeof(H5R_ref_t));

  // 2. COMPOUND with H5T_STD_REF changes member & compound size on DISK, and
  // copying it while on DISK exercises size adjustment in H5T__complete_copy.
  ScopedType cmp_ref2(H5Tcreate(H5T_COMPOUND, sizeof(H5R_ref_t) + 4));
  ASSERT_TRUE(cmp_ref2.valid());
  ASSERT_GE(H5Tinsert(cmp_ref2.get(), "r", 0, H5T_STD_REF), 0);
  ASSERT_GE(
      H5Tinsert(cmp_ref2.get(), "tail", sizeof(H5R_ref_t), H5T_NATIVE_INT32),
      0);
  EXPECT_GT(H5T_set_loc(as_ptr(cmp_ref2.get()), f_latest_vol, H5T_LOC_DISK), 0);
  {
    ScopedType copied_from_disk(H5Tcopy(cmp_ref2.get()));
    ASSERT_TRUE(copied_from_disk.valid());
    EXPECT_EQ(H5Tget_size(copied_from_disk.get()), sizeof(H5R_ref_t) + 4);
  }
  EXPECT_GT(H5T_set_loc(as_ptr(cmp_ref2.get()), nullptr, H5T_LOC_MEMORY), 0);

  // 3. COMPLEX with force_conv=true hits the atomic/default switch break.
  ScopedType cplx(H5Tcomplex_create(H5T_NATIVE_FLOAT));
  ASSERT_TRUE(cplx.valid());
  H5T_t* cplx_ptr = as_ptr(cplx.get());
  ASSERT_NE(cplx_ptr, nullptr);
  {
    cplx_ptr->shared->force_conv = true;
    absl::Cleanup restore_fc = [&] { cplx_ptr->shared->force_conv = false; };
    EXPECT_EQ(H5T_set_loc(cplx_ptr, f_latest_vol, H5T_LOC_DISK), 0);
  }

  // 4. Shrink/overflow guards in H5T_set_loc() for compound member offset,
  // zero old_size, and compound total size.
  {
    ScopedType cmp_off_neg(H5Tcreate(H5T_COMPOUND, sizeof(H5R_ref_t) + 4));
    ASSERT_TRUE(cmp_off_neg.valid());
    ASSERT_GE(H5Tinsert(cmp_off_neg.get(), "r", 0, H5T_STD_REF), 0);
    ASSERT_GE(H5Tinsert(cmp_off_neg.get(), "tail", sizeof(H5R_ref_t),
                        H5T_NATIVE_INT32),
              0);
    H5T_t* cp = as_ptr(cmp_off_neg.get());
    const size_t orig_off = cp->shared->u.compnd.memb[1].offset;
    cp->shared->u.compnd.memb[1].offset = static_cast<size_t>(-100);
    absl::Cleanup restore_off = [&] {
      cp->shared->u.compnd.memb[1].offset = orig_off;
    };
    ScopedErrorSilence silence;
    EXPECT_LT(H5T_set_loc(cp, f_latest_vol, H5T_LOC_DISK), 0);
  }

  {
    ScopedType cmp_sz_neg(H5Tcreate(H5T_COMPOUND, sizeof(H5R_ref_t)));
    ASSERT_TRUE(cmp_sz_neg.valid());
    ASSERT_GE(H5Tinsert(cmp_sz_neg.get(), "r", 0, H5T_STD_REF), 0);
    H5T_t* cp = as_ptr(cmp_sz_neg.get());
    const size_t orig_sz = cp->shared->size;
    cp->shared->size = static_cast<size_t>(-100);
    absl::Cleanup restore_sz = [&] { cp->shared->size = orig_sz; };
    ScopedErrorSilence silence;
    EXPECT_LT(H5T_set_loc(cp, f_latest_vol, H5T_LOC_DISK), 0);
  }

  {
    ScopedType vl_str(H5Tcopy(H5T_C_S1));
    ASSERT_TRUE(vl_str.valid());
    ASSERT_GE(H5Tset_size(vl_str.get(), H5T_VARIABLE), 0);
    ScopedType cmp_zero_old(H5Tcreate(H5T_COMPOUND, sizeof(char*)));
    ASSERT_TRUE(cmp_zero_old.valid());
    ASSERT_GE(H5Tinsert(cmp_zero_old.get(), "s", 0, vl_str.get()), 0);
    H5T_t* cp = as_ptr(cmp_zero_old.get());
    H5T_t* memb0 = cp->shared->u.compnd.memb[0].type;
    const size_t orig_memb_sz = memb0->shared->size;
    memb0->shared->size = 0;
    absl::Cleanup restore_msz = [&] {
      if (memb0->shared->size == 0) memb0->shared->size = orig_memb_sz;
    };
    ScopedErrorSilence silence;
    EXPECT_LT(H5T_set_loc(cp, f_latest_vol, H5T_LOC_DISK), 0);
  }

  // 5. H5T__detect_vlen_ref() non-vlen compound falls through loop break.
  ScopedType cmp_no_vref(H5Tcreate(H5T_COMPOUND, sizeof(H5R_ref_t) + 4));
  ASSERT_TRUE(cmp_no_vref.valid());
  ASSERT_GE(H5Tinsert(cmp_no_vref.get(), "i", 0, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(cmp_no_vref.get(), "r", 4, H5T_STD_REF), 0);
  EXPECT_EQ(H5T_is_vl_storage(as_ptr(cmp_no_vref.get())), 0);
}

TEST(H5TTest, VersionUpgradeErrorAndVolCommittedConvert) {
  auto as_ptr = [](hid_t id) {
    return static_cast<H5T_t*>(H5I_object_verify(id, H5I_DATATYPE));
  };

  ScopedPlist fapl_early = MakeCoreFapl();
  ASSERT_TRUE(fapl_early.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl_early.get(), H5F_LIBVER_EARLIEST,
                                 H5F_LIBVER_V18),
            0);
  ScopedFile file_early(
      H5Fcreate("t_ver_early.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_early.get()));
  ASSERT_TRUE(file_early.valid());

  ScopedPlist fapl_v18 = MakeCoreFapl();
  ASSERT_TRUE(fapl_v18.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl_v18.get(), H5F_LIBVER_V18, H5F_LIBVER_V18), 0);
  ScopedFile file_v18(
      H5Fcreate("t_ver_v18.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_v18.get()));
  ASSERT_TRUE(file_v18.valid());

  ScopedPlist fapl_latest = MakeCoreFapl();
  ASSERT_TRUE(fapl_latest.valid());
  ASSERT_GE(
      H5Pset_libver_bounds(fapl_latest.get(), H5F_LIBVER_V200, H5F_LIBVER_LATEST),
      0);
  ScopedFile file_latest(H5Fcreate("t_ver_latest.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                                    fapl_latest.get()));
  ASSERT_TRUE(file_latest.valid());

  auto* f_early =
      static_cast<H5F_t*>(H5VL_object_verify(file_early.get(), H5I_FILE));
  auto* f_v18 =
      static_cast<H5F_t*>(H5VL_object_verify(file_v18.get(), H5I_FILE));
  auto* f_latest =
      static_cast<H5F_t*>(H5VL_object_verify(file_latest.get(), H5I_FILE));
  ASSERT_NE(f_early, nullptr);
  ASSERT_NE(f_v18, nullptr);
  ASSERT_NE(f_latest, nullptr);

  // 1. Complex inherits higher parent version in H5T__upgrade_version_cb.
  ScopedType cplx(H5Tcomplex_create(H5T_NATIVE_FLOAT));
  ASSERT_TRUE(cplx.valid());
  H5T_t* cplx_ptr = as_ptr(cplx.get());
  ASSERT_NE(cplx_ptr, nullptr);
  {
    const unsigned orig_ver = cplx_ptr->shared->version;
    const unsigned orig_pver = cplx_ptr->shared->parent->shared->version;
    absl::Cleanup restore_vers = [&] {
      cplx_ptr->shared->version = orig_ver;
      cplx_ptr->shared->parent->shared->version = orig_pver;
    };
    // f_v18 has LOW=3, HIGH=3: upgrades version 1 -> parent version 4, then
    // rejects version 4 > HIGH(3).
    cplx_ptr->shared->version = H5O_DTYPE_VERSION_1;
    cplx_ptr->shared->parent->shared->version = H5O_DTYPE_VERSION_4;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5T_set_version(f_v18, cplx_ptr), 0);
      EXPECT_EQ(cplx_ptr->shared->version, H5O_DTYPE_VERSION_4);
    }
    // f_latest allows VERSION_5: succeeds and retains upgraded parent version.
    cplx_ptr->shared->version = H5O_DTYPE_VERSION_1;
    cplx_ptr->shared->parent->shared->version = H5O_DTYPE_VERSION_5;
    EXPECT_GE(H5T_set_version(f_latest, cplx_ptr), 0);
    EXPECT_EQ(cplx_ptr->shared->version, H5O_DTYPE_VERSION_5);
  }

  // 2. Corrupted parent class hits H5T__upgrade_version() + H5T_set_version()
  // error propagation cleanly and restores original class before close.
  constexpr hsize_t kDim = 2;
  ScopedType arr_i32(H5Tarray_create2(H5T_NATIVE_INT32, 1, &kDim));
  ASSERT_TRUE(arr_i32.valid());
  H5T_t* arr_ptr = as_ptr(arr_i32.get());
  ASSERT_NE(arr_ptr, nullptr);
  {
    const H5T_class_t orig_ptype = arr_ptr->shared->parent->shared->type;
    const unsigned orig_ver = arr_ptr->shared->version;
    absl::Cleanup restore_arr = [&] {
      arr_ptr->shared->parent->shared->type = orig_ptype;
      arr_ptr->shared->version = orig_ver;
    };
    arr_ptr->shared->version = H5O_DTYPE_VERSION_1;
    arr_ptr->shared->parent->shared->type = H5T_NO_CLASS;
    ScopedErrorSilence silence;
    EXPECT_LT(H5T_set_version(f_latest, arr_ptr), 0);
  }

  // 3. H5T_convert_committed_datatype() with non-null dt->vol_obj.
  ScopedType c_main(H5Tcopy(H5T_NATIVE_INT32));
  ScopedType c_holder(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(c_main.valid() && c_holder.valid());
  ASSERT_GE(H5Tcommit2(file_latest.get(), "c_main", c_main.get(), H5P_DEFAULT,
                      H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Tcommit2(file_latest.get(), "c_holder", c_holder.get(),
                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);
  H5T_t* main_outer = as_ptr(c_main.get());
  H5T_t* holder_outer = as_ptr(c_holder.get());
  ASSERT_NE(main_outer->vol_obj, nullptr);
  ASSERT_NE(holder_outer->vol_obj, nullptr);
  auto* main_vol_dt = static_cast<H5T_t*>(H5VL_object_data(main_outer->vol_obj));
  ASSERT_NE(main_vol_dt, nullptr);

  H5T_t* copy_vol = H5T_copy(main_vol_dt, H5T_COPY_ALL);
  ASSERT_NE(copy_vol, nullptr);
  // Transfer holder_outer's VOL object so copy_vol has non-null vol_obj, then
  // convert to f_early; H5T_convert_committed_datatype closes and nulls it.
  copy_vol->vol_obj = holder_outer->vol_obj;
  holder_outer->vol_obj = nullptr;
  {
    H5CX_node_t cx_node{};
    ASSERT_GE(H5CX_push(&cx_node), 0);
    absl::Cleanup pop_cx = [] { H5CX_pop(false); };
    EXPECT_GE(H5T_convert_committed_datatype(copy_vol, f_early), 0);
  }
  EXPECT_EQ(copy_vol->vol_obj, nullptr);
  EXPECT_EQ(copy_vol->shared->state, H5T_STATE_TRANSIENT);
  EXPECT_GE(H5T_close_real(copy_vol), 0);
}

TEST(H5TTest, CopyReopenMultiFileCorkAndErrorPaths) {
  auto as_ptr = [](hid_t id) {
    return static_cast<H5T_t*>(H5I_object_verify(id, H5I_DATATYPE));
  };

  // 1. Multi-file open on same physical file exercises H5FO_top_count == 0
  // branches in both H5T_copy_reopen() and H5T_close(), plus uncorking in close.
  TempFile tmp_file("t_multi_open.h5");
  {
    ScopedFile f1(H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT,
                            H5P_DEFAULT));
    ASSERT_TRUE(f1.valid());

    ScopedType c_type(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_TRUE(c_type.valid());
    ASSERT_GE(H5Tcommit2(f1.get(), "c", c_type.get(), H5P_DEFAULT, H5P_DEFAULT,
                        H5P_DEFAULT),
              0);
    constexpr hsize_t kDim = 1;
    ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset ds1(H5Dcreate2(f1.get(), "ds", c_type.get(), space.get(),
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ds1.valid());

    // Commit and cork a second datatype; closing it hits H5AC__UNCORK.
    ScopedType c_cork(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_TRUE(c_cork.valid());
    ASSERT_GE(H5Tcommit2(f1.get(), "c_cork", c_cork.get(), H5P_DEFAULT,
                        H5P_DEFAULT, H5P_DEFAULT),
              0);
    EXPECT_GE(H5Odisable_mdc_flushes(c_cork.get()), 0);
    c_cork.reset();

    ds1.reset();
    c_type.reset();

    ScopedType c1(H5Topen2(f1.get(), "c", H5P_DEFAULT));
    ASSERT_TRUE(c1.valid());

    ScopedFile f2(H5Fopen(tmp_file.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
    ASSERT_TRUE(f2.valid());
    ScopedDataset ds2(H5Dopen2(f2.get(), "ds", H5P_DEFAULT));
    ASSERT_TRUE(ds2.valid());

    // c1 is open on f1 (sharing f2's H5F_shared_t), but top_count on f2 is 0.
    ScopedType c2(H5Dget_type(ds2.get()));
    ASSERT_TRUE(c2.valid());
    EXPECT_GT(H5Tequal(c2.get(), H5T_NATIVE_INT32), 0);

    // Closing c2 decrements top_count on f2 back to 0 while c1 remains open.
    c2.reset();
    ds2.reset();
    f2.reset();
    c1.reset();
    f1.reset();
  }

  // 2. Empty enum copy, H5T_copy_reopen on IMMUTABLE, invalid bad_ref wrapped in
  // owned array to exercise recursive copy/reopen error & VOL cleanup paths.
  ScopedType enm_empty(H5Tenum_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(enm_empty.valid());
  ScopedType enm_empty_copy(H5Tcopy(enm_empty.get()));
  ASSERT_TRUE(enm_empty_copy.valid());
  EXPECT_EQ(H5Tget_nmembers(enm_empty_copy.get()), 0);

  H5T_t* reopened_imm = H5T_copy_reopen(as_ptr(H5T_NATIVE_INT));
  ASSERT_NE(reopened_imm, nullptr);
  EXPECT_EQ(reopened_imm->shared->state, H5T_STATE_RDONLY);
  EXPECT_GE(H5T_close_real(reopened_imm), 0);

  ScopedFile f_mem = CreateInMemoryFile("t_bad_ref_copy.h5");
  ASSERT_TRUE(f_mem.valid());
  H5VL_object_t* f_mem_vol = H5VL_vol_object_verify(f_mem.get(), H5I_FILE);
  ASSERT_NE(f_mem_vol, nullptr);

  H5T_t* bad_ref = H5T_copy(as_ptr(H5T_STD_REF), H5T_COPY_ALL);
  ASSERT_NE(bad_ref, nullptr);
  ASSERT_GE(H5T_own_vol_obj(bad_ref, f_mem_vol), 0);
  bad_ref->shared->u.atomic.u.r.rtype = H5R_BADTYPE;
  bad_ref->shared->u.atomic.u.r.opaque = false;
  bad_ref->shared->u.atomic.u.r.loc = H5T_LOC_DISK;
  bad_ref->shared->force_conv = true;

  constexpr hsize_t kDim = 1;
  ScopedType bad_arr(H5Tarray_create2(H5T_NATIVE_INT32, 1, &kDim));
  ASSERT_TRUE(bad_arr.valid());
  H5T_t* bad_arr_ptr = as_ptr(bad_arr.get());
  ASSERT_GE(H5T_own_vol_obj(bad_arr_ptr, f_mem_vol), 0);
  H5T_t* orig_arr_parent = bad_arr_ptr->shared->parent;
  bad_arr_ptr->shared->parent = bad_ref;
  {
    absl::Cleanup restore_bad = [&] {
      bad_arr_ptr->shared->parent = orig_arr_parent;
      EXPECT_GE(H5T_close_real(bad_ref), 0);
    };
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Tcopy(bad_arr.get()), H5I_INVALID_HID);
    EXPECT_EQ(H5T_copy_reopen(bad_arr_ptr), nullptr);
    EXPECT_EQ(H5T_copy_reopen(bad_ref), nullptr);
  }

  // 3. Immutable ID/child/parent teardown error paths & invalid state switch.
  ScopedType imm_dt(H5Tcreate(H5T_INTEGER, 4));
  ASSERT_TRUE(imm_dt.valid());
  H5T_t* imm_ptr = as_ptr(imm_dt.get());
  imm_ptr->shared->state = H5T_STATE_IMMUTABLE;
  {
    ScopedErrorSilence silence;
    // Directly invoke ID close callback path without H5Tclose() public check.
    EXPECT_LT(H5Idec_ref(imm_dt.get()), 0);
  }
  imm_ptr->shared->state = H5T_STATE_TRANSIENT;

  ScopedType cmp_imm_child(H5Tcreate(H5T_COMPOUND, 4));
  ASSERT_TRUE(cmp_imm_child.valid());
  ASSERT_GE(H5Tinsert(cmp_imm_child.get(), "a", 0, H5T_NATIVE_INT32), 0);
  H5T_t* cmp_ptr = as_ptr(cmp_imm_child.get());
  H5T_t* child0 = cmp_ptr->shared->u.compnd.memb[0].type;
  child0->shared->state = H5T_STATE_IMMUTABLE;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tclose(cmp_imm_child.get()), 0);
  }
  // H5T__free left member array & child0 unfreed after failing on child0; restore
  // child0 state and release both cleanly.
  child0->shared->state = H5T_STATE_TRANSIENT;
  EXPECT_GE(H5T_close_real(child0), 0);
  cmp_imm_child.reset();

  ScopedType arr_imm_parent(H5Tarray_create2(H5T_NATIVE_INT32, 1, &kDim));
  ASSERT_TRUE(arr_imm_parent.valid());
  H5T_t* arr_imm_ptr = as_ptr(arr_imm_parent.get());
  H5T_t* parent0 = arr_imm_ptr->shared->parent;
  parent0->shared->state = H5T_STATE_IMMUTABLE;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tclose(arr_imm_parent.get()), 0);
  }
  parent0->shared->state = H5T_STATE_TRANSIENT;
  EXPECT_GE(H5T_close_real(parent0), 0);
  arr_imm_parent.reset();

  // Invalid state enum values trigger default error branches in lock/oloc/nameof.
  ScopedType bad_state_dt(H5Tcreate(H5T_INTEGER, 4));
  ASSERT_TRUE(bad_state_dt.valid());
  H5T_t* bs_ptr = as_ptr(bad_state_dt.get());
  {
    bs_ptr->shared->state = static_cast<H5T_state_t>(99);
    absl::Cleanup restore_st = [&] { bs_ptr->shared->state = H5T_STATE_TRANSIENT; };
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tlock(bad_state_dt.get()), 0);
    EXPECT_EQ(H5T_oloc(bs_ptr), nullptr);
    EXPECT_EQ(H5T_nameof(bs_ptr), nullptr);
  }

  // VL string with unshrinkable float parent triggers H5T__set_size parent error.
  ScopedType vl_s(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vl_s.valid());
  ASSERT_GE(H5Tset_size(vl_s.get(), H5T_VARIABLE), 0);
  ScopedType flt_p(H5Tcopy(H5T_IEEE_F32LE));
  ASSERT_TRUE(flt_p.valid());
  H5T_t* vl_s_ptr = as_ptr(vl_s.get());
  H5T_t* orig_vl_parent = vl_s_ptr->shared->parent;
  H5T_t* flt_ptr = as_ptr(flt_p.get());
  {
    vl_s_ptr->shared->parent = flt_ptr;
    absl::Cleanup restore_vp = [&] { vl_s_ptr->shared->parent = orig_vl_parent; };
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tset_size(vl_s.get(), 1), 0);
  }
}

namespace {

int g_fail_conv_cmd = 0;

herr_t FailFreeConv(hid_t /*src_id*/, hid_t /*dst_id*/, H5T_cdata_t* cdata,
                    size_t /*nelmts*/, size_t /*buf_stride*/,
                    size_t /*bkg_stride*/, void* /*buf*/, void* /*bkg*/,
                    hid_t /*dxpl_id*/) {
  if (cdata->command == H5T_CONV_FREE) {
    ++g_fail_conv_cmd;
    return -1;
  }
  return 0;
}

herr_t FailConvExec(hid_t /*src_id*/, hid_t /*dst_id*/, H5T_cdata_t* cdata,
                    size_t /*nelmts*/, size_t /*buf_stride*/,
                    size_t /*bkg_stride*/, void* /*buf*/, void* /*bkg*/,
                    hid_t /*dxpl_id*/) {
  if (cdata->command == H5T_CONV_CONV) {
    return -1;
  }
  return 0;
}

struct RecursiveConvCtx {
  hid_t extra_src = H5I_INVALID_HID;
  hid_t extra_dst = H5I_INVALID_HID;
} g_rec_conv_ctx;

herr_t RecursiveInitConv(hid_t /*src_id*/, hid_t /*dst_id*/,
                         H5T_cdata_t* cdata, size_t /*nelmts*/,
                         size_t /*buf_stride*/, size_t /*bkg_stride*/,
                         void* /*buf*/, void* /*bkg*/, hid_t /*dxpl_id*/) {
  if (cdata->command == H5T_CONV_INIT &&
      g_rec_conv_ctx.extra_src != H5I_INVALID_HID &&
      g_rec_conv_ctx.extra_dst != H5I_INVALID_HID) {
    // Inserting another path inside INIT changes H5T_g.npaths during
    // H5T__path_find_real(), exercising the table re-search branch.
    return H5Tregister(H5T_PERS_HARD, "rec_nested_conv",
                       g_rec_conv_ctx.extra_src, g_rec_conv_ctx.extra_dst,
                       FailConvExec);
  }
  return 0;
}

}  // namespace

TEST(H5TTest, EncodeVaxUnregisterAndConvFailurePaths) {
  // 1. VAX byte order cannot be encoded by H5Odtype -> hits H5T_encode /
  // H5Tencode error paths without needing corrupt buffers.
  ScopedType vax_i32(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(vax_i32.valid());
  ASSERT_GE(H5Tset_order(vax_i32.get(), H5T_ORDER_VAX), 0);
  std::array<uint8_t, 64> enc_buf{};
  size_t enc_sz = enc_buf.size();
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tencode(vax_i32.get(), enc_buf.data(), &enc_sz), 0);
  }

  // 2. Soft unregister filter mismatches on src/dst/func, path-free error
  // ignoring, recursive table insertion during INIT, and CONV/ABORT failures.
  ScopedType opq1(H5Tcreate(H5T_OPAQUE, 4));
  ScopedType opq2(H5Tcreate(H5T_OPAQUE, 4));
  ScopedType opq3(H5Tcreate(H5T_OPAQUE, 4));
  ScopedType opq4(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(opq1.valid() && opq2.valid() && opq3.valid() && opq4.valid());
  ASSERT_GE(H5Tset_tag(opq1.get(), "fail_u1"), 0);
  ASSERT_GE(H5Tset_tag(opq2.get(), "fail_u2"), 0);
  ASSERT_GE(H5Tset_tag(opq3.get(), "fail_u3"), 0);
  ASSERT_GE(H5Tset_tag(opq4.get(), "fail_u4"), 0);

  g_rec_conv_ctx = {opq3.get(), opq4.get()};
  g_fail_conv_cmd = 0;

  absl::Cleanup cleanup_all = [&] {
    g_rec_conv_ctx = {H5I_INVALID_HID, H5I_INVALID_HID};
    H5Tunregister(H5T_PERS_DONTCARE, "soft_fail_free", H5I_INVALID_HID,
                  H5I_INVALID_HID, FailFreeConv);
    H5Tunregister(H5T_PERS_DONTCARE, "hard_fail_free", H5I_INVALID_HID,
                  H5I_INVALID_HID, FailFreeConv);
    H5Tunregister(H5T_PERS_DONTCARE, "hard_fail_conv", H5I_INVALID_HID,
                  H5I_INVALID_HID, FailConvExec);
    H5Tunregister(H5T_PERS_DONTCARE, "rec_outer_conv", H5I_INVALID_HID,
                  H5I_INVALID_HID, RecursiveInitConv);
    H5Tunregister(H5T_PERS_DONTCARE, "rec_nested_conv", H5I_INVALID_HID,
                  H5I_INVALID_HID, FailConvExec);
  };

  ASSERT_GE(H5Tregister(H5T_PERS_SOFT, "soft_fail_free", opq1.get(), opq2.get(),
                        FailFreeConv),
            0);
  // Exercise non-matching src type, non-matching dst type, and non-matching
  // func in H5T_unregister()'s soft list scan.
  EXPECT_GE(H5Tunregister(H5T_PERS_SOFT, nullptr, H5T_NATIVE_INT,
                          H5I_INVALID_HID, nullptr),
            0);
  EXPECT_GE(H5Tunregister(H5T_PERS_SOFT, nullptr, opq1.get(), H5T_NATIVE_INT,
                          nullptr),
            0);
  EXPECT_GE(H5Tunregister(H5T_PERS_SOFT, nullptr, opq1.get(), opq2.get(),
                          TrackedCustomConvB),
            0);
  EXPECT_GE(H5Tunregister(H5T_PERS_SOFT, "soft_fail_free", opq1.get(),
                          opq2.get(), FailFreeConv),
            0);

  // Replace hard path whose INIT inserts another hard path into the table.
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "hard_fail_free", opq1.get(), opq2.get(),
                        FailFreeConv),
            0);
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "rec_outer_conv", opq1.get(), opq2.get(),
                        RecursiveInitConv),
            0);
  // Previous path failed H5T_CONV_FREE cleanly (error ignored and stack cleared).
  EXPECT_EQ(g_fail_conv_cmd, 1);

  // App conversion returning -1 triggers H5T_convert_with_ctx() / H5T_convert()
  // / H5Tconvert() error returns.
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "hard_fail_conv", opq1.get(), opq2.get(),
                        FailConvExec),
            0);
  std::array<uint8_t, 4> opq_buf{};
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tconvert(opq1.get(), opq2.get(), 1, opq_buf.data(), nullptr,
                         H5P_DEFAULT),
              0);
  }

  // Built-in lib conversion returning H5T_CONV_ABORT from exception callback.
  ScopedPlist abort_dxpl(H5Pcreate(H5P_DATASET_XFER));
  ASSERT_TRUE(abort_dxpl.valid());
  auto abort_cb = [](H5T_conv_except_t, hid_t, hid_t, void*, void*,
                     void*) -> H5T_conv_ret_t { return H5T_CONV_ABORT; };
  ASSERT_GE(H5Pset_type_conv_cb(abort_dxpl.get(), abort_cb, nullptr), 0);
  uint8_t int_buf[sizeof(int64_t)]{};
  int64_t big_i64 = INT64_MAX;
  std::memcpy(int_buf, &big_i64, sizeof(big_i64));
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tconvert(H5T_NATIVE_INT64, H5T_NATIVE_INT8, 1, int_buf, nullptr,
                         abort_dxpl.get()),
              0);
  }
}

namespace {

struct MockVolState {
  bool fail_dset_get = false;
  bool fail_file_get = false;
  bool fail_dt_close = false;
  bool fail_terminate = false;
  bool return_async_req = false;
  hid_t dset_get_type_ret = H5I_INVALID_HID;
  int dummy_file_obj = 1;
  int dummy_dset_obj = 2;
  int dummy_dt_obj = 3;
  int dummy_req_obj = 4;
} g_mock_vol;

extern const H5VL_class_t kMockVolClass;

herr_t MockTerminate() { return g_mock_vol.fail_terminate ? -1 : 0; }

void* MockFileCreate(const char*, unsigned, hid_t, hid_t, hid_t, void**) {
  return &g_mock_vol.dummy_file_obj;
}

herr_t MockFileGet(void*, H5VL_file_get_args_t* args, hid_t, void**) {
  if (g_mock_vol.fail_file_get) {
    return -1;
  }
  if (args->op_type == H5VL_FILE_GET_CONT_INFO) {
    args->args.get_cont_info.info->feature_flags = 0;
    args->args.get_cont_info.info->token_size = sizeof(H5O_token_t);
    args->args.get_cont_info.info->blob_id_size = 8;
    return 0;
  }
  return -1;
}

herr_t MockFileClose(void*, hid_t, void**) { return 0; }

void* MockDatasetCreate(void*, const H5VL_loc_params_t*, const char*, hid_t,
                        hid_t, hid_t, hid_t, hid_t, hid_t, void**) {
  return &g_mock_vol.dummy_dset_obj;
}

herr_t MockDatasetGet(void*, H5VL_dataset_get_args_t* args, hid_t, void**) {
  if (g_mock_vol.fail_dset_get) {
    return -1;
  }
  if (args->op_type == H5VL_DATASET_GET_TYPE) {
    args->args.get_type.type_id = g_mock_vol.dset_get_type_ret;
    return 0;
  }
  return -1;
}

herr_t MockDatasetClose(void*, hid_t, void**) { return 0; }

void* MockDatatypeCommit(void*, const H5VL_loc_params_t*, const char*, hid_t,
                         hid_t, hid_t, hid_t, hid_t, void**) {
  return &g_mock_vol.dummy_dt_obj;
}

herr_t MockDatatypeClose(void*, hid_t, void** req) {
  if (g_mock_vol.fail_dt_close) {
    return -1;
  }
  if (req != nullptr && g_mock_vol.return_async_req) {
    *req = &g_mock_vol.dummy_req_obj;
  }
  return 0;
}

herr_t MockGetConnCls(void*, H5VL_get_conn_lvl_t lvl,
                      const H5VL_class_t** conn_cls) {
  if (lvl == H5VL_GET_CONN_LVL_CURR || lvl == H5VL_GET_CONN_LVL_TERM) {
    *conn_cls = &kMockVolClass;
    return 0;
  }
  return -1;
}

herr_t MockGetCapFlags(const void*, uint64_t* cap_flags) {
  *cap_flags = H5VL_CAP_FLAG_ASYNC;
  return 0;
}

herr_t MockOptQuery(void*, H5VL_subclass_t, int, uint64_t* flags) {
  *flags = 0;
  return 0;
}

herr_t MockRequestFree(void*) { return 0; }

const H5VL_class_t kMockVolClass = {
    /*version=*/H5VL_VERSION,
    /*value=*/static_cast<H5VL_class_value_t>(510),
    /*name=*/"h5t_test_mock_vol",
    /*conn_version=*/1,
    /*cap_flags=*/H5VL_CAP_FLAG_ASYNC,
    /*initialize=*/nullptr,
    /*terminate=*/MockTerminate,
    /*info_cls=*/{},
    /*wrap_cls=*/{},
    /*attr_cls=*/{},
    /*dataset_cls=*/
    {
        /*create=*/MockDatasetCreate,
        /*open=*/nullptr,
        /*read=*/nullptr,
        /*write=*/nullptr,
        /*get=*/MockDatasetGet,
        /*specific=*/nullptr,
        /*optional=*/nullptr,
        /*close=*/MockDatasetClose,
    },
    /*datatype_cls=*/
    {
        /*commit=*/MockDatatypeCommit,
        /*open=*/nullptr,
        /*get=*/nullptr,
        /*specific=*/nullptr,
        /*optional=*/nullptr,
        /*close=*/MockDatatypeClose,
    },
    /*file_cls=*/
    {
        /*create=*/MockFileCreate,
        /*open=*/nullptr,
        /*get=*/MockFileGet,
        /*specific=*/nullptr,
        /*optional=*/nullptr,
        /*close=*/MockFileClose,
    },
    /*group_cls=*/{},
    /*link_cls=*/{},
    /*object_cls=*/{},
    /*introspect_cls=*/
    {
        /*get_conn_cls=*/MockGetConnCls,
        /*get_cap_flags=*/MockGetCapFlags,
        /*opt_query=*/MockOptQuery,
    },
    /*request_cls=*/
    {
        /*wait=*/nullptr,
        /*notify=*/nullptr,
        /*cancel=*/nullptr,
        /*specific=*/nullptr,
        /*optional=*/nullptr,
        /*free=*/MockRequestFree,
    },
    /*blob_cls=*/{},
    /*token_cls=*/{},
    /*optional=*/nullptr,
};

}  // namespace

TEST(H5TTest, CustomVolDatatypeErrorsAndAsyncClose) {
  g_mock_vol = {};
  hid_t vol_id = H5VLregister_connector(&kMockVolClass, H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  absl::Cleanup unreg_vol = [&] { EXPECT_GE(H5VLunregister_connector(vol_id), 0); };

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_vol(fapl.get(), vol_id, nullptr), 0);

  ScopedFile mock_file(
      H5Fcreate("mock.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(mock_file.valid());
  H5VL_object_t* mock_file_vol = H5VL_vol_object_verify(mock_file.get(), H5I_FILE);
  ASSERT_NE(mock_file_vol, nullptr);

  auto as_ptr = [](hid_t id) {
    return static_cast<H5T_t*>(H5I_object_verify(id, H5I_DATATYPE));
  };

  // 1. H5Tcopy(dataset) error branches via MockDatasetGet:
  //    a) H5VL_dataset_get returns < 0
  //    b) returned dset_tid is H5I_INVALID_HID -> H5I_object fails cleanly
  //    c) H5T_copy(dt, H5T_COPY_TRANSIENT) fails on bad_ref inside dataset type
  constexpr hsize_t kDim = 1;
  ScopedSpace space(H5Screate_simple(1, &kDim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset mock_ds(H5Dcreate2(mock_file.get(), "ds", H5T_NATIVE_INT32,
                                   space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
  ASSERT_TRUE(mock_ds.valid());

  {
    ScopedErrorSilence silence;
    g_mock_vol.fail_dset_get = true;
    EXPECT_EQ(H5Tcopy(mock_ds.get()), H5I_INVALID_HID);
    g_mock_vol.fail_dset_get = false;

    g_mock_vol.dset_get_type_ret = H5I_INVALID_HID;
    EXPECT_EQ(H5Tcopy(mock_ds.get()), H5I_INVALID_HID);

    // Register bad_ref in a transient ID (so MockDatasetGet hands a valid ID
    // whose underlying H5T_t* fails H5T_copy(..., H5T_COPY_TRANSIENT)).
    H5T_t* ds_bad_ref = H5T_copy(as_ptr(H5T_STD_REF), H5T_COPY_ALL);
    ASSERT_NE(ds_bad_ref, nullptr);
    ds_bad_ref->shared->u.atomic.u.r.rtype = H5R_BADTYPE;
    ds_bad_ref->shared->u.atomic.u.r.opaque = false;
    ds_bad_ref->shared->u.atomic.u.r.loc = H5T_LOC_DISK;
    ds_bad_ref->shared->force_conv = true;
    hid_t bad_tid = H5I_register(H5I_DATATYPE, ds_bad_ref, true);
    ASSERT_GE(bad_tid, 0);
    // Transfer app ref via dset_get_type_ret: H5Tcopy calls H5I_dec_app_ref on
    // bad_tid, closing bad_tid and freeing ds_bad_ref automatically!
    g_mock_vol.dset_get_type_ret = bad_tid;
    EXPECT_EQ(H5Tcopy(mock_ds.get()), H5I_INVALID_HID);
  }
  mock_ds.reset();

  // 2. H5Tclose / H5Tclose_async failure when VOL datatype close fails.
  ScopedType mock_dt1(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(mock_dt1.valid());
  ASSERT_GE(H5Tcommit2(mock_file.get(), "dt1", mock_dt1.get(), H5P_DEFAULT,
                      H5P_DEFAULT, H5P_DEFAULT),
            0);
  ScopedEventSet es(H5EScreate());
  ASSERT_TRUE(es.valid());
  {
    g_mock_vol.fail_dt_close = true;
    absl::Cleanup restore_close = [&] {
      g_mock_vol.fail_dt_close = false;
      mock_dt1.reset();
    };
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tclose(mock_dt1.get()), 0);
    EXPECT_LT(H5Tclose_async(mock_dt1.get(), es.get()), 0);
  }

  // 3. H5Tclose_async non-null token + H5ES_insert failure: keep vol_obj alive
  // across the call via H5VL_object_inc_rc so H5VL_OBJ_CONNECTOR(vol_obj) after
  // H5I_dec_app_ref_async never touches freed memory.
  {
    ScopedType mock_dt2(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_TRUE(mock_dt2.valid());
    ASSERT_GE(H5Tcommit2(mock_file.get(), "dt2", mock_dt2.get(), H5P_DEFAULT,
                        H5P_DEFAULT, H5P_DEFAULT),
              0);
    H5T_t* dt2_ptr = as_ptr(mock_dt2.get());
    H5VL_object_t* saved_vol_obj = dt2_ptr->vol_obj;
    ASSERT_NE(saved_vol_obj, nullptr);
    H5VL_object_inc_rc(saved_vol_obj);
    absl::Cleanup release_saved = [&] {
      g_mock_vol.return_async_req = false;
      EXPECT_GE(H5VL_free_object(saved_vol_obj), 0);
    };
    g_mock_vol.return_async_req = true;
    ScopedErrorSilence silence;
    hid_t raw_id = mock_dt2.release();
    EXPECT_LT(H5Tclose_async(raw_id, H5I_INVALID_HID), 0);
  }

  // 4. H5T_convert_committed_datatype VOL close failure branch.
  {
    H5CX_node_t cx_node{};
    ASSERT_GE(H5CX_push(&cx_node), 0);
    absl::Cleanup pop_cx = [] { H5CX_pop(false); };

    ScopedFile f_core = CreateInMemoryFile("t_mock_conv.h5");
    ScopedFile f_other = CreateInMemoryFile("t_mock_conv_other.h5");
    ASSERT_TRUE(f_core.valid() && f_other.valid());
    auto* f_other_ptr =
        static_cast<H5F_t*>(H5VL_object_verify(f_other.get(), H5I_FILE));
    ASSERT_NE(f_other_ptr, nullptr);

    ScopedType mock_dt3(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_TRUE(mock_dt3.valid());
    ASSERT_GE(H5Tcommit2(mock_file.get(), "dt3", mock_dt3.get(), H5P_DEFAULT,
                        H5P_DEFAULT, H5P_DEFAULT),
              0);
    H5T_t* dt3_ptr = as_ptr(mock_dt3.get());
    H5VL_object_t* vol3 = dt3_ptr->vol_obj;
    ASSERT_NE(vol3, nullptr);
    dt3_ptr->vol_obj = nullptr;

    ScopedType c_core(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_TRUE(c_core.valid());
    ASSERT_GE(H5Tcommit2(f_core.get(), "c_core", c_core.get(), H5P_DEFAULT,
                        H5P_DEFAULT, H5P_DEFAULT),
              0);
    auto* c_core_vol =
        static_cast<H5T_t*>(H5VL_object_data(as_ptr(c_core.get())->vol_obj));
    ASSERT_NE(c_core_vol, nullptr);

    H5T_t* copy_vol = H5T_copy(c_core_vol, H5T_COPY_ALL);
    ASSERT_NE(copy_vol, nullptr);
    copy_vol->vol_obj = vol3;

    g_mock_vol.fail_dt_close = true;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5T_convert_committed_datatype(copy_vol, f_other_ptr), 0);
    }
    g_mock_vol.fail_dt_close = false;
    EXPECT_GE(H5VL_datatype_close(vol3, H5P_DATASET_XFER_DEFAULT, H5_REQUEST_NULL),
              0);
    EXPECT_GE(H5VL_free_object(vol3), 0);
    copy_vol->vol_obj = nullptr;
    EXPECT_GE(H5T_close_real(copy_vol), 0);
  }

  // 5. H5T_set_loc failure propagation across VLEN, nested VLEN, ARRAY,
  // COMPOUND, and REFERENCE when MockFileGet fails.
  {
    H5CX_node_t cx_node{};
    ASSERT_GE(H5CX_push(&cx_node), 0);
    absl::Cleanup pop_cx = [] { H5CX_pop(false); };

    ScopedType vl_i32(H5Tvlen_create(H5T_NATIVE_INT32));
    ScopedType vl_nested(H5Tvlen_create(vl_i32.get()));
    constexpr hsize_t kArrDim = 2;
    ScopedType arr_vl(H5Tarray_create2(vl_i32.get(), 1, &kArrDim));
    ScopedType cmp_vl(H5Tcreate(H5T_COMPOUND, sizeof(hvl_t)));
    ScopedType ref2_copy(H5Tcopy(H5T_STD_REF));
    ASSERT_TRUE(vl_i32.valid() && vl_nested.valid() && arr_vl.valid() &&
                cmp_vl.valid() && ref2_copy.valid());
    ASSERT_GE(H5Tinsert(cmp_vl.get(), "v", 0, vl_i32.get()), 0);

    g_mock_vol.fail_file_get = true;
    absl::Cleanup restore_fg = [&] { g_mock_vol.fail_file_get = false; };
    ScopedErrorSilence silence;
    EXPECT_LT(H5T_set_loc(as_ptr(vl_i32.get()), mock_file_vol, H5T_LOC_DISK), 0);
    EXPECT_LT(H5T_set_loc(as_ptr(vl_nested.get()), mock_file_vol, H5T_LOC_DISK),
              0);
    EXPECT_LT(H5T_set_loc(as_ptr(arr_vl.get()), mock_file_vol, H5T_LOC_DISK), 0);
    EXPECT_LT(H5T_set_loc(as_ptr(cmp_vl.get()), mock_file_vol, H5T_LOC_DISK), 0);
    EXPECT_LT(H5T_set_loc(as_ptr(ref2_copy.get()), mock_file_vol, H5T_LOC_DISK),
              0);
  }

  mock_file.reset();
}

namespace {

enum class EdgeConvBehavior {
  kStub,
  kCloseSrcFail,
  kCloseDstFail,
  kCloseSrcOk,
  kCloseDstOk,
  kRemoveIds,
};

bool HasRegEdgeTag(hid_t type_id) {
  if (type_id < 0 || H5Tget_class(type_id) != H5T_OPAQUE) {
    return false;
  }
  char* tag = H5Tget_tag(type_id);
  if (tag == nullptr) {
    return false;
  }
  const bool matches = (std::strncmp(tag, "reg_edge_", 9) == 0);
  H5free_memory(tag);
  return matches;
}

herr_t RunEdgeConv(EdgeConvBehavior mode, hid_t src_id, hid_t dst_id,
                   H5T_cdata_t* cdata) {
  if (cdata->command == H5T_CONV_INIT) {
    if (!HasRegEdgeTag(src_id)) {
      return -1;
    }
    switch (mode) {
      case EdgeConvBehavior::kStub:
      case EdgeConvBehavior::kRemoveIds:
        return 0;
      case EdgeConvBehavior::kCloseSrcFail:
        H5Tclose(src_id);
        return -1;
      case EdgeConvBehavior::kCloseDstFail:
        H5Tclose(dst_id);
        return -1;
      case EdgeConvBehavior::kCloseSrcOk:
        H5Tclose(src_id);
        return 0;
      case EdgeConvBehavior::kCloseDstOk:
        H5Tclose(dst_id);
        return 0;
    }
  }
  if (cdata->command == H5T_CONV_CONV && mode == EdgeConvBehavior::kRemoveIds) {
    if (src_id >= 0) {
      H5I_remove(src_id);
    }
    if (dst_id >= 0) {
      H5I_remove(dst_id);
    }
  }
  return 0;
}

herr_t SoftEdgeStub(hid_t s, hid_t d, H5T_cdata_t* c, size_t, size_t, size_t,
                    void*, void*, hid_t) {
  return RunEdgeConv(EdgeConvBehavior::kStub, s, d, c);
}

herr_t SoftEdgeCloseSrcFail(hid_t s, hid_t d, H5T_cdata_t* c, size_t, size_t,
                            size_t, void*, void*, hid_t) {
  return RunEdgeConv(EdgeConvBehavior::kCloseSrcFail, s, d, c);
}

herr_t SoftEdgeCloseDstFail(hid_t s, hid_t d, H5T_cdata_t* c, size_t, size_t,
                            size_t, void*, void*, hid_t) {
  return RunEdgeConv(EdgeConvBehavior::kCloseDstFail, s, d, c);
}

herr_t SoftEdgeCloseSrcOk(hid_t s, hid_t d, H5T_cdata_t* c, size_t, size_t,
                          size_t, void*, void*, hid_t) {
  return RunEdgeConv(EdgeConvBehavior::kCloseSrcOk, s, d, c);
}

herr_t SoftEdgeCloseDstOk(hid_t s, hid_t d, H5T_cdata_t* c, size_t, size_t,
                          size_t, void*, void*, hid_t) {
  return RunEdgeConv(EdgeConvBehavior::kCloseDstOk, s, d, c);
}

herr_t SoftEdgeOk(hid_t s, hid_t d, H5T_cdata_t* c, size_t, size_t, size_t,
                  void*, void*, hid_t) {
  return RunEdgeConv(EdgeConvBehavior::kStub, s, d, c);
}

herr_t HardEdgeCloseSrcOk(hid_t s, hid_t d, H5T_cdata_t* c, size_t, size_t,
                          size_t, void*, void*, hid_t) {
  return RunEdgeConv(EdgeConvBehavior::kCloseSrcOk, s, d, c);
}

herr_t HardEdgeCloseDstOk(hid_t s, hid_t d, H5T_cdata_t* c, size_t, size_t,
                          size_t, void*, void*, hid_t) {
  return RunEdgeConv(EdgeConvBehavior::kCloseDstOk, s, d, c);
}

herr_t ConvEdgeRemoveIds(hid_t s, hid_t d, H5T_cdata_t* c, size_t, size_t,
                         size_t, void*, void*, hid_t) {
  return RunEdgeConv(EdgeConvBehavior::kRemoveIds, s, d, c);
}

}  // namespace

TEST(H5TTest, VolTerminateFailureCleanups) {
  g_mock_vol = {};
  absl::Cleanup reset_vol = [] { g_mock_vol = {}; };

  auto as_ptr = [](hid_t id) {
    return static_cast<H5T_t*>(H5I_object_verify(id, H5I_DATATYPE));
  };

  auto create_sole_mock_dt = [&]() -> ScopedType {
    hid_t vid = H5VLregister_connector(&kMockVolClass, H5P_DEFAULT);
    if (vid < 0) return ScopedType();
    absl::Cleanup unreg = [&] { EXPECT_GE(H5VLunregister_connector(vid), 0); };
    ScopedPlist mfapl(H5Pcreate(H5P_FILE_ACCESS));
    if (!mfapl.valid() || H5Pset_vol(mfapl.get(), vid, nullptr) < 0) {
      return ScopedType();
    }
    ScopedFile mfile(
        H5Fcreate("mock_term.h5", H5F_ACC_TRUNC, H5P_DEFAULT, mfapl.get()));
    if (!mfile.valid()) return ScopedType();
    ScopedType mdt(H5Tcopy(H5T_NATIVE_INT32));
    if (!mdt.valid() || H5Tcommit2(mfile.get(), "dt", mdt.get(), H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT) < 0) {
      return ScopedType();
    }
    return mdt;
  };

  // 1. H5T__close_cb() H5VL_free_object() failure when terminate() returns -1.
  {
    ScopedType mdt = create_sole_mock_dt();
    ASSERT_TRUE(mdt.valid());
    H5T_t* mdt_ptr = as_ptr(mdt.get());
    H5VL_object_t* vol_obj = mdt_ptr->vol_obj;
    ASSERT_NE(vol_obj, nullptr);
    H5VL_connector_t* conn = vol_obj->connector;
    ASSERT_EQ(vol_obj->rc, 1u);
    ASSERT_EQ(conn->nrefs, 1);

    hid_t raw_id = mdt.release();
    g_mock_vol.fail_terminate = true;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Tclose(raw_id), 0);
    }
    g_mock_vol.fail_terminate = false;
    conn->prev = nullptr;
    conn->next = nullptr;
    conn->nrefs = 1;
    vol_obj->rc = 1;
    EXPECT_GE(H5Tclose(raw_id), 0);
  }

  // 2. H5Tclose_async() H5VL_conn_dec_rc() failure on cleanup.
  {
    ScopedType mdt = create_sole_mock_dt();
    ASSERT_TRUE(mdt.valid());
    H5T_t* mdt_ptr = as_ptr(mdt.get());
    H5VL_object_t* vol_obj = mdt_ptr->vol_obj;
    ASSERT_NE(vol_obj, nullptr);
    H5VL_connector_t* conn = vol_obj->connector;
    ASSERT_EQ(vol_obj->rc, 1u);
    ASSERT_EQ(conn->nrefs, 1);

    ScopedEventSet es(H5EScreate());
    ASSERT_TRUE(es.valid());
    hid_t raw_id = mdt.release();
    g_mock_vol.return_async_req = false;
    g_mock_vol.fail_terminate = true;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Tclose_async(raw_id, es.get()), 0);
    }
    g_mock_vol.fail_terminate = false;
    conn->prev = nullptr;
    conn->next = nullptr;
    conn->nrefs = 1;
    EXPECT_GE(H5VL_conn_dec_rc(conn), 0);
  }

  // 3-5. H5T_copy(), H5T_copy_reopen(), and H5T__free() owned_vol_obj close
  // failure paths on sole mock VOL object.
  {
    ScopedType holder = create_sole_mock_dt();
    ASSERT_TRUE(holder.valid());
    H5T_t* holder_ptr = as_ptr(holder.get());
    H5VL_object_t* vol_obj = holder_ptr->vol_obj;
    ASSERT_NE(vol_obj, nullptr);
    H5VL_connector_t* conn = vol_obj->connector;
    ASSERT_EQ(vol_obj->rc, 1u);
    ASSERT_EQ(conn->nrefs, 1);
    holder_ptr->vol_obj = nullptr;
    holder.reset();

    H5T_t* bad_ref = H5T_copy(as_ptr(H5T_STD_REF), H5T_COPY_ALL);
    ASSERT_NE(bad_ref, nullptr);
    bad_ref->shared->u.atomic.u.r.rtype = H5R_BADTYPE;
    bad_ref->shared->u.atomic.u.r.opaque = false;
    bad_ref->shared->u.atomic.u.r.loc = H5T_LOC_DISK;
    bad_ref->shared->force_conv = true;

    auto recover_vol = [&] {
      g_mock_vol.fail_terminate = false;
      conn->prev = nullptr;
      conn->next = nullptr;
      conn->nrefs = 1;
      vol_obj->rc = 1;
    };

    constexpr hsize_t kDim = 1;
    ScopedType bad_arr(H5Tarray_create2(H5T_NATIVE_INT32, 1, &kDim));
    ASSERT_TRUE(bad_arr.valid());
    H5T_t* bad_arr_ptr = as_ptr(bad_arr.get());
    H5T_t* orig_arr_parent = bad_arr_ptr->shared->parent;
    bad_arr_ptr->shared->parent = bad_ref;
    bad_arr_ptr->shared->owned_vol_obj = vol_obj;

    // H5T_copy() cleanup failure on owned_vol_obj.
    vol_obj->rc = 0;
    g_mock_vol.fail_terminate = true;
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5T_copy(bad_arr_ptr, H5T_COPY_TRANSIENT), nullptr);
    }
    EXPECT_EQ(vol_obj->rc, 0u);
    EXPECT_EQ(conn->nrefs, 0);
    recover_vol();

    // H5T_copy_reopen() cleanup failure on owned_vol_obj.
    vol_obj->rc = 0;
    g_mock_vol.fail_terminate = true;
    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5T_copy_reopen(bad_arr_ptr), nullptr);
    }
    EXPECT_EQ(vol_obj->rc, 0u);
    EXPECT_EQ(conn->nrefs, 0);
    recover_vol();

    bad_arr_ptr->shared->parent = orig_arr_parent;
    bad_arr_ptr->shared->owned_vol_obj = nullptr;

    // H5T__free() cleanup failure on owned_vol_obj.
    bad_ref->shared->owned_vol_obj = vol_obj;
    g_mock_vol.fail_terminate = true;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5T_close_real(bad_ref), 0);
    }
    EXPECT_EQ(vol_obj->rc, 0u);
    EXPECT_EQ(conn->nrefs, 0);
    recover_vol();
    EXPECT_GE(H5VL_free_object(vol_obj), 0);
    EXPECT_GE(H5T_close_real(bad_ref), 0);
  }

  // 6. H5T_convert_committed_datatype() H5VL_free_object() failure branch,
  // plus H5O_loc_free() failure branch when last holding open file ref.
  {
    H5CX_node_t cx_node{};
    ASSERT_GE(H5CX_push(&cx_node), 0);
    absl::Cleanup pop_cx = [] { H5CX_pop(false); };

    ScopedFile f1 = CreateInMemoryFile("t_vol_term_conv1.h5");
    ScopedFile f2 = CreateInMemoryFile("t_vol_term_conv2.h5");
    ASSERT_TRUE(f1.valid() && f2.valid());
    auto* f1_ptr = static_cast<H5F_t*>(H5VL_object_verify(f1.get(), H5I_FILE));
    auto* f2_ptr = static_cast<H5F_t*>(H5VL_object_verify(f2.get(), H5I_FILE));
    ASSERT_NE(f1_ptr, nullptr);
    ASSERT_NE(f2_ptr, nullptr);

    ScopedType c1(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_TRUE(c1.valid());
    ASSERT_GE(H5Tcommit2(f1.get(), "c1", c1.get(), H5P_DEFAULT, H5P_DEFAULT,
                        H5P_DEFAULT),
              0);
    auto* c1_inner =
        static_cast<H5T_t*>(H5VL_object_data(as_ptr(c1.get())->vol_obj));
    ASSERT_NE(c1_inner, nullptr);

    // 6a: H5VL_free_object() fails inside H5T_convert_committed_datatype().
    ScopedType holder = create_sole_mock_dt();
    ASSERT_TRUE(holder.valid());
    H5T_t* holder_ptr = as_ptr(holder.get());
    H5VL_object_t* vol_obj = holder_ptr->vol_obj;
    ASSERT_NE(vol_obj, nullptr);
    H5VL_connector_t* conn = vol_obj->connector;
    ASSERT_EQ(vol_obj->rc, 1u);
    ASSERT_EQ(conn->nrefs, 1);
    holder_ptr->vol_obj = nullptr;
    holder.reset();

    H5T_t* copy1 = H5T_copy(c1_inner, H5T_COPY_ALL);
    ASSERT_NE(copy1, nullptr);
    copy1->vol_obj = vol_obj;
    g_mock_vol.fail_terminate = true;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5T_convert_committed_datatype(copy1, f2_ptr), 0);
    }
    g_mock_vol.fail_terminate = false;
    conn->prev = nullptr;
    conn->next = nullptr;
    conn->nrefs = 1;
    vol_obj->rc = 1;
    EXPECT_GE(H5VL_free_object(vol_obj), 0);
    copy1->vol_obj = nullptr;
    EXPECT_GE(H5T_close_real(copy1), 0);

    // 6b: H5O_loc_free() fails inside H5T_convert_committed_datatype().
    H5T_t* copy2 = H5T_copy(c1_inner, H5T_COPY_ALL);
    ASSERT_NE(copy2, nullptr);
    const unsigned orig_nopen = f1_ptr->nopen_objs;
    const H5F_close_degree_t orig_deg = f1_ptr->shared->fc_degree;
    copy2->oloc.holding_file = true;
    f1_ptr->nopen_objs = 1;
    f1_ptr->shared->fc_degree = H5F_CLOSE_DEFAULT;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5T_convert_committed_datatype(copy2, f2_ptr), 0);
    }
    f1_ptr->shared->fc_degree = orig_deg;
    f1_ptr->nopen_objs = orig_nopen;
    EXPECT_GE(H5T_close_real(copy2), 0);
  }

  // 7. H5T_own_vol_obj() fails freeing prior owned_vol_obj.
  {
    ScopedType holder = create_sole_mock_dt();
    ASSERT_TRUE(holder.valid());
    H5T_t* holder_ptr = as_ptr(holder.get());
    H5VL_object_t* vol_obj = holder_ptr->vol_obj;
    ASSERT_NE(vol_obj, nullptr);
    H5VL_connector_t* conn = vol_obj->connector;
    ASSERT_EQ(vol_obj->rc, 1u);
    ASSERT_EQ(conn->nrefs, 1);
    holder_ptr->vol_obj = nullptr;
    holder.reset();

    ScopedFile f_other = CreateInMemoryFile("t_own_fail.h5");
    ASSERT_TRUE(f_other.valid());
    H5VL_object_t* f_other_vol =
        H5VL_vol_object_verify(f_other.get(), H5I_FILE);
    ASSERT_NE(f_other_vol, nullptr);

    ScopedType dt(H5Tcreate(H5T_INTEGER, 4));
    ASSERT_TRUE(dt.valid());
    H5T_t* dt_ptr = as_ptr(dt.get());
    dt_ptr->shared->owned_vol_obj = vol_obj;
    g_mock_vol.fail_terminate = true;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5T_own_vol_obj(dt_ptr, f_other_vol), 0);
    }
    g_mock_vol.fail_terminate = false;
    conn->prev = nullptr;
    conn->next = nullptr;
    conn->nrefs = 1;
    vol_obj->rc = 1;
    EXPECT_GE(H5VL_free_object(vol_obj), 0);
    dt_ptr->shared->owned_vol_obj = nullptr;
  }
}

TEST(H5TTest, CloseErrorPathsAndInvalidContext) {
  auto as_ptr = [](hid_t id) {
    return static_cast<H5T_t*>(H5I_object_verify(id, H5I_DATATYPE));
  };

  // 1. Stale closed datatype and dataset IDs passed to H5Tcopy().
  {
    ScopedType tmp_t(H5Tcreate(H5T_INTEGER, 4));
    ASSERT_TRUE(tmp_t.valid());
    hid_t closed_tid = tmp_t.release();
    ASSERT_GE(H5Tclose(closed_tid), 0);

    ScopedFile mf = CreateInMemoryFile("t_stale_copy.h5");
    ASSERT_TRUE(mf.valid());
    constexpr hsize_t kDim = 1;
    ScopedSpace sp(H5Screate_simple(1, &kDim, nullptr));
    ASSERT_TRUE(sp.valid());
    ScopedDataset ds(H5Dcreate2(mf.get(), "ds", H5T_NATIVE_INT32, sp.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());
    hid_t closed_did = ds.release();
    ASSERT_GE(H5Dclose(closed_did), 0);

    ScopedErrorSilence silence;
    EXPECT_EQ(H5Tcopy(closed_tid), H5I_INVALID_HID);
    EXPECT_EQ(H5Tcopy(closed_did), H5I_INVALID_HID);
  }

  // 2. H5T_decode() 1-byte buffer fails on version-byte bounds check.
  {
    std::array<uint8_t, 4> short_buf{H5O_DTYPE_ID, 0, 0, 0};
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Tdecode2(short_buf.data(), 1), H5I_INVALID_HID);
  }

  // 3. H5T_cmp() with one opaque tag nulled out transiently.
  {
    ScopedType op1(H5Tcreate(H5T_OPAQUE, 4));
    ScopedType op2(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(op1.valid() && op2.valid());
    H5T_t* op1_ptr = as_ptr(op1.get());
    char* saved_tag = op1_ptr->shared->u.opaque.tag;
    op1_ptr->shared->u.opaque.tag = nullptr;
    absl::Cleanup restore_tag = [&] {
      op1_ptr->shared->u.opaque.tag = saved_tag;
    };
    EXPECT_GT(H5Tequal(op1.get(), op2.get()), 0);
  }

  // 4. H5CX_get_dt_conv_cb() error propagation in H5T__path_find_real() and
  // H5T_convert() when dxpl_id is H5I_INVALID_HID.
  {
    H5T_path_t* path = H5T_path_find(as_ptr(H5T_NATIVE_INT32),
                                     as_ptr(H5T_NATIVE_DOUBLE));
    ASSERT_NE(path, nullptr);

    H5CX_node_t bad_cx{};
    ASSERT_GE(H5CX_push(&bad_cx), 0);
    absl::Cleanup pop_cx = [] { H5CX_pop(false); };
    H5CX_set_dxpl(H5I_INVALID_HID);

    double val = 1.0;
    ScopedErrorSilence silence;
    EXPECT_EQ(
        H5T_path_find(as_ptr(H5T_NATIVE_INT32), as_ptr(H5T_NATIVE_DOUBLE)),
        nullptr);
    EXPECT_LT(H5T_convert(path, as_ptr(H5T_NATIVE_INT32),
                          as_ptr(H5T_NATIVE_DOUBLE), 1, 0, 0, &val, nullptr),
              0);
  }

  // 5. H5T_close() error branches on committed open named datatypes.
  {
    ScopedFile f1 = CreateInMemoryFile("t_close_err_single.h5");
    ASSERT_TRUE(f1.valid());
    auto* f1_ptr = static_cast<H5F_t*>(H5VL_object_verify(f1.get(), H5I_FILE));
    ASSERT_NE(f1_ptr, nullptr);

    ScopedType ct(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_TRUE(ct.valid());
    ASSERT_GE(H5Tcommit2(f1.get(), "c", ct.get(), H5P_DEFAULT, H5P_DEFAULT,
                        H5P_DEFAULT),
              0);
    ct.reset();

    ScopedType c1(H5Topen2(f1.get(), "c", H5P_DEFAULT));
    ASSERT_TRUE(c1.valid());
    auto* c1_inner =
        static_cast<H5T_t*>(H5VL_object_data(as_ptr(c1.get())->vol_obj));
    ASSERT_NE(c1_inner, nullptr);
    const haddr_t oh_addr = c1_inner->sh_loc.u.loc.oh_addr;

    // 5a: H5FO_top_decr() fails.
    ASSERT_GE(H5FO_top_decr(f1_ptr, oh_addr), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5T_close(c1_inner), 0);
    }
    c1_inner->shared->fo_count = 1;
    ASSERT_GE(H5FO_top_incr(f1_ptr, oh_addr), 0);

    // 5b: H5FO_delete() fails when fo_count hits 0.
    ASSERT_GE(H5FO_delete(f1_ptr, oh_addr), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5T_close(c1_inner), 0);
    }
    c1_inner->shared->fo_count = 1;
    ASSERT_GE(H5FO_top_incr(f1_ptr, oh_addr), 0);
    ASSERT_GE(H5FO_insert(f1_ptr, oh_addr, c1_inner->shared, false), 0);

    // 5c: H5O_close() fails when fo_count hits 0.
    ASSERT_EQ(f1_ptr->nopen_objs, 1u);
    const H5F_close_degree_t orig_deg = f1_ptr->shared->fc_degree;
    f1_ptr->shared->fc_degree = H5F_CLOSE_DEFAULT;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5T_close(c1_inner), 0);
    }
    f1_ptr->shared->fc_degree = orig_deg;
    c1_inner->shared->fo_count = 1;
    ASSERT_GE(H5FO_top_incr(f1_ptr, oh_addr), 0);
    ASSERT_GE(H5FO_insert(f1_ptr, oh_addr, c1_inner->shared, false), 0);
    f1_ptr->nopen_objs = 1;

    // 5d: H5O_loc_free() fails when fo_count > 0 and top_count > 0.
    ScopedType c1_b(H5Topen2(f1.get(), "c", H5P_DEFAULT));
    ASSERT_TRUE(c1_b.valid());
    auto* c1b_inner =
        static_cast<H5T_t*>(H5VL_object_data(as_ptr(c1_b.get())->vol_obj));
    ASSERT_NE(c1b_inner, nullptr);
    ASSERT_EQ(c1b_inner->shared->fo_count, 2u);
    const unsigned orig_nopen = f1_ptr->nopen_objs;
    c1b_inner->oloc.holding_file = true;
    f1_ptr->nopen_objs = 1;
    f1_ptr->shared->fc_degree = H5F_CLOSE_DEFAULT;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5T_close(c1b_inner), 0);
    }
    f1_ptr->shared->fc_degree = orig_deg;
    f1_ptr->nopen_objs = orig_nopen;
    c1b_inner->shared->fo_count = 2;
    ASSERT_GE(H5FO_top_incr(f1_ptr, oh_addr), 0);
    c1_b.reset();
    c1.reset();
    f1.reset();
  }

  // 5e: H5O_close() fails when fo_count > 0 and H5FO_top_count() == 0 across
  // two H5F_t handles sharing the same underlying OS file.
  {
    TempFile tmp_file("t_close_4877.h5");
    ScopedFile fa(
        H5Fcreate(tmp_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(fa.valid());
    ScopedType ct(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_TRUE(ct.valid());
    ASSERT_GE(H5Tcommit2(fa.get(), "c", ct.get(), H5P_DEFAULT, H5P_DEFAULT,
                        H5P_DEFAULT),
              0);
    ct.reset();

    ScopedType ca(H5Topen2(fa.get(), "c", H5P_DEFAULT));
    ASSERT_TRUE(ca.valid());

    ScopedFile fb(H5Fopen(tmp_file.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
    ASSERT_TRUE(fb.valid());
    auto* fb_ptr = static_cast<H5F_t*>(H5VL_object_verify(fb.get(), H5I_FILE));
    ASSERT_NE(fb_ptr, nullptr);

    ScopedType cb(H5Topen2(fb.get(), "c", H5P_DEFAULT));
    ASSERT_TRUE(cb.valid());
    auto* cb_inner =
        static_cast<H5T_t*>(H5VL_object_data(as_ptr(cb.get())->vol_obj));
    ASSERT_NE(cb_inner, nullptr);
    ASSERT_EQ(cb_inner->shared->fo_count, 2u);
    ASSERT_EQ(fb_ptr->nopen_objs, 1u);

    const H5F_close_degree_t orig_deg = fb_ptr->shared->fc_degree;
    fb_ptr->shared->fc_degree = H5F_CLOSE_DEFAULT;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5T_close(cb_inner), 0);
    }
    fb_ptr->shared->fc_degree = orig_deg;
    fb_ptr->nopen_objs = 1;
    cb_inner->shared->fo_count = 2;
    ASSERT_GE(H5FO_top_incr(fb_ptr, cb_inner->sh_loc.u.loc.oh_addr), 0);
  }
}

TEST(H5TTest, RegisterPathFindInitUnregisterAndConvertEdgeErrors) {
  auto as_ptr = [](hid_t id) {
    return static_cast<H5T_t*>(H5I_object_verify(id, H5I_DATATYPE));
  };
  auto make_edge_opq = [](const char* tag) {
    ScopedType t(H5Tcreate(H5T_OPAQUE, 4));
    if (t.valid()) {
      H5Tset_tag(t.get(), tag);
    }
    return t;
  };

  ScopedType op_s1 = make_edge_opq("reg_edge_s1");
  ScopedType op_d1 = make_edge_opq("reg_edge_d1");
  ScopedType op_h1 = make_edge_opq("reg_edge_h1");
  ScopedType op_h2 = make_edge_opq("reg_edge_h2");
  ScopedType op_sf1 = make_edge_opq("reg_edge_sf1");
  ScopedType op_sf2 = make_edge_opq("reg_edge_sf2");
  ScopedType op_sf3 = make_edge_opq("reg_edge_sf3");
  ScopedType op_sf4 = make_edge_opq("reg_edge_sf4");
  ScopedType op_c1 = make_edge_opq("reg_edge_c1");
  ScopedType op_c2 = make_edge_opq("reg_edge_c2");
  ASSERT_TRUE(op_s1.valid() && op_d1.valid() && op_h1.valid() &&
              op_h2.valid() && op_sf1.valid() && op_sf2.valid() &&
              op_sf3.valid() && op_sf4.valid() && op_c1.valid() &&
              op_c2.valid());

  absl::Cleanup cleanup_convs = [] {
    H5Tunregister(H5T_PERS_DONTCARE, "edge_soft_stub", H5I_INVALID_HID,
                  H5I_INVALID_HID, SoftEdgeStub);
    H5Tunregister(H5T_PERS_DONTCARE, "edge_csrc_fail", H5I_INVALID_HID,
                  H5I_INVALID_HID, SoftEdgeCloseSrcFail);
    H5Tunregister(H5T_PERS_DONTCARE, "edge_cdst_fail", H5I_INVALID_HID,
                  H5I_INVALID_HID, SoftEdgeCloseDstFail);
    H5Tunregister(H5T_PERS_DONTCARE, "edge_csrc_ok", H5I_INVALID_HID,
                  H5I_INVALID_HID, SoftEdgeCloseSrcOk);
    H5Tunregister(H5T_PERS_DONTCARE, "edge_cdst_ok", H5I_INVALID_HID,
                  H5I_INVALID_HID, SoftEdgeCloseDstOk);
    H5Tunregister(H5T_PERS_DONTCARE, "edge_rep_err", H5I_INVALID_HID,
                  H5I_INVALID_HID, SoftEdgeOk);
    H5Tunregister(H5T_PERS_DONTCARE, "edge_sf_csrc", H5I_INVALID_HID,
                  H5I_INVALID_HID, SoftEdgeCloseSrcOk);
    H5Tunregister(H5T_PERS_DONTCARE, "edge_sf_cdst", H5I_INVALID_HID,
                  H5I_INVALID_HID, SoftEdgeCloseDstOk);
    H5Tunregister(H5T_PERS_DONTCARE, "edge_conv_rem", H5I_INVALID_HID,
                  H5I_INVALID_HID, ConvEdgeRemoveIds);
  };

  // 1. H5T__register() temporary ID dec_ref and old_path free failure paths.
  ASSERT_GE(H5Tregister(H5T_PERS_SOFT, "edge_soft_stub", op_s1.get(),
                        op_d1.get(), SoftEdgeStub),
            0);
  H5T_cdata_t* pcdata = nullptr;
  ASSERT_NE(H5Tfind(op_s1.get(), op_d1.get(), &pcdata), nullptr);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tregister(H5T_PERS_SOFT, "edge_csrc_fail", op_s1.get(),
                          op_d1.get(), SoftEdgeCloseSrcFail),
              0);
  }
  ASSERT_GE(H5Tunregister(H5T_PERS_SOFT, "edge_csrc_fail", H5I_INVALID_HID,
                          H5I_INVALID_HID, SoftEdgeCloseSrcFail),
            0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tregister(H5T_PERS_SOFT, "edge_cdst_fail", op_s1.get(),
                          op_d1.get(), SoftEdgeCloseDstFail),
              0);
  }
  ASSERT_GE(H5Tunregister(H5T_PERS_SOFT, "edge_cdst_fail", H5I_INVALID_HID,
                          H5I_INVALID_HID, SoftEdgeCloseDstFail),
            0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tregister(H5T_PERS_SOFT, "edge_csrc_ok", op_s1.get(),
                          op_d1.get(), SoftEdgeCloseSrcOk),
              0);
  }
  ASSERT_GE(H5Tunregister(H5T_PERS_SOFT, "edge_csrc_ok", H5I_INVALID_HID,
                          H5I_INVALID_HID, SoftEdgeCloseSrcOk),
            0);
  ASSERT_NE(H5Tfind(op_s1.get(), op_d1.get(), &pcdata), nullptr);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tregister(H5T_PERS_SOFT, "edge_cdst_ok", op_s1.get(),
                          op_d1.get(), SoftEdgeCloseDstOk),
              0);
  }
  ASSERT_GE(H5Tunregister(H5T_PERS_SOFT, "edge_cdst_ok", H5I_INVALID_HID,
                          H5I_INVALID_HID, SoftEdgeCloseDstOk),
            0);
  ASSERT_NE(H5Tfind(op_s1.get(), op_d1.get(), &pcdata), nullptr);

  // Replace cached soft path while old_path->src is marked IMMUTABLE so
  // H5T__path_free(old_path) fails cleanly after H5T_g.path[i] is updated.
  {
    H5T_path_t* old_path =
        H5T_path_find(as_ptr(op_s1.get()), as_ptr(op_d1.get()));
    ASSERT_NE(old_path, nullptr);
    ASSERT_FALSE(old_path->is_hard);
    H5T_t* saved_src = old_path->src;
    saved_src->shared->state = H5T_STATE_IMMUTABLE;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Tregister(H5T_PERS_SOFT, "edge_rep_err", op_s1.get(),
                            op_d1.get(), SoftEdgeOk),
                0);
    }
    saved_src->shared->state = H5T_STATE_RDONLY;
    EXPECT_GE(H5T_close_real(saved_src), 0);
    ASSERT_GE(H5Tunregister(H5T_PERS_SOFT, "edge_rep_err", H5I_INVALID_HID,
                            H5I_INVALID_HID, SoftEdgeOk),
              0);
  }
  ASSERT_GE(H5Tunregister(H5T_PERS_SOFT, "edge_soft_stub", H5I_INVALID_HID,
                          H5I_INVALID_HID, SoftEdgeStub),
            0);

  // 2. H5T__path_find_init_new_path() temporary ID dec_ref failures for hard
  // and soft application converters.
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tregister(H5T_PERS_HARD, "edge_hard_csrc", op_h1.get(),
                          op_h2.get(), HardEdgeCloseSrcOk),
              0);
    EXPECT_LT(H5Tregister(H5T_PERS_HARD, "edge_hard_cdst", op_h1.get(),
                          op_h2.get(), HardEdgeCloseDstOk),
              0);
  }

  ASSERT_GE(H5Tregister(H5T_PERS_SOFT, "edge_sf_csrc", op_sf1.get(),
                        op_sf2.get(), SoftEdgeCloseSrcOk),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Tfind(op_sf1.get(), op_sf2.get(), &pcdata), nullptr);
  }
  ASSERT_GE(H5Tunregister(H5T_PERS_SOFT, "edge_sf_csrc", H5I_INVALID_HID,
                          H5I_INVALID_HID, SoftEdgeCloseSrcOk),
            0);

  ASSERT_GE(H5Tregister(H5T_PERS_SOFT, "edge_sf_cdst", op_sf3.get(),
                        op_sf4.get(), SoftEdgeCloseDstOk),
            0);
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Tfind(op_sf3.get(), op_sf4.get(), &pcdata), nullptr);
  }
  ASSERT_GE(H5Tunregister(H5T_PERS_SOFT, "edge_sf_cdst", H5I_INVALID_HID,
                          H5I_INVALID_HID, SoftEdgeCloseDstOk),
            0);

  // 3. H5T_convert() temporary ID H5I_remove failure branch and
  // H5T_unregister() / H5T__path_free() src+dst close error propagation.
  ASSERT_GE(H5Tregister(H5T_PERS_HARD, "edge_conv_rem", op_c1.get(),
                        op_c2.get(), ConvEdgeRemoveIds),
            0);
  std::array<uint8_t, 4> buf{};
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tconvert(op_c1.get(), op_c2.get(), 1, buf.data(), nullptr,
                         H5P_DEFAULT),
              0);
  }

  {
    H5T_path_t* p = H5T_path_find(as_ptr(op_c1.get()), as_ptr(op_c2.get()));
    ASSERT_NE(p, nullptr);
    H5T_t* p_src = p->src;
    H5T_t* p_dst = p->dst;
    p_src->shared->state = H5T_STATE_IMMUTABLE;
    p_dst->shared->state = H5T_STATE_IMMUTABLE;
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Tunregister(H5T_PERS_HARD, "edge_conv_rem", op_c1.get(),
                              op_c2.get(), ConvEdgeRemoveIds),
                0);
    }
    p_src->shared->state = H5T_STATE_RDONLY;
    p_dst->shared->state = H5T_STATE_RDONLY;
    EXPECT_GE(H5T_close_real(p_src), 0);
    EXPECT_GE(H5T_close_real(p_dst), 0);
  }
}

}  // namespace
}  // namespace hdf5
