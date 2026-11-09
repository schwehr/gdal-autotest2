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
// Unit tests for internal HDF5 dataset lifecycle, object header minimization,
// prefix resolution, storage allocation, fill values, resizing, flushing,
// format conversion, and property list retrieval
// (third_party/hdf5/src/H5Dint.c) in Google3's default non-MPI Linux build
// (-DNDEBUG, deflate/zlib and szip/libaec enabled, parallel/plugins disabled).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Dint.c

#include "third_party/hdf5/src/hdf5.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"

// `:hdf5` is compiled with -DNDEBUG, which omits debug/stats fields from
// H5C_cache_entry_t and placeholder DCPL layout overrides in H5Dlayout.c.
// Define NDEBUG around the prelude and internal headers so struct offsets match
// `:hdf5`.
#ifndef NDEBUG
#define H5DINT_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// H5B2private.h declares a parameter named `try`, which collides with the C++
// keyword and Google3's `-fno-exceptions` `try` macro. Temporarily remap `try`
// while parsing the pure-C internal headers.
#pragma push_macro("try")
#ifdef try
#undef try
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define try h5_param_try
extern "C" {
#define H5D_FRIEND
#define H5S_FRIEND
#define H5T_FRIEND
#define H5VL_FRIEND
#include "third_party/hdf5/src/H5ACprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Dpkg.h"
#include "third_party/hdf5/src/H5Gprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5Pprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
#pragma push_macro("H5_HAVE__FLOAT16")
#ifdef H5_HAVE__FLOAT16
#undef H5_HAVE__FLOAT16
#endif
#include "third_party/hdf5/src/H5Tpkg.h"
#pragma pop_macro("H5_HAVE__FLOAT16")
#include "third_party/hdf5/src/H5VLpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#undef try
#pragma clang diagnostic pop
#pragma pop_macro("try")
#ifdef H5DINT_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5DINT_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::StartsWith;

// RAII wrapper for pushing/popping an HDF5 API context and metadata cache tag.
class ScopedApiContext {
 public:
  explicit ScopedApiContext(hid_t dcpl_id = H5P_DATASET_CREATE_DEFAULT,
                            hid_t dapl_id = H5P_DATASET_ACCESS_DEFAULT,
                            haddr_t tag = 0x10000) {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
      H5CX_set_dcpl(dcpl_id);
      if (dapl_id != H5P_DATASET_ACCESS_DEFAULT) {
        hid_t apl = dapl_id;
        H5CX_set_apl(&apl, H5P_CLS_DACC, H5I_INVALID_HID, false);
      }
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

// Saves and restores a single environment variable across a test scope.
class ScopedEnvVar {
 public:
  explicit ScopedEnvVar(const char* name) : name_(name) {
    if (const char* cur = std::getenv(name_)) {
      saved_ = cur;
    }
  }

  ~ScopedEnvVar() {
    if (saved_.has_value()) {
      setenv(name_, saved_->c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

  void Set(const char* val) const { setenv(name_, val, 1); }
  void Unset() const { unsetenv(name_); }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

 private:
  const char* name_;
  std::optional<std::string> saved_;
};

ScopedSpace MakeSimpleSpace(std::initializer_list<hsize_t> dims,
                            std::initializer_list<hsize_t> max_dims = {}) {
  std::vector<hsize_t> cur(dims);
  std::vector<hsize_t> max(max_dims);
  return ScopedSpace(H5Screate_simple(static_cast<int>(cur.size()), cur.data(),
                                      max.empty() ? nullptr : max.data()));
}

ScopedPlist MakeCoreFaplLibver(H5F_libver_t low, H5F_libver_t high,
                               uint64_t rfic_flags = 0,
                               bool evict_on_close = false) {
  ScopedPlist fapl = MakeCoreFapl();
  if (fapl.valid()) {
    if (H5Pset_libver_bounds(fapl, low, high) < 0) {
      fapl.reset();
      return fapl;
    }
    if (rfic_flags != 0 &&
        H5Pset_relax_file_integrity_checks(fapl, rfic_flags) < 0) {
      fapl.reset();
      return fapl;
    }
    if (evict_on_close && H5Pset_evict_on_close(fapl, true) < 0) {
      fapl.reset();
      return fapl;
    }
  }
  return fapl;
}

H5D_t* UnwrapDataset(hid_t dset_id) {
  return static_cast<H5D_t*>(H5VL_object_verify(dset_id, H5I_DATASET));
}

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

// Custom H5Z filters for testing H5D__create / H5D__check_filters branches.
constexpr H5Z_filter_t kCustomFilterId = 32700;
bool g_allow_can_apply = true;
bool g_allow_set_local = true;

htri_t CustomFilterCanApply(hid_t /*dcpl_id*/, hid_t /*type_id*/,
                            hid_t /*space_id*/) {
  return g_allow_can_apply ? 1 : 0;
}

herr_t CustomFilterSetLocal(hid_t /*dcpl_id*/, hid_t /*type_id*/,
                            hid_t /*space_id*/) {
  return g_allow_set_local ? 0 : -1;
}

size_t CustomFilterApply(unsigned int /*flags*/, size_t /*cd_nelmts*/,
                         const unsigned int /*cd_values*/[], size_t nbytes,
                         size_t* /*buf_size*/, void** /*buf*/) {
  return nbytes;
}

class ScopedCustomFilter {
 public:
  explicit ScopedCustomFilter(unsigned encoder_present = 1) {
    g_allow_can_apply = true;
    g_allow_set_local = true;
    const H5Z_class2_t cls{
        /*version=*/H5Z_CLASS_T_VERS,
        /*id=*/kCustomFilterId,
        /*encoder_present=*/encoder_present,
        /*decoder_present=*/1,
        /*name=*/"h5dint_test_filter",
        /*can_apply=*/CustomFilterCanApply,
        /*set_local=*/CustomFilterSetLocal,
        /*filter=*/CustomFilterApply,
    };
    registered_ = (H5Zregister(&cls) >= 0);
  }

  ~ScopedCustomFilter() {
    g_allow_can_apply = true;
    g_allow_set_local = true;
    if (registered_) {
      H5Zunregister(kCustomFilterId);
    }
  }

  bool ok() const { return registered_; }

  ScopedCustomFilter(const ScopedCustomFilter&) = delete;
  ScopedCustomFilter& operator=(const ScopedCustomFilter&) = delete;

 private:
  bool registered_ = false;
};

herr_t AppendFlushNoopCb(hid_t /*dset_id*/, hsize_t* /*cur_dims*/,
                         void* op_data) {
  if (op_data != nullptr) {
    (*static_cast<int*>(op_data))++;
  }
  return 0;
}

// ===========================================================================
// 1. Package Initialization, Env Var Prefixes & Termination
// ===========================================================================

TEST(H5DintTest, PackageInitAndEnvPrefixes) {
  ASSERT_GE(H5open(), 0);
  EXPECT_GE(H5D_init(), 0);

  ScopedEnvVar saved_vds("HDF5_VDS_PREFIX");
  ScopedEnvVar saved_ext("HDF5_EXTFILE_PREFIX");
  absl::Cleanup restore_lib = [] {
    H5close();
    H5open();
  };

  ASSERT_GE(H5close(), 0);
  saved_vds.Set("/env/vds");
  saved_ext.Set("/env/ext");
  ASSERT_GE(H5open(), 0);

  ScopedFile file = CreateInMemoryFile("env_prefixes.h5");
  ASSERT_TRUE(file.valid());
  ScopedSpace space = MakeSimpleSpace({4});
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file, "dset", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  ScopedPlist dapl(H5Dget_access_plist(dset));
  ASSERT_TRUE(dapl.valid());

  std::array<char, 64> vds_buf{};
  ASSERT_EQ(H5Pget_virtual_prefix(dapl, vds_buf.data(), vds_buf.size()), 8);
  EXPECT_STREQ(vds_buf.data(), "/env/vds");

  std::array<char, 64> ext_buf{};
  ASSERT_EQ(H5Pget_efile_prefix(dapl, ext_buf.data(), ext_buf.size()), 8);
  EXPECT_STREQ(ext_buf.data(), "/env/ext");
}

TEST(H5DintTest, LibraryCloseCleansDanglingDatasetHandles) {
  // Always re-open the HDF5 library so subsequent tests start cleanly.
  absl::Cleanup reopen_lib = [] { H5open(); };

  {
    ScopedFile file = CreateInMemoryFile("dangling.h5");
    ASSERT_TRUE(file.valid());
    ScopedSpace space = MakeSimpleSpace({4});
    ASSERT_TRUE(space.valid());

    ScopedDataset d1(H5Dcreate2(file, "d", H5T_NATIVE_INT32, space, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d1.valid());
    ScopedDataset d2(H5Dopen2(file, "d", H5P_DEFAULT));
    ASSERT_TRUE(d2.valid());

    // Transfer ownership to H5close()'s top-package teardown path.
    d2.release();
    d1.release();
    file.release();
    space.reset();

    EXPECT_GE(H5close(), 0);
  }
  ASSERT_GE(H5open(), 0);
}

// ===========================================================================
// 2. Creation, Named Links, Type/Space Init, Error Paths & Rollbacks
// ===========================================================================

TEST(H5DintTest, CreateNamedAndInternalCreateErrorPaths) {
  ScopedFile file = CreateInMemoryFile("create_errs.h5");
  ASSERT_TRUE(file.valid());
  ScopedSpace space = MakeSimpleSpace({4});
  ASSERT_TRUE(space.valid());

  // Public duplicate link & missing intermediate group -> H5D__create_named error.
  ScopedDataset first(H5Dcreate2(file, "dup", H5T_NATIVE_INT32, space,
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(first.valid());

  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID,
              H5Dcreate2(file, "dup", H5T_NATIVE_INT32, space, H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT));
    EXPECT_EQ(H5I_INVALID_HID,
              H5Dcreate2(file, "missing/sub/d", H5T_NATIVE_INT32, space,
                         H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  }

  H5F_t* f = UnwrapFile(file);
  ASSERT_NE(f, nullptr);
  H5S_t* raw_space = static_cast<H5S_t*>(H5I_object(space));
  ASSERT_NE(raw_space, nullptr);

  // Non-sensible empty compound datatype -> H5T_is_sensible fails.
  ScopedType empty_cmpd(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(empty_cmpd.valid());
  {
    ScopedErrorSilence silence;
    ScopedApiContext ctx;
    EXPECT_EQ(H5D__create(f, empty_cmpd, raw_space, H5P_DATASET_CREATE_DEFAULT,
                          H5P_DATASET_ACCESS_DEFAULT),
              nullptr);
  }

  // Dataspace with no extent -> H5S_has_extent fails.
  ScopedSpace no_ext(H5Screate(H5S_SIMPLE));
  ASSERT_TRUE(no_ext.valid());
  H5S_t* raw_no_ext = static_cast<H5S_t*>(H5I_object(no_ext));
  ASSERT_NE(raw_no_ext, nullptr);
  {
    ScopedErrorSilence silence;
    ScopedApiContext ctx;
    EXPECT_EQ(H5D__create(f, H5T_NATIVE_INT32, raw_no_ext,
                          H5P_DATASET_CREATE_DEFAULT,
                          H5P_DATASET_ACCESS_DEFAULT),
              nullptr);
  }

  // Invalid DCPL ID / Invalid DAPL ID -> H5D__new error cleanup.
  ScopedPlist bad_plist(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(bad_plist.valid());
  hid_t closed_id = bad_plist.get();
  bad_plist.reset();
  {
    ScopedErrorSilence silence;
    ScopedApiContext ctx;
    EXPECT_EQ(H5D__create(f, H5T_NATIVE_INT32, raw_space, closed_id,
                          H5P_DATASET_ACCESS_DEFAULT),
              nullptr);
    EXPECT_EQ(H5D__create(f, H5T_NATIVE_INT32, raw_space,
                          H5P_DATASET_CREATE_DEFAULT, closed_id),
              nullptr);
  }

  // Dimension >= (1ULL << 63) overflows H5VM_power2up in H5D__cache_dataspace_info.
  constexpr hsize_t kHugeDim = (1ULL << 63) + 1ULL;
  ScopedSpace huge_space(H5Screate_simple(1, &kHugeDim, nullptr));
  ASSERT_TRUE(huge_space.valid());
  H5S_t* raw_huge = static_cast<H5S_t*>(H5I_object(huge_space));
  ASSERT_NE(raw_huge, nullptr);
  {
    ScopedErrorSilence silence;
    ScopedApiContext ctx;
    EXPECT_EQ(H5D__create(f, H5T_NATIVE_INT32, raw_huge,
                          H5P_DATASET_CREATE_DEFAULT,
                          H5P_DATASET_ACCESS_DEFAULT),
              nullptr);
  }

  // Force H5D_ALLOC_TIME_DEFAULT inside DCPL -> hits line 1313 error check.
  ScopedPlist bad_alloc_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(bad_alloc_dcpl.valid());
  H5P_genplist_t* dcpl_obj =
      static_cast<H5P_genplist_t*>(H5I_object(bad_alloc_dcpl));
  ASSERT_NE(dcpl_obj, nullptr);
  H5O_fill_t bad_fill{};
  ASSERT_GE(H5P_get(dcpl_obj, H5D_CRT_FILL_VALUE_NAME, &bad_fill), 0);
  bad_fill.alloc_time = H5D_ALLOC_TIME_DEFAULT;
  ASSERT_GE(H5P_poke(dcpl_obj, H5D_CRT_FILL_VALUE_NAME, &bad_fill), 0);
  {
    ScopedErrorSilence silence;
    ScopedApiContext ctx(bad_alloc_dcpl);
    EXPECT_EQ(H5D__create(f, H5T_NATIVE_INT32, raw_space, bad_alloc_dcpl,
                          H5P_DATASET_ACCESS_DEFAULT),
              nullptr);
  }
}

TEST(H5DintTest, TypeInitVariantsAndCommittedAcrossFiles) {
  // File 1: earliest format + native int (reuses immutable type ID) + committed
  // compound type.
  ScopedPlist earliest_fapl =
      MakeCoreFaplLibver(H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
  ASSERT_TRUE(earliest_fapl.valid());
  ScopedFile f1(H5Fcreate("type_f1.h5", H5F_ACC_TRUNC, H5P_DEFAULT,
                          earliest_fapl));
  ASSERT_TRUE(f1.valid());

  ScopedSpace space = MakeSimpleSpace({3});
  ASSERT_TRUE(space.valid());

  ScopedDataset d_native(H5Dcreate2(f1, "native_i32", H5T_NATIVE_INT32, space,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(d_native.valid());

  ScopedType cmpd(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(cmpd.valid());
  ASSERT_GE(H5Tinsert(cmpd, "x", 0, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(cmpd, "y", 4, H5T_NATIVE_FLOAT), 0);
  ASSERT_GE(H5Tcommit2(f1, "committed_cmpd", cmpd, H5P_DEFAULT, H5P_DEFAULT,
                       H5P_DEFAULT),
            0);

  // File 2: v1.8+ format -> use_at_least_v18 = true + committed type from f1.
  ScopedPlist v18_fapl =
      MakeCoreFaplLibver(H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(v18_fapl.valid());
  ScopedFile f2(H5Fcreate("type_f2.h5", H5F_ACC_TRUNC, H5P_DEFAULT, v18_fapl));
  ASSERT_TRUE(f2.valid());

  ScopedDataset d_v18(H5Dcreate2(f2, "v18_i32", H5T_NATIVE_INT32, space,
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(d_v18.valid());

  ScopedDataset d_cross(H5Dcreate2(f2, "cross_cmpd", cmpd, space, H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(d_cross.valid());
}

TEST(H5DintTest, FiltersLayoutVersionBoundsAndCompactErrors) {
  ScopedCustomFilter custom_filter;
  ASSERT_TRUE(custom_filter.ok());

  ScopedFile file = CreateInMemoryFile("filters_bounds.h5");
  ASSERT_TRUE(file.valid());
  ScopedSpace space = MakeSimpleSpace({8});
  ASSERT_TRUE(space.valid());
  constexpr std::array<hsize_t, 1> kChunk{4};

  // Filter on contiguous layout fails.
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_deflate(dcpl, 6), 0);
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID,
              H5Dcreate2(file, "contig_filt", H5T_NATIVE_INT32, space,
                         H5P_DEFAULT, dcpl, H5P_DEFAULT));
  }

  // Filter callbacks failing (can_apply and set_local).
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl, 1, kChunk.data()), 0);
    ASSERT_GE(H5Pset_filter(dcpl, kCustomFilterId, H5Z_FLAG_MANDATORY, 0,
                            nullptr),
              0);

    ScopedErrorSilence silence;
    g_allow_can_apply = false;
    EXPECT_EQ(H5I_INVALID_HID,
              H5Dcreate2(file, "bad_can_apply", H5T_NATIVE_INT32, space,
                         H5P_DEFAULT, dcpl, H5P_DEFAULT));

    g_allow_can_apply = true;
    g_allow_set_local = false;
    EXPECT_EQ(H5I_INVALID_HID,
              H5Dcreate2(file, "bad_set_local", H5T_NATIVE_INT32, space,
                         H5P_DEFAULT, dcpl, H5P_DEFAULT));
    g_allow_set_local = true;
  }

  // ignore_filters == true: H5S_NULL dataspace with an optional filter
  // bypasses H5Z_can_apply / H5Z_set_local and non-chunked pipeline check;
  // a mandatory filter on H5S_NULL fails H5Z_ignore_filters().
  {
    ScopedSpace null_space(H5Screate(H5S_NULL));
    ASSERT_TRUE(null_space.valid());
    ScopedPlist dcpl_opt(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_opt.valid());
    ASSERT_GE(H5Pset_filter(dcpl_opt, kCustomFilterId, H5Z_FLAG_OPTIONAL, 0,
                            nullptr),
              0);
    g_allow_can_apply = false;
    ScopedDataset d_null(H5Dcreate2(file, "null_opt_filt", H5T_NATIVE_INT32,
                                    null_space, H5P_DEFAULT, dcpl_opt,
                                    H5P_DEFAULT));
    EXPECT_TRUE(d_null.valid());
    g_allow_can_apply = true;

    ScopedPlist dcpl_mand(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_mand.valid());
    ASSERT_GE(H5Pset_filter(dcpl_mand, kCustomFilterId, H5Z_FLAG_MANDATORY, 0,
                            nullptr),
              0);
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID,
              H5Dcreate2(file, "null_mand_filt", H5T_NATIVE_INT32, null_space,
                         H5P_DEFAULT, dcpl_mand, H5P_DEFAULT));
  }

  // Compact dataset errors: late alloc_time and oversized (> 64KB).
  {
    ScopedPlist dcpl_late(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_late.valid());
    ASSERT_GE(H5Pset_layout(dcpl_late, H5D_COMPACT), 0);
    ASSERT_GE(H5Pset_alloc_time(dcpl_late, H5D_ALLOC_TIME_LATE), 0);
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID,
              H5Dcreate2(file, "compact_late", H5T_NATIVE_INT32, space,
                         H5P_DEFAULT, dcpl_late, H5P_DEFAULT));

    ScopedSpace big_space = MakeSimpleSpace({20000});
    ASSERT_TRUE(big_space.valid());
    ScopedPlist dcpl_big(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_big.valid());
    ASSERT_GE(H5Pset_layout(dcpl_big, H5D_COMPACT), 0);
    EXPECT_EQ(H5I_INVALID_HID,
              H5Dcreate2(file, "compact_big", H5T_NATIVE_INT32, big_space,
                         H5P_DEFAULT, dcpl_big, H5P_DEFAULT));
  }

  // VDS in a file whose high libver bound is V18 rejects layout version > v3.
  {
    ScopedPlist old_fapl =
        MakeCoreFaplLibver(H5F_LIBVER_EARLIEST, H5F_LIBVER_V18);
    ASSERT_TRUE(old_fapl.valid());
    ScopedFile old_file(
        H5Fcreate("v18_bound.h5", H5F_ACC_TRUNC, H5P_DEFAULT, old_fapl));
    ASSERT_TRUE(old_file.valid());

    ScopedPlist vds_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(vds_dcpl.valid());
    ASSERT_GE(H5Pset_virtual(vds_dcpl, space, "src.h5", "src", space), 0);

    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID,
              H5Dcreate2(old_file, "vds_bad_bound", H5T_NATIVE_INT32, space,
                         H5P_DEFAULT, vds_dcpl, H5P_DEFAULT));
  }
}

TEST(H5DintTest, FillValueOhInfoAndUnusualUnusedBits) {
  ScopedPlist early_fapl =
      MakeCoreFaplLibver(H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
  ASSERT_TRUE(early_fapl.valid());
  ScopedFile file(
      H5Fcreate("oh_info.h5", H5F_ACC_TRUNC, H5P_DEFAULT, early_fapl));
  ASSERT_TRUE(file.valid());
  ScopedSpace space = MakeSimpleSpace({4});
  ASSERT_TRUE(space.valid());

  // 1. VL string with default IFSET -> promotes fill_time to ALLOC.
  ScopedType vlen_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vlen_str.valid());
  ASSERT_GE(H5Tset_size(vlen_str, H5T_VARIABLE), 0);
  {
    ScopedDataset d_vl(H5Dcreate2(file, "vl_ifset", vlen_str, space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_vl.valid());

    ScopedPlist dcpl(H5Dget_create_plist(d_vl));
    ASSERT_TRUE(dcpl.valid());
    H5D_fill_time_t ft = H5D_FILL_TIME_IFSET;
    ASSERT_GE(H5Pget_fill_time(dcpl, &ft), 0);
    EXPECT_EQ(ft, H5D_FILL_TIME_ALLOC);
  }

  // 2. VL type with H5D_FILL_TIME_NEVER must fail.
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_fill_time(dcpl, H5D_FILL_TIME_NEVER), 0);
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID,
              H5Dcreate2(file, "vl_never", vlen_str, space, H5P_DEFAULT, dcpl,
                         H5P_DEFAULT));
  }

  // 3. Undefined fill value with H5D_FILL_TIME_ALLOC must fail.
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_fill_value(dcpl, H5T_NATIVE_INT32, nullptr), 0);
    ASSERT_GE(H5Pset_fill_time(dcpl, H5D_FILL_TIME_ALLOC), 0);
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID,
              H5Dcreate2(file, "undef_alloc", H5T_NATIVE_INT32, space,
                         H5P_DEFAULT, dcpl, H5P_DEFAULT));
  }

  // 4. Fill value requiring type conversion (int32 fill -> double dataset)
  // on EARLIEST format (writes legacy H5O_FILL_ID + H5O_FILL_NEW_ID + mtime).
  {
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    constexpr int32_t kIntFill = 123;
    ASSERT_GE(H5Pset_fill_value(dcpl, H5T_NATIVE_INT32, &kIntFill), 0);
    ScopedDataset d_conv(H5Dcreate2(file, "fill_conv", H5T_NATIVE_DOUBLE, space,
                                    H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(d_conv.valid());

    // Compact dataset adds raw compact size to ohdr_size.
    ScopedPlist compact_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(compact_dcpl.valid());
    ASSERT_GE(H5Pset_layout(compact_dcpl, H5D_COMPACT), 0);
    ScopedDataset d_comp(H5Dcreate2(file, "compact_oh", H5T_NATIVE_INT32, space,
                                    H5P_DEFAULT, compact_dcpl, H5P_DEFAULT));
    ASSERT_TRUE(d_comp.valid());
  }

  // 5. Numeric type with unusual unused bits (precision = 8 in 32-bit int):
  // fails on v1 ohdr without RFIC flag, succeeds with RFIC or latest format.
  ScopedType unusual_dt(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(unusual_dt.valid());
  ASSERT_GE(H5Tset_precision(unusual_dt, 8), 0);
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID,
              H5Dcreate2(file, "unusual_fail", unusual_dt, space, H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT));
  }
  {
    ScopedPlist rfic_fapl = MakeCoreFaplLibver(
        H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST,
        H5F_RFIC_UNUSUAL_NUM_UNUSED_NUMERIC_BITS);
    ASSERT_TRUE(rfic_fapl.valid());
    ScopedFile rfic_file(
        H5Fcreate("rfic.h5", H5F_ACC_TRUNC, H5P_DEFAULT, rfic_fapl));
    ASSERT_TRUE(rfic_file.valid());
    ScopedDataset d_rfic(H5Dcreate2(rfic_file, "unusual_ok", unusual_dt, space,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    EXPECT_TRUE(d_rfic.valid());
  }
}

TEST(H5DintTest, MinimizedDatasetObjectHeaders) {
  TempDir dir("min_ohdr");

  // Case A: EARLIEST format with per-DCPL hint, user fill value (compat fill
  // message), chunked+deflate (pline), EFL, compact, and v1 mtime message.
  {
    ScopedPlist fapl =
        MakeCoreFaplLibver(H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
    ASSERT_TRUE(fapl.valid());
    const std::filesystem::path h5_path = dir.child("min_early.h5");
    ScopedFile file(
        H5Fcreate(h5_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(file.valid());
    ScopedSpace space = MakeSimpleSpace({4});
    ASSERT_TRUE(space.valid());

    // Chunked + deflate + user fill value + minimized ohdr.
    ScopedPlist dcpl_chunk(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_chunk.valid());
    constexpr std::array<hsize_t, 1> kChunk{2};
    constexpr int32_t kFill = 7;
    ASSERT_GE(H5Pset_chunk(dcpl_chunk, 1, kChunk.data()), 0);
    ASSERT_GE(H5Pset_deflate(dcpl_chunk, 1), 0);
    ASSERT_GE(H5Pset_fill_value(dcpl_chunk, H5T_NATIVE_INT32, &kFill), 0);
    ASSERT_GE(H5Pset_dset_no_attrs_hint(dcpl_chunk, true), 0);

    ScopedDataset d_chunk(H5Dcreate2(file, "min_chunk", H5T_NATIVE_INT32, space,
                                     H5P_DEFAULT, dcpl_chunk, H5P_DEFAULT));
    EXPECT_TRUE(d_chunk.valid());

    // Contiguous + EFL + minimized ohdr.
    const std::filesystem::path ext_path = dir.child("min_efl.bin");
    ScopedPlist dcpl_efl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_efl.valid());
    ASSERT_GE(H5Pset_external(dcpl_efl, ext_path.c_str(), 0, 16), 0);
    ASSERT_GE(H5Pset_dset_no_attrs_hint(dcpl_efl, true), 0);

    ScopedDataset d_efl(H5Dcreate2(file, "min_efl", H5T_NATIVE_INT32, space,
                                   H5P_DEFAULT, dcpl_efl, H5P_DEFAULT));
    EXPECT_TRUE(d_efl.valid());

    // Compact + minimized ohdr (uses H5O_apply_ohdr compact allocation).
    ScopedPlist dcpl_comp(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_comp.valid());
    ASSERT_GE(H5Pset_layout(dcpl_comp, H5D_COMPACT), 0);
    ASSERT_GE(H5Pset_dset_no_attrs_hint(dcpl_comp, true), 0);
    ScopedDataset d_comp(H5Dcreate2(file, "min_comp", H5T_NATIVE_INT32, space,
                                    H5P_DEFAULT, dcpl_comp, H5P_DEFAULT));
    EXPECT_TRUE(d_comp.valid());
  }

  // Case B: LATEST format with file-level H5Fset_dset_no_attrs_hint and
  // track_times disabled.
  {
    ScopedPlist fapl =
        MakeCoreFaplLibver(H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
    ASSERT_TRUE(fapl.valid());
    ScopedFile file(
        H5Fcreate("min_latest.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(file.valid());
    ASSERT_GE(H5Fset_dset_no_attrs_hint(file, true), 0);

    ScopedSpace space = MakeSimpleSpace({4});
    ASSERT_TRUE(space.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_obj_track_times(dcpl, false), 0);

    ScopedDataset d_min(H5Dcreate2(file, "min_latest", H5T_NATIVE_INT32, space,
                                   H5P_DEFAULT, dcpl, H5P_DEFAULT));
    EXPECT_TRUE(d_min.valid());
  }
}

TEST(H5DintTest, FilePrefixesOriginDotEmptyAndAbs) {
  TempDir dir("prefixes");
  const std::filesystem::path h5_path = dir.child("pref.h5");
  ScopedFile file(
      H5Fcreate(h5_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(file.valid());
  ScopedSpace space = MakeSimpleSpace({4});
  ASSERT_TRUE(space.valid());

  struct PrefixCase {
    const char* name;
    const char* efile_pref;
    const char* vds_pref;
    std::string_view expected_efile;
    std::string_view expected_vds;
  };
  constexpr auto kCases = std::to_array<PrefixCase>({
      {"p_null", nullptr, nullptr, "", ""},
      {"p_empty", "", "", "", ""},
      {"p_dot", ".", ".", "", ""},
      {"p_abs", "/opt/ext", "/opt/vds", "/opt/ext", "/opt/vds"},
  });

  for (const auto& tc : kCases) {
    ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dapl.valid());
    ASSERT_GE(H5Pset_efile_prefix(dapl, tc.efile_pref), 0);
    ASSERT_GE(H5Pset_virtual_prefix(dapl, tc.vds_pref), 0);

    ScopedDataset dset(H5Dcreate2(file, tc.name, H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, H5P_DEFAULT, dapl));
    ASSERT_TRUE(dset.valid());
    ScopedPlist out_dapl(H5Dget_access_plist(dset));
    ASSERT_TRUE(out_dapl.valid());

    std::array<char, 256> buf{};
    ssize_t len = H5Pget_efile_prefix(out_dapl, buf.data(), buf.size());
    ASSERT_GE(len, 0);
    EXPECT_EQ(std::string_view(buf.data(), static_cast<size_t>(len)),
              tc.expected_efile);

    len = H5Pget_virtual_prefix(out_dapl, buf.data(), buf.size());
    ASSERT_GE(len, 0);
    EXPECT_EQ(std::string_view(buf.data(), static_cast<size_t>(len)),
              tc.expected_vds);
  }

  // ${ORIGIN}/sub expands against directory of pref.h5.
  {
    ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dapl.valid());
    ASSERT_GE(H5Pset_efile_prefix(dapl, "${ORIGIN}/extsub"), 0);
    ASSERT_GE(H5Pset_virtual_prefix(dapl, "${ORIGIN}/vdssub"), 0);

    ScopedDataset dset(H5Dcreate2(file, "p_origin", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, H5P_DEFAULT, dapl));
    ASSERT_TRUE(dset.valid());
    ScopedPlist out_dapl(H5Dget_access_plist(dset));
    ASSERT_TRUE(out_dapl.valid());

    std::array<char, 512> ebuf{};
    ASSERT_GT(H5Pget_efile_prefix(out_dapl, ebuf.data(), ebuf.size()), 0);
    EXPECT_THAT(std::string(ebuf.data()), StartsWith(dir.string()));
    EXPECT_TRUE(std::string_view(ebuf.data()).ends_with("/extsub"));

    std::array<char, 512> vbuf{};
    ASSERT_GT(H5Pget_virtual_prefix(out_dapl, vbuf.data(), vbuf.size()), 0);
    EXPECT_THAT(std::string(vbuf.data()), StartsWith(dir.string()));
    EXPECT_TRUE(std::string_view(vbuf.data()).ends_with("/vdssub"));
  }
}

TEST(H5DintTest, AppendFlushSetupValidAndErrorPaths) {
  ScopedFile file = CreateInMemoryFile("append_flush.h5");
  ASSERT_TRUE(file.valid());

  ScopedSpace space = MakeSimpleSpace({4, 3}, {H5S_UNLIMITED, 3});
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  constexpr std::array<hsize_t, 2> kChunk{2, 3};
  ASSERT_GE(H5Pset_chunk(dcpl, 2, kChunk.data()), 0);

  // Valid append flush setup with boundary={2, 0}.
  int cb_count = 0;
  {
    ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dapl.valid());
    constexpr std::array<hsize_t, 2> kBoundary{2, 0};
    ASSERT_GE(H5Pset_append_flush(dapl, 2, kBoundary.data(), AppendFlushNoopCb,
                                  &cb_count),
              0);

    ScopedDataset dset(H5Dcreate2(file, "valid_af", H5T_NATIVE_INT32, space,
                                  H5P_DEFAULT, dcpl, dapl));
    ASSERT_TRUE(dset.valid());

    ScopedPlist out_dapl(H5Dget_access_plist(dset));
    ASSERT_TRUE(out_dapl.valid());
    std::array<hsize_t, 2> got_boundary{};
    H5D_append_cb_t got_cb = nullptr;
    void* got_udata = nullptr;
    ASSERT_GE(H5Pget_append_flush(out_dapl, 2, got_boundary.data(), &got_cb,
                                  &got_udata),
              0);
    EXPECT_THAT(got_boundary, ElementsAre(2, 0));
    EXPECT_EQ(got_cb, AppendFlushNoopCb);
    EXPECT_EQ(got_udata, &cb_count);
  }

  // Rank mismatch (ndims = 1 != dataset rank 2).
  {
    ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dapl.valid());
    constexpr std::array<hsize_t, 1> kBadRankBoundary{2};
    ASSERT_GE(H5Pset_append_flush(dapl, 1, kBadRankBoundary.data(),
                                  AppendFlushNoopCb, nullptr),
              0);
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID,
              H5Dcreate2(file, "bad_rank_af", H5T_NATIVE_INT32, space,
                         H5P_DEFAULT, dcpl, dapl));
  }

  // Non-zero boundary on non-extendible dimension (dim 1 fixed at 3).
  {
    ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dapl.valid());
    constexpr std::array<hsize_t, 2> kFixedDimBoundary{0, 2};
    ASSERT_GE(H5Pset_append_flush(dapl, 2, kFixedDimBoundary.data(),
                                  AppendFlushNoopCb, nullptr),
              0);
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID,
              H5Dcreate2(file, "bad_dim_af", H5T_NATIVE_INT32, space,
                         H5P_DEFAULT, dcpl, dapl));
  }
}

// ===========================================================================
// 3. Opening, Reopening, Legacy/Missing Fill Messages, Mismatched Prefixes
// ===========================================================================

TEST(H5DintTest, OpenNameErrorsAndPrefixMismatch) {
  TempDir dir("open_prefix");
  const std::filesystem::path h5_path = dir.child("open.h5");
  const std::filesystem::path ext_path = dir.child("ext_raw.bin");

  ScopedFile file(
      H5Fcreate(h5_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(file.valid());
  ScopedGroup grp(H5Gcreate2(file, "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  ScopedSpace space = MakeSimpleSpace({4});
  ASSERT_TRUE(space.valid());
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(H5Pset_external(dcpl, "ext_raw.bin", 0, 16), 0);

  ScopedPlist dapl1(H5Pcreate(H5P_DATASET_ACCESS));
  ASSERT_TRUE(dapl1.valid());
  ASSERT_GE(H5Pset_efile_prefix(dapl1, "${ORIGIN}"), 0);

  ScopedDataset d1(H5Dcreate2(file, "efl_d", H5T_NATIVE_INT32, space,
                              H5P_DEFAULT, dcpl, dapl1));
  ASSERT_TRUE(d1.valid());

  // Opening a non-existent path or a group fails inside H5D__open_name.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID, H5Dopen2(file, "no_such_dset", H5P_DEFAULT));
    EXPECT_EQ(H5I_INVALID_HID, H5Dopen2(file, "grp", H5P_DEFAULT));
  }

  // Opening a second handle with identical extfile_prefix succeeds (fo_count=2).
  ScopedDataset d2(H5Dopen2(file, "efl_d", dapl1));
  EXPECT_TRUE(d2.valid());

  // Reopening while d1 is open with different or unset extfile_prefix fails.
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID, H5Dopen2(file, "efl_d", H5P_DEFAULT));

    ScopedPlist dapl_diff(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dapl_diff.valid());
    ASSERT_GE(H5Pset_efile_prefix(dapl_diff, "/other/dir"), 0);
    EXPECT_EQ(H5I_INVALID_HID, H5Dopen2(file, "efl_d", dapl_diff));
  }

  // Also test when first open has NULL prefix and second open sets a prefix.
  d2.reset();
  d1.reset();
  ScopedDataset d_nopref(H5Dopen2(file, "efl_d", H5P_DEFAULT));
  ASSERT_TRUE(d_nopref.valid());
  {
    ScopedErrorSilence silence;
    EXPECT_EQ(H5I_INVALID_HID, H5Dopen2(file, "efl_d", dapl1));
  }
  d_nopref.reset();
  grp.reset();
  file.reset();
  std::error_code ec;
  std::filesystem::remove(ext_path, ec);
}

TEST(H5DintTest, OpenOidLegacyAndRemovedFillMessagesAndCompoundFillConv) {
  TempDir dir("legacy_fill");
  const std::filesystem::path h5_path = dir.child("legacy.h5");

  // Use persistent Sec2 file because we reopen after removing ohdr messages.
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl, H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST),
            0);

  // Create datasets to test:
  // - legacy H5O_FILL_ID only (H5O_FILL_NEW_ID removed)
  // - no fill messages on COMPACT, CONTIGUOUS, CHUNKED, and VIRTUAL
  // - VL string and compound (int32 + VL string) fill values persisted on disk
  struct CmpdVl {
    int32_t tag;
    const char* text;
  };
  ScopedType vl_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vl_str.valid());
  ASSERT_GE(H5Tset_size(vl_str, H5T_VARIABLE), 0);

  ScopedType cmpd_vl(H5Tcreate(H5T_COMPOUND, sizeof(CmpdVl)));
  ASSERT_TRUE(cmpd_vl.valid());
  ASSERT_GE(H5Tinsert(cmpd_vl, "tag", offsetof(CmpdVl, tag), H5T_NATIVE_INT32),
            0);
  ASSERT_GE(H5Tinsert(cmpd_vl, "text", offsetof(CmpdVl, text), vl_str), 0);

  {
    ScopedFile file(
        H5Fcreate(h5_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(file.valid());
    ScopedSpace space = MakeSimpleSpace({4});
    ASSERT_TRUE(space.valid());

    // 1. Old fill message test (EARLIEST writes both H5O_FILL_NEW_ID & OLD).
    ScopedPlist dcpl_fill(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_fill.valid());
    constexpr int32_t kFillVal = 99;
    ASSERT_GE(H5Pset_fill_value(dcpl_fill, H5T_NATIVE_INT32, &kFillVal), 0);
    ScopedDataset d_old_fill(H5Dcreate2(file, "old_fill", H5T_NATIVE_INT32,
                                        space, H5P_DEFAULT, dcpl_fill,
                                        H5P_DEFAULT));
    ASSERT_TRUE(d_old_fill.valid());
    {
      ScopedApiContext ctx;
      H5D_t* d = UnwrapDataset(d_old_fill);
      ASSERT_NE(d, nullptr);
      ASSERT_GE(H5O_msg_remove(&d->oloc, H5O_FILL_NEW_ID, H5O_ALL, false), 0);
    }

    // 2. Strip both fill messages from compact, contig, chunked, and vds.
    ScopedPlist dcpl_comp(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_comp.valid());
    ASSERT_GE(H5Pset_layout(dcpl_comp, H5D_COMPACT), 0);
    ScopedDataset d_comp(H5Dcreate2(file, "nofill_comp", H5T_NATIVE_INT32,
                                    space, H5P_DEFAULT, dcpl_comp,
                                    H5P_DEFAULT));
    ASSERT_TRUE(d_comp.valid());

    ScopedDataset d_contig(H5Dcreate2(file, "nofill_contig", H5T_NATIVE_INT32,
                                      space, H5P_DEFAULT, H5P_DEFAULT,
                                      H5P_DEFAULT));
    ASSERT_TRUE(d_contig.valid());

    ScopedPlist dcpl_chunk(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_chunk.valid());
    constexpr std::array<hsize_t, 1> kChunk{2};
    ASSERT_GE(H5Pset_chunk(dcpl_chunk, 1, kChunk.data()), 0);
    ScopedDataset d_chunk(H5Dcreate2(file, "nofill_chunk", H5T_NATIVE_INT32,
                                     space, H5P_DEFAULT, dcpl_chunk,
                                     H5P_DEFAULT));
    ASSERT_TRUE(d_chunk.valid());

    ScopedPlist dcpl_vds(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_vds.valid());
    ASSERT_GE(H5Pset_virtual(dcpl_vds, space, ".", "nofill_contig", space), 0);
    // VDS requires layout v4 even with H5F_LIBVER_LATEST high bound.
    ScopedDataset d_vds(H5Dcreate2(file, "nofill_vds", H5T_NATIVE_INT32, space,
                                   H5P_DEFAULT, dcpl_vds, H5P_DEFAULT));
    ASSERT_TRUE(d_vds.valid());

    for (hid_t did : {d_comp.get(), d_contig.get(), d_chunk.get(),
                      d_vds.get()}) {
      ScopedApiContext ctx;
      H5D_t* d = UnwrapDataset(did);
      ASSERT_NE(d, nullptr);
      ASSERT_GE(H5O_msg_remove(&d->oloc, H5O_FILL_NEW_ID, H5O_ALL, false), 0);
    }

    // 3. Dataset with compound (int32 + VL string) user-defined fill value
    // + zero-size fill check on legacy file if reopened.
    ScopedPlist dcpl_cmpd_fill(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_cmpd_fill.valid());
    CmpdVl cmpd_fill_val{42, "hello"};
    ASSERT_GE(H5Pset_fill_value(dcpl_cmpd_fill, cmpd_vl, &cmpd_fill_val), 0);
    ScopedDataset d_cmpd(H5Dcreate2(file, "cmpd_vl_fill", cmpd_vl, space,
                                    H5P_DEFAULT, dcpl_cmpd_fill, H5P_DEFAULT));
    ASSERT_TRUE(d_cmpd.valid());
  }

  // Reopen file and inspect all datasets + DCPLs.
  ScopedFile rfile(H5Fopen(h5_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(rfile.valid());

  {
    ScopedDataset d_old(H5Dopen2(rfile, "old_fill", H5P_DEFAULT));
    ASSERT_TRUE(d_old.valid());
    ScopedPlist dcpl(H5Dget_create_plist(d_old));
    ASSERT_TRUE(dcpl.valid());
    int32_t got_fill = 0;
    ASSERT_GE(H5Pget_fill_value(dcpl, H5T_NATIVE_INT32, &got_fill), 0);
    EXPECT_EQ(got_fill, 99);
  }

  for (const char* name : {"nofill_comp", "nofill_contig", "nofill_chunk",
                           "nofill_vds"}) {
    ScopedDataset d(H5Dopen2(rfile, name, H5P_DEFAULT));
    ASSERT_TRUE(d.valid()) << name;
  }

  {
    ScopedDataset d_cmpd(H5Dopen2(rfile, "cmpd_vl_fill", H5P_DEFAULT));
    ASSERT_TRUE(d_cmpd.valid());
    ScopedPlist dcpl(H5Dget_create_plist(d_cmpd));
    ASSERT_TRUE(dcpl.valid());
    CmpdVl out_fill{0, nullptr};
    ASSERT_GE(H5Pget_fill_value(dcpl, cmpd_vl, &out_fill), 0);
    EXPECT_EQ(out_fill.tag, 42);
    ASSERT_NE(out_fill.text, nullptr);
    EXPECT_STREQ(out_fill.text, "hello");
    std::free(const_cast<char*>(out_fill.text));
  }
}

// ===========================================================================
// 4. Close Edge Cases, Refresh (All Layouts), Flush, Mark & Accessors
// ===========================================================================

TEST(H5DintTest, CloseEdgeCasesSieveChunkedVdsSubDsetCorkEvictFileFirst) {
  TempDir dir("close_edges");

  // 1. VDS with %b printf mapping creating sub_dset entries, closed after I/O.
  {
    const std::filesystem::path main_path = dir.child("vds_main.h5");
    ScopedFile main_file(
        H5Fcreate(main_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(main_file.valid());

    ScopedSpace src_space = MakeSimpleSpace({2});
    ASSERT_TRUE(src_space.valid());
    ScopedDataset src0(H5Dcreate2(main_file, "src_0", H5T_NATIVE_INT32,
                                  src_space, H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(src0.valid());
    ScopedDataset src1(H5Dcreate2(main_file, "src_1", H5T_NATIVE_INT32,
                                  src_space, H5P_DEFAULT, H5P_DEFAULT,
                                  H5P_DEFAULT));
    ASSERT_TRUE(src1.valid());
    constexpr std::array<int32_t, 2> kV0{10, 11};
    constexpr std::array<int32_t, 2> kV1{20, 21};
    ASSERT_GE(H5Dwrite(src0, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kV0.data()),
              0);
    ASSERT_GE(H5Dwrite(src1, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kV1.data()),
              0);
    src0.reset();
    src1.reset();

    // Exact mapping (source_dset) + unlimited printf mapping (sub_dset).
    ScopedSpace vspace = MakeSimpleSpace({4, 2}, {H5S_UNLIMITED, 2});
    ASSERT_TRUE(vspace.valid());
    constexpr std::array<hsize_t, 2> kStartPrint{0, 0};
    constexpr std::array<hsize_t, 2> kStridePrint{2, 1};
    constexpr std::array<hsize_t, 2> kCountPrint{H5S_UNLIMITED, 1};
    constexpr std::array<hsize_t, 2> kBlockPrint{1, 2};
    ASSERT_GE(H5Sselect_hyperslab(vspace, H5S_SELECT_SET, kStartPrint.data(),
                                  kStridePrint.data(), kCountPrint.data(),
                                  kBlockPrint.data()),
              0);

    ScopedPlist vds_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(vds_dcpl.valid());
    ASSERT_GE(H5Pset_virtual(vds_dcpl, vspace, ".", "src_%b", src_space), 0);

    ScopedSpace vspace_exact = MakeSimpleSpace({4, 2}, {H5S_UNLIMITED, 2});
    ASSERT_TRUE(vspace_exact.valid());
    constexpr std::array<hsize_t, 2> kStartExact{1, 0};
    constexpr std::array<hsize_t, 2> kCountExact{1, 2};
    ASSERT_GE(H5Sselect_hyperslab(vspace_exact, H5S_SELECT_SET,
                                  kStartExact.data(), nullptr,
                                  kCountExact.data(), nullptr),
              0);
    ASSERT_GE(
        H5Pset_virtual(vds_dcpl, vspace_exact, ".", "src_1", src_space), 0);

    ScopedPlist vdapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(vdapl.valid());
    ASSERT_GE(H5Pset_virtual_printf_gap(vdapl, 1), 0);

    ScopedDataset vds(H5Dcreate2(main_file, "vds_printf", H5T_NATIVE_INT32,
                                 vspace, H5P_DEFAULT, vds_dcpl, vdapl));
    ASSERT_TRUE(vds.valid());

    std::array<int32_t, 8> read_buf{};
    ASSERT_GE(H5Dread(vds, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      read_buf.data()),
              0);
    EXPECT_THAT(read_buf, ElementsAre(10, 11, 20, 21, 20, 21, 0, 0));

    // Exercise H5Dget_space and H5Dget_access_plist on VDS.
    ScopedSpace got_vspace(H5Dget_space(vds));
    EXPECT_TRUE(got_vspace.valid());
    ScopedPlist got_vdapl(H5Dget_access_plist(vds));
    EXPECT_TRUE(got_vdapl.valid());
  }

  // 2. Evict-on-close, corking, partial contiguous sieve buf, single-element
  // chunked I/O, and closing file before dataset handle.
  ScopedPlist evict_fapl =
      MakeCoreFaplLibver(H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST, 0,
                         /*evict_on_close=*/true);
  ASSERT_TRUE(evict_fapl.valid());
  ScopedFile efile(
      H5Fcreate("evict_cork.h5", H5F_ACC_TRUNC, H5P_DEFAULT, evict_fapl));
  ASSERT_TRUE(efile.valid());

  ScopedSpace s1d = MakeSimpleSpace({16});
  ASSERT_TRUE(s1d.valid());

  // Contiguous partial write populates and dirties sieve_buf.
  {
    ScopedDataset d_contig(H5Dcreate2(efile, "c_sieve", H5T_NATIVE_INT32, s1d,
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_contig.valid());
    ScopedSpace sel(H5Scopy(s1d));
    ASSERT_TRUE(sel.valid());
    constexpr std::array<hsize_t, 1> kOff{1};
    constexpr std::array<hsize_t, 1> kCnt{2};
    ASSERT_GE(H5Sselect_hyperslab(sel, H5S_SELECT_SET, kOff.data(), nullptr,
                                  kCnt.data(), nullptr),
              0);
    ScopedSpace mspace = MakeSimpleSpace({2});
    ASSERT_TRUE(mspace.valid());
    constexpr std::array<int32_t, 2> kVals{55, 66};
    ASSERT_GE(H5Dwrite(d_contig, H5T_NATIVE_INT32, mspace, sel, H5P_DEFAULT,
                       kVals.data()),
              0);
    // Cork dataset metadata before close to hit uncork in H5D_close.
    ASSERT_GE(H5Odisable_mdc_flushes(d_contig), 0);
  }  // Closes d_contig while efile stays open -> evict-on-close branch!

  // Chunked single-element + multi-element I/O, then close file BEFORE dataset.
  ScopedPlist chunk_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(chunk_dcpl.valid());
  constexpr std::array<hsize_t, 1> kChunk4{4};
  ASSERT_GE(H5Pset_chunk(chunk_dcpl, 1, kChunk4.data()), 0);
  ScopedDataset d_chunk(H5Dcreate2(efile, "chunk_io", H5T_NATIVE_INT32, s1d,
                                   H5P_DEFAULT, chunk_dcpl, H5P_DEFAULT));
  ASSERT_TRUE(d_chunk.valid());

  ScopedSpace pt_fspace(H5Scopy(s1d));
  ASSERT_TRUE(pt_fspace.valid());
  constexpr std::array<hsize_t, 1> kCoord{3};
  ASSERT_GE(H5Sselect_elements(pt_fspace, H5S_SELECT_SET, 1, kCoord.data()), 0);
  ScopedSpace scalar = MakeSimpleSpace({1});
  ASSERT_TRUE(scalar.valid());
  int32_t single_val = 777;
  ASSERT_GE(H5Dwrite(d_chunk, H5T_NATIVE_INT32, scalar, pt_fspace, H5P_DEFAULT,
                     &single_val),
            0);

  std::array<int32_t, 16> full_buf{};
  ASSERT_GE(H5Dread(d_chunk, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    full_buf.data()),
            0);
  EXPECT_EQ(full_buf[3], 777);

  // Drop file handle first, then d_chunk closes the underlying file.
  efile.reset();
  d_chunk.reset();
}

TEST(H5DintTest, MultiOpenRefreshAndFlushAllAcrossFiles) {
  ScopedFile f1 = CreateInMemoryFile("flush1.h5");
  ASSERT_TRUE(f1.valid());
  ScopedFile f2 = CreateInMemoryFile("flush2.h5");
  ASSERT_TRUE(f2.valid());

  ScopedSpace s1 = MakeSimpleSpace({8});
  ASSERT_TRUE(s1.valid());
  ScopedSpace s2 = MakeSimpleSpace({2});
  ASSERT_TRUE(s2.valid());

  // Multi-open refresh across Contiguous (dirty sieve), Chunked, Compact, VDS.
  ScopedDataset d_contig_a(H5Dcreate2(f1, "contig", H5T_NATIVE_INT32, s1,
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(d_contig_a.valid());
  ScopedDataset d_contig_b(H5Dopen2(f1, "contig", H5P_DEFAULT));
  ASSERT_TRUE(d_contig_b.valid());
  {
    ScopedSpace sel(H5Scopy(s1));
    ASSERT_TRUE(sel.valid());
    constexpr std::array<hsize_t, 1> kOff{1};
    constexpr std::array<hsize_t, 1> kCnt{2};
    ASSERT_GE(H5Sselect_hyperslab(sel, H5S_SELECT_SET, kOff.data(), nullptr,
                                  kCnt.data(), nullptr),
              0);
    constexpr std::array<int32_t, 2> kW{3, 4};
    ASSERT_GE(H5Dwrite(d_contig_a, H5T_NATIVE_INT32, s2, sel, H5P_DEFAULT,
                       kW.data()),
              0);
  }
  EXPECT_GE(H5Drefresh(d_contig_a), 0);

  // Chunked multi-open with single + full I/O before refresh.
  ScopedPlist chunk_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(chunk_dcpl.valid());
  constexpr std::array<hsize_t, 1> kChunk4{4};
  ASSERT_GE(H5Pset_chunk(chunk_dcpl, 1, kChunk4.data()), 0);
  ScopedDataset d_chunk_a(H5Dcreate2(f1, "chunked", H5T_NATIVE_INT32, s1,
                                     H5P_DEFAULT, chunk_dcpl, H5P_DEFAULT));
  ASSERT_TRUE(d_chunk_a.valid());
  ScopedDataset d_chunk_b(H5Dopen2(f1, "chunked", H5P_DEFAULT));
  ASSERT_TRUE(d_chunk_b.valid());
  {
    ScopedSpace pt(H5Scopy(s1));
    ASSERT_TRUE(pt.valid());
    constexpr std::array<hsize_t, 1> kCoord{0};
    ASSERT_GE(H5Sselect_elements(pt, H5S_SELECT_SET, 1, kCoord.data()), 0);
    ScopedSpace sc = MakeSimpleSpace({1});
    int32_t v = 9;
    ASSERT_GE(H5Dwrite(d_chunk_a, H5T_NATIVE_INT32, sc, pt, H5P_DEFAULT, &v),
              0);
    std::array<int32_t, 8> all_v{};
    ASSERT_GE(H5Dwrite(d_chunk_a, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, all_v.data()),
              0);
  }
  EXPECT_GE(H5Drefresh(d_chunk_a), 0);

  // Compact multi-open refresh.
  ScopedPlist comp_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(comp_dcpl.valid());
  ASSERT_GE(H5Pset_layout(comp_dcpl, H5D_COMPACT), 0);
  ScopedDataset d_comp_a(H5Dcreate2(f1, "compact", H5T_NATIVE_INT32, s2,
                                    H5P_DEFAULT, comp_dcpl, H5P_DEFAULT));
  ASSERT_TRUE(d_comp_a.valid());
  ScopedDataset d_comp_b(H5Dopen2(f1, "compact", H5P_DEFAULT));
  ASSERT_TRUE(d_comp_b.valid());
  EXPECT_GE(H5Drefresh(d_comp_a), 0);

  // VDS multi-open refresh (covers H5D__refresh virtual branch too).
  ScopedPlist vds_dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(vds_dcpl.valid());
  ASSERT_GE(H5Pset_virtual(vds_dcpl, s1, ".", "contig", s1), 0);
  ScopedDataset d_vds_a(H5Dcreate2(f1, "vds", H5T_NATIVE_INT32, s1, H5P_DEFAULT,
                                   vds_dcpl, H5P_DEFAULT));
  ASSERT_TRUE(d_vds_a.valid());
  ScopedDataset d_vds_b(H5Dopen2(f1, "vds", H5P_DEFAULT));
  ASSERT_TRUE(d_vds_b.valid());
  EXPECT_GE(H5Drefresh(d_vds_a), 0);

  // Open dataset in second file f2 so H5Fflush(f1) tests both f == oloc.file
  // and f != oloc.file in H5D__flush_all_cb.
  ScopedDataset d_f2(H5Dcreate2(f2, "other", H5T_NATIVE_INT32, s2, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(d_f2.valid());

  // Re-dirty contig sieve buffer and exercise H5Dflush + H5Fflush.
  {
    ScopedSpace sel(H5Scopy(s1));
    ASSERT_TRUE(sel.valid());
    constexpr std::array<hsize_t, 1> kOff{2};
    constexpr std::array<hsize_t, 1> kCnt{2};
    ASSERT_GE(H5Sselect_hyperslab(sel, H5S_SELECT_SET, kOff.data(), nullptr,
                                  kCnt.data(), nullptr),
              0);
    constexpr std::array<int32_t, 2> kW{7, 8};
    ASSERT_GE(H5Dwrite(d_contig_a, H5T_NATIVE_INT32, s2, sel, H5P_DEFAULT,
                       kW.data()),
              0);
    EXPECT_GE(H5Dflush(d_contig_a), 0);

    ASSERT_GE(H5Dwrite(d_contig_a, H5T_NATIVE_INT32, s2, sel, H5P_DEFAULT,
                       kW.data()),
              0);
    EXPECT_GE(H5Fflush(f1, H5F_SCOPE_LOCAL), 0);
  }
}

TEST(H5DintTest, OlocNameofMarkAndDcplIdHelpers) {
  ScopedFile file = CreateInMemoryFile("helpers.h5");
  ASSERT_TRUE(file.valid());
  ScopedSpace space = MakeSimpleSpace({4});
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file, "d", H5T_NATIVE_INT32, space, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  H5D_t* d = UnwrapDataset(dset);
  ASSERT_NE(d, nullptr);

  EXPECT_EQ(H5D_oloc(nullptr), nullptr);
  EXPECT_EQ(H5D_nameof(nullptr), nullptr);
  EXPECT_EQ(H5D_oloc(d), &d->oloc);
  EXPECT_EQ(H5D_nameof(d), &d->path);

  H5D_obj_create_t crt{H5T_NATIVE_INT32, nullptr, H5P_DATASET_CREATE_DEFAULT,
                           H5P_DATASET_ACCESS_DEFAULT};
  EXPECT_EQ(H5D_get_dcpl_id(&crt), H5P_DATASET_CREATE_DEFAULT);

  ScopedApiContext ctx;
  EXPECT_GE(H5D__mark(d, 0), 0);
  EXPECT_GE(H5D__mark(d, H5D_MARK_LAYOUT | H5D_MARK_SPACE), 0);
}

// ===========================================================================
// 5. Storage Allocation, Space Status, Storage Size, Offset & Set Extent
// ===========================================================================

TEST(H5DintTest, SpaceStatusStorageSizeOffsetAndAllocStorageVariants) {
  TempDir dir("storage");

  // Userblock file for H5Dget_offset testing on allocated contiguous dataset.
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_userblock(fcpl, 512), 0);
  ScopedFile file(H5Fcreate("storage.h5", H5F_ACC_TRUNC, fcpl, MakeCoreFapl()));
  ASSERT_TRUE(file.valid());

  ScopedSpace s_nonempty = MakeSimpleSpace({4});
  ASSERT_TRUE(s_nonempty.valid());
  ScopedSpace s_zero = MakeSimpleSpace({0});
  ASSERT_TRUE(s_zero.valid());

  // 1. Contiguous late before write (size 0), then written (size 16, valid off).
  {
    ScopedDataset d(H5Dcreate2(file, "contig_late", H5T_NATIVE_INT32, s_nonempty,
                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d.valid());
    H5D_space_status_t st = H5D_SPACE_STATUS_ALLOCATED;
    ASSERT_GE(H5Dget_space_status(d, &st), 0);
    EXPECT_EQ(st, H5D_SPACE_STATUS_NOT_ALLOCATED);
    EXPECT_EQ(H5Dget_storage_size(d), 0);

    constexpr std::array<int32_t, 4> kVals{1, 2, 3, 4};
    ASSERT_GE(H5Dwrite(d, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kVals.data()),
              0);
    ASSERT_GE(H5Dget_space_status(d, &st), 0);
    EXPECT_EQ(st, H5D_SPACE_STATUS_ALLOCATED);
    EXPECT_EQ(H5Dget_storage_size(d), 16);
    haddr_t off = H5Dget_offset(d);
    EXPECT_NE(off, HADDR_UNDEF);
    EXPECT_GE(off, 512);
  }

  // 2. Contiguous 0-point dataset & unallocated offset without userblock.
  {
    ScopedDataset d0(H5Dcreate2(file, "contig_zero", H5T_NATIVE_INT32, s_zero,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d0.valid());
    EXPECT_EQ(H5Dget_storage_size(d0), 0);

    ScopedFile plain = CreateInMemoryFile("plain_off.h5");
    ASSERT_TRUE(plain.valid());
    ScopedDataset d_unalloc(H5Dcreate2(plain, "unalloc", H5T_NATIVE_INT32,
                                       s_nonempty, H5P_DEFAULT, H5P_DEFAULT,
                                       H5P_DEFAULT));
    ASSERT_TRUE(d_unalloc.valid());
    EXPECT_EQ(H5Dget_offset(d_unalloc), HADDR_UNDEF);
  }

  // 3. Contiguous early with custom fill value vs H5D_ALLOC_TIME_EARLY chunked.
  {
    ScopedPlist dcpl_early(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_early.valid());
    constexpr int32_t kFill = 42;
    ASSERT_GE(H5Pset_alloc_time(dcpl_early, H5D_ALLOC_TIME_EARLY), 0);
    ASSERT_GE(H5Pset_fill_value(dcpl_early, H5T_NATIVE_INT32, &kFill), 0);
    ScopedDataset d_ce(H5Dcreate2(file, "contig_early", H5T_NATIVE_INT32,
                                  s_nonempty, H5P_DEFAULT, dcpl_early,
                                  H5P_DEFAULT));
    ASSERT_TRUE(d_ce.valid());
    EXPECT_EQ(H5Dget_storage_size(d_ce), 16);

    constexpr std::array<hsize_t, 1> kChunk2{2};
    ASSERT_GE(H5Pset_chunk(dcpl_early, 1, kChunk2.data()), 0);
    ScopedDataset d_ke(H5Dcreate2(file, "chunk_early", H5T_NATIVE_INT32,
                                  s_nonempty, H5P_DEFAULT, dcpl_early,
                                  H5P_DEFAULT));
    ASSERT_TRUE(d_ke.valid());
    H5D_space_status_t st = H5D_SPACE_STATUS_NOT_ALLOCATED;
    ASSERT_GE(H5Dget_space_status(d_ke, &st), 0);
    EXPECT_EQ(st, H5D_SPACE_STATUS_ALLOCATED);
  }

  // 4. Chunked NOT_ALLOCATED -> PART_ALLOCATED -> ALLOCATED, offset == HADDR_UNDEF.
  {
    ScopedPlist dcpl_chk(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_chk.valid());
    constexpr std::array<hsize_t, 1> kChunk2{2};
    ASSERT_GE(H5Pset_chunk(dcpl_chk, 1, kChunk2.data()), 0);
    ScopedDataset d(H5Dcreate2(file, "chunk_part", H5T_NATIVE_INT32, s_nonempty,
                               H5P_DEFAULT, dcpl_chk, H5P_DEFAULT));
    ASSERT_TRUE(d.valid());
    EXPECT_EQ(H5Dget_offset(d), HADDR_UNDEF);
    EXPECT_EQ(H5Dget_storage_size(d), 0);

    H5D_space_status_t st = H5D_SPACE_STATUS_ALLOCATED;
    ASSERT_GE(H5Dget_space_status(d, &st), 0);
    EXPECT_EQ(st, H5D_SPACE_STATUS_NOT_ALLOCATED);

    ScopedSpace ms = MakeSimpleSpace({2});
    ScopedSpace fs(H5Scopy(s_nonempty));
    constexpr std::array<hsize_t, 1> kStart0{0};
    constexpr std::array<hsize_t, 1> kCnt2{2};
    ASSERT_GE(H5Sselect_hyperslab(fs, H5S_SELECT_SET, kStart0.data(), nullptr,
                                  kCnt2.data(), nullptr),
              0);
    constexpr std::array<int32_t, 2> kV{1, 2};
    ASSERT_GE(H5Dwrite(d, H5T_NATIVE_INT32, ms, fs, H5P_DEFAULT, kV.data()), 0);
    ASSERT_GE(H5Dget_space_status(d, &st), 0);
    EXPECT_EQ(st, H5D_SPACE_STATUS_PART_ALLOCATED);

    constexpr std::array<hsize_t, 1> kStart2{2};
    ASSERT_GE(H5Sselect_hyperslab(fs, H5S_SELECT_SET, kStart2.data(), nullptr,
                                  kCnt2.data(), nullptr),
              0);
    ASSERT_GE(H5Dwrite(d, H5T_NATIVE_INT32, ms, fs, H5P_DEFAULT, kV.data()), 0);
    ASSERT_GE(H5Dget_space_status(d, &st), 0);
    EXPECT_EQ(st, H5D_SPACE_STATUS_ALLOCATED);
  }

  // 5. Compact 0-point and non-empty + VDS + EFL.
  {
    ScopedPlist dcpl_comp(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_comp.valid());
    ASSERT_GE(H5Pset_layout(dcpl_comp, H5D_COMPACT), 0);
    ScopedSpace s_fixed0 = MakeSimpleSpace({0});
    ScopedDataset d_comp0(H5Dcreate2(file, "comp_zero", H5T_NATIVE_INT32,
                                     s_fixed0, H5P_DEFAULT, dcpl_comp,
                                     H5P_DEFAULT));
    ASSERT_TRUE(d_comp0.valid());
    EXPECT_EQ(H5Dget_storage_size(d_comp0), 0);
    EXPECT_EQ(H5Dget_offset(d_comp0), HADDR_UNDEF);

    ScopedPlist dcpl_vds(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_vds.valid());
    ASSERT_GE(
        H5Pset_virtual(dcpl_vds, s_nonempty, ".", "contig_late", s_nonempty),
        0);
    ScopedDataset d_vds(H5Dcreate2(file, "vds_st", H5T_NATIVE_INT32, s_nonempty,
                                   H5P_DEFAULT, dcpl_vds, H5P_DEFAULT));
    ASSERT_TRUE(d_vds.valid());
    H5D_space_status_t st = H5D_SPACE_STATUS_NOT_ALLOCATED;
    ASSERT_GE(H5Dget_space_status(d_vds, &st), 0);
    EXPECT_EQ(st, H5D_SPACE_STATUS_ALLOCATED);
    EXPECT_EQ(H5Dget_storage_size(d_vds), 0);
    EXPECT_EQ(H5Dget_offset(d_vds), HADDR_UNDEF);

    const std::filesystem::path efl_raw = dir.child("efl_storage.bin");
    ScopedPlist dcpl_efl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_efl.valid());
    ASSERT_GE(H5Pset_external(dcpl_efl, efl_raw.c_str(), 0, 16), 0);
    ScopedDataset d_efl(H5Dcreate2(file, "efl_st", H5T_NATIVE_INT32, s_nonempty,
                                   H5P_DEFAULT, dcpl_efl, H5P_DEFAULT));
    ASSERT_TRUE(d_efl.valid());
    EXPECT_EQ(H5Dget_offset(d_efl), HADDR_UNDEF);
  }
}

TEST(H5DintTest, SetExtentChunkedCheckFiltersEflVdsAndErrors) {
  TempDir dir("set_extent");
  ScopedCustomFilter custom_filter;
  ASSERT_TRUE(custom_filter.ok());

  const std::filesystem::path h5_path = dir.child("extent.h5");
  {
    ScopedFile file(
        H5Fcreate(h5_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    // 1. 2D chunked dataset with deflate + DONT_FILTER_PARTIAL_CHUNKS.
    ScopedSpace s2d = MakeSimpleSpace({3, 3}, {H5S_UNLIMITED, H5S_UNLIMITED});
    ASSERT_TRUE(s2d.valid());
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    constexpr std::array<hsize_t, 2> kChunk{2, 2};
    ASSERT_GE(H5Pset_chunk(dcpl, 2, kChunk.data()), 0);
    ASSERT_GE(H5Pset_deflate(dcpl, 1), 0);
    ASSERT_GE(H5Pset_chunk_opts(dcpl, H5D_CHUNK_DONT_FILTER_PARTIAL_CHUNKS), 0);
    constexpr int32_t kFill = 5;
    ASSERT_GE(H5Pset_fill_value(dcpl, H5T_NATIVE_INT32, &kFill), 0);

    ScopedDataset d2d(H5Dcreate2(file, "chunk_2d", H5T_NATIVE_INT32, s2d,
                                 H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(d2d.valid());
    std::array<int32_t, 9> vals{};
    vals.fill(1);
    ASSERT_GE(H5Dwrite(d2d, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       vals.data()),
              0);

    // 2. Extendible dataset with custom filter for reopening H5D__check_filters.
    ScopedSpace s1d = MakeSimpleSpace({4}, {H5S_UNLIMITED});
    ASSERT_TRUE(s1d.valid());
    ScopedPlist dcpl_cust(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_cust.valid());
    constexpr std::array<hsize_t, 1> kChunk4{4};
    ASSERT_GE(H5Pset_chunk(dcpl_cust, 1, kChunk4.data()), 0);
    ASSERT_GE(
        H5Pset_filter(dcpl_cust, kCustomFilterId, H5Z_FLAG_MANDATORY, 0,
                      nullptr),
        0);
    ASSERT_GE(H5Pset_fill_time(dcpl_cust, H5D_FILL_TIME_ALLOC), 0);
    ScopedDataset d_cust(H5Dcreate2(file, "cust_ext", H5T_NATIVE_INT32, s1d,
                                    H5P_DEFAULT, dcpl_cust, H5P_DEFAULT));
    ASSERT_TRUE(d_cust.valid());

    // 3. Extendible early-allocated chunked dataset -> hits H5D_ALLOC_EXTEND.
    ScopedPlist dcpl_early(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_early.valid());
    ASSERT_GE(H5Pset_chunk(dcpl_early, 1, kChunk4.data()), 0);
    ASSERT_GE(H5Pset_alloc_time(dcpl_early, H5D_ALLOC_TIME_EARLY), 0);
    ScopedDataset d_early(H5Dcreate2(file, "chunk_early", H5T_NATIVE_INT32, s1d,
                                     H5P_DEFAULT, dcpl_early, H5P_DEFAULT));
    ASSERT_TRUE(d_early.valid());
    constexpr std::array<hsize_t, 1> kExtEarly{8};
    EXPECT_GE(H5Dset_extent(d_early, kExtEarly.data()), 0);

    // 4. Extendible EFL dataset.
    const std::filesystem::path ext_raw = dir.child("ext_grow.bin");
    ScopedPlist dcpl_efl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_efl.valid());
    ASSERT_GE(H5Pset_external(dcpl_efl, ext_raw.c_str(), 0, H5F_UNLIMITED), 0);
    ScopedDataset d_efl(H5Dcreate2(file, "efl_ext", H5T_NATIVE_INT32, s1d,
                                   H5P_DEFAULT, dcpl_efl, H5P_DEFAULT));
    ASSERT_TRUE(d_efl.valid());
    constexpr std::array<hsize_t, 1> kGrowEfl{12};
    EXPECT_GE(H5Dset_extent(d_efl, kGrowEfl.data()), 0);

    // 5. Compact and non-EFL contiguous fail set_extent.
    ScopedPlist dcpl_comp(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_comp.valid());
    ASSERT_GE(H5Pset_layout(dcpl_comp, H5D_COMPACT), 0);
    ScopedSpace s_fix = MakeSimpleSpace({4});
    ScopedDataset d_comp(H5Dcreate2(file, "comp", H5T_NATIVE_INT32, s_fix,
                                    H5P_DEFAULT, dcpl_comp, H5P_DEFAULT));
    ASSERT_TRUE(d_comp.valid());
    ScopedDataset d_contig(H5Dcreate2(file, "contig", H5T_NATIVE_INT32, s_fix,
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_contig.valid());
    {
      ScopedErrorSilence silence;
      constexpr std::array<hsize_t, 1> kSize4{4};
      EXPECT_LT(H5Dset_extent(d_comp, kSize4.data()), 0);
      EXPECT_LT(H5Dset_extent(d_contig, kSize4.data()), 0);
    }

    // 6. VDS set_extent valid and min_dims failure.
    ScopedSpace src_fixed = MakeSimpleSpace({2});
    ScopedDataset vsrc(H5Dcreate2(file, "vsrc_0", H5T_NATIVE_INT32, src_fixed,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(vsrc.valid());
    vsrc.reset();

    ScopedSpace vspace = MakeSimpleSpace({4, 2}, {H5S_UNLIMITED, 2});
    constexpr std::array<hsize_t, 2> kVStart{0, 0};
    constexpr std::array<hsize_t, 2> kVStride{2, 1};
    constexpr std::array<hsize_t, 2> kVCount{H5S_UNLIMITED, 1};
    constexpr std::array<hsize_t, 2> kVBlock{1, 2};
    ASSERT_GE(H5Sselect_hyperslab(vspace, H5S_SELECT_SET, kVStart.data(),
                                  kVStride.data(), kVCount.data(),
                                  kVBlock.data()),
              0);
    ScopedPlist dcpl_vds(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_virtual(dcpl_vds, vspace, ".", "vsrc_%b", src_fixed), 0);

    ScopedSpace vspace_bounded = MakeSimpleSpace({4, 2}, {H5S_UNLIMITED, 2});
    constexpr std::array<hsize_t, 2> kVBStart{2, 0};
    constexpr std::array<hsize_t, 2> kVBCount{1, 2};
    ASSERT_GE(H5Sselect_hyperslab(vspace_bounded, H5S_SELECT_SET,
                                  kVBStart.data(), nullptr, kVBCount.data(),
                                  nullptr),
              0);
    ASSERT_GE(
        H5Pset_virtual(dcpl_vds, vspace_bounded, ".", "vsrc_0", src_fixed), 0);

    ScopedDataset d_vds(H5Dcreate2(file, "vds_ext", H5T_NATIVE_INT32, vspace,
                                   H5P_DEFAULT, dcpl_vds, H5P_DEFAULT));
    ASSERT_TRUE(d_vds.valid());
    std::array<int32_t, 8> vbuf{};
    ASSERT_GE(H5Dread(d_vds, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      vbuf.data()),
              0);

    constexpr std::array<hsize_t, 2> kVdsGrow{6, 2};
    EXPECT_GE(H5Dset_extent(d_vds, kVdsGrow.data()), 0);

    {
      ScopedErrorSilence silence;
      // Shrinking dim 0 to 1 violates bounded mapping at [2, 0].
      constexpr std::array<hsize_t, 2> kVdsTooSmall{1, 2};
      EXPECT_LT(H5Dset_extent(d_vds, kVdsTooSmall.data()), 0);
    }
  }

  // Reopen RDWR to test H5D__check_filters + scaled_power2up crossing + prune.
  {
    ScopedFile rwf(H5Fopen(h5_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
    ASSERT_TRUE(rwf.valid());

    // Fail H5D__check_filters first, then allow and succeed.
    ScopedDataset d_cust(H5Dopen2(rwf, "cust_ext", H5P_DEFAULT));
    ASSERT_TRUE(d_cust.valid());
    {
      ScopedErrorSilence silence;
      g_allow_can_apply = false;
      constexpr std::array<hsize_t, 1> kNew8{8};
      EXPECT_LT(H5Dset_extent(d_cust, kNew8.data()), 0);
      g_allow_can_apply = true;
      EXPECT_GE(H5Dset_extent(d_cust, kNew8.data()), 0);
    }

    // Reopen 2D chunked dataset, expand past chunk/slot boundaries (triggers
    // scaled_power2up & update_old_edge_chunks), then shrink (prune).
    ScopedDataset d2d(H5Dopen2(rwf, "chunk_2d", H5P_DEFAULT));
    ASSERT_TRUE(d2d.valid());
    constexpr std::array<hsize_t, 2> kExpand{20, 20};
    EXPECT_GE(H5Dset_extent(d2d, kExpand.data()), 0);
    // Keep one dim unchanged to hit both changed/unchanged paths in loop.
    constexpr std::array<hsize_t, 2> kSame{20, 20};
    EXPECT_GE(H5Dset_extent(d2d, kSame.data()), 0);

    // Dirty/cache chunks before shrinking so prune runs cleanly.
    std::array<int32_t, 400> vals{};
    vals.fill(3);
    ASSERT_GE(H5Dwrite(d2d, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       vals.data()),
              0);
    constexpr std::array<hsize_t, 2> kShrink{2, 2};
    EXPECT_GE(H5Dset_extent(d2d, kShrink.data()), 0);
  }

  // Reopen RDONLY and attempt set_extent -> H5F_ACC_RDWR error branch.
  {
    ScopedFile rof(H5Fopen(h5_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(rof.valid());
    ScopedDataset d2d(H5Dopen2(rof, "chunk_2d", H5P_DEFAULT));
    ASSERT_TRUE(d2d.valid());
    ScopedErrorSilence silence;
    constexpr std::array<hsize_t, 2> kTry{4, 4};
    EXPECT_LT(H5Dset_extent(d2d, kTry.data()), 0);
  }
}

// ===========================================================================
// 6. Format Convert, Property List Getters, Space & Type Helpers
// ===========================================================================

TEST(H5DintTest, FormatConvertAllIndicesAndLayoutsAndGetters) {
  TempDir dir("fmt_conv");
  const std::filesystem::path h5_path = dir.child("fmt.h5");
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_libver_bounds(fapl, H5F_LIBVER_V110, H5F_LIBVER_LATEST), 0);

  ScopedFile file(
      H5Fcreate(h5_path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(file.valid());

  // 1. SINGLE chunk index (both empty and non-empty).
  {
    ScopedSpace s = MakeSimpleSpace({4});
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    constexpr std::array<hsize_t, 1> kChunk4{4};
    ASSERT_GE(H5Pset_chunk(dcpl, 1, kChunk4.data()), 0);
    ScopedDataset d_empty(H5Dcreate2(file, "single_empty", H5T_NATIVE_INT32, s,
                                     H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(d_empty.valid());
    EXPECT_GE(H5Dformat_convert(d_empty), 0);

    ScopedDataset d_full(H5Dcreate2(file, "single_full", H5T_NATIVE_INT32, s,
                                    H5P_DEFAULT, dcpl, H5P_DEFAULT));
    ASSERT_TRUE(d_full.valid());
    constexpr std::array<int32_t, 4> kData{1, 2, 3, 4};
    ASSERT_GE(H5Dwrite(d_full, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kData.data()),
              0);
    ASSERT_GE(H5Dflush(d_full), 0);
    EXPECT_GE(H5Dformat_convert(d_full), 0);
  }

  // 2. FARRAY (fixed size multi-chunk) + EARRAY (1D unlim) + BT2 (2D unlim).
  {
    ScopedSpace s_fa = MakeSimpleSpace({8});
    ScopedPlist dcpl_fa(H5Pcreate(H5P_DATASET_CREATE));
    constexpr std::array<hsize_t, 1> kChunk2{2};
    ASSERT_GE(H5Pset_chunk(dcpl_fa, 1, kChunk2.data()), 0);
    ScopedDataset d_fa(H5Dcreate2(file, "farray", H5T_NATIVE_INT32, s_fa,
                                  H5P_DEFAULT, dcpl_fa, H5P_DEFAULT));
    ASSERT_TRUE(d_fa.valid());
    constexpr std::array<int32_t, 8> kVals8{1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_GE(H5Dwrite(d_fa, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kVals8.data()),
              0);
    ASSERT_GE(H5Dflush(d_fa), 0);
    EXPECT_GE(H5Dformat_convert(d_fa), 0);

    ScopedSpace s_ea = MakeSimpleSpace({4}, {H5S_UNLIMITED});
    ScopedDataset d_ea(H5Dcreate2(file, "earray", H5T_NATIVE_INT32, s_ea,
                                  H5P_DEFAULT, dcpl_fa, H5P_DEFAULT));
    ASSERT_TRUE(d_ea.valid());
    EXPECT_GE(H5Dformat_convert(d_ea), 0);

    ScopedSpace s_bt2 = MakeSimpleSpace({4, 4}, {H5S_UNLIMITED, H5S_UNLIMITED});
    ScopedPlist dcpl_bt2(H5Pcreate(H5P_DATASET_CREATE));
    constexpr std::array<hsize_t, 2> kChunk22{2, 2};
    ASSERT_GE(H5Pset_chunk(dcpl_bt2, 2, kChunk22.data()), 0);
    ScopedDataset d_bt2(H5Dcreate2(file, "bt2", H5T_NATIVE_INT32, s_bt2,
                                   H5P_DEFAULT, dcpl_bt2, H5P_DEFAULT));
    ASSERT_TRUE(d_bt2.valid());
    std::array<int32_t, 16> kVals16{};
    ASSERT_GE(H5Dwrite(d_bt2, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kVals16.data()),
              0);
    ASSERT_GE(H5Dflush(d_bt2), 0);
    EXPECT_GE(H5Dformat_convert(d_bt2), 0);
  }

  // 3. Reopened chunked dataset -> H5Dget_create_plist exercises
  // H5D_flush_layout_to_dcpl (ndims-- / ndims++), plus EFL / compact / VDS DCPLs.
  {
    ScopedSpace s = MakeSimpleSpace({4});
    const std::filesystem::path efl_path = dir.child("efl_fmt.bin");
    ScopedPlist dcpl_efl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_external(dcpl_efl, efl_path.c_str(), 0, 16), 0);
    ScopedDataset d_efl(H5Dcreate2(file, "efl_d", H5T_NATIVE_INT32, s,
                                   H5P_DEFAULT, dcpl_efl, H5P_DEFAULT));
    ASSERT_TRUE(d_efl.valid());
    ScopedPlist got_efl_dcpl(H5Dget_create_plist(d_efl));
    EXPECT_TRUE(got_efl_dcpl.valid());

    ScopedPlist dcpl_comp(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_layout(dcpl_comp, H5D_COMPACT), 0);
    ScopedDataset d_comp(H5Dcreate2(file, "comp_d", H5T_NATIVE_INT32, s,
                                    H5P_DEFAULT, dcpl_comp, H5P_DEFAULT));
    ASSERT_TRUE(d_comp.valid());
    constexpr std::array<int32_t, 4> kCData{9, 8, 7, 6};
    ASSERT_GE(H5Dwrite(d_comp, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       kCData.data()),
              0);
    ScopedPlist got_comp_dcpl(H5Dget_create_plist(d_comp));
    EXPECT_TRUE(got_comp_dcpl.valid());

    ScopedPlist dcpl_vds(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_virtual(dcpl_vds, s, ".", "efl_d", s), 0);
    ScopedDataset d_vds(H5Dcreate2(file, "vds_d", H5T_NATIVE_INT32, s,
                                   H5P_DEFAULT, dcpl_vds, H5P_DEFAULT));
    ASSERT_TRUE(d_vds.valid());
    ScopedPlist got_vds_dcpl(H5Dget_create_plist(d_vds));
    EXPECT_TRUE(got_vds_dcpl.valid());

    ScopedDataset d_contig(H5Dcreate2(file, "contig_d", H5T_NATIVE_INT32, s,
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_contig.valid());

    // Internal H5D__format_convert coverage for VDS error + CONTIGUOUS/COMPACT
    // v4 -> v3 downgrade.
    {
      ScopedApiContext ctx;
      ScopedErrorSilence silence;
      H5D_t* raw_vds = UnwrapDataset(d_vds);
      ASSERT_NE(raw_vds, nullptr);
      EXPECT_LT(H5D__format_convert(raw_vds), 0);

      H5D_t* raw_comp = UnwrapDataset(d_comp);
      ASSERT_NE(raw_comp, nullptr);
      raw_comp->shared->layout.version = H5O_LAYOUT_VERSION_4;
      EXPECT_GE(H5D__format_convert(raw_comp), 0);
      EXPECT_EQ(raw_comp->shared->layout.version, H5O_LAYOUT_VERSION_DEFAULT);

      H5D_t* raw_contig = UnwrapDataset(d_contig);
      ASSERT_NE(raw_contig, nullptr);
      raw_contig->shared->layout.version = H5O_LAYOUT_VERSION_4;
      EXPECT_GE(H5D__format_convert(raw_contig), 0);
      EXPECT_EQ(raw_contig->shared->layout.version, H5O_LAYOUT_VERSION_DEFAULT);
    }

    // Committed datatype dataset -> H5Dget_type named datatype branch.
    ScopedType named_dt(H5Tcopy(H5T_NATIVE_INT32));
    ASSERT_GE(H5Tcommit2(file, "named_i32", named_dt, H5P_DEFAULT, H5P_DEFAULT,
                         H5P_DEFAULT),
              0);
    ScopedDataset d_named_t(H5Dcreate2(file, "named_t_dset", named_dt, s,
                                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_named_t.valid());
    ScopedType got_named(H5Dget_type(d_named_t));
    ASSERT_TRUE(got_named.valid());
    EXPECT_GT(H5Tcommitted(got_named), 0);
  }

  // Reopen file to verify layout_copied_to_dcpl == false path on chunked dset.
  EXPECT_EQ(H5Fget_obj_count(file, H5F_OBJ_DATASET), 0);
  ASSERT_GE(H5Fclose(file.release()), 0);
  ScopedFile rfile(H5Fopen(h5_path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(rfile.valid());
  ScopedDataset d_reopen(H5Dopen2(rfile, "farray", H5P_DEFAULT));
  ASSERT_TRUE(d_reopen.valid());
  ScopedPlist reopened_dcpl(H5Dget_create_plist(d_reopen));
  ASSERT_TRUE(reopened_dcpl.valid());
  std::array<hsize_t, 1> got_chunk{};
  EXPECT_EQ(H5Pget_chunk(reopened_dcpl, 1, got_chunk.data()), 1);
  EXPECT_EQ(got_chunk[0], 2);

  ScopedType got_anon_type(H5Dget_type(d_reopen));
  EXPECT_TRUE(got_anon_type.valid());
}

// ===========================================================================
// 7. Variable-Length Buffer Sizing (Native & Generic VOL Paths)
// ===========================================================================

TEST(H5DintTest, VlenGetBufSizeNativeAndGeneric) {
  ScopedFile file = CreateInMemoryFile("vlen_bufsz.h5");
  ASSERT_TRUE(file.valid());
  ScopedSpace space = MakeSimpleSpace({4});
  ASSERT_TRUE(space.valid());

  ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vl_int.valid());
  ScopedDataset dset(H5Dcreate2(file, "vl", vl_int, space, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  // Increasing element lengths force H5D__vlen_get_buf_size_alloc to realloc.
  std::array<int32_t, 1> row0{10};
  std::array<int32_t, 3> row1{20, 21, 22};
  std::array<int32_t, 8> row2{30, 31, 32, 33, 34, 35, 36, 37};
  std::array<int32_t, 20> row3{};
  row3.fill(40);
  std::array<hvl_t, 4> wdata{{
      {row0.size(), row0.data()},
      {row1.size(), row1.data()},
      {row2.size(), row2.data()},
      {row3.size(), row3.data()},
  }};
  ASSERT_GE(H5Dwrite(dset, vl_int, H5S_ALL, H5S_ALL, H5P_DEFAULT, wdata.data()),
            0);

  // Full dataspace selection: (1 + 3 + 8 + 20) * sizeof(int32_t) = 128.
  hsize_t sz = 0;
  ASSERT_GE(H5Dvlen_get_buf_size(dset, vl_int, space, &sz), 0);
  EXPECT_EQ(sz, 32 * sizeof(int32_t));

  // Empty selection returns 0 bytes.
  ScopedSpace none_space(H5Scopy(space));
  ASSERT_GE(H5Sselect_none(none_space), 0);
  sz = 999;
  ASSERT_GE(H5Dvlen_get_buf_size(dset, vl_int, none_space, &sz), 0);
  EXPECT_EQ(sz, 0);

  // Native error paths: invalid type, invalid space, extentless space.
  ScopedSpace no_ext(H5Screate(H5S_SIMPLE));
  ASSERT_TRUE(no_ext.valid());
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Dvlen_get_buf_size(dset, H5I_INVALID_HID, space, &sz), 0);
    EXPECT_LT(H5Dvlen_get_buf_size(dset, vl_int, H5I_INVALID_HID, &sz), 0);
    EXPECT_LT(H5Dvlen_get_buf_size(dset, vl_int, no_ext, &sz), 0);
  }

  // Generic path: H5D__vlen_get_buf_size_gen on native VOL object.
  H5VL_object_t* vol_obj = H5VL_vol_object(dset);
  ASSERT_NE(vol_obj, nullptr);
  {
    ScopedApiContext ctx;
    hsize_t gen_sz = 0;
    ASSERT_GE(H5D__vlen_get_buf_size_gen(vol_obj, vl_int, space, &gen_sz), 0);
    EXPECT_EQ(gen_sz, 32 * sizeof(int32_t));

    ScopedErrorSilence silence;
    EXPECT_LT(H5D__vlen_get_buf_size_gen(vol_obj, H5I_INVALID_HID, space,
                                         &gen_sz),
              0);
    EXPECT_LT(H5D__vlen_get_buf_size_gen(vol_obj, vl_int, H5I_INVALID_HID,
                                         &gen_sz),
              0);
    EXPECT_LT(H5D__vlen_get_buf_size_gen(vol_obj, vl_int, no_ext, &gen_sz), 0);
  }
}

// ===========================================================================
// 8. Multi-Open Read-Only Refresh, Separate Top Files, and Uncork on Close
// ===========================================================================

TEST(H5DintTest, ReadOnlyMultiOpenRefreshAndSeparateFileHandles) {
  TempFile temp_h5("h5dint_multi_open_refresh.h5");
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST),
              0);
    ScopedFile wfile(
        H5Fcreate(temp_h5.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(wfile.valid());

    // 1. Contiguous dataset with 16 elements.
    ScopedSpace s16 = MakeSimpleSpace({16});
    ASSERT_TRUE(s16.valid());
    ScopedDataset d_contig(H5Dcreate2(wfile, "contig", H5T_NATIVE_INT32, s16,
                                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_contig.valid());
    std::array<int32_t, 16> data16{};
    for (int i = 0; i < 16; ++i) {
      data16[i] = 100 + i;
    }
    ASSERT_GE(H5Dwrite(d_contig, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, data16.data()),
              0);

    // 2. Chunked dataset (chunk size 4).
    ScopedPlist dcpl_chk(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_chk.valid());
    hsize_t chunk_dim = 4;
    ASSERT_GE(H5Pset_chunk(dcpl_chk, 1, &chunk_dim), 0);
    ScopedDataset d_chunk(H5Dcreate2(wfile, "chunked", H5T_NATIVE_INT32, s16,
                                     H5P_DEFAULT, dcpl_chk, H5P_DEFAULT));
    ASSERT_TRUE(d_chunk.valid());
    ASSERT_GE(H5Dwrite(d_chunk, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       data16.data()),
              0);

    // 3. Compact dataset.
    ScopedSpace s4 = MakeSimpleSpace({4});
    ASSERT_TRUE(s4.valid());
    ScopedPlist dcpl_cmp(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_cmp.valid());
    ASSERT_GE(H5Pset_layout(dcpl_cmp, H5D_COMPACT), 0);
    ScopedDataset d_cmp(H5Dcreate2(wfile, "compact", H5T_NATIVE_INT32, s4,
                                   H5P_DEFAULT, dcpl_cmp, H5P_DEFAULT));
    ASSERT_TRUE(d_cmp.valid());
    std::array<int32_t, 4> data4{1, 2, 3, 4};
    ASSERT_GE(H5Dwrite(d_cmp, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       data4.data()),
              0);

    // 4. Virtual dataset backed by contig.
    ScopedPlist dcpl_vds(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_vds.valid());
    ScopedSpace vsrc = MakeSimpleSpace({4});
    ASSERT_TRUE(vsrc.valid());
    ASSERT_GE(H5Pset_virtual(dcpl_vds, s4, ".", "compact", vsrc), 0);
    ScopedDataset d_vds(H5Dcreate2(wfile, "vds", H5T_NATIVE_INT32, s4,
                                   H5P_DEFAULT, dcpl_vds, H5P_DEFAULT));
    ASSERT_TRUE(d_vds.valid());

    // Corked dataset uncorks automatically on close.
    ScopedDataset d_cork(H5Dcreate2(wfile, "corked", H5T_NATIVE_INT32, s4,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_cork.valid());
    ASSERT_GE(H5Odisable_mdc_flushes(d_cork), 0);
  }

  // Open two independent top-level RDONLY file handles to the same OS file so
  // shared_fo is reused across top files (H5FO_top_count == 0 on rf2).
  ScopedFile rf1(H5Fopen(temp_h5.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ScopedFile rf2(H5Fopen(temp_h5.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
  ASSERT_TRUE(rf1.valid());
  ASSERT_TRUE(rf2.valid());

  {
    ScopedDataset d1(H5Dopen2(rf1, "contig", H5P_DEFAULT));
    ASSERT_TRUE(d1.valid());
    ScopedDataset d2(H5Dopen2(rf2, "contig", H5P_DEFAULT));
    ASSERT_TRUE(d2.valid());

    // Partial read populates cache.sieve.sieve_buf before H5Drefresh.
    ScopedSpace fspace(H5Dget_space(d1));
    ScopedSpace mspace = MakeSimpleSpace({2});
    ASSERT_TRUE(fspace.valid());
    ASSERT_TRUE(mspace.valid());
    hsize_t start = 1;
    hsize_t count = 2;
    ASSERT_GE(
        H5Sselect_hyperslab(fspace, H5S_SELECT_SET, &start, nullptr, &count,
                            nullptr),
        0);
    std::array<int32_t, 2> part{};
    ASSERT_GE(
        H5Dread(d1, H5T_NATIVE_INT32, mspace, fspace, H5P_DEFAULT, part.data()),
        0);
    EXPECT_EQ(part[0], 101);

    // fo_count > 1 on RDONLY triggers H5D_mult_refresh_close +
    // H5D_mult_refresh_reopen (including freeing cached sieve_buf).
    EXPECT_GE(H5Drefresh(d1), 0);

    // Temporarily force H5D_NLAYOUTS and call H5D_mult_refresh_close directly
    // (calling H5Drefresh here would fail inside H5O__refresh_metadata_close
    // after deep-copying obj_loc without freeing it on error).
    H5D_t* raw1 = UnwrapDataset(d1);
    ASSERT_NE(raw1, nullptr);
    const H5D_layout_t saved_type = raw1->shared->layout.type;
    absl::Cleanup restore_type = [&] {
      raw1->shared->layout.type = saved_type;
    };
    raw1->shared->layout.type = H5D_NLAYOUTS;
    {
      ScopedErrorSilence silence;
      ScopedApiContext ctx;
      EXPECT_LT(H5D_mult_refresh_close(d1), 0);
    }
    // d2 closes first from rf2 (top_count == 0 -> H5O_close).
  }

  // Multi-open refresh for chunked (covering sel_chunks, single_space,
  // single_piece_info cleanup), compact, and virtual layouts.
  ScopedDataset c1(H5Dopen2(rf1, "chunked", H5P_DEFAULT));
  ScopedDataset c2(H5Dopen2(rf1, "chunked", H5P_DEFAULT));
  ASSERT_TRUE(c1.valid());
  ASSERT_TRUE(c2.valid());
  {
    // 1) Single-element point read caches single_space + single_piece_info.
    ScopedSpace fpt(H5Dget_space(c1));
    ScopedSpace mpt = MakeSimpleSpace({1});
    hsize_t coord = 2;
    ASSERT_GE(H5Sselect_elements(fpt, H5S_SELECT_SET, 1, &coord), 0);
    int32_t one = 0;
    ASSERT_GE(H5Dread(c1, H5T_NATIVE_INT32, mpt, fpt, H5P_DEFAULT, &one), 0);
    EXPECT_EQ(one, 102);

    // 2) Full read caches sel_chunks skip list.
    std::array<int32_t, 16> all{};
    ASSERT_GE(H5Dread(c1, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                      all.data()),
              0);
    EXPECT_EQ(all[15], 115);

    EXPECT_GE(H5Drefresh(c1), 0);
  }

  ScopedDataset m1(H5Dopen2(rf1, "compact", H5P_DEFAULT));
  ScopedDataset m2(H5Dopen2(rf1, "compact", H5P_DEFAULT));
  ASSERT_TRUE(m1.valid());
  ASSERT_TRUE(m2.valid());
  EXPECT_GE(H5Drefresh(m1), 0);

  ScopedDataset v1(H5Dopen2(rf1, "vds", H5P_DEFAULT));
  ScopedDataset v2(H5Dopen2(rf1, "vds", H5P_DEFAULT));
  ASSERT_TRUE(v1.valid());
  ASSERT_TRUE(v2.valid());
  EXPECT_GE(H5Drefresh(v1), 0);

  {
    ScopedErrorSilence silence;
    ScopedApiContext ctx;
    EXPECT_LT(H5D_mult_refresh_close(H5I_INVALID_HID), 0);
  }
}

// ===========================================================================
// 9. Compact Fill Allocation, Ref Fill Reopen, Version Bounds & Invalid Layouts
// ===========================================================================

herr_t FailingFlushCb(hid_t /*obj_id*/, void* /*udata*/) { return -1; }

TEST(H5DintTest, CompactFillRefFillVersionBoundsAndInvalidLayouts) {
  ScopedFile file = CreateInMemoryFile("compact_ref_bounds.h5");
  ASSERT_TRUE(file.valid());
  ScopedSpace space4 = MakeSimpleSpace({4});
  ASSERT_TRUE(space4.valid());

  // 1. Compact dataset with H5D_FILL_TIME_ALLOC and explicit fill value hits
  // H5D__alloc_storage -> H5D__init_storage -> H5D__compact_fill.
  ScopedPlist dcpl_cmp(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_cmp.valid());
  int32_t cmp_fill = 77;
  ASSERT_GE(H5Pset_layout(dcpl_cmp, H5D_COMPACT), 0);
  ASSERT_GE(H5Pset_fill_value(dcpl_cmp, H5T_NATIVE_INT32, &cmp_fill), 0);
  ASSERT_GE(H5Pset_fill_time(dcpl_cmp, H5D_FILL_TIME_ALLOC), 0);
  ScopedDataset d_cmp(H5Dcreate2(file, "cmp_fill", H5T_NATIVE_INT32, space4,
                                 H5P_DEFAULT, dcpl_cmp, H5P_DEFAULT));
  ASSERT_TRUE(d_cmp.valid());
  std::array<int32_t, 4> cmp_read{};
  ASSERT_GE(H5Dread(d_cmp, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    cmp_read.data()),
            0);
  EXPECT_THAT(cmp_read, ElementsAre(77, 77, 77, 77));

  // 2. Incompatible fill value type fails H5O_fill_convert in
  // H5D__update_oh_info.
  {
    ScopedErrorSilence silence;
    ScopedType opaque_t(H5Tcreate(H5T_OPAQUE, 4));
    ASSERT_TRUE(opaque_t.valid());
    EXPECT_FALSE(ScopedDataset(H5Dcreate2(file, "bad_fill_conv", opaque_t,
                                          space4, H5P_DEFAULT, dcpl_cmp,
                                          H5P_DEFAULT))
                     .valid());
  }

  // 3. Object-reference fill value + reopen exercises !H5T_path_noop() and
  // H5T_REFERENCE copy branch in H5D_get_create_plist.
  TempFile ref_h5("h5dint_ref_fill.h5");
  {
    ScopedFile wf(
        H5Fcreate(ref_h5.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(wf.valid());
    ScopedDataset target(H5Dcreate2(wf, "target", H5T_NATIVE_INT32, space4,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(target.valid());
    hobj_ref_t fill_ref = 0;
    ASSERT_GE(H5Rcreate(&fill_ref, wf, "/target", H5R_OBJECT, -1), 0);
    ScopedPlist dcpl_ref(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_ref.valid());
    ASSERT_GE(H5Pset_fill_value(dcpl_ref, H5T_STD_REF_OBJ, &fill_ref), 0);
    ScopedDataset ref_ds(H5Dcreate2(wf, "ref_ds", H5T_STD_REF_OBJ, space4,
                                    H5P_DEFAULT, dcpl_ref, H5P_DEFAULT));
    ASSERT_TRUE(ref_ds.valid());
  }
  {
    ScopedFile rf(H5Fopen(ref_h5.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(rf.valid());
    ScopedDataset reopened_ref(H5Dopen2(rf, "ref_ds", H5P_DEFAULT));
    ASSERT_TRUE(reopened_ref.valid());
    ScopedPlist got_dcpl(H5Dget_create_plist(reopened_ref));
    EXPECT_TRUE(got_dcpl.valid());
  }

  // 4. Shrinking an unwritten chunked dataset hits the
  // !is_space_alloc && !is_data_cached skip branch in H5D__set_extent.
  {
    ScopedSpace chunk_space = MakeSimpleSpace({8}, {16});
    ScopedPlist dcpl_chk(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(chunk_space.valid());
    ASSERT_TRUE(dcpl_chk.valid());
    hsize_t cdim = 4;
    ASSERT_GE(H5Pset_chunk(dcpl_chk, 1, &cdim), 0);
    ScopedDataset unwr_chk(H5Dcreate2(file, "unwr_chk", H5T_NATIVE_INT32,
                                      chunk_space, H5P_DEFAULT, dcpl_chk,
                                      H5P_DEFAULT));
    ASSERT_TRUE(unwr_chk.valid());
    hsize_t shrink_dim = 4;
    EXPECT_GE(H5Dset_extent(unwr_chk, &shrink_dim), 0);
  }

  // 5. Temporarily zeroing chunk.dim[0] on a 2D chunked dataset triggers the
  // chunk size == 0 validation error in H5D__set_extent.
  {
    ScopedSpace s2d = MakeSimpleSpace({4, 4}, {8, 8});
    ScopedPlist dcpl_2d(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(s2d.valid());
    ASSERT_TRUE(dcpl_2d.valid());
    std::array<hsize_t, 2> cdims{2, 2};
    ASSERT_GE(H5Pset_chunk(dcpl_2d, 2, cdims.data()), 0);
    ScopedDataset d2d(H5Dcreate2(file, "chk2d", H5T_NATIVE_INT32, s2d,
                                 H5P_DEFAULT, dcpl_2d, H5P_DEFAULT));
    ASSERT_TRUE(d2d.valid());
    H5D_t* raw2d = UnwrapDataset(d2d);
    ASSERT_NE(raw2d, nullptr);
    const hsize_t saved_dim0 = raw2d->shared->layout.u.chunk.dim[0];
    absl::Cleanup restore_dim0 = [&] {
      raw2d->shared->layout.u.chunk.dim[0] = saved_dim0;
    };
    raw2d->shared->layout.u.chunk.dim[0] = 0;
    std::array<hsize_t, 2> new_dims{6, 6};
    ScopedErrorSilence silence;
    ScopedApiContext ctx;
    EXPECT_LT(H5D__set_extent(raw2d, new_dims.data()), 0);
  }

  // 6. Object flush callback returning -1 fails H5Dflush at H5O_flush_common.
  {
    ScopedPlist fapl_cb = MakeCoreFapl();
    ASSERT_TRUE(fapl_cb.valid());
    ASSERT_GE(H5Pset_object_flush_cb(fapl_cb, FailingFlushCb, nullptr), 0);
    ScopedFile f_cb(
        H5Fcreate("flush_cb_err.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_cb));
    ASSERT_TRUE(f_cb.valid());
    ScopedDataset d_cb(H5Dcreate2(f_cb, "d", H5T_NATIVE_INT32, space4,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_cb.valid());
    ScopedErrorSilence silence;
    EXPECT_LT(H5Dflush(d_cb), 0);
  }

  // 7. Version bounds errors in H5D__init_type and H5D__create.
  {
    ScopedPlist fapl_early =
        MakeCoreFaplLibver(H5F_LIBVER_V18, H5F_LIBVER_V18);
    ASSERT_TRUE(fapl_early.valid());
    ScopedFile f_early(
        H5Fcreate("ver_bounds.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_early));
    ASSERT_TRUE(f_early.valid());
    H5F_t* raw_file = UnwrapFile(f_early);
    H5S_t* raw_space = static_cast<H5S_t*>(H5I_object(space4));
    ASSERT_NE(raw_file, nullptr);
    ASSERT_NE(raw_space, nullptr);

    ScopedErrorSilence silence;
    // H5T_STD_REF requires >= v1.12, failing H5T_set_version + H5D__create
    // cleanup when type_id == 0 (H5T_close_real).
    EXPECT_FALSE(ScopedDataset(H5Dcreate2(f_early, "ref_early", H5T_STD_REF,
                                          space4, H5P_DEFAULT, H5P_DEFAULT,
                                          H5P_DEFAULT))
                     .valid());

    // Poke out-of-range pline version and fill version into copied DCPLs.
    ScopedPlist dcpl_bad_pline(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_bad_pline.valid());
    H5P_genplist_t* pl_p =
        static_cast<H5P_genplist_t*>(H5I_object(dcpl_bad_pline));
    ASSERT_NE(pl_p, nullptr);
    H5O_pline_t bad_pline{};
    ASSERT_GE(H5P_peek(pl_p, H5O_CRT_PIPELINE_NAME, &bad_pline), 0);
    bad_pline.version = 99;
    ASSERT_GE(H5P_poke(pl_p, H5O_CRT_PIPELINE_NAME, &bad_pline), 0);
    {
      ScopedApiContext ctx(dcpl_bad_pline);
      EXPECT_EQ(H5D__create(raw_file, H5T_NATIVE_INT32, raw_space,
                            dcpl_bad_pline, H5P_DATASET_ACCESS_DEFAULT),
                nullptr);
    }

    ScopedPlist dcpl_bad_fill(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl_bad_fill.valid());
    H5P_genplist_t* fill_p =
        static_cast<H5P_genplist_t*>(H5I_object(dcpl_bad_fill));
    ASSERT_NE(fill_p, nullptr);
    H5O_fill_t bad_fill{};
    ASSERT_GE(H5P_peek(fill_p, H5D_CRT_FILL_VALUE_NAME, &bad_fill), 0);
    bad_fill.version = 99;
    ASSERT_GE(H5P_poke(fill_p, H5D_CRT_FILL_VALUE_NAME, &bad_fill), 0);
    {
      ScopedApiContext ctx(dcpl_bad_fill);
      EXPECT_EQ(H5D__create(raw_file, H5T_NATIVE_INT32, raw_space,
                            dcpl_bad_fill, H5P_DATASET_ACCESS_DEFAULT),
                nullptr);
    }
  }

  // 8. Temporarily corrupt layout.type = H5D_NLAYOUTS on d_cmp (NEVER calling
  // H5D_close with H5D_NLAYOUTS) to test switch default/unsupported guards.
  {
    H5D_t* raw_cmp = UnwrapDataset(d_cmp);
    ASSERT_NE(raw_cmp, nullptr);
    const H5D_layout_t saved_layout = raw_cmp->shared->layout.type;
    absl::Cleanup restore_layout = [&] {
      raw_cmp->shared->layout.type = saved_layout;
    };
    raw_cmp->shared->layout.type = H5D_NLAYOUTS;
    hsize_t st_size = 0;
    ScopedErrorSilence silence;
    ScopedApiContext ctx;
    EXPECT_LT(
        H5D__alloc_storage(raw_cmp, H5D_ALLOC_WRITE, false, nullptr), 0);
    EXPECT_LT(H5D__get_storage_size(raw_cmp, &st_size), 0);
    EXPECT_EQ(H5D__get_offset(raw_cmp), HADDR_UNDEF);
    EXPECT_LT(H5D__format_convert(raw_cmp), 0);
  }

  // 9. H5D_flush_layout_to_dcpl error/default paths.
  {
    H5D_t* raw_cmp = UnwrapDataset(d_cmp);
    ASSERT_NE(raw_cmp, nullptr);
    const hid_t orig_dcpl = raw_cmp->shared->dcpl_id;
    const bool orig_copied = raw_cmp->shared->layout_copied_to_dcpl;
    ScopedPlist no_layout_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(no_layout_dcpl.valid());
    H5P_genplist_t* nl_ptr =
        static_cast<H5P_genplist_t*>(H5I_object(no_layout_dcpl));
    // Verify H5Premove strips H5D_CRT_LAYOUT_NAME so H5P_set fails.
    ASSERT_GE(H5Premove(no_layout_dcpl, H5D_CRT_LAYOUT_NAME), 0);
    (void)nl_ptr;

    absl::Cleanup restore_dcpl = [&] {
      raw_cmp->shared->dcpl_id = orig_dcpl;
      raw_cmp->shared->layout_copied_to_dcpl = orig_copied;
    };
    ScopedErrorSilence silence;
    ScopedApiContext ctx;

    // Invalid dcpl_id.
    raw_cmp->shared->dcpl_id = H5I_INVALID_HID;
    EXPECT_LT(H5D_flush_layout_to_dcpl(raw_cmp), 0);

    // Default DCPL short-circuits cleanly.
    raw_cmp->shared->dcpl_id = H5P_DATASET_CREATE_DEFAULT;
    raw_cmp->shared->layout_copied_to_dcpl = false;
    EXPECT_GE(H5D_flush_layout_to_dcpl(raw_cmp), 0);

    // Stripped DCPL fails H5P_set.
    raw_cmp->shared->dcpl_id = no_layout_dcpl.get();
    raw_cmp->shared->layout_copied_to_dcpl = false;
    EXPECT_LT(H5D_flush_layout_to_dcpl(raw_cmp), 0);
  }
}

// ===========================================================================
// 10. Stripped Plists & H5D_get_{create,access}_plist Error Paths
// ===========================================================================

TEST(H5DintTest, StrippedPlistsAndPropertyErrorPaths) {
  ScopedFile file = CreateInMemoryFile("stripped_plists.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* raw_file = UnwrapFile(file);
  ASSERT_NE(raw_file, nullptr);

  ScopedSpace s8 = MakeSimpleSpace({8});
  ScopedSpace s_vsrc = MakeSimpleSpace({4});
  ASSERT_TRUE(s8.valid());
  ASSERT_TRUE(s_vsrc.valid());
  H5S_t* raw_space = static_cast<H5S_t*>(H5I_object(s8));
  ASSERT_NE(raw_space, nullptr);

  // Build one dataset per layout category needed by get_access_plist /
  // get_create_plist / create error paths.
  ScopedDataset d_contig(H5Dcreate2(file, "contig", H5T_NATIVE_INT32, s8,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedPlist dcpl_chk(H5Pcreate(H5P_DATASET_CREATE));
  hsize_t cdim = 4;
  ASSERT_TRUE(dcpl_chk.valid());
  ASSERT_GE(H5Pset_chunk(dcpl_chk, 1, &cdim), 0);
  ScopedDataset d_chunk(H5Dcreate2(file, "chunk", H5T_NATIVE_INT32, s8,
                                   H5P_DEFAULT, dcpl_chk, H5P_DEFAULT));

  ScopedPlist dcpl_vds(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl_vds.valid());
  ASSERT_GE(H5Pset_virtual(dcpl_vds, s_vsrc, ".", "contig", s_vsrc), 0);
  ScopedDataset d_vds(H5Dcreate2(file, "vds", H5T_NATIVE_INT32, s_vsrc,
                                 H5P_DEFAULT, dcpl_vds, H5P_DEFAULT));
  ASSERT_TRUE(d_contig.valid());
  ASSERT_TRUE(d_chunk.valid());
  ASSERT_TRUE(d_vds.valid());

  H5D_t* raw_contig = UnwrapDataset(d_contig);
  H5D_t* raw_chunk = UnwrapDataset(d_chunk);
  H5D_t* raw_vds = UnwrapDataset(d_vds);
  ASSERT_NE(raw_contig, nullptr);
  ASSERT_NE(raw_chunk, nullptr);
  ASSERT_NE(raw_vds, nullptr);

  // Helper returning a fresh DCPL/DAPL copy with one named property removed.
  auto make_stripped_dcpl = [](const char* prop_name) {
    ScopedPlist p(H5Pcreate(H5P_DATASET_CREATE));
    if (p.valid() && H5Premove(p, prop_name) < 0) {
      p.reset();
    }
    return p;
  };
  auto make_stripped_dapl = [](const char* prop_name) {
    ScopedPlist p(H5Pcreate(H5P_DATASET_ACCESS));
    if (p.valid() && H5Premove(p, prop_name) < 0) {
      p.reset();
    }
    return p;
  };

  ScopedErrorSilence silence;

  // A. H5D__create failures via stripped DCPL/DAPL properties.
  {
    ScopedPlist no_layout = make_stripped_dcpl(H5D_CRT_LAYOUT_NAME);
    ScopedPlist no_fill = make_stripped_dcpl(H5D_CRT_FILL_VALUE_NAME);
    ScopedPlist no_efl = make_stripped_dcpl(H5D_CRT_EXT_FILE_LIST_NAME);
    ScopedPlist no_min_oh = make_stripped_dcpl(H5D_CRT_MIN_DSET_HDR_SIZE_NAME);
    ScopedPlist no_efile_prefix = make_stripped_dapl(H5D_ACS_EFILE_PREFIX_NAME);
    ScopedPlist no_vds_prefix = make_stripped_dapl(H5D_ACS_VDS_PREFIX_NAME);
    ASSERT_TRUE(no_layout.valid());
    ASSERT_TRUE(no_fill.valid());
    ASSERT_TRUE(no_efl.valid());
    ASSERT_TRUE(no_min_oh.valid());
    ASSERT_TRUE(no_efile_prefix.valid());
    ASSERT_TRUE(no_vds_prefix.valid());

    for (hid_t bad_dcpl : {no_layout.get(), no_fill.get(), no_efl.get(),
                           no_min_oh.get()}) {
      ScopedApiContext ctx(bad_dcpl);
      EXPECT_EQ(H5D__create(raw_file, H5T_NATIVE_INT32, raw_space, bad_dcpl,
                            H5P_DATASET_ACCESS_DEFAULT),
                nullptr);
    }

    // efile_prefix / vds_prefix failure in H5D__create triggers full cleanup
    // of layout_init, pline/layout/fill/efl caches, and oloc object header!
    {
      ScopedApiContext ctx(H5P_DATASET_CREATE_DEFAULT, no_efile_prefix);
      EXPECT_EQ(H5D__create(raw_file, H5T_NATIVE_INT32, raw_space,
                            H5P_DATASET_CREATE_DEFAULT, no_efile_prefix),
                nullptr);
    }
    {
      ScopedApiContext ctx(H5P_DATASET_CREATE_DEFAULT, no_vds_prefix);
      EXPECT_EQ(H5D__create(raw_file, H5T_NATIVE_INT32, raw_space,
                            H5P_DATASET_CREATE_DEFAULT, no_vds_prefix),
                nullptr);
    }

    // H5D_open prefix failure branches via H5Dopen2 (so H5D__open_name owns
    // and frees the temporary H5G_loc_t).
    EXPECT_FALSE(
        ScopedDataset(H5Dopen2(file, "contig", no_efile_prefix)).valid());
    EXPECT_FALSE(
        ScopedDataset(H5Dopen2(file, "contig", no_vds_prefix)).valid());
  }

  // B. H5D_get_create_plist error branches.
  {
    const hid_t saved_dcpl = raw_contig->shared->dcpl_id;
    const bool saved_copied = raw_contig->shared->layout_copied_to_dcpl;
    ScopedPlist no_layout = make_stripped_dcpl(H5D_CRT_LAYOUT_NAME);
    ScopedPlist no_fill = make_stripped_dcpl(H5D_CRT_FILL_VALUE_NAME);
    ScopedPlist no_efl = make_stripped_dcpl(H5D_CRT_EXT_FILE_LIST_NAME);
    ASSERT_TRUE(no_layout.valid());
    ASSERT_TRUE(no_fill.valid());
    ASSERT_TRUE(no_efl.valid());
    absl::Cleanup restore = [&] {
      raw_contig->shared->dcpl_id = saved_dcpl;
      raw_contig->shared->layout_copied_to_dcpl = saved_copied;
    };

    ScopedApiContext ctx;
    raw_contig->shared->dcpl_id = H5I_INVALID_HID;
    EXPECT_LT(H5D_get_create_plist(raw_contig), 0);

    // Stripped layout / fill / efl (keeping layout_copied_to_dcpl = true so
    // flush_layout_to_dcpl does not fail first).
    raw_contig->shared->layout_copied_to_dcpl = true;
    for (hid_t bad_dcpl : {no_layout.get(), no_fill.get(), no_efl.get()}) {
      raw_contig->shared->dcpl_id = bad_dcpl;
      EXPECT_LT(H5D_get_create_plist(raw_contig), 0);
    }
  }

  // C. H5D_get_access_plist error branches across chunked, contig, and VDS.
  {
    ScopedPlist no_nslots = make_stripped_dapl(H5D_ACS_DATA_CACHE_NUM_SLOTS_NAME);
    ScopedPlist no_nbytes =
        make_stripped_dapl(H5D_ACS_DATA_CACHE_BYTE_SIZE_NAME);
    ScopedPlist no_w0 = make_stripped_dapl(H5D_ACS_PREEMPT_READ_CHUNKS_NAME);
    ScopedPlist no_app_flush = make_stripped_dapl(H5D_ACS_APPEND_FLUSH_NAME);
    ScopedPlist no_vds_view = make_stripped_dapl(H5D_ACS_VDS_VIEW_NAME);
    ScopedPlist no_vds_gap = make_stripped_dapl(H5D_ACS_VDS_PRINTF_GAP_NAME);
    ScopedPlist no_vds_pref = make_stripped_dapl(H5D_ACS_VDS_PREFIX_NAME);
    ScopedPlist no_ef_pref = make_stripped_dapl(H5D_ACS_EFILE_PREFIX_NAME);
    ASSERT_TRUE(no_nslots.valid());
    ASSERT_TRUE(no_nbytes.valid());
    ASSERT_TRUE(no_w0.valid());
    ASSERT_TRUE(no_app_flush.valid());
    ASSERT_TRUE(no_vds_view.valid());
    ASSERT_TRUE(no_vds_gap.valid());
    ASSERT_TRUE(no_vds_pref.valid());
    ASSERT_TRUE(no_ef_pref.valid());

    // Invalid dapl_id on contig.
    {
      const hid_t saved = raw_contig->shared->dapl_id;
      absl::Cleanup restore = [&] { raw_contig->shared->dapl_id = saved; };
      raw_contig->shared->dapl_id = H5I_INVALID_HID;
      ScopedApiContext ctx;
      EXPECT_LT(H5D_get_access_plist(raw_contig), 0);
    }

    // Chunked branch (sets rdcc_*, append_flush, and non-VDS vds_* + prefixes).
    {
      const hid_t saved = raw_chunk->shared->dapl_id;
      absl::Cleanup restore = [&] { raw_chunk->shared->dapl_id = saved; };
      ScopedApiContext ctx;
      for (hid_t bad_dapl : {no_nslots.get(), no_nbytes.get(), no_w0.get(),
                             no_app_flush.get(), no_vds_view.get(),
                             no_vds_gap.get(), no_vds_pref.get(),
                             no_ef_pref.get()}) {
        raw_chunk->shared->dapl_id = bad_dapl;
        EXPECT_LT(H5D_get_access_plist(raw_chunk), 0);
      }
    }

    // Contig else branch for rdcc_* and append_flush.
    {
      const hid_t saved = raw_contig->shared->dapl_id;
      absl::Cleanup restore = [&] { raw_contig->shared->dapl_id = saved; };
      ScopedApiContext ctx;
      for (hid_t bad_dapl : {no_nslots.get(), no_nbytes.get(), no_w0.get(),
                             no_app_flush.get()}) {
        raw_contig->shared->dapl_id = bad_dapl;
        EXPECT_LT(H5D_get_access_plist(raw_contig), 0);
      }
    }

    // VDS branch for vds_view and vds_printf_gap.
    {
      const hid_t saved = raw_vds->shared->dapl_id;
      absl::Cleanup restore = [&] { raw_vds->shared->dapl_id = saved; };
      ScopedApiContext ctx;
      for (hid_t bad_dapl : {no_vds_view.get(), no_vds_gap.get()}) {
        raw_vds->shared->dapl_id = bad_dapl;
        EXPECT_LT(H5D_get_access_plist(raw_vds), 0);
      }
    }
  }
}

// ===========================================================================
// 11. Corrupt Object Headers on Reopen & append_flush Setup Errors
// ===========================================================================

TEST(H5DintTest, CorruptObjectHeadersOnOpenAndOidErrors) {
  auto test_remove_msg_and_reopen = [](std::string_view fname, unsigned msg_id) {
    TempFile tmp(fname);
    {
      ScopedFile wf(
          H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(wf.valid());
      ScopedSpace s = MakeSimpleSpace({4});
      ASSERT_TRUE(s.valid());
      ScopedDataset d(H5Dcreate2(wf, "ds", H5T_NATIVE_INT32, s, H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(d.valid());
      H5D_t* raw = UnwrapDataset(d);
      ASSERT_NE(raw, nullptr);
      ScopedApiContext ctx;
      ASSERT_GE(H5O_msg_remove(&raw->oloc, msg_id, H5O_ALL, false), 0);
    }
    ScopedFile rf(H5Fopen(tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(rf.valid());
    ScopedErrorSilence silence;
    EXPECT_FALSE(
        ScopedDataset(H5Dopen2(rf, "ds", H5P_DEFAULT)).valid());
  };

  test_remove_msg_and_reopen("missing_dtype.h5", H5O_DTYPE_ID);
  test_remove_msg_and_reopen("missing_space.h5", H5O_SDSPACE_ID);
  test_remove_msg_and_reopen("missing_layout.h5", H5O_LAYOUT_ID);

  // Wrong-ndims append_flush on reopen fails H5D__append_flush_setup cleanly.
  {
    TempFile tmp("bad_append_flush_open.h5");
    {
      ScopedFile wf(
          H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(wf.valid());
      ScopedSpace s = MakeSimpleSpace({4}, {H5S_UNLIMITED});
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(s.valid());
      ASSERT_TRUE(dcpl.valid());
      hsize_t cdim = 2;
      ASSERT_GE(H5Pset_chunk(dcpl, 1, &cdim), 0);
      ScopedDataset d(H5Dcreate2(wf, "chk", H5T_NATIVE_INT32, s, H5P_DEFAULT,
                                 dcpl, H5P_DEFAULT));
      ASSERT_TRUE(d.valid());
    }
    ScopedFile rf(H5Fopen(tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(rf.valid());
    ScopedPlist bad_dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(bad_dapl.valid());
    std::array<hsize_t, 2> boundary2d{2, 2};
    ASSERT_GE(H5Pset_append_flush(bad_dapl, 2, boundary2d.data(),
                                  AppendFlushNoopCb, nullptr),
              0);
    ScopedErrorSilence silence;
    EXPECT_FALSE(ScopedDataset(H5Dopen2(rf, "chk", bad_dapl)).valid());
  }

  // VL dataset with an undersized 1-byte fill message triggers fill size
  // mismatch in H5D__open_oid + fill_init reset cleanup.
  {
    TempFile tmp("undersized_vl_fill.h5");
    {
      ScopedFile wf(
          H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(wf.valid());
      ScopedSpace s = MakeSimpleSpace({2});
      ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
      ASSERT_TRUE(s.valid());
      ASSERT_TRUE(vl_int.valid());
      ScopedDataset d(H5Dcreate2(wf, "vl", vl_int, s, H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT));
      ASSERT_TRUE(d.valid());
      H5D_t* raw = UnwrapDataset(d);
      ASSERT_NE(raw, nullptr);
      ScopedApiContext ctx;
      ASSERT_GE(H5O_msg_remove(&raw->oloc, H5O_FILL_NEW_ID, H5O_ALL, false), 0);
      (void)H5O_msg_remove(&raw->oloc, H5O_FILL_ID, H5O_ALL, false);

      uint8_t tiny_byte = 0x5A;
      H5O_fill_t bad_fill{};
      bad_fill.version = H5O_FILL_VERSION_3;
      bad_fill.alloc_time = H5D_ALLOC_TIME_LATE;
      bad_fill.fill_time = H5D_FILL_TIME_ALLOC;
      bad_fill.fill_defined = true;
      bad_fill.size = 1;
      bad_fill.buf = &tiny_byte;
      bad_fill.type = nullptr;
      ASSERT_GE(H5O_msg_create(&raw->oloc, H5O_FILL_NEW_ID,
                               H5O_MSG_FLAG_CONSTANT, 0, &bad_fill),
                0);
    }
    ScopedFile rf(H5Fopen(tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(rf.valid());
    ScopedErrorSilence silence;
    EXPECT_FALSE(ScopedDataset(H5Dopen2(rf, "vl", H5P_DEFAULT)).valid());
  }
}

// ===========================================================================
// 12. Native & Generic VOL Vlen Get-Buf-Size Error Paths (Mock VOL)
// ===========================================================================

enum class MockVolMode {
  kFailGet,
  kBadGetSpace,
  kFailRead,
};

struct MockVolState {
  MockVolMode mode = MockVolMode::kFailGet;
  std::array<hsize_t, 1> dims{2};
};

herr_t MockVolDatasetGet(void* obj, H5VL_dataset_get_args_t* args,
                         hid_t /*dxpl_id*/, void** /*req*/) {
  auto* st = static_cast<MockVolState*>(obj);
  if (st == nullptr || args == nullptr ||
      args->op_type != H5VL_DATASET_GET_SPACE) {
    return -1;
  }
  switch (st->mode) {
    case MockVolMode::kFailGet:
      return -1;
    case MockVolMode::kBadGetSpace:
      args->args.get_space.space_id = H5I_INVALID_HID;
      return 0;
    case MockVolMode::kFailRead:
      args->args.get_space.space_id = H5Screate_simple(1, st->dims.data(),
                                                       nullptr);
      return args->args.get_space.space_id >= 0 ? 0 : -1;
  }
  return -1;
}

herr_t MockVolDatasetRead(size_t /*count*/, void* /*dset*/[],
                          hid_t /*mem_type_id*/[], hid_t /*mem_space_id*/[],
                          hid_t /*file_space_id*/[], hid_t /*dxpl_id*/,
                          void* /*buf*/[], void** /*req*/) {
  return -1;
}

TEST(H5DintTest, VlenBufSizeMockVolAndErrorPaths) {
  // Native H5D__vlen_get_buf_size direct arg/read error paths.
  ScopedFile file = CreateInMemoryFile("vlen_errors.h5");
  ASSERT_TRUE(file.valid());
  ScopedSpace s2 = MakeSimpleSpace({2});
  ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
  ScopedType opaque(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(s2.valid());
  ASSERT_TRUE(vl_int.valid());
  ASSERT_TRUE(opaque.valid());

  ScopedDataset d_vl(H5Dcreate2(file, "vl", vl_int, s2, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(d_vl.valid());
  H5D_t* raw_vl = UnwrapDataset(d_vl);
  ASSERT_NE(raw_vl, nullptr);
  {
    ScopedErrorSilence silence;
    ScopedApiContext ctx;
    hsize_t sz = 0;
    EXPECT_LT(H5D__vlen_get_buf_size(raw_vl, H5I_INVALID_HID, s2, &sz), 0);
    EXPECT_LT(H5D__vlen_get_buf_size(raw_vl, vl_int, H5I_INVALID_HID, &sz), 0);
    // Reading VL dataset as opaque triggers H5D__read failure in cb.
    EXPECT_LT(H5D__vlen_get_buf_size(raw_vl, opaque, s2, &sz), 0);
  }

  // Register minimal mock VOL connector for H5D__vlen_get_buf_size_gen.
  H5VL_class_t mock_vol_cls{};
  mock_vol_cls.version = H5VL_VERSION;
  mock_vol_cls.value = H5_VOL_RESERVED + 17;
  mock_vol_cls.name = "h5dint_mock_vol";
  mock_vol_cls.cap_flags = H5VL_CAP_FLAG_NONE;
  mock_vol_cls.dataset_cls.get = MockVolDatasetGet;
  mock_vol_cls.dataset_cls.read = MockVolDatasetRead;

  hid_t vol_id = H5VLregister_connector(&mock_vol_cls, H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  absl::Cleanup unreg_vol = [&] { H5VLunregister_connector(vol_id); };

  auto* conn = static_cast<H5VL_connector_t*>(H5I_object(vol_id));
  ASSERT_NE(conn, nullptr);

  MockVolState state{};
  H5VL_object_t mock_obj{};
  mock_obj.data = &state;
  mock_obj.connector = conn;
  mock_obj.rc = 1;

  ScopedErrorSilence silence;
  ScopedApiContext ctx;
  hsize_t gen_sz = 0;

  state.mode = MockVolMode::kFailGet;
  EXPECT_LT(H5D__vlen_get_buf_size_gen(&mock_obj, vl_int, s2, &gen_sz), 0);

  state.mode = MockVolMode::kBadGetSpace;
  EXPECT_LT(H5D__vlen_get_buf_size_gen(&mock_obj, vl_int, s2, &gen_sz), 0);

  state.mode = MockVolMode::kFailRead;
  EXPECT_LT(H5D__vlen_get_buf_size_gen(&mock_obj, vl_int, s2, &gen_sz), 0);
}

// ===========================================================================
// 13. Custom In-Memory VFD for Early Alloc & I/O Failure Injection
// ===========================================================================

struct MockVfdController {
  std::unordered_map<std::string, std::vector<uint8_t>> files;
  unsigned long extra_query_flags = 0;
  int fail_read_after = -1;
  int fail_write_after = -1;
  int fail_set_eoa_after = -1;
  int fail_get_eoa_after = -1;

  void Disarm() {
    fail_read_after = -1;
    fail_write_after = -1;
    fail_set_eoa_after = -1;
    fail_get_eoa_after = -1;
  }

  static bool ShouldFail(int& counter) {
    if (counter < 0) {
      return false;
    }
    if (counter == 0) {
      return true;
    }
    --counter;
    return false;
  }
};

struct MockVfdFile {
  H5FD_t pub;
  std::string name;
  haddr_t eoa = 0;
  haddr_t eof = 0;
};

MockVfdController* g_mock_vfd = nullptr;

H5FD_t* MockVfdOpen(const char* name, unsigned flags, hid_t /*fapl_id*/,
                    haddr_t /*maxaddr*/) {
  if (g_mock_vfd == nullptr || name == nullptr) {
    return nullptr;
  }
  auto it = g_mock_vfd->files.find(name);
  const bool exists = (it != g_mock_vfd->files.end());
  if ((flags & H5F_ACC_EXCL) && exists) {
    return nullptr;
  }
  if (!(flags & H5F_ACC_CREAT) && !exists) {
    return nullptr;
  }
  auto& bytes = g_mock_vfd->files[name];
  if (flags & H5F_ACC_TRUNC) {
    bytes.clear();
  }
  auto* file = new MockVfdFile();
  std::memset(&file->pub, 0, sizeof(file->pub));
  file->name = name;
  file->eoa = 0;
  file->eof = bytes.size();
  return &file->pub;
}

herr_t MockVfdClose(H5FD_t* f) {
  delete reinterpret_cast<MockVfdFile*>(f);
  return 0;
}

int MockVfdCmp(const H5FD_t* f1, const H5FD_t* f2) {
  const auto* a = reinterpret_cast<const MockVfdFile*>(f1);
  const auto* b = reinterpret_cast<const MockVfdFile*>(f2);
  if (a->name < b->name) return -1;
  if (a->name > b->name) return 1;
  return 0;
}

herr_t MockVfdQuery(const H5FD_t* /*f*/, unsigned long* flags) {
  if (flags != nullptr) {
    *flags = H5FD_FEAT_AGGREGATE_METADATA | H5FD_FEAT_ACCUMULATE_METADATA |
             H5FD_FEAT_DATA_SIEVE | H5FD_FEAT_AGGREGATE_SMALLDATA |
             (g_mock_vfd != nullptr ? g_mock_vfd->extra_query_flags : 0);
  }
  return 0;
}

haddr_t MockVfdGetEoa(const H5FD_t* f, H5FD_mem_t /*type*/) {
  return reinterpret_cast<const MockVfdFile*>(f)->eoa;
}

herr_t MockVfdSetEoa(H5FD_t* f, H5FD_mem_t /*type*/, haddr_t addr) {
  if (g_mock_vfd != nullptr &&
      MockVfdController::ShouldFail(g_mock_vfd->fail_set_eoa_after)) {
    return -1;
  }
  reinterpret_cast<MockVfdFile*>(f)->eoa = addr;
  return 0;
}

haddr_t MockVfdGetEof(const H5FD_t* f, H5FD_mem_t /*type*/) {
  return reinterpret_cast<const MockVfdFile*>(f)->eof;
}

herr_t MockVfdGetHandle(H5FD_t* f, hid_t /*fapl*/, void** file_handle) {
  if (file_handle == nullptr) {
    return -1;
  }
  *file_handle = f;
  return 0;
}

herr_t MockVfdRead(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                   haddr_t addr, size_t size, void* buf) {
  if (g_mock_vfd == nullptr) {
    return -1;
  }
  if (MockVfdController::ShouldFail(g_mock_vfd->fail_read_after)) {
    return -1;
  }
  auto* file = reinterpret_cast<MockVfdFile*>(f);
  const auto& bytes = g_mock_vfd->files[file->name];
  if (addr < bytes.size()) {
    const size_t avail = bytes.size() - static_cast<size_t>(addr);
    const size_t n = std::min(size, avail);
    std::memcpy(buf, bytes.data() + addr, n);
    if (n < size) {
      std::memset(static_cast<uint8_t*>(buf) + n, 0, size - n);
    }
  } else if (size > 0) {
    std::memset(buf, 0, size);
  }
  return 0;
}

herr_t MockVfdWrite(H5FD_t* f, H5FD_mem_t /*type*/, hid_t /*dxpl_id*/,
                    haddr_t addr, size_t size, const void* buf) {
  if (g_mock_vfd == nullptr) {
    return -1;
  }
  if (MockVfdController::ShouldFail(g_mock_vfd->fail_write_after)) {
    return -1;
  }
  auto* file = reinterpret_cast<MockVfdFile*>(f);
  auto& bytes = g_mock_vfd->files[file->name];
  const size_t end = static_cast<size_t>(addr) + size;
  if (end > bytes.size()) {
    bytes.resize(end, 0);
    file->eof = bytes.size();
  }
  if (size > 0) {
    std::memcpy(bytes.data() + addr, buf, size);
  }
  return 0;
}

TEST(H5DintTest, CustomVfdEarlyAllocAndIoFailureInjection) {
  MockVfdController ctrl;
  g_mock_vfd = &ctrl;
  absl::Cleanup clear_ctrl = [&] {
    ctrl.Disarm();
    g_mock_vfd = nullptr;
  };

  H5FD_class_t mock_cls{};
  mock_cls.version = H5FD_CLASS_VERSION;
  mock_cls.value = H5_VFD_RESERVED + 17;
  mock_cls.name = "h5dint_mock_vfd";
  mock_cls.maxaddr = (haddr_t{1} << 30);
  mock_cls.fc_degree = H5F_CLOSE_WEAK;
  mock_cls.open = MockVfdOpen;
  mock_cls.close = MockVfdClose;
  mock_cls.cmp = MockVfdCmp;
  mock_cls.query = MockVfdQuery;
  mock_cls.get_eoa = MockVfdGetEoa;
  mock_cls.set_eoa = MockVfdSetEoa;
  mock_cls.get_eof = MockVfdGetEof;
  mock_cls.get_handle = MockVfdGetHandle;
  mock_cls.read = MockVfdRead;
  mock_cls.write = MockVfdWrite;
  constexpr H5FD_mem_t flmap[H5FD_MEM_NTYPES] = H5FD_FLMAP_DICHOTOMY;
  std::memcpy(mock_cls.fl_map, flmap, sizeof(flmap));

  hid_t drv_id = H5FDregister(&mock_cls);
  ASSERT_GE(drv_id, 0);
  absl::Cleanup unreg_drv = [&] { H5FDunregister(drv_id); };

  auto make_mock_fapl = [&](bool latest = true) {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    if (fapl.valid()) {
      if (H5Pset_driver(fapl, drv_id, nullptr) < 0 ||
          (latest &&
           H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST) <
               0)) {
        fapl.reset();
      }
    }
    return fapl;
  };

  ScopedPlist fapl = make_mock_fapl(/*latest=*/true);
  ASSERT_TRUE(fapl.valid());

  // 1. H5FD_FEAT_ALLOCATE_EARLY on create, open (RDWR + unallocated), and
  // open-alloc failure. Also check H5FD_FEAT_HAS_MPI reject in H5Dflush.
  {
    ctrl.extra_query_flags = H5FD_FEAT_ALLOCATE_EARLY;
    ScopedFile wf(
        H5Fcreate("early_create.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(wf.valid());
    ScopedSpace s4 = MakeSimpleSpace({4});
    ASSERT_TRUE(s4.valid());
    ScopedDataset d_early(H5Dcreate2(wf, "early", H5T_NATIVE_INT32, s4,
                                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d_early.valid());

    // Temporarily toggle H5FD_FEAT_HAS_MPI around H5Dflush -> error 3326.
    void* raw_vfd_ptr = nullptr;
    ASSERT_GE(H5Fget_vfd_handle(wf, H5P_DEFAULT, &raw_vfd_ptr), 0);
    auto* vfd_f = static_cast<MockVfdFile*>(raw_vfd_ptr);
    {
      const unsigned long saved_flags = vfd_f->pub.feature_flags;
      absl::Cleanup restore_flags = [&] {
        vfd_f->pub.feature_flags = saved_flags;
      };
      vfd_f->pub.feature_flags |= H5FD_FEAT_HAS_MPI;
      ScopedErrorSilence silence;
      EXPECT_LT(H5Dflush(d_early), 0);
    }

    // Create with H5FD_FEAT_HAS_MPI on a dataset triggers line 1340 cleanly.
    {
      const unsigned long saved_flags = vfd_f->pub.feature_flags;
      absl::Cleanup restore_flags = [&] {
        vfd_f->pub.feature_flags = saved_flags;
      };
      vfd_f->pub.feature_flags |= H5FD_FEAT_HAS_MPI;
      ScopedDataset d_mpi(H5Dcreate2(wf, "mpi_flag", H5T_NATIVE_INT32, s4,
                                     H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      EXPECT_TRUE(d_mpi.valid());
    }
  }

  // Create unallocated 4096-element contig datasets (early_alloc OFF), then
  // reopen RDWR with ALLOCATE_EARLY ON.
  {
    ctrl.extra_query_flags = 0;
    {
      ScopedFile wf(
          H5Fcreate("late_then_early.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
      ASSERT_TRUE(wf.valid());
      ScopedSpace s_big = MakeSimpleSpace({4096});
      ASSERT_TRUE(s_big.valid());
      ScopedDataset d1(H5Dcreate2(wf, "u1", H5T_NATIVE_INT32, s_big,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ScopedDataset d2(H5Dcreate2(wf, "u2", H5T_NATIVE_INT32, s_big,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(d1.valid());
      ASSERT_TRUE(d2.valid());
    }
    ctrl.extra_query_flags = H5FD_FEAT_ALLOCATE_EARLY;
    ScopedFile rf(
        H5Fopen("late_then_early.h5", H5F_ACC_RDWR, fapl));
    ASSERT_TRUE(rf.valid());
    ScopedDataset open_ok(H5Dopen2(rf, "u1", H5P_DEFAULT));
    EXPECT_TRUE(open_ok.valid());

    // Arm set_eoa failure so H5D__alloc_storage fails inside H5D__open_oid.
    ctrl.fail_set_eoa_after = 0;
    {
      ScopedErrorSilence silence;
      EXPECT_FALSE(ScopedDataset(H5Dopen2(rf, "u2", H5P_DEFAULT)).valid());
    }
    ctrl.Disarm();
    ctrl.extra_query_flags = 0;
  }

  // 2. Alloc/fill/extend/chunked status failures with set_eoa & write faults.
  {
    ctrl.extra_query_flags = 0;
    ScopedFile wf(
        H5Fcreate("alloc_faults.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(wf.valid());

    ScopedSpace s_big = MakeSimpleSpace({32768});
    ScopedSpace s_chk = MakeSimpleSpace({2048}, {4096});
    ASSERT_TRUE(s_big.valid());
    ASSERT_TRUE(s_chk.valid());

    // Contiguous alloc failure (set_eoa fails).
    {
      ScopedPlist dcpl_alloc(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl_alloc.valid());
      ASSERT_GE(H5Pset_alloc_time(dcpl_alloc, H5D_ALLOC_TIME_EARLY), 0);
      ctrl.fail_set_eoa_after = 0;
      ScopedErrorSilence silence;
      EXPECT_FALSE(ScopedDataset(H5Dcreate2(wf, "contig_eoa_fail",
                                            H5T_NATIVE_INT32, s_big,
                                            H5P_DEFAULT, dcpl_alloc,
                                            H5P_DEFAULT))
                       .valid());
      ctrl.Disarm();
    }

    // Contiguous early fill write failure (2447, 2505). 32768 ints = 128 KiB
    // exceeds the default 64 KiB data sieve buffer so fill reaches MockVfdWrite.
    {
      ScopedPlist dcpl_fill(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl_fill.valid());
      int32_t fv = 11;
      ASSERT_GE(H5Pset_alloc_time(dcpl_fill, H5D_ALLOC_TIME_EARLY), 0);
      ASSERT_GE(H5Pset_fill_value(dcpl_fill, H5T_NATIVE_INT32, &fv), 0);
      ASSERT_GE(H5Pset_fill_time(dcpl_fill, H5D_FILL_TIME_ALLOC), 0);
      ctrl.fail_write_after = 0;
      ScopedErrorSilence silence;
      EXPECT_FALSE(ScopedDataset(H5Dcreate2(wf, "contig_fill_wfail",
                                            H5T_NATIVE_INT32, s_big,
                                            H5P_DEFAULT, dcpl_fill,
                                            H5P_DEFAULT))
                       .valid());
      ctrl.Disarm();
    }

    // Chunked early create EOA failure (2354) and early chunk allocate failure
    // (2431, 2521), plus extend EOA failure (3201). 2048-int = 8 KiB chunks
    // exceed the default small-data aggregator block size so EOA is invoked.
    {
      ScopedPlist dcpl_chk(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl_chk.valid());
      hsize_t cdim = 2048;
      int32_t fv = 22;
      ASSERT_GE(H5Pset_chunk(dcpl_chk, 1, &cdim), 0);
      ASSERT_GE(H5Pset_alloc_time(dcpl_chk, H5D_ALLOC_TIME_EARLY), 0);
      ASSERT_GE(H5Pset_fill_value(dcpl_chk, H5T_NATIVE_INT32, &fv), 0);
      ASSERT_GE(H5Pset_fill_time(dcpl_chk, H5D_FILL_TIME_ALLOC), 0);

      ScopedErrorSilence silence;
      ctrl.fail_set_eoa_after = 0;
      EXPECT_FALSE(ScopedDataset(H5Dcreate2(wf, "chk_eoa_fail",
                                            H5T_NATIVE_INT32, s_chk,
                                            H5P_DEFAULT, dcpl_chk, H5P_DEFAULT))
                       .valid());
      ctrl.Disarm();

      // Fresh file + 32768-int (128 KiB) chunked dataset so extending allocates
      // and fills a chunk > 64 KiB sieve buffer, failing at 3201.
      {
        ScopedFile wf_ext(
            H5Fcreate("chk_ext_eoa.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
        ASSERT_TRUE(wf_ext.valid());
        ScopedSpace s_big_chk = MakeSimpleSpace({32768}, {65536});
        ScopedPlist dcpl_big_chk(H5Pcreate(H5P_DATASET_CREATE));
        ASSERT_TRUE(s_big_chk.valid());
        ASSERT_TRUE(dcpl_big_chk.valid());
        hsize_t big_cdim = 32768;
        ASSERT_GE(H5Pset_chunk(dcpl_big_chk, 1, &big_cdim), 0);
        ASSERT_GE(H5Pset_alloc_time(dcpl_big_chk, H5D_ALLOC_TIME_EARLY), 0);
        ASSERT_GE(H5Pset_fill_value(dcpl_big_chk, H5T_NATIVE_INT32, &fv), 0);
        ASSERT_GE(H5Pset_fill_time(dcpl_big_chk, H5D_FILL_TIME_ALLOC), 0);
        ScopedDataset chk_ok(H5Dcreate2(wf_ext, "chk_ext", H5T_NATIVE_INT32,
                                        s_big_chk, H5P_DEFAULT, dcpl_big_chk,
                                        H5P_DEFAULT));
        ASSERT_TRUE(chk_ok.valid());
        hsize_t big_ext = 65536;
        ctrl.fail_write_after = 0;
        EXPECT_LT(H5Dset_extent(chk_ok, &big_ext), 0);
        ctrl.Disarm();
      }

      // Write failure during early chunked creation (H5D__chunk_allocate).
      ctrl.fail_write_after = 0;
      EXPECT_FALSE(ScopedDataset(H5Dcreate2(wf, "chk_wfail",
                                            H5T_NATIVE_INT32, s_chk,
                                            H5P_DEFAULT, dcpl_chk, H5P_DEFAULT))
                       .valid());
      ctrl.Disarm();
    }

    // Dirty sieve buffer flush failure (3263, 3330, 3581, 3608).
    {
      ScopedSpace s64 = MakeSimpleSpace({64});
      ASSERT_TRUE(s64.valid());
      ScopedDataset d_sieve(H5Dcreate2(wf, "sieve_ds", H5T_NATIVE_INT32, s64,
                                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(d_sieve.valid());
      ScopedSpace m2 = MakeSimpleSpace({2});
      ScopedSpace f2(H5Dget_space(d_sieve));
      ASSERT_TRUE(m2.valid());
      ASSERT_TRUE(f2.valid());
      std::array<hsize_t, 2> pts{1, 10};
      ASSERT_GE(H5Sselect_elements(f2, H5S_SELECT_SET, 2, pts.data()), 0);
      std::array<int32_t, 2> wvals{123, 456};
      ASSERT_GE(H5Dwrite(d_sieve, H5T_NATIVE_INT32, m2, f2, H5P_DEFAULT,
                         wvals.data()),
                0);

      ctrl.fail_write_after = 0;
      {
        ScopedErrorSilence silence;
        EXPECT_LT(H5Dflush(d_sieve), 0);
        ScopedApiContext ctx;
        EXPECT_LT(H5D_flush_all(UnwrapFile(wf)), 0);
      }
      ctrl.Disarm();
      EXPECT_GE(H5Dflush(d_sieve), 0);
    }
  }

  // 3. Chunked format_convert, get_space_status, get_storage_size, H5D__mark,
  // and object header creation write failures.
  {
    {
      ScopedFile wf(
          H5Fcreate("conv_and_status.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
      ASSERT_TRUE(wf.valid());
      ScopedSpace s32 = MakeSimpleSpace({32}, {H5S_UNLIMITED});
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(s32.valid());
      ASSERT_TRUE(dcpl.valid());
      hsize_t cdim = 4;
      ASSERT_GE(H5Pset_chunk(dcpl, 1, &cdim), 0);
      ScopedDataset d(H5Dcreate2(wf, "ea", H5T_NATIVE_INT32, s32, H5P_DEFAULT,
                                 dcpl, H5P_DEFAULT));
      ASSERT_TRUE(d.valid());
      std::array<int32_t, 32> buf{};
      ASSERT_GE(H5Dwrite(d, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                         buf.data()),
                0);
      ScopedSpace s_dc = MakeSimpleSpace({32});
      ASSERT_TRUE(s_dc.valid());
      ScopedDataset dc(H5Dcreate2(wf, "contig_v4", H5T_NATIVE_INT32, s_dc,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(dc.valid());
      ASSERT_GE(H5Dwrite(dc, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                         buf.data()),
                0);
    }

    // Reopen fresh so chunk index / EA metadata is not cached in RAM yet.
    {
      ScopedFile rf(H5Fopen("conv_and_status.h5", H5F_ACC_RDONLY, fapl));
      ASSERT_TRUE(rf.valid());
      ScopedDataset d_ea(H5Dopen2(rf, "ea", H5P_DEFAULT));
      ASSERT_TRUE(d_ea.valid());

      H5D_space_status_t st = H5D_SPACE_STATUS_ERROR;
      hsize_t ssz = 0;
      ScopedErrorSilence silence;
      ctrl.fail_read_after = 0;
      EXPECT_LT(H5Dget_space_status(d_ea, &st), 0);
      ctrl.Disarm();

      ctrl.fail_read_after = 0;
      EXPECT_EQ(H5Dget_storage_size(d_ea), 0);
      ctrl.Disarm();

      // Internal call covers 2560-2561 check directly too.
      {
        ScopedApiContext ctx;
        ctrl.fail_read_after = 0;
        EXPECT_LT(H5D__get_storage_size(UnwrapDataset(d_ea), &ssz), 0);
        ctrl.Disarm();
      }
    }

    // Read-only create error (H5O_create fails at 993 / 870).
    {
      ScopedFile rf(H5Fopen("conv_and_status.h5", H5F_ACC_RDONLY, fapl));
      ASSERT_TRUE(rf.valid());
      ScopedSpace s4 = MakeSimpleSpace({4});
      ASSERT_TRUE(s4.valid());
      ScopedPlist dcpl_min(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl_min.valid());
      ASSERT_GE(H5Pset_dset_no_attrs_hint(dcpl_min, true), 0);
      ScopedErrorSilence silence;
      EXPECT_FALSE(ScopedDataset(H5Dcreate2(rf, "ro_def", H5T_NATIVE_INT32, s4,
                                            H5P_DEFAULT, H5P_DEFAULT,
                                            H5P_DEFAULT))
                       .valid());
      EXPECT_FALSE(ScopedDataset(H5Dcreate2(rf, "ro_min", H5T_NATIVE_INT32, s4,
                                            H5P_DEFAULT, dcpl_min,
                                            H5P_DEFAULT))
                       .valid());
    }

    // H5D__format_convert read-only failures cover 3416 (EA remove) and 3442
    // (contiguous layout write) cleanly before any RW mutation.
    {
      ScopedFile ro(H5Fopen("conv_and_status.h5", H5F_ACC_RDONLY, fapl));
      ASSERT_TRUE(ro.valid());
      ScopedDataset d_ea(H5Dopen2(ro, "ea", H5P_DEFAULT));
      ScopedDataset d_c(H5Dopen2(ro, "contig_v4", H5P_DEFAULT));
      ASSERT_TRUE(d_ea.valid());
      ASSERT_TRUE(d_c.valid());
      H5D_t* raw_ea = UnwrapDataset(d_ea);
      H5D_t* raw_c = UnwrapDataset(d_c);
      ASSERT_NE(raw_ea, nullptr);
      ASSERT_NE(raw_c, nullptr);

      ScopedErrorSilence silence;
      ScopedApiContext ctx;

      const haddr_t saved_idx = raw_ea->shared->layout.storage.u.chunk.idx_addr;
      raw_ea->shared->layout.storage.u.chunk.idx_addr = HADDR_UNDEF;
      absl::Cleanup restore_idx = [&] {
        raw_ea->shared->layout.storage.u.chunk.idx_addr = saved_idx;
      };
      EXPECT_LT(H5D__format_convert(raw_ea), 0);

      const unsigned saved_ver = raw_c->shared->layout.version;
      raw_c->shared->layout.version = H5O_LAYOUT_VERSION_4;
      absl::Cleanup restore_ver = [&] {
        raw_c->shared->layout.version = saved_ver;
      };
      EXPECT_LT(H5D__format_convert(raw_c), 0);
    }

    // H5D__format_convert EOA failure on B-tree create (3405, 3470, 3483) and
    // read failure during H5D__chunk_format_convert (3411, 3471-3487).
    {
      ScopedFile rw(H5Fopen("conv_and_status.h5", H5F_ACC_RDWR, fapl));
      ASSERT_TRUE(rw.valid());
      ScopedDataset d_ea(H5Dopen2(rw, "ea", H5P_DEFAULT));
      ASSERT_TRUE(d_ea.valid());
      H5D_t* raw_ea = UnwrapDataset(d_ea);
      ASSERT_NE(raw_ea, nullptr);

      ScopedErrorSilence silence;
      ScopedApiContext ctx;

      ctrl.fail_set_eoa_after = 0;
      EXPECT_LT(H5D__format_convert(raw_ea), 0);
      ctrl.Disarm();

      ctrl.fail_read_after = 0;
      EXPECT_LT(H5D__format_convert(raw_ea), 0);
      ctrl.Disarm();
    }

    // Read failures during H5Dopen2 / H5Drefresh / H5D__mark with cache eviction.
    {
      ScopedPlist fapl_evict = make_mock_fapl(/*latest=*/true);
      ASSERT_TRUE(fapl_evict.valid());
      ASSERT_GE(H5Pset_evict_on_close(fapl_evict, true), 0);
      {
        ScopedFile wf(
            H5Fcreate("evict_ops.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_evict));
        ASSERT_TRUE(wf.valid());
        ScopedSpace s4 = MakeSimpleSpace({4});
        ASSERT_TRUE(s4.valid());
        ScopedDataset d(H5Dcreate2(wf, "c", H5T_NATIVE_INT32, s4, H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
        ASSERT_TRUE(d.valid());
        std::array<int32_t, 4> a{1, 2, 3, 4};
        ASSERT_GE(
            H5Dwrite(d, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     a.data()),
            0);
      }

      // Sweep read failure counts on H5Dopen2 to exercise H5D__open_oid error
      // points (1741, 1784).
      for (int k = 0; k <= 4; ++k) {
        ScopedFile rf(H5Fopen("evict_ops.h5", H5F_ACC_RDONLY, fapl_evict));
        if (!rf.valid()) {
          continue;
        }
        ScopedErrorSilence silence;
        ctrl.fail_read_after = k;
        ScopedDataset d(H5Dopen2(rf, "c", H5P_DEFAULT));
        ctrl.Disarm();
      }
    }
  }
}

// ===========================================================================
// 14. Additional OH Creation, Minimized Header & DCPL Create Error Paths
// ===========================================================================

constexpr H5Z_filter_t kDropPlineFilterId = 32701;
bool g_drop_pline_removed = false;

htri_t DropPlineCanApply(hid_t /*dcpl_id*/, hid_t /*type_id*/,
                         hid_t /*space_id*/) {
  return 1;
}

herr_t DropPlineSetLocal(hid_t dcpl_id, hid_t /*type_id*/,
                         hid_t /*space_id*/) {
  g_drop_pline_removed = (H5Premove(dcpl_id, H5O_CRT_PIPELINE_NAME) >= 0);
  return 0;
}

size_t DropPlineFilter(unsigned int /*flags*/, size_t /*cd_nelmts*/,
                       const unsigned int /*cd_values*/[], size_t nbytes,
                       size_t* /*buf_size*/, void** /*buf*/) {
  return nbytes;
}

TEST(H5DintTest, OhCreationAndMinimizedHdrOverflowErrors) {
  // 1. Direct H5D__create failures: invalid type_id (1219), H5S_set_version
  // failure (680), corrupted fill in DCPL (926), set_local deleting pipeline
  // (1291), and optional filter on SCALAR with missing layout / bad layout
  // (1295, 1344).
  {
    ScopedFile file = CreateInMemoryFile("oh_create_errs.h5");
    ASSERT_TRUE(file.valid());
    ScopedSpace space = MakeSimpleSpace({4});
    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(space.valid());
    ASSERT_TRUE(scalar.valid());
    H5F_t* raw_file = UnwrapFile(file);
    H5S_t* raw_space = static_cast<H5S_t*>(H5I_object(space));
    H5S_t* raw_scalar = static_cast<H5S_t*>(H5I_object(scalar));
    ASSERT_NE(raw_file, nullptr);
    ASSERT_NE(raw_space, nullptr);
    ASSERT_NE(raw_scalar, nullptr);

    ScopedErrorSilence silence;

    // Invalid datatype ID -> H5I_object(type_id) == NULL (line 1219).
    {
      ScopedApiContext ctx;
      EXPECT_EQ(H5D__create(raw_file, H5I_INVALID_HID, raw_space,
                            H5P_DATASET_CREATE_DEFAULT,
                            H5P_DATASET_ACCESS_DEFAULT),
                nullptr);
    }

    // Dataspace extent version > high bound -> H5S_set_version fails (680).
    {
      const unsigned saved_sver = raw_space->extent.version;
      raw_space->extent.version = 99;
      absl::Cleanup restore_sver = [&] {
        raw_space->extent.version = saved_sver;
      };
      ScopedApiContext ctx;
      EXPECT_EQ(H5D__create(raw_file, H5T_NATIVE_INT32, raw_space,
                            H5P_DATASET_CREATE_DEFAULT,
                            H5P_DATASET_ACCESS_DEFAULT),
                nullptr);
    }

    // Invalid fill size (-2) poked into DCPL -> H5D__update_oh_info fails (926).
    {
      ScopedPlist dcpl_bad_fill(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl_bad_fill.valid());
      auto* plist = static_cast<H5P_genplist_t*>(H5I_object(dcpl_bad_fill));
      ASSERT_NE(plist, nullptr);
      H5O_fill_t bad_fill{};
      ASSERT_GE(H5P_get(plist, H5D_CRT_FILL_VALUE_NAME, &bad_fill), 0);
      bad_fill.size = -2;
      bad_fill.buf = nullptr;
      ASSERT_GE(H5P_poke(plist, H5D_CRT_FILL_VALUE_NAME, &bad_fill), 0);
      ScopedApiContext ctx(dcpl_bad_fill);
      EXPECT_EQ(H5D__create(raw_file, H5T_NATIVE_INT32, raw_space,
                            dcpl_bad_fill, H5P_DATASET_ACCESS_DEFAULT),
                nullptr);
    }
  }
}

TEST(H5DintTest, OhCreationFilterLayoutErrors) {
  {
    ScopedFile file = CreateInMemoryFile("oh_create_flt_errs.h5");
    ASSERT_TRUE(file.valid());
    ScopedSpace space = MakeSimpleSpace({4});
    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(space.valid());
    ASSERT_TRUE(scalar.valid());
    H5F_t* raw_file = UnwrapFile(file);
    H5S_t* raw_scalar = static_cast<H5S_t*>(H5I_object(scalar));
    ASSERT_NE(raw_file, nullptr);
    ASSERT_NE(raw_scalar, nullptr);

    ScopedErrorSilence silence;

    // Custom filter whose set_local deletes H5O_CRT_PIPELINE_NAME -> fails at
    // line 1291 when retrieving pipeline filter from DCPL.
    {
      g_drop_pline_removed = false;
      const H5Z_class2_t drop_cls{
          /*version=*/H5Z_CLASS_T_VERS,
          /*id=*/kDropPlineFilterId,
          /*encoder_present=*/1,
          /*decoder_present=*/1,
          /*name=*/"h5dint_drop_pline",
          /*can_apply=*/DropPlineCanApply,
          /*set_local=*/DropPlineSetLocal,
          /*filter=*/DropPlineFilter,
      };
      ASSERT_GE(H5Zregister(&drop_cls), 0);
      absl::Cleanup unreg_drop = [] { H5Zunregister(kDropPlineFilterId); };

      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl.valid());
      const hsize_t cdim = 2;
      ASSERT_GE(H5Pset_chunk(dcpl, 1, &cdim), 0);
      ASSERT_GE(H5Pset_filter(dcpl, kDropPlineFilterId, H5Z_FLAG_OPTIONAL, 0,
                              nullptr),
                0);
      EXPECT_FALSE(ScopedDataset(H5Dcreate2(file, "drop_pline",
                                            H5T_NATIVE_INT32, space,
                                            H5P_DEFAULT, dcpl, H5P_DEFAULT))
                       .valid());
      EXPECT_TRUE(g_drop_pline_removed);
    }
  }
}

TEST(H5DintTest, OhCreationScalarLayoutErrors) {
  {
    ScopedFile file = CreateInMemoryFile("oh_create_scalar_errs.h5");
    ASSERT_TRUE(file.valid());
    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    H5F_t* raw_file = UnwrapFile(file);
    H5S_t* raw_scalar = static_cast<H5S_t*>(H5I_object(scalar));
    ASSERT_NE(raw_file, nullptr);
    ASSERT_NE(raw_scalar, nullptr);

    ScopedErrorSilence silence;

    // Optional filter on SCALAR sets ignore_filters = true, bypassing
    // H5Z_can_apply/H5Z_set_local so missing layout (1295) is reached inside
    // H5D__create; invalid chunk idx_type reaches H5D__layout_set_io_ops (1344)
    // without triggering H5P_copy_plist pre-inc_lst failure on H5D_NLAYOUTS.
    {
      ScopedPlist dcpl_opt_nolayout(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl_opt_nolayout.valid());
      ASSERT_GE(H5Pset_filter(dcpl_opt_nolayout, H5Z_FILTER_FLETCHER32,
                              H5Z_FLAG_OPTIONAL, 0, nullptr),
                0);
      ASSERT_GE(H5Premove(dcpl_opt_nolayout, H5D_CRT_LAYOUT_NAME), 0);
      ScopedApiContext ctx1(dcpl_opt_nolayout);
      EXPECT_EQ(H5D__create(raw_file, H5T_NATIVE_INT32, raw_scalar,
                            dcpl_opt_nolayout, H5P_DATASET_ACCESS_DEFAULT),
                nullptr);

      ScopedSpace s1d = MakeSimpleSpace({4});
      ASSERT_TRUE(s1d.valid());
      auto* raw_s1d = static_cast<H5S_t*>(H5I_object(s1d));
      ASSERT_NE(raw_s1d, nullptr);

      ScopedPlist dcpl_bad_chunk_idx(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl_bad_chunk_idx.valid());
      const hsize_t cdim = 2;
      ASSERT_GE(H5Pset_chunk(dcpl_bad_chunk_idx, 1, &cdim), 0);
      auto* pl_bad_l =
          static_cast<H5P_genplist_t*>(H5I_object(dcpl_bad_chunk_idx));
      ASSERT_NE(pl_bad_l, nullptr);
      H5O_layout_t bad_l{};
      ASSERT_GE(H5P_get(pl_bad_l, H5D_CRT_LAYOUT_NAME, &bad_l), 0);
      bad_l.u.chunk.idx_type = H5D_CHUNK_IDX_NTYPES;
      ASSERT_GE(H5P_poke(pl_bad_l, H5D_CRT_LAYOUT_NAME, &bad_l), 0);
      ScopedApiContext ctx2(dcpl_bad_chunk_idx);
      EXPECT_EQ(H5D__create(raw_file, H5T_NATIVE_INT32, raw_s1d,
                            dcpl_bad_chunk_idx, H5P_DATASET_ACCESS_DEFAULT),
                nullptr);
    }
  }
}

TEST(H5DintTest, OhUpdateOverflowErrors) {
  // 2. MockVfd with meta_block_size=0 to hit EOA/continuation overflows in
  // H5D__update_oh_info (1011, 1015, 1019, 1035, 1080, 1092).
  {
    MockVfdController ctrl;
    g_mock_vfd = &ctrl;
    absl::Cleanup clear_ctrl = [&] {
      ctrl.Disarm();
      g_mock_vfd = nullptr;
    };

    H5FD_class_t mock_cls{};
    mock_cls.version = H5FD_CLASS_VERSION;
    mock_cls.value = H5_VFD_RESERVED + 18;
    mock_cls.name = "h5dint_mock_vfd_oh";
    mock_cls.maxaddr = (haddr_t{1} << 30);
    mock_cls.fc_degree = H5F_CLOSE_WEAK;
    mock_cls.open = MockVfdOpen;
    mock_cls.close = MockVfdClose;
    mock_cls.cmp = MockVfdCmp;
    mock_cls.query = MockVfdQuery;
    mock_cls.get_eoa = MockVfdGetEoa;
    mock_cls.set_eoa = MockVfdSetEoa;
    mock_cls.get_eof = MockVfdGetEof;
    mock_cls.get_handle = MockVfdGetHandle;
    mock_cls.read = MockVfdRead;
    mock_cls.write = MockVfdWrite;
    constexpr H5FD_mem_t flmap[H5FD_MEM_NTYPES] = H5FD_FLMAP_DICHOTOMY;
    std::memcpy(mock_cls.fl_map, flmap, sizeof(flmap));

    hid_t drv_id = H5FDregister(&mock_cls);
    ASSERT_GE(drv_id, 0);
    absl::Cleanup unreg_drv = [&] { H5FDunregister(drv_id); };

    auto make_fapl_zerometa = [&](H5F_libver_t low, H5F_libver_t high) {
      ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
      if (fapl.valid()) {
        if (H5Pset_driver(fapl, drv_id, nullptr) < 0 ||
            H5Pset_meta_block_size(fapl, 0) < 0 ||
            H5Pset_small_data_block_size(fapl, 0) < 0 ||
            H5Pset_libver_bounds(fapl, low, high) < 0) {
          fapl.reset();
        }
      }
      return fapl;
    };

    ScopedPlist fapl_latest =
        make_fapl_zerometa(H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
    ScopedPlist fapl_earliest =
        make_fapl_zerometa(H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
    ASSERT_TRUE(fapl_latest.valid());
    ASSERT_TRUE(fapl_earliest.valid());

    auto run_direct_create_eoa = [&](const std::string& fname, hid_t fapl_id,
                                     hid_t dtype_id, hid_t space_id,
                                     hid_t dcpl_id, int eoa_fail_after) {
      ScopedFile wf(
          H5Fcreate(fname.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl_id));
      ASSERT_TRUE(wf.valid());
      H5F_t* raw_wf = UnwrapFile(wf);
      auto* raw_s = static_cast<H5S_t*>(H5I_object(space_id));
      ASSERT_NE(raw_wf, nullptr);
      ASSERT_NE(raw_s, nullptr);
      {
        ScopedErrorSilence silence;
        ScopedApiContext ctx(dcpl_id);
        ctrl.fail_set_eoa_after = eoa_fail_after;
        H5D_t* res = H5D__create(raw_wf, dtype_id, raw_s, dcpl_id,
                                 H5P_DATASET_ACCESS_DEFAULT);
        ctrl.Disarm();
        if (res != nullptr) {
          EXPECT_GE(H5O_dec_rc_by_loc(H5D_oloc(res)), 0);
          EXPECT_GE(H5D_close(res), 0);
        }
      }
      EXPECT_GE(H5Fclose(wf.release()), 0);
      ctrl.files.erase(fname);
    };

    // 32-D dataspace overflows chunk 0 in H5S_append (1011).
    {
      std::array<hsize_t, 32> dims32;
      dims32.fill(1);
      ScopedSpace s32(H5Screate_simple(32, dims32.data(), nullptr));
      ASSERT_TRUE(s32.valid());
      for (int k = 0; k <= 2; ++k) {
        run_direct_create_eoa("s32_" + std::to_string(k) + ".h5", fapl_latest,
                              H5T_NATIVE_INT32, s32, H5P_DATASET_CREATE_DEFAULT,
                              k);
      }
    }

    // 40-field compound overflows chunk 0 when writing DTYPE message (1015).
    {
      ScopedSpace s1 = MakeSimpleSpace({1});
      ScopedType cmpd(H5Tcreate(H5T_COMPOUND, 40 * sizeof(int32_t)));
      ASSERT_TRUE(s1.valid());
      ASSERT_TRUE(cmpd.valid());
      for (int i = 0; i < 40; ++i) {
        const std::string field = "field_long_name_" + std::to_string(i);
        ASSERT_GE(
            H5Tinsert(cmpd, field.c_str(),
                      static_cast<size_t>(i) * sizeof(int32_t), H5T_NATIVE_INT32),
            0);
      }
      for (int k = 0; k <= 2; ++k) {
        run_direct_create_eoa("cmpd_" + std::to_string(k) + ".h5", fapl_latest,
                              cmpd, s1, H5P_DATASET_CREATE_DEFAULT, k);
      }
    }

    // Opaque type has tiny DTYPE (~16B) and large FILL_NEW (~240B) -> (1019).
    {
      ScopedSpace s1 = MakeSimpleSpace({1});
      ScopedType opq(H5Tcreate(H5T_OPAQUE, 240));
      ASSERT_TRUE(s1.valid());
      ASSERT_TRUE(opq.valid());
      std::vector<uint8_t> fill_bytes(240, 0x3C);
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl.valid());
      ASSERT_GE(H5Pset_fill_value(dcpl, opq, fill_bytes.data()), 0);
      for (int k = 0; k <= 2; ++k) {
        run_direct_create_eoa("fill_new_" + std::to_string(k) + ".h5",
                              fapl_latest, opq, s1, dcpl, k);
      }
    }

    // On EARLIEST low-bound, both FILL_NEW and legacy H5O_FILL_ID are written.
    // ~140B opaque fill fits S+T+FILL_NEW in chunk 0, then overflows on legacy
    // H5O_FILL_ID -> hits 1035.
    for (size_t fsz : {size_t{110}, size_t{130}, size_t{150}, size_t{170}}) {
      ScopedSpace s1 = MakeSimpleSpace({1});
      ScopedType opq(H5Tcreate(H5T_OPAQUE, fsz));
      ASSERT_TRUE(s1.valid());
      ASSERT_TRUE(opq.valid());
      std::vector<uint8_t> fill_bytes(fsz, 0x7A);
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl.valid());
      ASSERT_GE(H5Pset_fill_value(dcpl, opq, fill_bytes.data()), 0);
      for (int k = 0; k <= 2; ++k) {
        run_direct_create_eoa(
            "fill_old_" + std::to_string(fsz) + "_" + std::to_string(k) + ".h5",
            fapl_earliest, opq, s1, dcpl, k);
      }
    }

    // Overflow right at H5O_touch_oh (1080) after layout_init = true (1092).
    // Sweep cd_nelmts so PLINE + LAYOUT fit in chunk 0 and MTIME_NEW overflows.
    {
      ScopedCustomFilter custom_flt;
      ASSERT_TRUE(custom_flt.ok());
      ScopedSpace s8 = MakeSimpleSpace({8});
      ASSERT_TRUE(s8.valid());
      const hsize_t cdim = 2;
      for (size_t cd_cnt = 16; cd_cnt <= 52; ++cd_cnt) {
        std::vector<unsigned int> cd_vals(cd_cnt, 7U);
        ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
        ASSERT_TRUE(dcpl.valid());
        ASSERT_GE(H5Pset_chunk(dcpl, 1, &cdim), 0);
        ASSERT_GE(H5Pset_filter(dcpl, kCustomFilterId, H5Z_FLAG_OPTIONAL,
                                cd_cnt, cd_vals.data()),
                  0);
        run_direct_create_eoa("touch_ovf_" + std::to_string(cd_cnt) + ".h5",
                              fapl_earliest, H5T_NATIVE_INT32, s8, dcpl, 1);
      }
    }
  }
}

// ===========================================================================
// 15. Open OID Direct Failures, SOHM/Cont Fill, MPI Open & Close/Refresh Flush
// ===========================================================================

TEST(H5DintTest, OpenOidContinuationSohmMpiAndCloseFlushErrors) {
  auto open_direct_via_loc_find = [](hid_t file_id, const char* name,
                                     hid_t dapl_id) -> H5D_t* {
    H5G_loc_t root_loc{};
    if (H5G_loc(file_id, &root_loc) < 0) {
      return nullptr;
    }
    H5G_loc_t dset_loc{};
    H5O_loc_t oloc{};
    H5G_name_t path{};
    dset_loc.oloc = &oloc;
    dset_loc.path = &path;
    H5G_loc_reset(&dset_loc);
    if (H5G_loc_find(&root_loc, name, &dset_loc) < 0) {
      return nullptr;
    }
    bool cleanup_loc = true;
    absl::Cleanup free_loc = [&] {
      if (cleanup_loc) {
        H5G_loc_free(&dset_loc);
      }
    };
    H5D_t* res = H5D_open(&dset_loc, dapl_id);
    if (res != nullptr) {
      cleanup_loc = false;
    }
    return res;
  };

  // 1. Direct H5D_open on missing_dtype (1745) and missing_space (1751, 1913)
  // bypasses H5O_obj_type check in H5D__open_name.
  auto test_direct_missing_msg = [&](std::string_view fname, unsigned msg_id) {
    TempFile tmp(fname);
    {
      ScopedFile wf(
          H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(wf.valid());
      ScopedSpace s = MakeSimpleSpace({4});
      ASSERT_TRUE(s.valid());
      ScopedDataset d(H5Dcreate2(wf, "ds", H5T_NATIVE_INT32, s, H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(d.valid());
      H5D_t* raw = UnwrapDataset(d);
      ASSERT_NE(raw, nullptr);
      ScopedApiContext ctx;
      ASSERT_GE(H5O_msg_remove(&raw->oloc, msg_id, H5O_ALL, false), 0);
    }
    ScopedFile rf(H5Fopen(tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(rf.valid());
    ScopedErrorSilence silence;
    ScopedApiContext ctx;
    EXPECT_EQ(open_direct_via_loc_find(rf, "ds", H5P_DATASET_ACCESS_DEFAULT),
              nullptr);
  };

  test_direct_missing_msg("direct_missing_dtype.h5", H5O_DTYPE_ID);
  test_direct_missing_msg("direct_missing_space.h5", H5O_SDSPACE_ID);

  // Direct H5D_open with H5I_INVALID_HID fails H5D__new in H5D__open_oid (1737).
  {
    ScopedFile f = CreateInMemoryFile("direct_bad_dapl.h5");
    ASSERT_TRUE(f.valid());
    ScopedSpace s = MakeSimpleSpace({4});
    ASSERT_TRUE(s.valid());
    {
      ScopedDataset d(H5Dcreate2(f, "ds", H5T_NATIVE_INT32, s, H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(d.valid());
    }
    ScopedErrorSilence silence;
    ScopedApiContext ctx;
    EXPECT_EQ(open_direct_via_loc_find(f, "ds", H5I_INVALID_HID), nullptr);
  }

  // 2. MockVfd tests for H5O_open failure (1741), SOHM fill reads (1784, 1795),
  // HAS_MPI early init branch (1890), H5D_close evict flush error (2088), and
  // H5D_mult_refresh_reopen errors (2242, 2254).
  MockVfdController ctrl;
  g_mock_vfd = &ctrl;
  absl::Cleanup clear_ctrl = [&] {
    ctrl.Disarm();
    ctrl.extra_query_flags = 0;
    g_mock_vfd = nullptr;
  };

  H5FD_class_t mock_cls{};
  mock_cls.version = H5FD_CLASS_VERSION;
  mock_cls.value = H5_VFD_RESERVED + 19;
  mock_cls.name = "h5dint_mock_vfd_oid";
  mock_cls.maxaddr = (haddr_t{1} << 30);
  mock_cls.fc_degree = H5F_CLOSE_WEAK;
  mock_cls.open = MockVfdOpen;
  mock_cls.close = MockVfdClose;
  mock_cls.cmp = MockVfdCmp;
  mock_cls.query = MockVfdQuery;
  mock_cls.get_eoa = MockVfdGetEoa;
  mock_cls.set_eoa = MockVfdSetEoa;
  mock_cls.get_eof = MockVfdGetEof;
  mock_cls.get_handle = MockVfdGetHandle;
  mock_cls.read = MockVfdRead;
  mock_cls.write = MockVfdWrite;
  constexpr H5FD_mem_t flmap[H5FD_MEM_NTYPES] = H5FD_FLMAP_DICHOTOMY;
  std::memcpy(mock_cls.fl_map, flmap, sizeof(flmap));

  hid_t drv_id = H5FDregister(&mock_cls);
  ASSERT_GE(drv_id, 0);
  absl::Cleanup unreg_drv = [&] { H5FDunregister(drv_id); };

  auto make_fapl = [&](bool latest, bool evict_on_close = false) {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    if (fapl.valid()) {
      if (H5Pset_driver(fapl, drv_id, nullptr) < 0 ||
          (latest &&
           H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST) <
               0) ||
          (!latest && H5Pset_libver_bounds(fapl, H5F_LIBVER_EARLIEST,
                                           H5F_LIBVER_LATEST) < 0) ||
          (evict_on_close && H5Pset_evict_on_close(fapl, true) < 0)) {
        fapl.reset();
      }
    }
    return fapl;
  };

  ScopedPlist fapl_latest = make_fapl(/*latest=*/true);
  ScopedPlist fapl_earliest = make_fapl(/*latest=*/false);
  ScopedPlist fapl_evict = make_fapl(/*latest=*/true, /*evict_on_close=*/true);
  ASSERT_TRUE(fapl_latest.valid());
  ASSERT_TRUE(fapl_earliest.valid());
  ASSERT_TRUE(fapl_evict.valid());

  // 2a. Evict OH after H5G_loc_find and fail H5O_open inside H5D__open_oid
  // (1741).
  {
    {
      ScopedFile wf(
          H5Fcreate("oid_open_fail.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_latest));
      ASSERT_TRUE(wf.valid());
      ScopedSpace s4 = MakeSimpleSpace({4});
      ASSERT_TRUE(s4.valid());
      ScopedDataset d(H5Dcreate2(wf, "c", H5T_NATIVE_INT32, s4, H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(d.valid());
    }
    ScopedFile rf(H5Fopen("oid_open_fail.h5", H5F_ACC_RDONLY, fapl_latest));
    ASSERT_TRUE(rf.valid());
    H5F_t* raw_rf = UnwrapFile(rf);
    ASSERT_NE(raw_rf, nullptr);
    {
      ScopedErrorSilence silence;
      ScopedApiContext ctx;
      H5G_loc_t root_loc{};
      ASSERT_GE(H5G_loc(rf, &root_loc), 0);
      H5G_loc_t dset_loc{};
      H5O_loc_t oloc{};
      H5G_name_t path{};
      dset_loc.oloc = &oloc;
      dset_loc.path = &path;
      H5G_loc_reset(&dset_loc);
      ASSERT_GE(H5G_loc_find(&root_loc, "c", &dset_loc), 0);
      bool owns_loc = true;
      absl::Cleanup free_loc = [&] {
        if (owns_loc) {
          H5G_loc_free(&dset_loc);
        }
      };
      ASSERT_GE(H5AC_evict_tagged_metadata(raw_rf, oloc.addr, false), 0);
      ctrl.fail_read_after = 0;
      EXPECT_EQ(H5D_open(&dset_loc, H5P_DATASET_ACCESS_DEFAULT), nullptr);
      ctrl.Disarm();
    }
  }

  // 2b. SOHM-shared fill value messages (H5O_FILL_NEW_ID -> 1784,
  // H5O_FILL_ID -> 1795) and shared PLINE with tiny MDC (1781, 1792).
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl, 1), 0);
    ASSERT_GE(H5Pset_shared_mesg_index(fcpl, 0, H5O_SHMESG_FILL_FLAG, 4), 0);

    ScopedSpace s1 = MakeSimpleSpace({1});
    ScopedType opq(H5Tcreate(H5T_OPAQUE, 24));
    ASSERT_TRUE(s1.valid());
    ASSERT_TRUE(opq.valid());
    std::array<uint8_t, 24> fill_val{};
    fill_val.fill(0x55);
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_fill_value(dcpl, opq, fill_val.data()), 0);

    {
      ScopedFile wf(
          H5Fcreate("sohm_fill.h5", H5F_ACC_TRUNC, fcpl, fapl_earliest));
      ASSERT_TRUE(wf.valid());
      ScopedDataset d_new(
          H5Dcreate2(wf, "fnew", opq, s1, H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ScopedDataset d_old(
          H5Dcreate2(wf, "fold", opq, s1, H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ASSERT_TRUE(d_new.valid());
      ASSERT_TRUE(d_old.valid());
      H5D_t* raw_old = UnwrapDataset(d_old);
      ASSERT_NE(raw_old, nullptr);
      ScopedApiContext ctx;
      ASSERT_GE(H5O_msg_remove(&raw_old->oloc, H5O_FILL_NEW_ID, H5O_ALL, false),
                0);
    }

    for (const char* name : {"fnew", "fold"}) {
      for (int k = 0; k <= 6; ++k) {
        ScopedFile rf(H5Fopen("sohm_fill.h5", H5F_ACC_RDONLY, fapl_earliest));
        ASSERT_TRUE(rf.valid());
        ScopedErrorSilence silence;
        ScopedApiContext ctx;
        ctrl.fail_read_after = k;
        H5D_t* res =
            open_direct_via_loc_find(rf, name, H5P_DATASET_ACCESS_DEFAULT);
        ctrl.Disarm();
        if (res != nullptr) {
          H5D_close(res);
          break;
        }
      }
    }
  }

  // 2c. H5FD_FEAT_HAS_MPI during open checks pline.nused == 0 (line 1890).
  {
    {
      ScopedFile wf(
          H5Fcreate("mpi_open.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_latest));
      ASSERT_TRUE(wf.valid());
      ScopedSpace s4 = MakeSimpleSpace({4});
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(s4.valid());
      ASSERT_TRUE(dcpl.valid());
      const hsize_t cdim = 2;
      ASSERT_GE(H5Pset_chunk(dcpl, 1, &cdim), 0);
      ASSERT_GE(H5Pset_deflate(dcpl, 6), 0);
      ScopedDataset d_contig(H5Dcreate2(wf, "contig", H5T_NATIVE_INT32, s4,
                                        H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
      ScopedDataset d_filt(H5Dcreate2(wf, "filt", H5T_NATIVE_INT32, s4,
                                      H5P_DEFAULT, dcpl, H5P_DEFAULT));
      ASSERT_TRUE(d_contig.valid());
      ASSERT_TRUE(d_filt.valid());
    }
    ctrl.extra_query_flags = H5FD_FEAT_HAS_MPI;
    absl::Cleanup clear_mpi = [&] { ctrl.extra_query_flags = 0; };
    ScopedFile rw(H5Fopen("mpi_open.h5", H5F_ACC_RDWR, fapl_latest));
    ASSERT_TRUE(rw.valid());
    ScopedDataset d_filt(H5Dopen2(rw, "filt", H5P_DEFAULT));
    ScopedDataset d_contig(H5Dopen2(rw, "contig", H5P_DEFAULT));
    EXPECT_TRUE(d_filt.valid());
    EXPECT_TRUE(d_contig.valid());
    EXPECT_GT(H5Dget_storage_size(d_contig), 0U);
  }

  // 2d. H5D_close evict-on-close tagged metadata flush failure (2088).
  {
    ScopedFile wf(
        H5Fcreate("evict_flush.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_evict));
    ASSERT_TRUE(wf.valid());
    ScopedSpace s4 = MakeSimpleSpace({4});
    ASSERT_TRUE(s4.valid());
    ScopedDataset d(H5Dcreate2(wf, "c", H5T_NATIVE_INT32, s4, H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d.valid());
    H5D_t* raw = UnwrapDataset(d);
    ASSERT_NE(raw, nullptr);
    {
      ScopedApiContext ctx;
      ASSERT_GE(H5D__mark(raw, H5D_MARK_LAYOUT), 0);
    }
    ScopedErrorSilence silence;
    ctrl.fail_write_after = 0;
    d.reset();
    ctrl.Disarm();
  }

  // 2e. H5D_mult_refresh_reopen H5S_read failure (2242) and H5O_msg_read
  // failure (2254). Reopen the file so the metadata accumulator is empty and
  // evicting OH forces H5S_read through MockVfdRead.
  {
    {
      ScopedFile wf(
          H5Fcreate("mult_reopen.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl_latest));
      ASSERT_TRUE(wf.valid());
      ScopedSpace s4 = MakeSimpleSpace({4});
      ASSERT_TRUE(s4.valid());
      ScopedDataset d(H5Dcreate2(wf, "m", H5T_NATIVE_INT32, s4, H5P_DEFAULT,
                                 H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(d.valid());
    }
    ScopedFile wf(H5Fopen("mult_reopen.h5", H5F_ACC_RDWR, fapl_latest));
    ASSERT_TRUE(wf.valid());
    ScopedDataset d1(H5Dopen2(wf, "m", H5P_DEFAULT));
    ScopedDataset d2(H5Dopen2(wf, "m", H5P_DEFAULT));
    ASSERT_TRUE(d1.valid());
    ASSERT_TRUE(d2.valid());
    H5D_t* raw1 = UnwrapDataset(d1);
    ASSERT_NE(raw1, nullptr);
    ASSERT_EQ(raw1->shared->fo_count, 2U);

    // 2242: H5S_read failure when OH is evicted from cache.
    H5S_t* backup_space = H5S_copy(raw1->shared->space, false, true);
    ASSERT_NE(backup_space, nullptr);
    absl::Cleanup free_backup_space = [&] {
      if (backup_space != nullptr) {
        H5S_close(backup_space);
      }
    };
    {
      ScopedErrorSilence silence;
      ScopedApiContext ctx;
      ASSERT_GE(
          H5AC_evict_tagged_metadata(raw1->oloc.file, raw1->oloc.addr, false),
          0);
      ctrl.fail_read_after = 0;
      EXPECT_LT(H5D_mult_refresh_reopen(raw1), 0);
      ctrl.Disarm();
    }
    if (raw1->shared->space == nullptr) {
      raw1->shared->space = backup_space;
      backup_space = nullptr;
    }

    // 2254: H5O_msg_read(H5O_LAYOUT_ID) failure after space reload succeeds.
    const H5O_layout_t saved_layout = raw1->shared->layout;
    absl::Cleanup restore_layout = [&] {
      raw1->shared->layout = saved_layout;
    };
    {
      ScopedErrorSilence silence;
      ScopedApiContext ctx;
      ASSERT_GE(H5O_msg_remove(&raw1->oloc, H5O_LAYOUT_ID, H5O_ALL, false), 0);
      EXPECT_LT(H5D_mult_refresh_reopen(raw1), 0);
    }
  }
}

// ===========================================================================
// 16. Alloc Storage, VLEN Gen CB, Check Filters, and Set Extent Error Paths
// ===========================================================================

H5T_t* g_removed_vlen_dt = nullptr;
hid_t g_removed_vlen_dt_id = H5I_INVALID_HID;

herr_t MockVolRemoveTypeRead(size_t /*count*/, void* /*dset*/[],
                             hid_t mem_type_id[], hid_t /*mem_space_id*/[],
                             hid_t /*file_space_id*/[], hid_t /*dxpl_id*/,
                             void* /*buf*/[], void** /*req*/) {
  if (g_removed_vlen_dt == nullptr && mem_type_id != nullptr &&
      mem_type_id[0] >= 0) {
    g_removed_vlen_dt_id = mem_type_id[0];
    g_removed_vlen_dt = static_cast<H5T_t*>(H5I_remove(mem_type_id[0]));
  }
  return 0;
}

TEST(H5DintTest, AllocStorageVlenGenCbAndSetExtentErrors) {
  // 1. Direct H5D__alloc_storage & H5D__init_storage error/edge branches
  // (2343, 2392-2393, 2439, 2494).
  {
    ScopedFile f = CreateInMemoryFile("alloc_errs.h5");
    ASSERT_TRUE(f.valid());
    ScopedSpace s4 = MakeSimpleSpace({4});
    ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
    ScopedPlist dcpl_cmp(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(s4.valid());
    ASSERT_TRUE(vl_int.valid());
    ASSERT_TRUE(dcpl_cmp.valid());
    ASSERT_GE(H5Pset_layout(dcpl_cmp, H5D_COMPACT), 0);

    ScopedDataset d_ctg(H5Dcreate2(f, "ctg", H5T_NATIVE_INT32, s4, H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT));
    ScopedDataset d_cmp(H5Dcreate2(f, "cmp", H5T_NATIVE_INT32, s4, H5P_DEFAULT,
                                   dcpl_cmp, H5P_DEFAULT));
    ScopedDataset d_cvl(H5Dcreate2(f, "cvl", vl_int, s4, H5P_DEFAULT, dcpl_cmp,
                                   H5P_DEFAULT));
    ASSERT_TRUE(d_ctg.valid());
    ASSERT_TRUE(d_cmp.valid());
    ASSERT_TRUE(d_cvl.valid());
    H5D_t* raw_ctg = UnwrapDataset(d_ctg);
    H5D_t* raw_cmp = UnwrapDataset(d_cmp);
    H5D_t* raw_cvl = UnwrapDataset(d_cvl);
    ASSERT_NE(raw_ctg, nullptr);
    ASSERT_NE(raw_cmp, nullptr);
    ASSERT_NE(raw_cvl, nullptr);

    ScopedErrorSilence silence;
    ScopedApiContext ctx;

    // 2343: unallocated contig with contig.size == 0.
    {
      const hsize_t saved_csz = raw_ctg->shared->layout.storage.u.contig.size;
      raw_ctg->shared->layout.storage.u.contig.size = 0;
      absl::Cleanup restore = [&] {
        raw_ctg->shared->layout.storage.u.contig.size = saved_csz;
      };
      EXPECT_GE(
          H5D__alloc_storage(raw_ctg, H5D_ALLOC_CREATE, false, nullptr), 0);
    }

    // 2392-2393: compact with buf == NULL and size == 0.
    {
      void* saved_buf = raw_cmp->shared->layout.storage.u.compact.buf;
      const size_t saved_sz = raw_cmp->shared->layout.storage.u.compact.size;
      raw_cmp->shared->layout.storage.u.compact.buf = nullptr;
      raw_cmp->shared->layout.storage.u.compact.size = 0;
      absl::Cleanup restore = [&] {
        raw_cmp->shared->layout.storage.u.compact.buf = saved_buf;
        raw_cmp->shared->layout.storage.u.compact.size = saved_sz;
      };
      EXPECT_GE(
          H5D__alloc_storage(raw_cmp, H5D_ALLOC_CREATE, false, nullptr), 0);
    }

    // 2494: compact VLEN with corrupt disk fill buffer (seq_len=1,
    // addr=HADDR_UNDEF) fails H5D__compact_fill via H5D__fill_refill_vl.
    {
      void* saved_buf = raw_cvl->shared->layout.storage.u.compact.buf;
      const H5O_fill_t saved_fill = raw_cvl->shared->dcpl_cache.fill;
      const size_t disk_vl_sz = H5T_get_size(raw_cvl->shared->type);
      ASSERT_GE(disk_vl_sz, 8U);
      std::vector<uint8_t> bad_vl_fill(disk_vl_sz, 0xFF);
      bad_vl_fill[0] = 1;
      bad_vl_fill[1] = 0;
      bad_vl_fill[2] = 0;
      bad_vl_fill[3] = 0;
      raw_cvl->shared->layout.storage.u.compact.buf = nullptr;
      raw_cvl->shared->dcpl_cache.fill.buf = bad_vl_fill.data();
      raw_cvl->shared->dcpl_cache.fill.size = static_cast<ssize_t>(disk_vl_sz);
      raw_cvl->shared->dcpl_cache.fill.type = nullptr;
      raw_cvl->shared->dcpl_cache.fill.fill_defined = true;
      raw_cvl->shared->dcpl_cache.fill.fill_time = H5D_FILL_TIME_ALLOC;
      absl::Cleanup restore = [&] {
        H5MM_xfree(raw_cvl->shared->layout.storage.u.compact.buf);
        raw_cvl->shared->layout.storage.u.compact.buf = saved_buf;
        raw_cvl->shared->dcpl_cache.fill = saved_fill;
      };
      EXPECT_LT(
          H5D__alloc_storage(raw_cvl, H5D_ALLOC_CREATE, false, nullptr), 0);
    }

    // 2439: H5P_is_fill_value_defined fails when fill.size == -2 after alloc.
    {
      const ssize_t saved_fsz = raw_ctg->shared->dcpl_cache.fill.size;
      raw_ctg->shared->dcpl_cache.fill.size = -2;
      absl::Cleanup restore = [&] {
        raw_ctg->shared->dcpl_cache.fill.size = saved_fsz;
      };
      EXPECT_LT(
          H5D__alloc_storage(raw_ctg, H5D_ALLOC_CREATE, false, nullptr), 0);
    }
  }

  // 2. H5D__vlen_get_buf_size_gen_cb invalid type_id on 2nd element (2856).
  {
    g_removed_vlen_dt = nullptr;
    g_removed_vlen_dt_id = H5I_INVALID_HID;
    absl::Cleanup free_dt = [] {
      if (g_removed_vlen_dt != nullptr) {
        H5T_close_real(g_removed_vlen_dt);
        g_removed_vlen_dt = nullptr;
      }
    };

    H5VL_class_t mock_vol_cls{};
    mock_vol_cls.version = H5VL_VERSION;
    mock_vol_cls.value = H5_VOL_RESERVED + 18;
    mock_vol_cls.name = "h5dint_mock_vol_drop_dt";
    mock_vol_cls.cap_flags = H5VL_CAP_FLAG_NONE;
    mock_vol_cls.dataset_cls.get = MockVolDatasetGet;
    mock_vol_cls.dataset_cls.read = MockVolRemoveTypeRead;

    hid_t vol_id = H5VLregister_connector(&mock_vol_cls, H5P_DEFAULT);
    ASSERT_GE(vol_id, 0);
    absl::Cleanup unreg_vol = [&] { H5VLunregister_connector(vol_id); };

    auto* conn = static_cast<H5VL_connector_t*>(H5I_object(vol_id));
    ASSERT_NE(conn, nullptr);

    MockVolState state{};
    state.mode = MockVolMode::kFailRead;
    H5VL_object_t mock_obj{};
    mock_obj.data = &state;
    mock_obj.connector = conn;
    mock_obj.rc = 1;

    ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
    ScopedSpace s2 = MakeSimpleSpace({2});
    ASSERT_TRUE(vl_int.valid());
    ASSERT_TRUE(s2.valid());
    const hid_t tmp_dt = H5Tcopy(vl_int);
    ASSERT_GE(tmp_dt, 0);

    hsize_t gen_sz = 0;
    ScopedErrorSilence silence;
    ScopedApiContext ctx;
    EXPECT_LT(H5D__vlen_get_buf_size_gen(&mock_obj, tmp_dt, s2, &gen_sz), 0);
    if (g_removed_vlen_dt == nullptr) {
      H5Tclose(tmp_dt);
    }
  }

  // 3. In-memory extend errors: H5D__check_filters (3023, 3031), H5S_set_extent
  // max exceeded (3091), H5VM_power2up overflow (3134), chunk dim==0 with
  // ndims==1 (3161), and H5D__mark missing SDSPACE (3228, 3536).
  {
    ScopedFile f = CreateInMemoryFile("set_extent_errs.h5");
    ASSERT_TRUE(f.valid());
    ScopedSpace s4_lim = MakeSimpleSpace({4}, {8});
    ScopedSpace s2d = MakeSimpleSpace({2, 2}, {H5S_UNLIMITED, H5S_UNLIMITED});
    ScopedSpace s1d = MakeSimpleSpace({4}, {H5S_UNLIMITED});
    ScopedPlist dcpl1(H5Pcreate(H5P_DATASET_CREATE));
    ScopedPlist dcpl2(H5Pcreate(H5P_DATASET_CREATE));
    ScopedPlist no_layout_dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(s4_lim.valid());
    ASSERT_TRUE(s2d.valid());
    ASSERT_TRUE(s1d.valid());
    ASSERT_TRUE(dcpl1.valid());
    ASSERT_TRUE(dcpl2.valid());
    ASSERT_TRUE(no_layout_dcpl.valid());
    const hsize_t c1 = 2;
    const std::array<hsize_t, 2> c2{1, 1};
    ASSERT_GE(H5Pset_chunk(dcpl1, 1, &c1), 0);
    ASSERT_GE(H5Pset_chunk(dcpl2, 2, c2.data()), 0);
    ASSERT_GE(H5Premove(no_layout_dcpl, H5D_CRT_LAYOUT_NAME), 0);

    ScopedDataset d_lim(H5Dcreate2(f, "lim", H5T_NATIVE_INT32, s4_lim,
                                   H5P_DEFAULT, dcpl1, H5P_DEFAULT));
    ScopedDataset d1d(H5Dcreate2(f, "d1d", H5T_NATIVE_INT32, s1d, H5P_DEFAULT,
                                 dcpl1, H5P_DEFAULT));
    ScopedDataset d2d(H5Dcreate2(f, "d2d", H5T_NATIVE_INT32, s2d, H5P_DEFAULT,
                                 dcpl2, H5P_DEFAULT));
    ASSERT_TRUE(d_lim.valid());
    ASSERT_TRUE(d1d.valid());
    ASSERT_TRUE(d2d.valid());
    H5D_t* raw1d = UnwrapDataset(d1d);
    ASSERT_NE(raw1d, nullptr);

    ScopedErrorSilence silence;
    const hsize_t ext6 = 6;
    const hsize_t ext10 = 10;
    const std::array<hsize_t, 2> ext_huge{(1ULL << 63), 2};

    // 3091: exceeding finite max_dims.
    EXPECT_LT(H5Dset_extent(d_lim, &ext10), 0);

    // 3134: scaled >= (1ULL << 63) overflows H5VM_power2up.
    EXPECT_LT(H5Dset_extent(d2d, ext_huge.data()), 0);

    // 3023: H5P_is_fill_value_defined fails in H5D__check_filters.
    {
      const ssize_t saved_fsz = raw1d->shared->dcpl_cache.fill.size;
      const bool saved_cf = raw1d->shared->checked_filters;
      raw1d->shared->dcpl_cache.fill.size = -2;
      raw1d->shared->checked_filters = false;
      absl::Cleanup restore = [&] {
        raw1d->shared->dcpl_cache.fill.size = saved_fsz;
        raw1d->shared->checked_filters = saved_cf;
      };
      EXPECT_LT(H5Dset_extent(d1d, &ext6), 0);
    }

    // 3031: H5D_flush_layout_to_dcpl fails in H5D__check_filters when fill
    // policy requires fill checking on extend.
    {
      const hid_t saved_dcpl = raw1d->shared->dcpl_id;
      const bool saved_copied = raw1d->shared->layout_copied_to_dcpl;
      const bool saved_cf = raw1d->shared->checked_filters;
      const H5D_fill_time_t saved_ft = raw1d->shared->dcpl_cache.fill.fill_time;
      raw1d->shared->dcpl_id = no_layout_dcpl;
      raw1d->shared->layout_copied_to_dcpl = false;
      raw1d->shared->checked_filters = false;
      raw1d->shared->dcpl_cache.fill.fill_time = H5D_FILL_TIME_ALLOC;
      absl::Cleanup restore = [&] {
        raw1d->shared->dcpl_id = saved_dcpl;
        raw1d->shared->layout_copied_to_dcpl = saved_copied;
        raw1d->shared->checked_filters = saved_cf;
        raw1d->shared->dcpl_cache.fill.fill_time = saved_ft;
      };
      EXPECT_LT(H5Dset_extent(d1d, &ext6), 0);
    }

    // 3161: ndims == 0 fails H5D__chunk_set_info during extend.
    {
      ScopedDataset d_nd0(H5Dcreate2(f, "d_nd0", H5T_NATIVE_INT32, s1d,
                                     H5P_DEFAULT, dcpl1, H5P_DEFAULT));
      ASSERT_TRUE(d_nd0.valid());
      H5D_t* raw_nd0 = UnwrapDataset(d_nd0);
      ASSERT_NE(raw_nd0, nullptr);
      const unsigned saved_ndims = raw_nd0->shared->ndims;
      raw_nd0->shared->ndims = 0;
      absl::Cleanup restore = [&] { raw_nd0->shared->ndims = saved_ndims; };
      EXPECT_LT(H5Dset_extent(d_nd0, &ext6), 0);
    }

    // 3536 & 3228: missing H5O_SDSPACE_ID fails H5D__mark(H5D_MARK_SPACE).
    {
      ScopedApiContext ctx;
      ASSERT_GE(H5O_msg_remove(&raw1d->oloc, H5O_SDSPACE_ID, H5O_ALL, false),
                0);
    }
    const hsize_t ext7 = 7;
    EXPECT_LT(H5Dset_extent(d1d, &ext7), 0);
  }

  // 4. VDS H5D__set_extent patching failure on source_dset (3183) and
  // sub_dset (3190).
  {
    ScopedFile f = CreateInMemoryFile("vds_set_extent.h5");
    ASSERT_TRUE(f.valid());
    ScopedSpace s4 = MakeSimpleSpace({4});
    ScopedSpace vds_space = MakeSimpleSpace({4}, {H5S_UNLIMITED});
    ScopedSpace vspace = MakeSimpleSpace({4}, {H5S_UNLIMITED});
    ASSERT_TRUE(s4.valid());
    ASSERT_TRUE(vds_space.valid());
    ASSERT_TRUE(vspace.valid());

    // 4a. Non-printf source_dset mapping -> hits 3183.
    ScopedDataset src0(H5Dcreate2(f, "src_fixed", H5T_NATIVE_INT32, s4,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(src0.valid());
    ScopedPlist vdcpl1(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(vdcpl1.valid());
    ASSERT_GE(H5Pset_virtual(vdcpl1, vspace, ".", "src_fixed", s4), 0);
    ScopedDataset vds1(H5Dcreate2(f, "vds_fixed", H5T_NATIVE_INT32, vds_space,
                                  H5P_DEFAULT, vdcpl1, H5P_DEFAULT));
    ASSERT_TRUE(vds1.valid());
    H5D_t* raw_vds1 = UnwrapDataset(vds1);
    ASSERT_NE(raw_vds1, nullptr);
    ASSERT_GE(raw_vds1->shared->layout.storage.u.virt.list_nused, 1U);
    H5S_t* vs1 =
        raw_vds1->shared->layout.storage.u.virt.list[0].source_dset.virtual_select;
    ASSERT_NE(vs1, nullptr);
    ASSERT_NE(vs1->extent.max, nullptr);
    {
      const hsize_t saved_max = vs1->extent.max[0];
      vs1->extent.max[0] = 4;
      absl::Cleanup restore = [&] { vs1->extent.max[0] = saved_max; };
      ScopedErrorSilence silence;
      const hsize_t ext8 = 8;
      EXPECT_LT(H5Dset_extent(vds1, &ext8), 0);
    }

    // 4b. Printf mapping populates sub_dset_nused -> hits 3190.
    ScopedDataset psrc(H5Dcreate2(f, "psrc_0", H5T_NATIVE_INT32, s4,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(psrc.valid());
    std::array<int32_t, 4> pdata{10, 20, 30, 40};
    ASSERT_GE(H5Dwrite(psrc, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       pdata.data()),
              0);
    psrc.reset();

    ScopedSpace pvds_space = MakeSimpleSpace({4}, {H5S_UNLIMITED});
    ScopedSpace pvspace = MakeSimpleSpace({4}, {H5S_UNLIMITED});
    ASSERT_TRUE(pvds_space.valid());
    ASSERT_TRUE(pvspace.valid());
    const hsize_t start = 0;
    const hsize_t stride = 4;
    const hsize_t count = H5S_UNLIMITED;
    const hsize_t block = 4;
    ASSERT_GE(H5Sselect_hyperslab(pvspace, H5S_SELECT_SET, &start, &stride,
                                  &count, &block),
              0);
    ScopedPlist vdcpl2(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(vdcpl2.valid());
    ASSERT_GE(H5Pset_virtual(vdcpl2, pvspace, ".", "psrc_%b", s4), 0);
    ScopedPlist vdapl2(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(vdapl2.valid());
    ASSERT_GE(H5Pset_virtual_view(vdapl2, H5D_VDS_FIRST_MISSING), 0);
    ScopedDataset vds2(H5Dcreate2(f, "vds_printf", H5T_NATIVE_INT32,
                                  pvds_space, H5P_DEFAULT, vdcpl2, vdapl2));
    ASSERT_TRUE(vds2.valid());
    // Trigger VDS initialization to populate sub_dset[].
    std::array<int32_t, 4> out{};
    ASSERT_GE(
        H5Dread(vds2, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                out.data()),
        0);
    EXPECT_THAT(out, ElementsAre(10, 20, 30, 40));
    H5D_t* raw_vds2 = UnwrapDataset(vds2);
    ASSERT_NE(raw_vds2, nullptr);
    const auto& ent0 = raw_vds2->shared->layout.storage.u.virt.list[0];
    ASSERT_GE(ent0.sub_dset_nused, 1U);
    H5S_t* sub_vs0 = ent0.sub_dset[0].virtual_select;
    ASSERT_NE(sub_vs0, nullptr);
    ASSERT_NE(sub_vs0->extent.max, nullptr);
    {
      const hsize_t saved_sub_max = sub_vs0->extent.max[0];
      sub_vs0->extent.max[0] = 4;
      absl::Cleanup restore = [&] { sub_vs0->extent.max[0] = saved_sub_max; };
      ScopedErrorSilence silence;
      const hsize_t ext8 = 8;
      EXPECT_LT(H5Dset_extent(vds2, &ext8), 0);
    }
  }

  // 5. MockVfd tests for H5D__mark in H5D__alloc_storage (2462, 3522),
  // H5D__chunk_update_cache eviction write failure (3167),
  // H5D__chunk_prune_by_extent read failure (3215), and
  // H5D__chunk_update_old_edge_chunks read failure (3223).
  {
    MockVfdController ctrl;
    g_mock_vfd = &ctrl;
    absl::Cleanup clear_ctrl = [&] {
      ctrl.Disarm();
      g_mock_vfd = nullptr;
    };

    H5FD_class_t mock_cls{};
    mock_cls.version = H5FD_CLASS_VERSION;
    mock_cls.value = H5_VFD_RESERVED + 20;
    mock_cls.name = "h5dint_mock_vfd_ext";
    mock_cls.maxaddr = (haddr_t{1} << 30);
    mock_cls.fc_degree = H5F_CLOSE_WEAK;
    mock_cls.open = MockVfdOpen;
    mock_cls.close = MockVfdClose;
    mock_cls.cmp = MockVfdCmp;
    mock_cls.query = MockVfdQuery;
    mock_cls.get_eoa = MockVfdGetEoa;
    mock_cls.set_eoa = MockVfdSetEoa;
    mock_cls.get_eof = MockVfdGetEof;
    mock_cls.get_handle = MockVfdGetHandle;
    mock_cls.read = MockVfdRead;
    mock_cls.write = MockVfdWrite;
    constexpr H5FD_mem_t flmap[H5FD_MEM_NTYPES] = H5FD_FLMAP_DICHOTOMY;
    std::memcpy(mock_cls.fl_map, flmap, sizeof(flmap));

    hid_t drv_id = H5FDregister(&mock_cls);
    ASSERT_GE(drv_id, 0);
    absl::Cleanup unreg_drv = [&] { H5FDunregister(drv_id); };

    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_driver(fapl, drv_id, nullptr), 0);
    ASSERT_GE(
        H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST), 0);

    // 5a. H5D__alloc_storage(H5D_ALLOC_OPEN) -> H5D__mark fail (2462, 3522).
    {
      {
        ScopedFile wf(
            H5Fcreate("alloc_mark.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
        ASSERT_TRUE(wf.valid());
        ScopedSpace s4 = MakeSimpleSpace({4});
        ASSERT_TRUE(s4.valid());
        ScopedDataset dc(H5Dcreate2(wf, "ctg", H5T_NATIVE_INT32, s4,
                                    H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
        ASSERT_TRUE(dc.valid());
      }
      ScopedFile rw(H5Fopen("alloc_mark.h5", H5F_ACC_RDWR, fapl));
      ASSERT_TRUE(rw.valid());
      ScopedDataset dc(H5Dopen2(rw, "ctg", H5P_DEFAULT));
      ASSERT_TRUE(dc.valid());
      H5D_t* raw = UnwrapDataset(dc);
      ASSERT_NE(raw, nullptr);
      ScopedErrorSilence silence;
      ScopedApiContext ctx;
      ASSERT_GE(H5AC_evict_tagged_metadata(raw->oloc.file, raw->oloc.addr, false),
                0);
      ctrl.fail_read_after = 0;
      EXPECT_LT(H5D__alloc_storage(raw, H5D_ALLOC_OPEN, false, nullptr), 0);
      ctrl.Disarm();
    }

    // 5b. Chunk cache update collision eviction write failure (3167):
    // nslots=3, 2D {2,2} chunk {1,1} (encode_bits[1]=1).
    // Chunk (0,1) -> hash (0<<1)^1 % 3 = 1; Chunk (1,0) -> (1<<1)^0 % 3 = 2.
    // Extending dims to {2,4} sets encode_bits[1]=2 -> both hash to slot 1!
    {
      ScopedFile wf(
          H5Fcreate("chk_cache_upd.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
      ASSERT_TRUE(wf.valid());
      ScopedSpace s2d = MakeSimpleSpace({2, 2}, {H5S_UNLIMITED, H5S_UNLIMITED});
      ScopedSpace smem = MakeSimpleSpace({1, 1});
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
      ASSERT_TRUE(s2d.valid());
      ASSERT_TRUE(smem.valid());
      ASSERT_TRUE(dcpl.valid());
      ASSERT_TRUE(dapl.valid());
      const std::array<hsize_t, 2> cdims{1, 1};
      ASSERT_GE(H5Pset_chunk(dcpl, 2, cdims.data()), 0);
      ASSERT_GE(H5Pset_deflate(dcpl, 1), 0);
      ASSERT_GE(H5Pset_chunk_cache(dapl, 3, 1024 * 1024, 0.0), 0);

      ScopedDataset d2(H5Dcreate2(wf, "c2", H5T_NATIVE_INT32, s2d, H5P_DEFAULT,
                                  dcpl, dapl));
      ASSERT_TRUE(d2.valid());

      ScopedSpace sf(H5Dget_space(d2));
      ASSERT_TRUE(sf.valid());
      const std::array<hsize_t, 2> coord01{0, 1};
      const std::array<hsize_t, 2> coord10{1, 0};
      const int32_t v1 = 11;
      const int32_t v2 = 22;
      ASSERT_GE(
          H5Sselect_elements(sf, H5S_SELECT_SET, 1, coord01.data()), 0);
      ASSERT_GE(
          H5Dwrite(d2, H5T_NATIVE_INT32, smem, sf, H5P_DEFAULT, &v1), 0);
      ASSERT_GE(
          H5Sselect_elements(sf, H5S_SELECT_SET, 1, coord10.data()), 0);
      ASSERT_GE(
          H5Dwrite(d2, H5T_NATIVE_INT32, smem, sf, H5P_DEFAULT, &v2), 0);

      ScopedErrorSilence silence;
      ctrl.fail_write_after = 0;
      const std::array<hsize_t, 2> new_dims{2, 4};
      EXPECT_LT(H5Dset_extent(d2, new_dims.data()), 0);
      ctrl.Disarm();
    }

    // 5c. Shrinking chunked dataset with uncached EA index -> read fail (3215),
    // and expanding DONT_FILTER_PARTIAL_CHUNKS filtered dataset -> read fail
    // (3223).
    {
      {
        ScopedFile wf(
            H5Fcreate("ext_io_fail.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
        ASSERT_TRUE(wf.valid());
        ScopedSpace s4 = MakeSimpleSpace({4}, {H5S_UNLIMITED});
        ScopedSpace s3 = MakeSimpleSpace({3}, {H5S_UNLIMITED});
        ScopedPlist dcpl_plain(H5Pcreate(H5P_DATASET_CREATE));
        ScopedPlist dcpl_edge(H5Pcreate(H5P_DATASET_CREATE));
        ASSERT_TRUE(s4.valid());
        ASSERT_TRUE(s3.valid());
        ASSERT_TRUE(dcpl_plain.valid());
        ASSERT_TRUE(dcpl_edge.valid());
        const hsize_t cdim = 2;
        ASSERT_GE(H5Pset_chunk(dcpl_plain, 1, &cdim), 0);
        ASSERT_GE(H5Pset_chunk(dcpl_edge, 1, &cdim), 0);
        ASSERT_GE(
            H5Pset_chunk_opts(dcpl_edge, H5D_CHUNK_DONT_FILTER_PARTIAL_CHUNKS),
            0);
        ASSERT_GE(H5Pset_deflate(dcpl_edge, 1), 0);

        ScopedDataset d_shrink(H5Dcreate2(wf, "shrink", H5T_NATIVE_INT32, s4,
                                          H5P_DEFAULT, dcpl_plain,
                                          H5P_DEFAULT));
        ScopedDataset d_edge(H5Dcreate2(wf, "edge", H5T_NATIVE_INT32, s3,
                                        H5P_DEFAULT, dcpl_edge, H5P_DEFAULT));
        ASSERT_TRUE(d_shrink.valid());
        ASSERT_TRUE(d_edge.valid());
        std::array<int32_t, 4> w4{1, 2, 3, 4};
        std::array<int32_t, 3> w3{1, 2, 3};
        ASSERT_GE(H5Dwrite(d_shrink, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                           H5P_DEFAULT, w4.data()),
                  0);
        ASSERT_GE(H5Dwrite(d_edge, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                           H5P_DEFAULT, w3.data()),
                  0);
      }

      ScopedFile rw(H5Fopen("ext_io_fail.h5", H5F_ACC_RDWR, fapl));
      ASSERT_TRUE(rw.valid());
      ScopedDataset d_shrink(H5Dopen2(rw, "shrink", H5P_DEFAULT));
      ScopedDataset d_edge(H5Dopen2(rw, "edge", H5P_DEFAULT));
      ASSERT_TRUE(d_shrink.valid());
      ASSERT_TRUE(d_edge.valid());

      ScopedErrorSilence silence;
      const hsize_t ext2 = 2;
      ctrl.fail_read_after = 0;
      EXPECT_LT(H5Dset_extent(d_shrink, &ext2), 0);
      ctrl.Disarm();

      const hsize_t ext5 = 5;
      ctrl.fail_read_after = 0;
      EXPECT_LT(H5Dset_extent(d_edge, &ext5), 0);
      ctrl.Disarm();
    }
  }
}

// ===========================================================================
// 17. Format Convert, Mark, Get Create Plist Fill Conv, VDS Space & Refresh
// ===========================================================================

TEST(H5DintTest, FormatConvertMarkGetPlistSpaceAndRefreshErrors) {
  // 1. MockVfd H5D__format_convert EOA failure on H5O_msg_create (3422).
  {
    MockVfdController ctrl;
    g_mock_vfd = &ctrl;
    absl::Cleanup clear_ctrl = [&] {
      ctrl.Disarm();
      g_mock_vfd = nullptr;
    };

    H5FD_class_t mock_cls{};
    mock_cls.version = H5FD_CLASS_VERSION;
    mock_cls.value = H5_VFD_RESERVED + 21;
    mock_cls.name = "h5dint_mock_vfd_fmt";
    mock_cls.maxaddr = (haddr_t{1} << 30);
    mock_cls.fc_degree = H5F_CLOSE_WEAK;
    mock_cls.open = MockVfdOpen;
    mock_cls.close = MockVfdClose;
    mock_cls.cmp = MockVfdCmp;
    mock_cls.query = MockVfdQuery;
    mock_cls.get_eoa = MockVfdGetEoa;
    mock_cls.set_eoa = MockVfdSetEoa;
    mock_cls.get_eof = MockVfdGetEof;
    mock_cls.get_handle = MockVfdGetHandle;
    mock_cls.read = MockVfdRead;
    mock_cls.write = MockVfdWrite;
    constexpr H5FD_mem_t flmap[H5FD_MEM_NTYPES] = H5FD_FLMAP_DICHOTOMY;
    std::memcpy(mock_cls.fl_map, flmap, sizeof(flmap));

    hid_t drv_id = H5FDregister(&mock_cls);
    ASSERT_GE(drv_id, 0);
    absl::Cleanup unreg_drv = [&] { H5FDunregister(drv_id); };

    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl.valid());
    ASSERT_GE(H5Pset_driver(fapl, drv_id, nullptr), 0);
    ASSERT_GE(H5Pset_meta_block_size(fapl, 0), 0);
    ASSERT_GE(H5Pset_libver_bounds(fapl, H5F_LIBVER_V110, H5F_LIBVER_LATEST),
              0);

    ScopedFile wf(
        H5Fcreate("fmt_msg_create.h5", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
    ASSERT_TRUE(wf.valid());
    std::array<hsize_t, 32> dims32{};
    dims32.fill(1);
    dims32[0] = 2;
    ScopedSpace s32(H5Screate_simple(32, dims32.data(), nullptr));
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(s32.valid());
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl, 32, dims32.data()), 0);
    ASSERT_GE(H5Pset_dset_no_attrs_hint(dcpl, true), 0);
    ScopedDataset d32(H5Dcreate2(wf, "s", H5T_NATIVE_INT32, s32, H5P_DEFAULT,
                                 dcpl, H5P_DEFAULT));
    ASSERT_TRUE(d32.valid());
    H5D_t* raw32 = UnwrapDataset(d32);
    ASSERT_NE(raw32, nullptr);

    ScopedErrorSilence silence;
    ScopedApiContext ctx;
    ctrl.fail_set_eoa_after = 0;
    EXPECT_LT(H5D__format_convert(raw32), 0);
    ctrl.Disarm();
  }

  // 2. H5D__format_convert & H5D__mark default switch (3453, 3527), and
  // H5D_get_create_plist error branches (3646-3647, 3657, 3697, 3720,
  // 3744-3747, 3789).
  {
    TempFile tmp("get_dcpl_conv.h5");
    ScopedPlist latest_fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(latest_fapl.valid());
    ASSERT_GE(
        H5Pset_libver_bounds(latest_fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST),
        0);

    {
      ScopedFile wf(
          H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, latest_fapl));
      ASSERT_TRUE(wf.valid());
      ScopedSpace s4 = MakeSimpleSpace({4});
      ScopedType opq4(H5Tcreate(H5T_OPAQUE, 4));
      ScopedType vl_int(H5Tvlen_create(H5T_NATIVE_INT32));
      ScopedType cmpd_vl(H5Tcreate(H5T_COMPOUND, sizeof(hvl_t)));
      ASSERT_TRUE(s4.valid());
      ASSERT_TRUE(opq4.valid());
      ASSERT_TRUE(vl_int.valid());
      ASSERT_TRUE(cmpd_vl.valid());
      ASSERT_GE(H5Tinsert(cmpd_vl, "v", 0, vl_int), 0);

      std::array<uint8_t, 4> opq_fill{0x11, 0x22, 0x33, 0x44};
      ScopedPlist dcpl_opq(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl_opq.valid());
      ASSERT_GE(H5Pset_fill_value(dcpl_opq, opq4, opq_fill.data()), 0);

      int32_t vl_elem = 7;
      struct {
        hvl_t v;
      } init_vl{};
      init_vl.v.len = 1;
      init_vl.v.p = &vl_elem;
      ScopedPlist dcpl_vl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl_vl.valid());
      ASSERT_GE(H5Pset_fill_value(dcpl_vl, cmpd_vl, &init_vl), 0);

      ScopedDataset d_opq(H5Dcreate2(wf, "opq", opq4, s4, H5P_DEFAULT, dcpl_opq,
                                     H5P_DEFAULT));
      ScopedDataset d_vl(H5Dcreate2(wf, "cmpd_vl", cmpd_vl, s4, H5P_DEFAULT,
                                    dcpl_vl, H5P_DEFAULT));
      ASSERT_TRUE(d_opq.valid());
      ASSERT_TRUE(d_vl.valid());

      // Rewrite cmpd_vl's H5O_FILL_NEW_ID on disk with non-zero seq_len and
      // HADDR_UNDEF collection address so H5T_convert fails on reopen.
      H5D_t* raw_vl = UnwrapDataset(d_vl);
      ASSERT_NE(raw_vl, nullptr);
      {
        ScopedApiContext ctx;
        H5O_fill_t* on_disk_fill = static_cast<H5O_fill_t*>(
            H5O_msg_read(&raw_vl->oloc, H5O_FILL_NEW_ID, nullptr));
        ASSERT_NE(on_disk_fill, nullptr);
        absl::Cleanup free_msg = [&] {
          H5O_msg_free(H5O_FILL_NEW_ID, on_disk_fill);
        };
        const ssize_t fsz = on_disk_fill->size;
        ASSERT_GE(fsz, 12);
        std::vector<uint8_t> bad_disk_vl(static_cast<size_t>(fsz), 0xFF);
        // Set 4-byte LE sequence length = 1 while leaving collection addr as
        // 0xFF...FF (HADDR_UNDEF).
        bad_disk_vl[0] = 1;
        bad_disk_vl[1] = 0;
        bad_disk_vl[2] = 0;
        bad_disk_vl[3] = 0;
        H5O_fill_t custom_fill{};
        custom_fill.version = H5O_FILL_VERSION_3;
        custom_fill.alloc_time = H5D_ALLOC_TIME_LATE;
        custom_fill.fill_time = H5D_FILL_TIME_ALLOC;
        custom_fill.fill_defined = true;
        custom_fill.size = fsz;
        custom_fill.buf = bad_disk_vl.data();
        custom_fill.type = nullptr;
        ASSERT_GE(H5O_msg_remove(&raw_vl->oloc, H5O_FILL_NEW_ID, H5O_ALL, false),
                  0);
        ASSERT_GE(H5O_msg_create(&raw_vl->oloc, H5O_FILL_NEW_ID,
                                 H5O_MSG_FLAG_CONSTANT, 0, &custom_fill),
                  0);
      }
    }

    // Reopen RDONLY for H5D__refresh failure (4044), default switch tests, and
    // H5D_get_create_plist failure paths.
    {
      ScopedFile rf(H5Fopen(tmp.c_str(), H5F_ACC_RDONLY, latest_fapl));
      ASSERT_TRUE(rf.valid());
      ScopedDataset d_opq(H5Dopen2(rf, "opq", H5P_DEFAULT));
      ScopedDataset d_vl(H5Dopen2(rf, "cmpd_vl", H5P_DEFAULT));
      ASSERT_TRUE(d_opq.valid());
      ASSERT_TRUE(d_vl.valid());
      H5D_t* raw_opq = UnwrapDataset(d_opq);
      H5D_t* raw_vl = UnwrapDataset(d_vl);
      ASSERT_NE(raw_opq, nullptr);
      ASSERT_NE(raw_vl, nullptr);

      ScopedErrorSilence silence;

      // 4044: H5D__refresh with invalid ID on RDONLY dataset fails
      // H5O_refresh_metadata.
      {
        ScopedApiContext ctx;
        EXPECT_LT(H5D__refresh(raw_opq, H5I_INVALID_HID), 0);
      }

      // 3453 & 3527: layout.type == 99 hits default: in H5D__format_convert and
      // H5D__mark.
      {
        const H5D_layout_t saved_ltype = raw_opq->shared->layout.type;
        raw_opq->shared->layout.type = static_cast<H5D_layout_t>(99);
        absl::Cleanup restore = [&] {
          raw_opq->shared->layout.type = saved_ltype;
        };
        ScopedApiContext ctx;
        EXPECT_LT(H5D__format_convert(raw_opq), 0);
        EXPECT_LT(H5D__mark(raw_opq, H5D_MARK_LAYOUT), 0);
      }

      // 3646-3647: H5D_flush_layout_to_dcpl failure inside H5D_get_create_plist.
      {
        ScopedPlist no_layout_dcpl(H5Pcreate(H5P_DATASET_CREATE));
        ASSERT_TRUE(no_layout_dcpl.valid());
        ASSERT_GE(H5Premove(no_layout_dcpl, H5D_CRT_LAYOUT_NAME), 0);
        const hid_t saved_dcpl = raw_opq->shared->dcpl_id;
        const bool saved_copied = raw_opq->shared->layout_copied_to_dcpl;
        raw_opq->shared->dcpl_id = no_layout_dcpl;
        raw_opq->shared->layout_copied_to_dcpl = false;
        absl::Cleanup restore = [&] {
          raw_opq->shared->dcpl_id = saved_dcpl;
          raw_opq->shared->layout_copied_to_dcpl = saved_copied;
        };
        ScopedApiContext ctx;
        EXPECT_LT(H5D_get_create_plist(raw_opq), 0);
      }

      // 3657: H5O_get_create_plist failure when attr max compact is stripped on
      // LATEST OH.
      {
        ScopedPlist no_attr_dcpl(H5Pcreate(H5P_DATASET_CREATE));
        ASSERT_TRUE(no_attr_dcpl.valid());
        ASSERT_GE(H5Premove(no_attr_dcpl, H5O_CRT_ATTR_MAX_COMPACT_NAME), 0);
        const hid_t saved_dcpl = raw_opq->shared->dcpl_id;
        const bool saved_copied = raw_opq->shared->layout_copied_to_dcpl;
        raw_opq->shared->dcpl_id = no_attr_dcpl;
        raw_opq->shared->layout_copied_to_dcpl = true;
        absl::Cleanup restore = [&] {
          raw_opq->shared->dcpl_id = saved_dcpl;
          raw_opq->shared->layout_copied_to_dcpl = saved_copied;
        };
        ScopedApiContext ctx;
        EXPECT_LT(H5D_get_create_plist(raw_opq), 0);
      }

      // 3651: User DCPL subclass whose copy callback fails after
      // H5P_MOD_INC_LST fails H5P_copy_plist cleanly in H5D_get_create_plist.
      {
        auto fail_copy_cb = [](hid_t /*new_id*/, hid_t /*old_id*/,
                               void* /*copy_data*/) -> herr_t { return -1; };
        hid_t fail_cls = H5Pcreate_class(
            H5P_DATASET_CREATE, "h5dint_fail_copy_dcpl", nullptr, nullptr,
            +fail_copy_cb, nullptr, nullptr, nullptr);
        ASSERT_GE(fail_cls, 0);
        absl::Cleanup close_fail_cls = [&] { H5Pclose_class(fail_cls); };
        ScopedPlist fail_copy_dcpl(H5Pcreate(fail_cls));
        ASSERT_TRUE(fail_copy_dcpl.valid());

        const hid_t saved_dcpl = raw_opq->shared->dcpl_id;
        const bool saved_copied = raw_opq->shared->layout_copied_to_dcpl;
        raw_opq->shared->dcpl_id = fail_copy_dcpl;
        raw_opq->shared->layout_copied_to_dcpl = true;
        absl::Cleanup restore = [&] {
          raw_opq->shared->dcpl_id = saved_dcpl;
          raw_opq->shared->layout_copied_to_dcpl = saved_copied;
        };
        ScopedApiContext ctx;
        EXPECT_EQ(H5D_get_create_plist(raw_opq), H5I_INVALID_HID);
      }

      // 3720 & 3789: force_conv = true on H5T_OPAQUE causes H5T_path_find to
      // fail and frees copied_fill.type in done:.
      {
        const bool saved_fc = raw_opq->shared->type->shared->force_conv;
        raw_opq->shared->type->shared->force_conv = true;
        absl::Cleanup restore = [&] {
          raw_opq->shared->type->shared->force_conv = saved_fc;
        };
        EXPECT_EQ(H5Dget_create_plist(d_opq), H5I_INVALID_HID);
      }

      // 3744-3747 & 3789: H5T_convert fails on corrupt on-disk VLEN compound
      // fill value before H5P_poke(FILL_VALUE), freeing bkg_buf &
      // copied_fill.type.
      EXPECT_EQ(H5Dget_create_plist(d_vl), H5I_INVALID_HID);
    }
  }

  // 3. H5D__get_space VDS unlimited extent update failure (3936).
  {
    ScopedFile f = CreateInMemoryFile("vds_get_space_err.h5");
    ASSERT_TRUE(f.valid());
    ScopedGroup grp(
        H5Gcreate2(f, "not_a_dset", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(grp.valid());

    ScopedSpace vds_space = MakeSimpleSpace({0}, {H5S_UNLIMITED});
    ScopedSpace vspace = MakeSimpleSpace({0}, {H5S_UNLIMITED});
    ScopedSpace src_space = MakeSimpleSpace({0}, {H5S_UNLIMITED});
    ASSERT_TRUE(vds_space.valid());
    ASSERT_TRUE(vspace.valid());
    ASSERT_TRUE(src_space.valid());
    constexpr hsize_t kStart = 0;
    constexpr hsize_t kStride = 1;
    constexpr hsize_t kCount = H5S_UNLIMITED;
    constexpr hsize_t kBlock = 1;
    ASSERT_GE(H5Sselect_hyperslab(vspace, H5S_SELECT_SET, &kStart, &kStride,
                                  &kCount, &kBlock),
              0);
    ASSERT_GE(H5Sselect_hyperslab(src_space, H5S_SELECT_SET, &kStart, &kStride,
                                  &kCount, &kBlock),
              0);
    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ScopedPlist dapl(H5Pcreate(H5P_DATASET_ACCESS));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_TRUE(dapl.valid());
    ASSERT_GE(H5Pset_virtual(dcpl, vspace, ".", "not_a_dset", src_space), 0);
    ASSERT_GE(H5Pset_virtual_view(dapl, H5D_VDS_FIRST_MISSING), 0);

    ScopedDataset vds(H5Dcreate2(f, "vds", H5T_NATIVE_INT32, vds_space,
                                 H5P_DEFAULT, dcpl, dapl));
    ASSERT_TRUE(vds.valid());
    ScopedErrorSilence silence;
    EXPECT_EQ(H5Dget_space(vds), H5I_INVALID_HID);
  }
}

}  // namespace
}  // namespace hdf5
