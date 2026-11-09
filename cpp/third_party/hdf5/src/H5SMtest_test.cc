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
// Tests internal shared object header message (SOHM) count helper
// H5SM__get_mesg_count_test (third_party/hdf5/src/H5SMtest.c) in Google3's
// default non-MPI Linux build (-DNDEBUG, zlib/deflate enabled).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5SMtest.c

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, which omits #ifndef NDEBUG fields from
// H5C_cache_entry_t (H5AC_info_t), the first member of H5SM_master_table_t.
// Define NDEBUG around hdf5_internal_prelude.h and internal headers so struct
// layouts match `:hdf5`.
#ifndef NDEBUG
#define H5SMTEST_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#ifdef try
#undef try
#endif
#define try hdf5_try_param
extern "C" {
#define H5SM_FRIEND
#define H5SM_TESTING
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fprivate.h"
#include "third_party/hdf5/src/H5Oprivate.h"
#include "third_party/hdf5/src/H5SMpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5SMTEST_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5SMTEST_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

// RAII guard for pushing/popping an HDF5 API context so FUNC_ENTER_PACKAGE_TAG
// inside H5SM__get_mesg_count_test can update H5AC metadata tags cleanly.
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

  bool valid() const { return pushed_; }

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

ScopedPlist MakeSingleIndexSohmFcpl(unsigned mesg_flags, unsigned min_size,
                                    unsigned max_list, unsigned min_btree) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  if (!fcpl.valid()) {
    return fcpl;
  }
  if (H5Pset_shared_mesg_nindexes(fcpl.get(), 1) < 0 ||
      H5Pset_shared_mesg_index(fcpl.get(), 0, mesg_flags, min_size) < 0 ||
      H5Pset_shared_mesg_phase_change(fcpl.get(), max_list, min_btree) < 0) {
    fcpl.reset();
  }
  return fcpl;
}

