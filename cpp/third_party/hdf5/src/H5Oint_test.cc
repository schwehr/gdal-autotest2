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
// Tests internal object header lifecycle, protection, pinning, continuation
// chunks, link/refcount management, timestamp touching, deletion, metadata
// queries, recursive visitation, and H5O_loc_t helpers in
// third_party/hdf5/src/H5Oint.c.
//
// Google3 build caveats: H5O_ENABLE_BOGUS, H5O_ENABLE_BAD_MESG_COUNT,
// H5_STRICT_FORMAT_CHECKS, and H5O_DEBUG are undefined, while NDEBUG is
// defined when compiling `:hdf5`.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Oint.c

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is compiled with -DNDEBUG, omitting debug fields from cache and
// object header structs (`H5C_cache_entry_t`, `H5O_t::ndecode_dirtied`).
// Define NDEBUG around `hdf5_internal_prelude.h` and internal headers so
// struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5OINT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5F_FRIEND
#define H5G_FRIEND
#define H5O_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5FOprivate.h"
// H5Fpkg.h and H5Gpkg.h (via H5B2private.h) use `try` as a parameter name,
// which collides with libc++'s -fno-exceptions `#define try if (1)` macro.
#pragma push_macro("try")
#undef try
#define try h5_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Gpkg.h"
#undef try
#pragma pop_macro("try")
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5Pprivate.h"
#include "third_party/hdf5/src/H5VLnative_private.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5OINT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OINT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::Contains;
using ::testing::Not;
using ::testing::UnorderedElementsAre;

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

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

struct VisitTracker {
  std::vector<std::string> paths;
  std::string stop_on_path;
  std::string error_on_path;
};

herr_t RecordVisitCallback(hid_t /*obj*/, const char* name,
                           const H5O_info2_t* /*info*/, void* op_data) {
  auto* tracker = static_cast<VisitTracker*>(op_data);
  tracker->paths.emplace_back(name);
  if (!tracker->error_on_path.empty() && tracker->error_on_path == name) {
    return -1;
  }
  if (!tracker->stop_on_path.empty() && tracker->stop_on_path == name) {
    return H5_ITER_STOP;
  }
  return H5_ITER_CONT;
}

herr_t DeleteSiblingVisitCallback(hid_t obj, const char* name,
                                  const H5O_info2_t* /*info*/, void* op_data) {
  const auto* target = static_cast<const char*>(op_data);
  if (std::string(name) == "a_first") {
    (void)H5Ldelete(obj, target, H5P_DEFAULT);
  }
  return H5_ITER_CONT;
}

TEST(H5OintTest, InitAndVersionBounds) {
  EXPECT_GE(H5O_init(), 0);

  uint8_t ver = 0;
  EXPECT_GE(H5O_get_version_bound(H5F_LIBVER_EARLIEST, &ver), 0);
  EXPECT_EQ(ver, H5O_VERSION_1);

  EXPECT_GE(H5O_get_version_bound(H5F_LIBVER_V18, &ver), 0);
  EXPECT_EQ(ver, H5O_VERSION_2);

  EXPECT_GE(H5O_get_version_bound(H5F_LIBVER_LATEST, &ver), 0);
  EXPECT_EQ(ver, H5O_VERSION_LATEST);
}

TEST(H5OintTest, CreateOhdrAndApplyOhdrBranchesAndErrors) {
  // 1. Earliest bounds: v1 header default, out-of-bounds error with v2 feature.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
        0);
    ScopedFile file(
        H5Fcreate("v1_create.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedPlist ocpl(H5Pcreate(H5P_OBJECT_CREATE));
    ASSERT_TRUE(ocpl.valid());

    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());

      // H5P_DATASET_CREATE_DEFAULT special branch in H5O_create_ohdr.
      H5O_t* oh_def = H5O_create_ohdr(f, H5P_DATASET_CREATE_DEFAULT);
      ASSERT_NE(oh_def, nullptr);
      EXPECT_EQ(H5O_get_oh_version(oh_def), H5O_VERSION_1);
      H5O_loc_t loc{};
      H5O_loc_reset(&loc);
      ASSERT_GE(
          H5O_apply_ohdr(f, oh_def, H5P_DATASET_CREATE_DEFAULT, 0, 0, &loc), 0);
      EXPECT_NE(loc.addr, HADDR_UNDEF);
      EXPECT_GE(H5O_close(&loc, nullptr), 0);

      // Explicit OCPL fails H5O_apply_ohdr when invalid/wrong plist passed,
      // verifying H5O_apply_ohdr cleanup path via H5O__free.
      H5O_t* oh_bad_apply = H5O_create_ohdr(f, ocpl.get());
      ASSERT_NE(oh_bad_apply, nullptr);
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5O_apply_ohdr(f, oh_bad_apply, H5I_INVALID_HID, 0, 0, &loc),
                  0);
      }
      EXPECT_GE(H5O__free(oh_bad_apply, true), 0);

      // Bad OCPL (not object create) in H5O_create_ohdr + H5I_INVALID_HID.
      {
        ScopedErrorSilence silence;
        EXPECT_EQ(H5O_create_ohdr(f, H5I_INVALID_HID), nullptr);
        EXPECT_EQ(H5O_create_ohdr(f, fapl.get()), nullptr);

        // Version out of bounds: tracked attr order forces v2 > EARLIEST.
        const H5F_libver_t saved_high = f->shared->high_bound;
        f->shared->high_bound = H5F_LIBVER_EARLIEST;
        absl::Cleanup restore_high = [&] {
          f->shared->high_bound = saved_high;
        };
        ASSERT_GE(H5Pset_attr_creation_order(ocpl.get(), H5P_CRT_ORDER_TRACKED),
                  0);
        EXPECT_EQ(H5O_create_ohdr(f, ocpl.get()), nullptr);
        H5O_loc_t fail_loc{};
        H5O_loc_reset(&fail_loc);
        EXPECT_LT(H5O_create(f, 0, 0, ocpl.get(), &fail_loc), 0);
      }
    }
  }

  // 2. Oversized size_hint hits H5O_HDR_CHUNK0_8 and causes H5MF_alloc to fail
  //    inside H5O_apply_ohdr called from H5O_create, covering H5O_create's
  //    H5O__free cleanup path without allocating chunk RAM.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);
    ScopedFile file(
        H5Fcreate("oversize_addr.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedPlist ocpl(H5Pcreate(H5P_OBJECT_CREATE));
    ASSERT_TRUE(ocpl.valid());
    ScopedErrorSilence silence;
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5O_loc_t loc{};
    H5O_loc_reset(&loc);
    const size_t huge_hint =
        ~size_t{0x7f};  // Aligned; overflows eoa + oh_size.
    EXPECT_LT(H5O_create(f, huge_hint, 0, ocpl.get(), &loc), 0);
  }

  // 3. V2 options: store_msg_crt_idx, custom phase change, no track times,
  //    size_hint thresholds (>255 and >65535), initial_rc > 0, and H5O_apply
  //    property errors.
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
    ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_ATTR_FLAG, 4),
              0);

    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);

    ScopedFile file(
        H5Fcreate("v2_create.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedPlist ocpl(H5Pcreate(H5P_OBJECT_CREATE));
    ASSERT_TRUE(ocpl.valid());
    ASSERT_GE(H5Pset_obj_track_times(ocpl.get(), false), 0);
    ASSERT_GE(H5Pset_attr_phase_change(ocpl.get(), 12, 4), 0);

    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());

      // size_hint > 255 -> CHUNK0_2; store_msg_crt_idx -> CRT_ORDER_TRACKED.
      H5O_loc_t loc2{};
      H5O_loc_reset(&loc2);
      ASSERT_GE(H5O_create(f, 300, 1, ocpl.get(), &loc2), 0);
      H5O_t* oh2 = H5O_protect(&loc2, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh2, nullptr);
      EXPECT_EQ(H5O_get_oh_version(oh2), H5O_VERSION_2);
      EXPECT_TRUE(H5O_has_chksum(oh2));
      EXPECT_EQ(H5O_get_proxy(oh2), nullptr);
      EXPECT_NE(H5O_get_oh_flags(oh2) & H5O_HDR_CHUNK0_2, 0U);
      EXPECT_NE(H5O_get_oh_flags(oh2) & H5O_HDR_ATTR_CRT_ORDER_TRACKED, 0U);
      EXPECT_NE(H5O_get_oh_flags(oh2) & H5O_HDR_ATTR_STORE_PHASE_CHANGE, 0U);
      EXPECT_EQ(H5O_get_oh_flags(oh2) & H5O_HDR_STORE_TIMES, 0U);
      EXPECT_GE(H5O_unprotect(&loc2, oh2, H5AC__NO_FLAGS_SET), 0);
      // Unpin initial_rc = 1 before closing file.
      EXPECT_GE(H5O_dec_rc_by_loc(&loc2), 0);
      EXPECT_GE(H5O_close(&loc2, nullptr), 0);

      // size_hint > 65535 -> CHUNK0_4 with times enabled.
      ASSERT_GE(H5Pset_obj_track_times(ocpl.get(), true), 0);
      H5O_loc_t loc4{};
      H5O_loc_reset(&loc4);
      ASSERT_GE(H5O_create(f, 70000, 0, ocpl.get(), &loc4), 0);
      H5O_t* oh4 = H5O_protect(&loc4, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh4, nullptr);
      EXPECT_NE(H5O_get_oh_flags(oh4) & H5O_HDR_CHUNK0_4, 0U);
      EXPECT_GT(H5O_get_oh_mtime(oh4), 0);
      EXPECT_EQ(H5O_get_oh_addr(oh4), loc4.addr);
      EXPECT_GE(H5O_unprotect(&loc4, oh4, H5AC__NO_FLAGS_SET), 0);
      EXPECT_GE(H5O_close(&loc4, nullptr), 0);

      // Wrong plist class in H5O_apply_ohdr hits H5P_get failure at line 451.
      H5O_t* oh_err = H5O_create_ohdr(f, ocpl.get());
      ASSERT_NE(oh_err, nullptr);
      H5O_loc_t unused_loc{};
      H5O_loc_reset(&unused_loc);
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5O_apply_ohdr(f, oh_err, fapl.get(), 0, 0, &unused_loc), 0);
      }
      EXPECT_GE(H5O__free(oh_err, true), 0);
    }
  }

  // 4. Read-only file rejects H5O_create_ohdr and H5O_create.
  {
    TempFile path("ro_ohdr.h5");
    ScopedPlist sec2(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(sec2.valid());
    ASSERT_GE(H5Pset_fapl_sec2(sec2.get()), 0);
    {
      ScopedFile wf(
          H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, sec2.get()));
      ASSERT_TRUE(wf.valid());
    }
    ScopedFile ro(H5Fopen(path.c_str(), H5F_ACC_RDONLY, sec2.get()));
    ASSERT_TRUE(ro.valid());
    H5F_t* rf = UnwrapFile(ro.get());
    ASSERT_NE(rf, nullptr);

    ScopedErrorSilence silence;
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    EXPECT_EQ(H5O_create_ohdr(rf, H5P_DATASET_CREATE_DEFAULT), nullptr);
    H5O_loc_t loc{};
    H5O_loc_reset(&loc);
    EXPECT_LT(H5O_create(rf, 0, 0, H5P_DATASET_CREATE_DEFAULT, &loc), 0);
  }
}