std::vector<uint8_t> SnapshotFileImage(hid_t file_id) {
  const ssize_t size = H5Fget_file_image(file_id, nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::vector<uint8_t> image(static_cast<size_t>(size));
  if (H5Fget_file_image(file_id, image.data(), image.size()) != size) {
    return {};
  }
  return image;
}

TEST(H5SMtestTest, ReturnsZeroWithoutErrorWhenSohmDisabled) {
  ScopedFile file = CreateInMemoryFile("sohm_disabled.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  EXPECT_FALSE(H5_addr_defined(H5F_get_sohm_addr(f)));

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  // When SOHM is disabled, H5SM__get_mesg_count_test returns 0 for any type_id
  // without validating whether the type is shareable.
  constexpr auto kTypeIds = std::to_array<unsigned>({
      H5O_SDSPACE_ID,
      H5O_DTYPE_ID,
      H5O_FILL_ID,
      H5O_FILL_NEW_ID,
      H5O_PLINE_ID,
      H5O_ATTR_ID,
      H5O_NULL_ID,
      H5O_LINK_ID,
      999U,
  });
  for (unsigned type_id : kTypeIds) {
    size_t count = 12345;
    EXPECT_GE(H5SM__get_mesg_count_test(f, type_id, &count), 0)
        << "type_id=" << type_id;
    EXPECT_EQ(count, 0U) << "type_id=" << type_id;
  }
}

TEST(H5SMtestTest, SohmEnabledEmptyFileRejectsInvalidAndUnconfiguredTypes) {
  ScopedPlist fcpl = MakeSingleIndexSohmFcpl(
      H5O_SHMESG_ATTR_FLAG, /*min_size=*/1, /*max_list=*/10, /*min_btree=*/5);
  ASSERT_TRUE(fcpl.valid());
  ScopedFile file =
      CreateInMemoryFile("sohm_empty.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);
  EXPECT_TRUE(H5_addr_defined(H5F_get_sohm_addr(f)));

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  // 1. Configured message type reports 0 on an empty SOHM index.
  size_t count = 999;
  EXPECT_GE(H5SM__get_mesg_count_test(f, H5O_ATTR_ID, &count), 0);
  EXPECT_EQ(count, 0U);

  // 2. Non-shareable message types fail inside H5SM__get_index.
  constexpr auto kNonShareableTypes = std::to_array<unsigned>({
      H5O_NULL_ID,
      H5O_LINK_ID,
      H5O_LAYOUT_ID,
      H5O_NAME_ID,
      999U,
  });
  {
    ScopedErrorSilence silence;
    for (unsigned type_id : kNonShareableTypes) {
      size_t sentinel = 777;
      EXPECT_LT(H5SM__get_mesg_count_test(f, type_id, &sentinel), 0)
          << "type_id=" << type_id;
      EXPECT_EQ(sentinel, 777U);
    }
  }

  // 3. Valid shareable message types not present in any configured index fail
  // with H5E_NOTFOUND (index_num < 0).
  constexpr auto kUnconfiguredShareableTypes = std::to_array<unsigned>({
      H5O_SDSPACE_ID,
      H5O_DTYPE_ID,
      H5O_FILL_ID,
      H5O_FILL_NEW_ID,
      H5O_PLINE_ID,
  });
  {
    ScopedErrorSilence silence;
    for (unsigned type_id : kUnconfiguredShareableTypes) {
      size_t sentinel = 888;
      EXPECT_LT(H5SM__get_mesg_count_test(f, type_id, &sentinel), 0)
          << "type_id=" << type_id;
      EXPECT_EQ(sentinel, 888U);
    }
  }
}

TEST(H5SMtestTest, TracksDistinctSharedAndDeletedAttributesInListIndex) {
  ScopedPlist fcpl = MakeSingleIndexSohmFcpl(
      H5O_SHMESG_ATTR_FLAG, /*min_size=*/1, /*max_list=*/10, /*min_btree=*/5);
  ASSERT_TRUE(fcpl.valid());
  ScopedFile file =
      CreateInMemoryFile("sohm_attr_list.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedGroup g1(
      H5Gcreate2(file.get(), "/g1", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup g2(
      H5Gcreate2(file.get(), "/g2", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ScopedGroup g3(
      H5Gcreate2(file.get(), "/g3", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(g1.valid());
  ASSERT_TRUE(g2.valid());
  ASSERT_TRUE(g3.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  // 1. Writing the first shareable attribute adds one entry to the SOHM index.
  constexpr int32_t kFirstVal = 42;
  {
    ScopedAttr a1(H5Acreate2(g1.get(), "tag", H5T_NATIVE_INT32, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a1.valid());
    ASSERT_GE(H5Awrite(a1.get(), H5T_NATIVE_INT32, &kFirstVal), 0);
  }
  size_t count = 0;
  ASSERT_GE(H5SM__get_mesg_count_test(f, H5O_ATTR_ID, &count), 0);
  EXPECT_EQ(count, 1U);

  // 2. Writing an identical attribute on /g2 shares the existing entry without
  // increasing the tracked message count.
  {
    ScopedAttr a2(H5Acreate2(g2.get(), "tag", H5T_NATIVE_INT32, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a2.valid());
    ASSERT_GE(H5Awrite(a2.get(), H5T_NATIVE_INT32, &kFirstVal), 0);
  }
  ASSERT_GE(H5SM__get_mesg_count_test(f, H5O_ATTR_ID, &count), 0);
  EXPECT_EQ(count, 1U);

  // 3. Writing a distinct attribute on /g3 adds a second tracked message.
  constexpr int32_t kSecondVal = 99;
  {
    ScopedAttr a3(H5Acreate2(g3.get(), "other", H5T_NATIVE_INT32, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a3.valid());
    ASSERT_GE(H5Awrite(a3.get(), H5T_NATIVE_INT32, &kSecondVal), 0);
  }
  ASSERT_GE(H5SM__get_mesg_count_test(f, H5O_ATTR_ID, &count), 0);
  EXPECT_EQ(count, 2U);

  // 4. Deleting the unique attribute decrements count back to 1; deleting only
  // one copy of the shared attribute keeps count at 1 until both are deleted.
  ASSERT_GE(H5Adelete(g3.get(), "other"), 0);
  ASSERT_GE(H5SM__get_mesg_count_test(f, H5O_ATTR_ID, &count), 0);
  EXPECT_EQ(count, 1U);

  ASSERT_GE(H5Adelete(g1.get(), "tag"), 0);
  ASSERT_GE(H5SM__get_mesg_count_test(f, H5O_ATTR_ID, &count), 0);
  EXPECT_EQ(count, 1U);

  ASSERT_GE(H5Adelete(g2.get(), "tag"), 0);
  ASSERT_GE(H5SM__get_mesg_count_test(f, H5O_ATTR_ID, &count), 0);
  EXPECT_EQ(count, 0U);
}

TEST(H5SMtestTest, TracksMultipleIndexesPhaseChangesAndReopenedImage) {
  // Configure three independent SOHM indexes with a small list-to-btree cutoff
  // (max_list=2, min_btree=1) so index 1 converts to a B-tree once 3 distinct
  // messages are stored.
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 3), 0);
  ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_ATTR_FLAG, 1),
            0);
  ASSERT_GE(
      H5Pset_shared_mesg_index(
          fcpl.get(), 1, H5O_SHMESG_DTYPE_FLAG | H5O_SHMESG_SDSPACE_FLAG, 1),
      0);
  ASSERT_GE(H5Pset_shared_mesg_index(
                fcpl.get(), 2, H5O_SHMESG_FILL_FLAG | H5O_SHMESG_PLINE_FLAG, 1),
            0);
  ASSERT_GE(H5Pset_shared_mesg_phase_change(fcpl.get(), /*max_list=*/2,
                                            /*min_btree=*/1),
            0);

  std::vector<uint8_t> image;
  size_t expected_idx1_count = 0;
  size_t expected_idx2_count = 0;

  {
    ScopedFile file =
        CreateInMemoryFile("sohm_multi_index.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    // Build 3 datasets with distinct 1D dataspaces, distinct compound types,
    // explicit fill values, and deflate filter pipeline on each dataset.
    struct Pair {
      int64_t a;
      double b;
    };
    static_assert(sizeof(Pair) == sizeof(int64_t) + sizeof(double));
    ScopedType cmpd1(H5Tcreate(H5T_COMPOUND, sizeof(Pair)));
    ASSERT_TRUE(cmpd1.valid());
    ASSERT_GE(H5Tinsert(cmpd1.get(), "a", HOFFSET(Pair, a), H5T_NATIVE_INT64),
              0);
    ASSERT_GE(H5Tinsert(cmpd1.get(), "b", HOFFSET(Pair, b), H5T_NATIVE_DOUBLE),
              0);

    ScopedType cmpd2(H5Tcreate(H5T_COMPOUND, sizeof(Pair)));
    ASSERT_TRUE(cmpd2.valid());
    ASSERT_GE(H5Tinsert(cmpd2.get(), "x", HOFFSET(Pair, a), H5T_NATIVE_INT64),
              0);
    ASSERT_GE(H5Tinsert(cmpd2.get(), "y", HOFFSET(Pair, b), H5T_NATIVE_DOUBLE),
              0);

    constexpr std::array<hsize_t, 3> kDims = {4, 8, 12};
    for (size_t i = 0; i < kDims.size(); ++i) {
      const hsize_t dim = kDims[i];
      const hsize_t chunk_dim = 4;
      ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
      ASSERT_TRUE(space.valid());
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl.valid());
      ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, &chunk_dim), 0);
      ASSERT_GE(H5Pset_deflate(dcpl.get(), 4), 0);
      Pair fill_val;
      std::memset(&fill_val, 0, sizeof(fill_val));
      fill_val.a = static_cast<int64_t>(i + 1);
      fill_val.b = 1.5;
      const hid_t dtype = (i % 2 == 0) ? cmpd1.get() : cmpd2.get();
      ASSERT_GE(H5Pset_fill_value(dcpl.get(), dtype, &fill_val), 0);

      const std::string name = "/ds_" + std::to_string(i);
      ScopedDataset dset(H5Dcreate2(file.get(), name.c_str(), dtype,
                                    space.get(), H5P_DEFAULT, dcpl.get(),
                                    H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());
    }

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    // 1. Index 0 has no attributes yet.
    size_t attr_count = 99;
    ASSERT_GE(H5SM__get_mesg_count_test(f, H5O_ATTR_ID, &attr_count), 0);
    EXPECT_EQ(attr_count, 0U);

    // 2. Index 1 combines DTYPE and SDSPACE messages (exceeding max_list=2 so
    // index 1 converted to B-tree).
    size_t dtype_count = 0;
    size_t sdspace_count = 0;
    ASSERT_GE(H5SM__get_mesg_count_test(f, H5O_DTYPE_ID, &dtype_count), 0);
    ASSERT_GE(H5SM__get_mesg_count_test(f, H5O_SDSPACE_ID, &sdspace_count), 0);
    EXPECT_EQ(dtype_count, sdspace_count);
    EXPECT_GE(dtype_count, 3U);
    expected_idx1_count = dtype_count;

    // 3. Index 2 combines FILL and PLINE messages; legacy H5O_FILL_ID aliases
    // to the same index as H5O_FILL_NEW_ID and H5O_PLINE_ID.
    size_t fill_old_count = 0;
    size_t fill_new_count = 0;
    size_t pline_count = 0;
    ASSERT_GE(H5SM__get_mesg_count_test(f, H5O_FILL_ID, &fill_old_count), 0);
    ASSERT_GE(H5SM__get_mesg_count_test(f, H5O_FILL_NEW_ID, &fill_new_count),
              0);
    ASSERT_GE(H5SM__get_mesg_count_test(f, H5O_PLINE_ID, &pline_count), 0);
    EXPECT_EQ(fill_old_count, fill_new_count);
    EXPECT_EQ(fill_new_count, pline_count);
    EXPECT_GE(pline_count, 3U);
    expected_idx2_count = pline_count;

    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);
    image = SnapshotFileImage(file.get());
    ASSERT_FALSE(image.empty());
  }

  // 4. Reopen the serialized image read-only and verify counts match persisted
  // SOHM master table headers.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_file_image(fapl.get(), image.data(), image.size()), 0);
  ScopedFile reopened(H5Fopen("sohm_reopen.h5", H5F_ACC_RDONLY, fapl.get()));
  ASSERT_TRUE(reopened.valid());
  H5F_t* rf = UnwrapFile(reopened.get());
  ASSERT_NE(rf, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  size_t count = 99;
  ASSERT_GE(H5SM__get_mesg_count_test(rf, H5O_ATTR_ID, &count), 0);
  EXPECT_EQ(count, 0U);
  ASSERT_GE(H5SM__get_mesg_count_test(rf, H5O_DTYPE_ID, &count), 0);
  EXPECT_EQ(count, expected_idx1_count);
  ASSERT_GE(H5SM__get_mesg_count_test(rf, H5O_FILL_ID, &count), 0);
  EXPECT_EQ(count, expected_idx2_count);
}

TEST(H5SMtestTest, BtreeOnlyAndPhaseDownTransitionsReportAccurateCounts) {
  // 1. Direct B-tree SOHM index (max_list=0, min_btree=0).
  {
    ScopedPlist fcpl = MakeSingleIndexSohmFcpl(
        H5O_SHMESG_ATTR_FLAG, /*min_size=*/1, /*max_list=*/0, /*min_btree=*/0);
    ASSERT_TRUE(fcpl.valid());
    ScopedFile file =
        CreateInMemoryFile("sohm_btree_only.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    for (int32_t i = 0; i < 3; ++i) {
      const std::string name = "attr_" + std::to_string(i);
      ScopedAttr attr(H5Acreate2(file.get(), name.c_str(), H5T_NATIVE_INT32,
                                 scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(attr.valid());
      ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT32, &i), 0);
    }

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    size_t count = 0;
    ASSERT_GE(H5SM__get_mesg_count_test(f, H5O_ATTR_ID, &count), 0);
    EXPECT_EQ(count, 3U);

    ASSERT_GE(H5Adelete(file.get(), "attr_1"), 0);
    ASSERT_GE(H5SM__get_mesg_count_test(f, H5O_ATTR_ID, &count), 0);
    EXPECT_EQ(count, 2U);
  }

  // 2. Phase transition list -> B-tree (> max_list=2) -> list (< min_btree=2).
  {
    ScopedPlist fcpl = MakeSingleIndexSohmFcpl(
        H5O_SHMESG_ATTR_FLAG, /*min_size=*/1, /*max_list=*/2, /*min_btree=*/2);
    ASSERT_TRUE(fcpl.valid());
    ScopedFile file =
        CreateInMemoryFile("sohm_phase_down.h5", H5F_ACC_TRUNC, fcpl.get());
    ASSERT_TRUE(file.valid());
    H5F_t* f = UnwrapFile(file.get());
    ASSERT_NE(f, nullptr);

    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());
    for (int32_t i = 0; i < 4; ++i) {
      const std::string name = "p_" + std::to_string(i);
      ScopedAttr attr(H5Acreate2(file.get(), name.c_str(), H5T_NATIVE_INT32,
                                 scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(attr.valid());
      ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT32, &i), 0);
    }

    ScopedApiContext api_ctx;
    ASSERT_TRUE(api_ctx.valid());

    size_t count = 0;
    ASSERT_GE(H5SM__get_mesg_count_test(f, H5O_ATTR_ID, &count), 0);
    EXPECT_EQ(count, 4U);

    ASSERT_GE(H5Adelete(file.get(), "p_0"), 0);
    ASSERT_GE(H5Adelete(file.get(), "p_1"), 0);
    ASSERT_GE(H5Adelete(file.get(), "p_2"), 0);
    ASSERT_GE(H5SM__get_mesg_count_test(f, H5O_ATTR_ID, &count), 0);
    EXPECT_EQ(count, 1U);
  }
}

TEST(H5SMtestTest, FailsWhenSohmMasterTableProtectFails) {
  ScopedPlist fcpl = MakeSingleIndexSohmFcpl(
      H5O_SHMESG_ATTR_FLAG, /*min_size=*/1, /*max_list=*/10, /*min_btree=*/5);
  ASSERT_TRUE(fcpl.valid());
  ScopedFile file =
      CreateInMemoryFile("sohm_bad_addr.h5", H5F_ACC_TRUNC, fcpl.get());
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedApiContext api_ctx;
  ASSERT_TRUE(api_ctx.valid());

  // Temporarily point sohm_addr beyond EOA so H5AC_protect fails on load/read
  // instead of hitting the cached table; restore unconditionally before close.
  const haddr_t orig_sohm_addr = H5F_get_sohm_addr(f);
  ASSERT_TRUE(H5_addr_defined(orig_sohm_addr));
  absl::Cleanup restore_sohm_addr = [f, orig_sohm_addr] {
    H5F_set_sohm_addr(f, orig_sohm_addr);
  };

  const haddr_t eoa = H5F_get_eoa(f, H5FD_MEM_SOHM_TABLE);
  ASSERT_TRUE(H5_addr_defined(eoa));
  ASSERT_GE(H5F_set_sohm_addr(f, eoa + 4096), 0);

  ScopedErrorSilence silence;
  size_t count = 555;
  EXPECT_LT(H5SM__get_mesg_count_test(f, H5O_ATTR_ID, &count), 0);
  EXPECT_EQ(count, 555U);
}

}  // namespace
}  // namespace hdf5