TEST(H5OintTest, SwmrProxyCreateProtectUnprotectAndFree) {
  TempFile path("swmr_chunks.h5");
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_fapl_sec2(fapl.get()), 0);
  ASSERT_GE(
      H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);

  // 1. Build a multi-chunk v2 object header on disk.
  {
    ScopedFile file(
        H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 64, 32), 0);
    ScopedGroup grp(
        H5Gcreate2(file.get(), "multi", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    // Box in chunk 0 so adding attributes allocates chunk 1.
    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    ScopedDataset blocker(H5Dcreate2(file.get(), "blocker", H5T_NATIVE_INT32,
                                     scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                     H5P_DEFAULT));
    ASSERT_TRUE(blocker.valid());
    const int val = 11;
    ASSERT_GE(H5Dwrite(blocker.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, &val),
              0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    H5O_loc_t* gloc = H5O_get_loc(grp.get());
    ASSERT_NE(gloc, nullptr);
    for (int i = 0; i < 24; ++i) {
      std::string name = absl::StrCat("a_", i);
      ScopedAttr a(H5Acreate2(grp.get(), name.c_str(), H5T_NATIVE_INT32,
                              scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(a.valid());
      ASSERT_GE(H5Awrite(a.get(), H5T_NATIVE_INT32, &i), 0);
      if (i % 4 == 3) {
        ScopedApiContext ctx;
        H5O_t* oh = H5O_protect(gloc, H5AC__READ_ONLY_FLAG, false);
        ASSERT_NE(oh, nullptr);
        const size_t nchunks = oh->nchunks;
        ASSERT_GE(H5O_unprotect(gloc, oh, H5AC__NO_FLAGS_SET), 0);
        if (nchunks >= 2) {
          break;
        }
      }
    }
  }

  // 2. Reopen in SWMR write mode: covers oh->proxy creation and H5O_protect
  //    with pin_all_chunks = true across multiple chunks.
  {
    ScopedFile swmr_file(
        H5Fopen(path.c_str(), H5F_ACC_RDWR | H5F_ACC_SWMR_WRITE, fapl.get()));
    ASSERT_TRUE(swmr_file.valid());
    H5F_t* f = UnwrapFile(swmr_file.get());
    ASSERT_NE(f, nullptr);

    ScopedGroup grp(H5Gopen2(swmr_file.get(), "multi", H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    H5O_loc_t* gloc = H5O_get_loc(grp.get());
    ASSERT_NE(gloc, nullptr);

    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      H5O_t* oh =
          H5O_protect(gloc, H5AC__READ_ONLY_FLAG, /*pin_all_chunks=*/true);
      ASSERT_NE(oh, nullptr);
      EXPECT_GT(oh->nchunks, 1U);
      EXPECT_TRUE(oh->chunks_pinned);
      EXPECT_NE(H5O_get_proxy(oh), nullptr);
      EXPECT_GE(H5O_unprotect(gloc, oh, H5AC__NO_FLAGS_SET), 0);

      // Also exercise H5O_apply_ohdr creating oh->proxy and H5O__free
      // destroying oh->proxy on error.
      ScopedPlist ocpl(H5Pcreate(H5P_OBJECT_CREATE));
      ASSERT_TRUE(ocpl.valid());
      H5O_t* swmr_oh = H5O_create_ohdr(f, ocpl.get());
      ASSERT_NE(swmr_oh, nullptr);
      H5O_loc_t unused_loc{};
      H5O_loc_reset(&unused_loc);
      {
        ScopedErrorSilence silence;
        EXPECT_LT(
            H5O_apply_ohdr(f, swmr_oh, H5I_INVALID_HID, 0, 0, &unused_loc), 0);
      }
      EXPECT_NE(H5O_get_proxy(swmr_oh), nullptr);
      EXPECT_GE(H5O__free(swmr_oh, true), 0);
    }
  }
}

TEST(H5OintTest, ProtectFailureExpungesLoadedContinuationChunks) {
  TempFile path("corrupt_chunk2.h5");
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_fapl_sec2(fapl.get()), 0);
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);

  haddr_t obj_addr = HADDR_UNDEF;
  haddr_t chunk2_addr = HADDR_UNDEF;

  // 1. Create a 3-chunk object header by interleaving blocker datasets between
  //    attribute batches.
  {
    ScopedFile file(
        H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());

    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 100, 50), 0);
    ScopedGroup grp(
        H5Gcreate2(file.get(), "g3", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    H5O_loc_t* gloc = H5O_get_loc(grp.get());
    ASSERT_NE(gloc, nullptr);
    obj_addr = gloc->addr;

    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    ScopedDataset b1(H5Dcreate2(file.get(), "b1", H5T_NATIVE_INT32,
                                scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
    ASSERT_TRUE(b1.valid());
    const int val = 1;
    ASSERT_GE(H5Dwrite(b1.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, &val),
              0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

    int attr_idx = 0;
    bool boxed_second = false;
    ScopedDataset b2;
    for (; attr_idx < 80; ++attr_idx) {
      std::string name = absl::StrCat("attr_", attr_idx);
      ScopedAttr a(H5Acreate2(grp.get(), name.c_str(), H5T_NATIVE_INT32,
                              scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(a.valid());
      ASSERT_GE(H5Awrite(a.get(), H5T_NATIVE_INT32, &attr_idx), 0);

      ScopedApiContext ctx;
      H5O_t* oh = H5O_protect(gloc, H5AC__READ_ONLY_FLAG, false);
      ASSERT_NE(oh, nullptr);
      size_t nchunks = oh->nchunks;
      if (nchunks >= 3) {
        chunk2_addr = oh->chunk[2].addr;
      }
      ASSERT_GE(H5O_unprotect(gloc, oh, H5AC__NO_FLAGS_SET), 0);

      if (nchunks == 2 && !boxed_second) {
        b2.reset(H5Dcreate2(file.get(), "b2", H5T_NATIVE_INT32, scalar.get(),
                            H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
        ASSERT_TRUE(b2.valid());
        ASSERT_GE(H5Dwrite(b2.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                           H5P_DEFAULT, &val),
                  0);
        ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
        boxed_second = true;
      }
      if (nchunks >= 3) {
        chunk2_addr = oh->chunk[nchunks - 1].addr;
        break;
      }
    }
    ASSERT_NE(chunk2_addr, HADDR_UNDEF);
  }

  // 1b. Reopen read-only cleanly to resolve Chunk 2's disk decode order,
  //     cover H5O_CONT_ID accounting in H5O_get_hdr_info, and verify
  //     H5O_protect write-intent error on read-only files.
  {
    ScopedFile ro_check(H5Fopen(path.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(ro_check.valid());
    H5F_t* rf = UnwrapFile(ro_check.get());
    ASSERT_NE(rf, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5O_loc_t loc{};
    H5O_loc_reset(&loc);
    loc.file = rf;
    loc.addr = obj_addr;

    H5O_hdr_info_t hdr{};
    ASSERT_GE(H5O_get_hdr_info(&loc, &hdr), 0);
    EXPECT_GE(hdr.nchunks, 3U);

    H5O_t* oh = H5O_protect(&loc, H5AC__READ_ONLY_FLAG, false);
    ASSERT_NE(oh, nullptr);
    ASSERT_GE(oh->nchunks, 3U);
    chunk2_addr = oh->chunk[oh->nchunks - 1].addr;
    EXPECT_GE(H5O_unprotect(&loc, oh, H5AC__NO_FLAGS_SET), 0);

    ScopedErrorSilence silence;
    EXPECT_EQ(H5O_protect(&loc, H5AC__NO_FLAGS_SET, false), nullptr);
  }

  // 2. Corrupt disk-order Chunk (N-1)'s magic prefix so loading Chunk 0 &
  //    Chunk 1 succeeds, then protecting the last chunk fails and triggers
  //    H5O_protect's error cleanup + H5O_unprotect(H5AC__DELETED_FLAG)
  //    expunging already-loaded continuation chunks.
  {
    std::fstream fs(path.c_str(),
                    std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(fs.is_open());
    fs.seekp(static_cast<std::streamoff>(chunk2_addr));
    const char zeros[4] = {0, 0, 0, 0};
    fs.write(zeros, sizeof(zeros));
    ASSERT_TRUE(fs.good());
  }

  ScopedFile ro(H5Fopen(path.c_str(), H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(ro.valid());
  H5F_t* f = UnwrapFile(ro.get());
  ASSERT_NE(f, nullptr);

  ScopedErrorSilence silence;
  ScopedApiContext ctx;
  ASSERT_TRUE(ctx.valid());
  H5O_loc_t loc{};
  H5O_loc_reset(&loc);
  loc.file = f;
  loc.addr = obj_addr;
  EXPECT_EQ(H5O_protect(&loc, H5AC__READ_ONLY_FLAG, false), nullptr);
}

TEST(H5OintTest, OpenNameByIdxByAddrByLocCloseAndObjCreate) {
  ScopedFile file = CreateInMemoryFile("open_ops.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedGroup grp(
      H5Gcreate2(file.get(), "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "dset", H5T_NATIVE_INT32,
                                scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ScopedType dtype(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(dtype.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "dtype", dtype.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Public opens exercise H5O_open_name, H5O__open_by_idx, H5O__open_by_addr,
  // H5O_open_by_loc, and H5O_obj_create across group/dataset/datatype.
  for (const char* name : {"grp", "dset", "dtype"}) {
    ScopedObject o_name(H5Oopen(file.get(), name, H5P_DEFAULT));
    EXPECT_TRUE(o_name.valid());

    H5O_info2_t info{};
    ASSERT_GE(H5Oget_info3(o_name.get(), &info, H5O_INFO_BASIC), 0);
    ScopedObject o_tok(H5Oopen_by_token(file.get(), info.token));
    EXPECT_TRUE(o_tok.valid());
  }

  ScopedObject o_idx(H5Oopen_by_idx(file.get(), "/", H5_INDEX_NAME, H5_ITER_INC,
                                    0, H5P_DEFAULT));
  EXPECT_TRUE(o_idx.valid());

  {
    ScopedErrorSilence silence;
    ScopedObject bad_name(H5Oopen(file.get(), "missing", H5P_DEFAULT));
    EXPECT_FALSE(bad_name.valid());
    ScopedObject bad_idx(H5Oopen_by_idx(file.get(), "/", H5_INDEX_NAME,
                                        H5_ITER_INC, 99, H5P_DEFAULT));
    EXPECT_FALSE(bad_idx.valid());
  }

  // Direct internal H5O_open_name, H5O__open_by_addr, H5O_open(holding_file),
  // H5O_obj_create.
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5G_loc_t root_loc{};
    ASSERT_GE(H5G_root_loc(f, &root_loc), 0);

    H5I_type_t otype = H5I_BADID;
    void* obj = H5O_open_name(&root_loc, "grp", &otype);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(otype, H5I_GROUP);
    EXPECT_GE(H5G_close(static_cast<H5G_t*>(obj)), 0);

    H5O_loc_t* gloc = H5O_get_loc(grp.get());
    ASSERT_NE(gloc, nullptr);
    void* obj_addr = H5O__open_by_addr(&root_loc, gloc->addr, &otype);
    ASSERT_NE(obj_addr, nullptr);
    EXPECT_EQ(otype, H5I_GROUP);
    EXPECT_GE(H5G_close(static_cast<H5G_t*>(obj_addr)), 0);

    {
      ScopedErrorSilence silence;
      EXPECT_EQ(H5O_open_name(&root_loc, "nonexistent", &otype), nullptr);
      EXPECT_EQ(H5O__open_by_addr(&root_loc, gloc->addr + 0x100000ULL, &otype),
                nullptr);
    }

    // H5O_open with loc->holding_file == true.
    H5O_loc_t hold_loc{};
    H5O_loc_reset(&hold_loc);
    hold_loc.file = f;
    hold_loc.addr = gloc->addr;
    EXPECT_GE(H5O_loc_hold_file(&hold_loc), 0);
    EXPECT_TRUE(hold_loc.holding_file);
    EXPECT_GE(H5O_open(&hold_loc), 0);
    EXPECT_FALSE(hold_loc.holding_file);
    EXPECT_GE(H5O_close(&hold_loc, nullptr), 0);

    // Direct H5O_obj_create for H5O_TYPE_GROUP.
    H5G_obj_create_t gcrt{};
    gcrt.gcpl_id = H5P_GROUP_CREATE_DEFAULT;
    gcrt.cache_type = H5G_NOTHING_CACHED;
    H5G_name_t created_path{};
    H5O_loc_t created_oloc{};
    H5O_loc_reset(&created_oloc);
    H5G_loc_t created_loc{&created_oloc, &created_path};
    void* raw_grp = H5O_obj_create(f, H5O_TYPE_GROUP, &gcrt, &created_loc);
    ASSERT_NE(raw_grp, nullptr);
    EXPECT_GE(H5O_dec_rc_by_loc(created_loc.oloc), 0);
    EXPECT_GE(H5G_close(static_cast<H5G_t*>(raw_grp)), 0);
  }

  // Test H5O_obj_create error branch on a read-only file.
  {
    TempFile ro_path("ro_obj_create.h5");
    ScopedPlist sec2(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(sec2.valid());
    ASSERT_GE(H5Pset_fapl_sec2(sec2.get()), 0);
    {
      ScopedFile wf(
          H5Fcreate(ro_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, sec2.get()));
      ASSERT_TRUE(wf.valid());
    }
    ScopedFile ro(H5Fopen(ro_path.c_str(), H5F_ACC_RDONLY, sec2.get()));
    ASSERT_TRUE(ro.valid());
    H5F_t* rf = UnwrapFile(ro.get());
    ASSERT_NE(rf, nullptr);

    ScopedErrorSilence silence;
    ScopedApiContext ctx;
    H5G_obj_create_t gcrt{};
    gcrt.gcpl_id = H5P_GROUP_CREATE_DEFAULT;
    gcrt.cache_type = H5G_NOTHING_CACHED;
    H5G_name_t dummy_path{};
    H5O_loc_t dummy_oloc{};
    H5O_loc_reset(&dummy_oloc);
    H5G_loc_t dummy_loc{&dummy_oloc, &dummy_path};
    EXPECT_EQ(H5O_obj_create(rf, H5O_TYPE_GROUP, &gcrt, &dummy_loc), nullptr);
  }
}

TEST(H5OintTest, CloseTriggersFileTryCloseAndLocHoldFile) {
  // 1. Closing H5F before closing the last open object triggers H5F_try_close
  //    inside H5O_close.
  {
    ScopedFile file = CreateInMemoryFile("delayed_close.h5");
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(
        H5Gcreate2(file.get(), "g", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());
    file.reset();  // Weak close defers until grp closes.
    grp.reset();   // H5O_close triggers H5F_try_close -> closes file.
  }

  // 2. H5O_loc_hold_file + closing file ID + H5O_loc_free reaching
  //    NOPEN_OBJS <= 0 triggers H5F_try_close inside H5O_loc_free.
  {
    ScopedFile file = CreateInMemoryFile("loc_free_close.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    H5O_loc_t loc{};
    H5O_loc_reset(&loc);
    loc.file = f;
    EXPECT_GE(H5O_loc_hold_file(&loc), 0);
    EXPECT_TRUE(loc.holding_file);
    // Calling H5O_loc_hold_file a second time is an idempotent no-op.
    EXPECT_GE(H5O_loc_hold_file(&loc), 0);

    // Close the file handle while loc holds it open, then release via
    // H5O_loc_free (with an active API context for H5F_try_close).
    ASSERT_GE(H5Fclose(file.release()), 0);
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    EXPECT_GE(H5O_loc_free(&loc), 0);
    EXPECT_FALSE(loc.holding_file);
  }
}

TEST(H5OintTest, LinkCountAndOpenMarkedObjectLifecycle) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST),
            0);
  ScopedFile file(
      H5Fcreate("links_rc.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "d1", H5T_NATIVE_INT32,
                                scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  H5O_loc_t dloc = *H5O_get_loc(dset.get());

  // 1 -> 2 adds H5O_MSG_REFCOUNT; 2 -> 3 updates it.
  EXPECT_GE(H5Olink(dset.get(), file.get(), "d2", H5P_DEFAULT, H5P_DEFAULT), 0);
  EXPECT_GE(H5Olink(dset.get(), file.get(), "d3", H5P_DEFAULT, H5P_DEFAULT), 0);

  // 3 -> 2 updates REFCOUNT; 2 -> 1 deletes REFCOUNT message.
  EXPECT_GE(H5Ldelete(file.get(), "d3", H5P_DEFAULT), 0);
  EXPECT_GE(H5Ldelete(file.get(), "d2", H5P_DEFAULT), 0);

  // 1 -> 0 while dataset is still open: marks object in H5FO rather than
  // deleting immediately.
  EXPECT_GE(H5Ldelete(file.get(), "d1", H5P_DEFAULT), 0);
  EXPECT_TRUE(H5FO_marked(f, dloc.addr));

  // Re-linking 0 -> 1 while open clears the H5FO deletion mark.
  EXPECT_GE(
      H5Olink(dset.get(), file.get(), "d_restored", H5P_DEFAULT, H5P_DEFAULT),
      0);
  EXPECT_FALSE(H5FO_marked(f, dloc.addr));

  // Direct H5O_link edge cases: adjust == 0 and negative underflow error.
  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    EXPECT_EQ(H5O_link(&dloc, 0), 1);

    ScopedErrorSilence silence;
    EXPECT_LT(H5O_link(&dloc, -5), 0);
  }

  // Close dataset so it leaves H5FO, then H5Ldelete triggers *deleted = true
  // -> H5O_delete inside H5O_link.
  dset.reset();
  EXPECT_GE(H5Ldelete(file.get(), "d_restored", H5P_DEFAULT), 0);

  // Corked dataset deleted via H5O_delete, plus direct corking on an unlinked
  // header so H5O_delete explicitly observes H5AC__GET_CORKED == true and
  // uncorks before deletion.
  ScopedDataset cork_ds(H5Dcreate2(file.get(), "corked", H5T_NATIVE_INT32,
                                   scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
  ASSERT_TRUE(cork_ds.valid());
  EXPECT_GE(H5Odisable_mdc_flushes(cork_ds.get()), 0);
  cork_ds.reset();
  EXPECT_GE(H5Ldelete(file.get(), "corked", H5P_DEFAULT), 0);

  {
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5O_loc_t cork_loc{};
    H5O_loc_reset(&cork_loc);
    ASSERT_GE(H5O_create(f, 32, 0, H5P_GROUP_CREATE_DEFAULT, &cork_loc), 0);
    const haddr_t cork_addr = cork_loc.addr;
    EXPECT_GE(H5O_close(&cork_loc, nullptr), 0);
    EXPECT_GE(H5AC_cork(f, cork_addr, H5AC__SET_CORK, nullptr), 0);
    EXPECT_GE(H5O_delete(f, cork_addr), 0);
  }
}

TEST(H5OintTest, TouchMtimeVariantsAndErrors) {
  // 1. V2 header with timestamps disabled vs enabled.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);
    ScopedFile file(
        H5Fcreate("touch_v2.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedPlist gcpl_off(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl_off.valid());
    ASSERT_GE(H5Pset_obj_track_times(gcpl_off.get(), false), 0);
    ScopedGroup g_off(H5Gcreate2(file.get(), "g_off", H5P_DEFAULT,
                                 gcpl_off.get(), H5P_DEFAULT));
    ASSERT_TRUE(g_off.valid());
    H5O_loc_t* loc_off = H5O_get_loc(g_off.get());
    ASSERT_NE(loc_off, nullptr);

    ScopedGroup g_on(
        H5Gcreate2(file.get(), "g_on", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g_on.valid());
    H5O_loc_t* loc_on = H5O_get_loc(g_on.get());
    ASSERT_NE(loc_on, nullptr);

    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      // No-op when STORE_TIMES is off.
      EXPECT_GE(H5O_touch(loc_off, true), 0);
      H5O_info2_t info{};
      ASSERT_GE(H5O_get_info(loc_off, &info, H5O_INFO_TIME), 0);
      EXPECT_EQ(info.ctime, 0);

      // Updates v2 oh->atime / oh->ctime when STORE_TIMES is on.
      EXPECT_GE(H5O_touch(loc_on, false), 0);
      ASSERT_GE(H5O_get_info(loc_on, &info, H5O_INFO_TIME), 0);
      EXPECT_GT(info.ctime, 0);
    }
  }

  // 2. V1 header on disk: no MTIME, old MTIME_ID, and MTIME_NEW_ID reloaded
  //    lazily (native == NULL) before H5O_touch.
  {
    TempFile path("touch_v1.h5");
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_sec2(fapl.get()), 0);
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
        0);

    haddr_t new_mtime_addr = HADDR_UNDEF;
    haddr_t old_mtime_addr = HADDR_UNDEF;
    {
      ScopedFile file(
          H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(file.valid());
      H5F_t* f = UnwrapFile(file.get());
      ASSERT_NE(f, nullptr);

      ScopedGroup g1(
          H5Gcreate2(file.get(), "g1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(g1.valid());
      H5O_loc_t* g1_loc = H5O_get_loc(g1.get());
      ASSERT_NE(g1_loc, nullptr);
      new_mtime_addr = g1_loc->addr;

      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      // Force creation of MTIME_NEW on g1 if absent, or update existing.
      EXPECT_GE(H5O_touch(g1_loc, /*force=*/true), 0);

      // Create an independent v1 object header pinned via initial_rc = 0 and
      // kept alive in cache by H5O_link(+1) so we can test absent MTIME,
      // force=false early return, and old H5O_MTIME_ID.
      H5O_loc_t custom_loc{};
      H5O_loc_reset(&custom_loc);
      ASSERT_GE(H5O_create(f, 64, 0, H5P_GROUP_CREATE_DEFAULT, &custom_loc), 0);
      old_mtime_addr = custom_loc.addr;
      EXPECT_GE(H5O_link(&custom_loc, 1), 0);

      // No mtime message yet + force = false -> returns early without adding.
      EXPECT_GE(H5O_touch(&custom_loc, /*force=*/false), 0);
      H5O_info2_t info{};
      ASSERT_GE(H5O_get_info(&custom_loc, &info, H5O_INFO_TIME), 0);
      EXPECT_EQ(info.ctime, 0);

      // Add old-style H5O_MTIME_ID message and exercise H5O_get_info +
      // H5O_touch updating it.
      std::time_t sample = 1700000000;
      ASSERT_GE(H5O_msg_create(&custom_loc, H5O_MTIME_ID, 0, 0, &sample), 0);
      ASSERT_GE(H5O_get_info(&custom_loc, &info, H5O_INFO_TIME), 0);
      EXPECT_EQ(info.ctime, sample);
      EXPECT_GE(H5O_touch(&custom_loc, /*force=*/false), 0);

      EXPECT_GE(H5O_close(&custom_loc, nullptr), 0);
    }

    // Reopen RDWR so cached messages have native == NULL (lazy decode), then
    // call H5O_touch on g1 (hits H5FL_MALLOC(time_t) in H5O_touch_oh) and
    // H5O_get_info on both v1 headers.
    ScopedFile reopened(H5Fopen(path.c_str(), H5F_ACC_RDWR, fapl.get()));
    ASSERT_TRUE(reopened.valid());
    H5F_t* rf = UnwrapFile(reopened.get());
    ASSERT_NE(rf, nullptr);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5O_loc_t g1_loc{};
    H5O_loc_reset(&g1_loc);
    g1_loc.file = rf;
    g1_loc.addr = new_mtime_addr;
    EXPECT_GE(H5O_touch(&g1_loc, /*force=*/false), 0);

    H5O_info2_t info{};
    ASSERT_GE(H5O_get_info(&g1_loc, &info, H5O_INFO_TIME), 0);
    EXPECT_GT(info.ctime, 0);

    H5O_loc_t old_loc{};
    H5O_loc_reset(&old_loc);
    old_loc.file = rf;
    old_loc.addr = old_mtime_addr;
    ASSERT_GE(H5O_get_info(&old_loc, &info, H5O_INFO_TIME), 0);
    EXPECT_GT(info.ctime, 0);

    // Invalid addresses fail cleanly across H5O_touch / H5O_delete / H5O_pin.
    ScopedErrorSilence silence;
    H5O_loc_t bad_loc{};
    H5O_loc_reset(&bad_loc);
    bad_loc.file = rf;
    bad_loc.addr = new_mtime_addr + 0x100000ULL;
    EXPECT_LT(H5O_touch(&bad_loc, false), 0);
    EXPECT_LT(H5O_delete(rf, bad_loc.addr), 0);
    EXPECT_EQ(H5O_pin(&bad_loc), nullptr);
    EXPECT_LT(H5O_link(&bad_loc, 1), 0);
    EXPECT_LT(H5O_dec_rc_by_loc(&bad_loc), 0);
    EXPECT_LT(H5O__dec_rc(nullptr), 0);
    bad_loc.addr = HADDR_UNDEF;
    EXPECT_EQ(H5O_protect(&bad_loc, H5AC__READ_ONLY_FLAG, false), nullptr);
  }
}

TEST(H5OintTest, InfoQueriesEmptyHeaderSharedMesgAndLocHelpers) {
  // 1. SOHM file so attribute messages get H5O_MSG_FLAG_SHARED, testing
  //    H5O_get_hdr_info shared message accounting.
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
    ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_ATTR_FLAG, 10),
              0);

    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V18, H5F_LIBVER_LATEST), 0);
    ScopedFile file(
        H5Fcreate("sohm_hdr.h5", H5F_ACC_TRUNC, fcpl.get(), fapl.get()));
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    const hsize_t dim = 8;
    ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
    ASSERT_TRUE(space.valid());
    const std::vector<int32_t> vals(dim, 42);

    ScopedGroup g1(
        H5Gcreate2(file.get(), "g1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g1.valid());
    ScopedGroup g2(
        H5Gcreate2(file.get(), "g2", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g2.valid());

    // Write identical large attribute to both groups so SOHM shares it.
    ScopedAttr a1(H5Acreate2(g1.get(), "shared_attr", H5T_NATIVE_INT32,
                             space.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a1.valid());
    ASSERT_GE(H5Awrite(a1.get(), H5T_NATIVE_INT32, vals.data()), 0);
    ScopedAttr a2(H5Acreate2(g2.get(), "shared_attr", H5T_NATIVE_INT32,
                             space.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a2.valid());
    ASSERT_GE(H5Awrite(a2.get(), H5T_NATIVE_INT32, vals.data()), 0);

    // Chunked dataset + committed datatype + subgroup for H5O_get_native_info.
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    const hsize_t cdim = 4;
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &cdim), 0);
    ScopedDataset ds(H5Dcreate2(file.get(), "ds", H5T_NATIVE_INT32, space.get(),
                                H5P_DEFAULT, dcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(ds.valid());
    ScopedType dt(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_TRUE(dt.valid());
    ASSERT_GE(H5Tcommit2(file.get(), "dt", dt.get(), H5P_DEFAULT, H5P_DEFAULT,
                         H5P_DEFAULT),
              0);

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5O_hdr_info_t hdr{};
    ASSERT_GE(H5O_get_hdr_info(H5O_get_loc(g2.get()), &hdr), 0);
    EXPECT_NE(hdr.mesg.shared, 0ULL);

    // Exercise H5O_get_native_info on group, chunked dataset, and committed dt.
    H5O_native_info_t ninfo{};
    EXPECT_GE(
        H5O_get_native_info(H5O_get_loc(g2.get()), &ninfo,
                            H5O_NATIVE_INFO_HDR | H5O_NATIVE_INFO_META_SIZE),
        0);
    EXPECT_GE(H5O_get_native_info(H5O_get_loc(ds.get()), &ninfo,
                                  H5O_NATIVE_INFO_META_SIZE),
              0);
    EXPECT_GE(H5O_get_native_info(H5O_get_loc(dt.get()), &ninfo,
                                  H5O_NATIVE_INFO_META_SIZE),
              0);

    // H5O_get_create_plist & H5O_get_rc_and_type on v2 group.
    ScopedPlist ocpl_out(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(ocpl_out.valid());
    auto* plist_ptr = static_cast<H5P_genplist_t*>(H5I_object(ocpl_out.get()));
    ASSERT_NE(plist_ptr, nullptr);
    EXPECT_GE(H5O_get_create_plist(H5O_get_loc(g2.get()), plist_ptr), 0);

    unsigned rc = 0;
    H5O_type_t otype = H5O_TYPE_UNKNOWN;
    EXPECT_GE(H5O_get_rc_and_type(H5O_get_loc(g2.get()), &rc, &otype), 0);
    EXPECT_EQ(rc, 1U);
    EXPECT_EQ(otype, H5O_TYPE_GROUP);
    EXPECT_GE(H5O_get_rc_and_type(H5O_get_loc(ds.get()), nullptr, nullptr), 0);

    // H5O_obj_type across group/dataset/datatype.
    EXPECT_GE(H5O_obj_type(H5O_get_loc(ds.get()), &otype), 0);
    EXPECT_EQ(otype, H5O_TYPE_DATASET);
    EXPECT_GE(H5O_obj_type(H5O_get_loc(dt.get()), &otype), 0);
    EXPECT_EQ(otype, H5O_TYPE_NAMED_DATATYPE);

    // H5O_get_nlinks on root/group.
    hsize_t nlinks = 99;
    EXPECT_GE(H5O_get_nlinks(H5O_get_loc(g1.get()), &nlinks), 0);
    EXPECT_EQ(nlinks, 0ULL);

    // Empty object header fails H5O__obj_class_real ("unable to determine
    // object type") across H5O_obj_type, H5O__obj_class, H5O_get_info,
    // H5O_get_native_info, H5O_get_rc_and_type.
    H5O_loc_t empty_loc{};
    H5O_loc_reset(&empty_loc);
    ASSERT_GE(H5O_create(f, 32, 0, H5P_GROUP_CREATE_DEFAULT, &empty_loc), 0);
    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5O_obj_type(&empty_loc, &otype), 0);
      EXPECT_EQ(H5O__obj_class(&empty_loc), nullptr);
      H5O_info2_t oinfo{};
      EXPECT_LT(H5O_get_info(&empty_loc, &oinfo, H5O_INFO_BASIC), 0);
      EXPECT_LT(
          H5O_get_native_info(&empty_loc, &ninfo, H5O_NATIVE_INFO_META_SIZE),
          0);
      EXPECT_LT(H5O_get_rc_and_type(&empty_loc, &rc, &otype), 0);
    }
    EXPECT_GE(H5O_close(&empty_loc, nullptr), 0);

    // H5O_get_loc across non-object handle types (including transient dtype)
    // returns nullptr.
    ScopedType transient_dt(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_TRUE(transient_dt.valid());
    ScopedErrorSilence silence;
    EXPECT_EQ(H5O_get_loc(transient_dt.get()), nullptr);
    EXPECT_EQ(H5O_get_loc(file.get()), nullptr);
    EXPECT_EQ(H5O_get_loc(space.get()), nullptr);
    EXPECT_EQ(H5O_get_loc(a1.get()), nullptr);
    EXPECT_EQ(H5O_get_loc(dcpl.get()), nullptr);
    EXPECT_EQ(H5O_get_loc(H5I_INVALID_HID), nullptr);

    // Wrong plist class in H5O_get_create_plist hits H5P_set error branch.
    auto* wrong_plist = static_cast<H5P_genplist_t*>(H5I_object(fapl.get()));
    ASSERT_NE(wrong_plist, nullptr);
    EXPECT_LT(H5O_get_create_plist(H5O_get_loc(g2.get()), wrong_plist), 0);

    // Invalid location errors for info / plist / nlinks / obj_type queries.
    H5O_loc_t bad_loc = *H5O_get_loc(g1.get());
    bad_loc.addr += 0x100000ULL;
    H5O_info2_t oinfo{};
    EXPECT_LT(H5O_get_hdr_info(&bad_loc, &hdr), 0);
    EXPECT_LT(H5O_get_info(&bad_loc, &oinfo, H5O_INFO_BASIC), 0);
    EXPECT_LT(H5O_get_native_info(&bad_loc, &ninfo, H5O_NATIVE_INFO_HDR), 0);
    EXPECT_LT(H5O_get_create_plist(&bad_loc, plist_ptr), 0);
    EXPECT_LT(H5O_get_nlinks(&bad_loc, &nlinks), 0);
    EXPECT_LT(H5O_get_rc_and_type(&bad_loc, &rc, &otype), 0);
    EXPECT_LT(H5O_obj_type(&bad_loc, &otype), 0);
  }

  // 2. H5O_loc_copy (SHALLOW & DEEP) and v1 H5O_get_create_plist.
  {
    ScopedPlist fapl = MakeCoreFapl();
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
        0);
    ScopedFile file(
        H5Fcreate("v1_loc_helpers.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
    ASSERT_TRUE(file.valid());
    ScopedGroup grp(
        H5Gcreate2(file.get(), "g1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    H5O_loc_t* gloc = H5O_get_loc(grp.get());
    ASSERT_NE(gloc, nullptr);

    // v1 object header in H5O_get_create_plist takes false branch for
    // version>1.
    ScopedPlist ocpl_out(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(ocpl_out.valid());
    auto* plist_ptr = static_cast<H5P_genplist_t*>(H5I_object(ocpl_out.get()));
    ASSERT_NE(plist_ptr, nullptr);
    EXPECT_GE(H5O_get_create_plist(gloc, plist_ptr), 0);

    H5O_loc_t src{};
    H5O_loc_reset(&src);
    src.file = gloc->file;
    src.addr = gloc->addr;
    EXPECT_GE(H5O_loc_hold_file(&src), 0);

    H5O_loc_t deep_dst{};
    H5O_loc_reset(&deep_dst);
    EXPECT_GE(H5O_loc_copy(&deep_dst, &src, H5_COPY_DEEP), 0);
    EXPECT_EQ(deep_dst.addr, src.addr);
    EXPECT_TRUE(deep_dst.holding_file);
    EXPECT_GE(H5O_loc_free(&deep_dst), 0);

    H5O_loc_t shallow_dst{};
    H5O_loc_reset(&shallow_dst);
    EXPECT_GE(H5O_loc_copy(&shallow_dst, &src, H5_COPY_SHALLOW), 0);
    EXPECT_EQ(shallow_dst.addr, gloc->addr);
    EXPECT_EQ(src.addr, HADDR_UNDEF);
    EXPECT_FALSE(src.holding_file);
    EXPECT_GE(H5O_loc_free(&shallow_dst), 0);
  }
}

TEST(H5OintTest, VisitHardLinksSoftLinksNonGroupAndInterrupts) {
  ScopedFile file = CreateInMemoryFile("visit.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup sub(
      H5Gcreate2(file.get(), "sub", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(sub.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedDataset ds(H5Dcreate2(sub.get(), "d", H5T_NATIVE_INT32, scalar.get(),
                              H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ds.valid());

  // Hard link subgroup and dataset so rc == 2 triggers visited skip list on
  // both children and root starting object.
  ASSERT_GE(H5Lcreate_hard(file.get(), "/sub", file.get(), "/sub_hard",
                           H5P_DEFAULT, H5P_DEFAULT),
            0);
  ASSERT_GE(H5Lcreate_hard(sub.get(), "d", file.get(), "/d_hard", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);
  // Soft links are ignored during H5Ovisit.
  ASSERT_GE(H5Lcreate_soft("/d_hard", file.get(), "/soft_link", H5P_DEFAULT,
                           H5P_DEFAULT),
            0);

  // 1. Full visit from "/" visits each hard-linked object only once.
  {
    VisitTracker tracker;
    EXPECT_GE(H5Ovisit3(file.get(), H5_INDEX_NAME, H5_ITER_INC,
                        RecordVisitCallback, &tracker, H5O_INFO_ALL),
              0);
    EXPECT_THAT(tracker.paths, Not(Contains("soft_link")));
    EXPECT_EQ(tracker.paths.size(), 3U);  // ".", "d_hard", "sub" (via alpha)
  }

  // 2. Visit starting on "/sub" (rc > 1) and without H5O_INFO_BASIC in fields.
  {
    VisitTracker tracker;
    EXPECT_GE(H5Ovisit_by_name3(file.get(), "/sub", H5_INDEX_NAME, H5_ITER_INC,
                                RecordVisitCallback, &tracker, H5O_INFO_TIME,
                                H5P_DEFAULT),
              0);
    EXPECT_THAT(tracker.paths, UnorderedElementsAre(".", "d"));
  }

  // 3. Visit directly on a non-group object (dataset).
  {
    VisitTracker tracker;
    EXPECT_GE(H5Ovisit_by_name3(file.get(), "/d_hard", H5_INDEX_NAME,
                                H5_ITER_INC, RecordVisitCallback, &tracker,
                                H5O_INFO_BASIC, H5P_DEFAULT),
              0);
    EXPECT_THAT(tracker.paths, UnorderedElementsAre("."));
  }

  // 4. Stop and error return values on root "." and child, plus missing name.
  {
    VisitTracker stop_root{.stop_on_path = "."};
    EXPECT_EQ(H5Ovisit3(file.get(), H5_INDEX_NAME, H5_ITER_INC,
                        RecordVisitCallback, &stop_root, H5O_INFO_BASIC),
              H5_ITER_STOP);

    VisitTracker stop_child{.stop_on_path = "d_hard"};
    EXPECT_EQ(H5Ovisit3(file.get(), H5_INDEX_NAME, H5_ITER_INC,
                        RecordVisitCallback, &stop_child, H5O_INFO_BASIC),
              H5_ITER_STOP);

    ScopedErrorSilence silence;
    VisitTracker err_root{.error_on_path = "."};
    EXPECT_LT(H5Ovisit3(file.get(), H5_INDEX_NAME, H5_ITER_INC,
                        RecordVisitCallback, &err_root, H5O_INFO_BASIC),
              0);

    VisitTracker err_child{.error_on_path = "d_hard"};
    EXPECT_LT(H5Ovisit3(file.get(), H5_INDEX_NAME, H5_ITER_INC,
                        RecordVisitCallback, &err_child, H5O_INFO_BASIC),
              0);

    VisitTracker missing;
    EXPECT_LT(H5Ovisit_by_name3(file.get(), "/does_not_exist", H5_INDEX_NAME,
                                H5_ITER_INC, RecordVisitCallback, &missing,
                                H5O_INFO_BASIC, H5P_DEFAULT),
              0);
  }
}

TEST(H5OintTest, BadAndIncompleteObjectOpenAndVisitPaths) {
  // 1. In-memory file testing linked empty header, dataset missing LAYOUT
  //    message, and sibling unlinked mid-visit.
  {
    ScopedFile file = CreateInMemoryFile("incomplete_objs.h5");
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedGroup g_empty(H5Gcreate2(file.get(), "g_empty", H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g_empty.valid());
    ScopedGroup g_nolayout(H5Gcreate2(file.get(), "g_nolayout", H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g_nolayout.valid());
    ScopedGroup g_sib(
        H5Gcreate2(file.get(), "g_sib", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(g_sib.valid());

    // Sibling groups for H5Ovisit deleting a later link inside callback.
    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    ScopedDataset ds_keep(H5Dcreate2(g_sib.get(), "a_first", H5T_NATIVE_INT32,
                                     scalar.get(), H5P_DEFAULT, H5P_DEFAULT,
                                     H5P_DEFAULT));
    ASSERT_TRUE(ds_keep.valid());
    ScopedDataset ds_doomed(H5Dcreate2(g_sib.get(), "z_doomed",
                                       H5T_NATIVE_INT32, scalar.get(),
                                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(ds_doomed.valid());
    ds_keep.reset();
    ds_doomed.reset();

    {
      ScopedErrorSilence silence;
      EXPECT_LT(H5Ovisit3(g_sib.get(), H5_INDEX_NAME, H5_ITER_INC,
                          DeleteSiblingVisitCallback,
                          const_cast<char*>("z_doomed"), H5O_INFO_BASIC),
                0);
    }

    // Dataset whose H5O_LAYOUT_ID message was removed passes isa() and
    // H5O_get_info(), then fails obj_class->open in H5O_open_by_loc and
    // H5O__visit.
    haddr_t nolayout_addr = HADDR_UNDEF;
    {
      ScopedDataset ds_broken(
          H5Dcreate2(g_nolayout.get(), "half", H5T_NATIVE_INT32, scalar.get(),
                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(ds_broken.valid());
      nolayout_addr = H5O_get_loc(ds_broken.get())->addr;
    }

    {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());

      H5O_loc_t half_oloc{};
      H5O_loc_reset(&half_oloc);
      half_oloc.file = f;
      half_oloc.addr = nolayout_addr;
      ASSERT_GE(H5O_msg_remove(&half_oloc, H5O_LAYOUT_ID, H5O_ALL, false), 0);

      H5G_name_t half_path{};
      H5G_name_reset(&half_path);
      H5G_loc_t half_gloc{&half_oloc, &half_path};
      H5I_type_t otype = H5I_BADID;

      ScopedErrorSilence silence;
      EXPECT_EQ(H5O_open_by_loc(&half_gloc, &otype), nullptr);
      VisitTracker tracker;
      EXPECT_LT(H5Ovisit_by_name3(file.get(), "/g_nolayout/half", H5_INDEX_NAME,
                                  H5_ITER_INC, RecordVisitCallback, &tracker,
                                  H5O_INFO_BASIC, H5P_DEFAULT),
                0);

      // Link an empty header into /g_empty so H5O_INFO_TIME succeeds on v2
      // header prefix while H5O_INFO_BASIC and open fail.
      H5O_loc_t empty_oloc{};
      H5O_loc_reset(&empty_oloc);
      ASSERT_GE(H5O_create(f, 32, 0, H5P_GROUP_CREATE_DEFAULT, &empty_oloc), 0);
      H5O_link_t lnk{};
      lnk.type = H5L_TYPE_HARD;
      lnk.cset = H5T_CSET_ASCII;
      lnk.name = const_cast<char*>("empty");
      lnk.u.hard.addr = empty_oloc.addr;
      H5G_loc_t g_empty_loc{};
      ASSERT_GE(H5G_loc(g_empty.get(), &g_empty_loc), 0);
      ASSERT_GE(H5G_obj_insert(g_empty_loc.oloc, &lnk, /*adj_link=*/true,
                               H5O_TYPE_UNKNOWN, nullptr),
                0);
      EXPECT_GE(H5O_close(&empty_oloc, nullptr), 0);

      EXPECT_EQ(H5O_open_name(&g_empty_loc, "empty", &otype), nullptr);
      EXPECT_EQ(H5O__open_by_idx(&g_empty_loc, ".", H5_INDEX_NAME, H5_ITER_INC,
                                 0, &otype),
                nullptr);
      EXPECT_LT(H5Ovisit_by_name3(file.get(), "/g_empty/empty", H5_INDEX_NAME,
                                  H5_ITER_INC, RecordVisitCallback, &tracker,
                                  H5O_INFO_TIME, H5P_DEFAULT),
                0);
    }
  }

  // 2. Disk file with a corrupted linked target header: H5G_loc_find succeeds
  //    then H5O_get_info fails in both H5O__visit and H5O__visit_cb.
  {
    TempFile path("corrupt_visit_target.h5");
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_fapl_sec2(fapl.get()), 0);
    haddr_t victim_addr = HADDR_UNDEF;
    {
      ScopedFile file(
          H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl.get()));
      ASSERT_TRUE(file.valid());
      ScopedGroup grp(H5Gcreate2(file.get(), "victim", H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT));
      ASSERT_TRUE(grp.valid());
      victim_addr = H5O_get_loc(grp.get())->addr;
    }

    {
      std::fstream fs(path.c_str(),
                      std::ios::in | std::ios::out | std::ios::binary);
      ASSERT_TRUE(fs.is_open());
      fs.seekp(static_cast<std::streamoff>(victim_addr));
      const char zeros[4] = {0, 0, 0, 0};
      fs.write(zeros, sizeof(zeros));
      ASSERT_TRUE(fs.good());
    }

    ScopedFile ro(H5Fopen(path.c_str(), H5F_ACC_RDONLY, fapl.get()));
    ASSERT_TRUE(ro.valid());
    ScopedErrorSilence silence;
    VisitTracker tracker;
    EXPECT_LT(H5Ovisit3(ro.get(), H5_INDEX_NAME, H5_ITER_INC,
                        RecordVisitCallback, &tracker, H5O_INFO_BASIC),
              0);
    EXPECT_LT(H5Ovisit_by_name3(ro.get(), "/victim", H5_INDEX_NAME, H5_ITER_INC,
                                RecordVisitCallback, &tracker, H5O_INFO_BASIC,
                                H5P_DEFAULT),
              0);
  }
}

}  // namespace
}  // namespace hdf5
