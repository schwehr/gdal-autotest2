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
// Tests object header attribute operations: creation, compact/dense phase
// transitions, SOHM-shared attribute creation/updates/deletes, already-opened
// attribute sharing, cache-evicted writes, renames, iteration, deletion,
// attribute counts, existence checks, and B-tree/heap metadata accounting
// (third_party/hdf5/src/H5Oattribute.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Oattribute.c

#include <cstdint>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// Match `:hdf5` NDEBUG layout when using H5O/H5A/H5F internal interfaces.
#ifndef NDEBUG
#define H5OATTRIBUTE_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// Work around `bool try` parameter names in H5B2private.h / H5Fpkg.h.
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
extern "C" {
#define H5A_FRIEND
#define H5F_FRIEND
#define H5O_FRIEND
#define H5O_TESTING
#include "third_party/hdf5/src/H5Apkg.h"
#include "third_party/hdf5/src/H5Aprivate.h"
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Fpkg.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#ifdef H5OATTRIBUTE_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OATTRIBUTE_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

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

 private:
  H5CX_node_t node_{};
  haddr_t prev_tag_ = HADDR_UNDEF;
  bool pushed_ = false;
  bool tagged_ = false;
};

ScopedFile CreateCoreFileWithLibver(const char* name, H5F_libver_t low,
                                    H5F_libver_t high,
                                    hid_t fcpl = H5P_DEFAULT,
                                    uint64_t rfic_flags = 0) {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) return ScopedFile();
  if (H5Pset_libver_bounds(fapl.get(), low, high) < 0) return ScopedFile();
  if (rfic_flags != 0 &&
      H5Pset_relax_file_integrity_checks(fapl.get(), rfic_flags) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fcreate(name, H5F_ACC_TRUNC, fcpl, fapl.get()));
}

ScopedDataset CreateScalarIntDataset(hid_t loc, const char* name,
                                     hid_t dcpl = H5P_DEFAULT) {
  ScopedSpace space(H5Screate(H5S_SCALAR));
  if (!space.valid()) return ScopedDataset();
  return ScopedDataset(H5Dcreate2(loc, name, H5T_NATIVE_INT, space.get(),
                                  H5P_DEFAULT, dcpl, H5P_DEFAULT));
}

struct IterRecord {
  std::vector<std::string> names;
  int stop_after = -1;
  int error_after = -1;
};

herr_t CollectAttrNames(hid_t /*location_id*/, const char* attr_name,
                        const H5A_info_t* /*ainfo*/, void* op_data) {
  auto* rec = static_cast<IterRecord*>(op_data);
  if (rec->error_after >= 0 &&
      static_cast<int>(rec->names.size()) == rec->error_after) {
    return -1;
  }
  rec->names.emplace_back(attr_name);
  if (rec->stop_after > 0 &&
      static_cast<int>(rec->names.size()) >= rec->stop_after) {
    return 1;
  }
  return 0;
}

TEST(H5OattributeTest, RejectsUnusualNumericBitsOnV1UnlessRelaxedOrV2) {
  ScopedType unusual_dt(H5Tcopy(H5T_NATIVE_UINT32));
  ASSERT_TRUE(unusual_dt.valid());
  // 32-bit container with 8 bits precision and 0 offset triggers
  // H5T_is_numeric_with_unusual_unused_bits().
  ASSERT_GE(H5Tset_precision(unusual_dt.get(), 8), 0);

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  {
    ScopedFile v1_strict = CreateCoreFileWithLibver(
        "v1_strict.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
    ASSERT_TRUE(v1_strict.valid());
    ScopedErrorSilence silence;
    ScopedAttr bad_attr(H5Acreate2(v1_strict.get(), "unusual", unusual_dt.get(),
                                   scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    EXPECT_FALSE(bad_attr.valid());
  }

  {
    ScopedFile v1_relaxed = CreateCoreFileWithLibver(
        "v1_relaxed.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST,
        H5P_DEFAULT, H5F_RFIC_UNUSUAL_NUM_UNUSED_NUMERIC_BITS);
    ASSERT_TRUE(v1_relaxed.valid());
    ScopedAttr ok_attr(H5Acreate2(v1_relaxed.get(), "unusual", unusual_dt.get(),
                                  scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    EXPECT_TRUE(ok_attr.valid());
  }

  {
    // v2 object headers have checksums, so creation succeeds without RFIC.
    ScopedFile v2_file = CreateCoreFileWithLibver(
        "v2_unusual.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(v2_file.valid());
    ScopedAttr ok_attr(H5Acreate2(v2_file.get(), "unusual", unusual_dt.get(),
                                  scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    EXPECT_TRUE(ok_attr.valid());
  }
}

TEST(H5OattributeTest, CompactToDenseByCountCreationOrderBhInfoAndCount) {
  ScopedFile file = CreateCoreFileWithLibver(
      "dense_phase.h5", H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());

  ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
  ASSERT_TRUE(gcpl.valid());
  ASSERT_GE(
      H5Pset_attr_phase_change(gcpl.get(), /*max_compact=*/3, /*min_dense=*/2),
      0);
  ASSERT_GE(H5Pset_attr_creation_order(
                gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
            0);

  ScopedGroup grp(H5Gcreate2(file.get(), "g", H5P_DEFAULT, gcpl.get(),
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  // 0-attribute object header checks H5O__attr_count_real (!ainfo_exists),
  // H5O__attr_exists, and H5O__attr_bh_info.
  H5O_info2_t oinfo{};
  ASSERT_GE(H5Oget_info3(grp.get(), &oinfo, H5O_INFO_NUM_ATTRS), 0);
  EXPECT_EQ(oinfo.num_attrs, 0u);
  EXPECT_EQ(H5Aexists(grp.get(), "missing"), 0);
  H5O_native_info_t native_info{};
  ASSERT_GE(H5Oget_native_info(grp.get(), &native_info,
                               H5O_NATIVE_INFO_META_SIZE),
            0);
  EXPECT_EQ(native_info.meta_size.attr.index_size, 0u);
  EXPECT_EQ(native_info.meta_size.attr.heap_size, 0u);

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  // Add 3 compact attributes.
  for (int i = 0; i < 3; ++i) {
    const std::string name = absl::StrCat("a_", 2 - i);
    ScopedAttr a(H5Acreate2(grp.get(), name.c_str(), H5T_NATIVE_INT,
                            scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a.valid());
    ASSERT_GE(H5Awrite(a.get(), H5T_NATIVE_INT, &i), 0);
  }
  EXPECT_EQ(H5O__is_attr_dense_test(grp.get()), 0);
  EXPECT_GT(H5Aexists(grp.get(), "a_1"), 0);
  EXPECT_EQ(H5Aexists(grp.get(), "absent"), 0);
  ASSERT_GE(H5Oget_info3(grp.get(), &oinfo, H5O_INFO_NUM_ATTRS), 0);
  EXPECT_EQ(oinfo.num_attrs, 3u);
  ASSERT_GE(H5Oget_native_info(grp.get(), &native_info,
                               H5O_NATIVE_INFO_META_SIZE),
            0);
  EXPECT_EQ(native_info.meta_size.attr.index_size, 0u);
  EXPECT_EQ(native_info.meta_size.attr.heap_size, 0u);

  // Adding a 4th triggers compact -> dense conversion via H5O__attr_to_dense_cb.
  {
    ScopedAttr a3(H5Acreate2(grp.get(), "a_3", H5T_NATIVE_INT, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a3.valid());
    int val = 3;
    ASSERT_GE(H5Awrite(a3.get(), H5T_NATIVE_INT, &val), 0);
  }
  EXPECT_GT(H5O__is_attr_dense_test(grp.get()), 0);
  EXPECT_GT(H5Aexists(grp.get(), "a_0"), 0);
  EXPECT_EQ(H5Aexists(grp.get(), "absent"), 0);
  ASSERT_GE(H5Oget_info3(grp.get(), &oinfo, H5O_INFO_NUM_ATTRS), 0);
  EXPECT_EQ(oinfo.num_attrs, 4u);

  ASSERT_GE(H5Oget_native_info(grp.get(), &native_info,
                               H5O_NATIVE_INFO_META_SIZE),
            0);
  EXPECT_GT(native_info.meta_size.attr.index_size, 0u);
  EXPECT_GT(native_info.meta_size.attr.heap_size, 0u);

  // Verify creation-order iteration and name iteration on dense storage.
  IterRecord corder_rec;
  hsize_t idx = 0;
  ASSERT_GE(H5Aiterate2(grp.get(), H5_INDEX_CRT_ORDER, H5_ITER_INC, &idx,
                        CollectAttrNames, &corder_rec),
            0);
  EXPECT_THAT(corder_rec.names, ElementsAre("a_2", "a_1", "a_0", "a_3"));

  // Dense write + rename + missing-rename error.
  {
    ScopedAttr dense_a(H5Aopen(grp.get(), "a_1", H5P_DEFAULT));
    ASSERT_TRUE(dense_a.valid());
    int updated = 99;
    ASSERT_GE(H5Awrite(dense_a.get(), H5T_NATIVE_INT, &updated), 0);
    int check = 0;
    ASSERT_GE(H5Aread(dense_a.get(), H5T_NATIVE_INT, &check), 0);
    EXPECT_EQ(check, 99);
  }
  ASSERT_GE(H5Arename(grp.get(), "a_1", "a_1_renamed"), 0);
  EXPECT_GT(H5Aexists(grp.get(), "a_1_renamed"), 0);
  EXPECT_EQ(H5Aexists(grp.get(), "a_1"), 0);

  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Arename(grp.get(), "no_such_attr", "x"), 0);
    EXPECT_LT(H5Arename(grp.get(), "a_0", "a_1_renamed"), 0);
    ScopedAttr missing_open(H5Aopen(grp.get(), "no_such_attr", H5P_DEFAULT));
    EXPECT_FALSE(missing_open.valid());
    EXPECT_LT(H5Adelete(grp.get(), "no_such_attr"), 0);
  }
}

TEST(H5OattributeTest, OversizedAttributeForcesDenseAndBlocksRevert) {
  ScopedFile file = CreateCoreFileWithLibver(
      "oversized_attr.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(
      H5Pset_attr_phase_change(dcpl.get(), /*max_compact=*/5, /*min_dense=*/3),
      0);

  ScopedDataset dset = CreateScalarIntDataset(file.get(), "d", dcpl.get());
  ASSERT_TRUE(dset.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  {
    ScopedAttr s1(H5Acreate2(dset.get(), "small1", H5T_NATIVE_INT, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(s1.valid());
    ScopedAttr s2(H5Acreate2(dset.get(), "small2", H5T_NATIVE_INT, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(s2.valid());
  }
  EXPECT_EQ(H5O__is_attr_dense_test(dset.get()), 0);

  // Attribute >= H5O_MESG_MAX_SIZE (65536) forces dense transition even below
  // max_compact.
  const hsize_t big_dims[1] = {70000};
  ScopedSpace big_space(H5Screate_simple(1, big_dims, nullptr));
  ASSERT_TRUE(big_space.valid());
  std::vector<uint8_t> big_data(70000, 0x5a);
  {
    ScopedAttr big_a(H5Acreate2(dset.get(), "big", H5T_NATIVE_UINT8,
                                big_space.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(big_a.valid());
    ASSERT_GE(H5Awrite(big_a.get(), H5T_NATIVE_UINT8, big_data.data()), 0);
  }
  EXPECT_GT(H5O__is_attr_dense_test(dset.get()), 0);

  // Deleting small attrs drops nattrs below min_dense (3), but the >64KB
  // attribute prevents converting dense -> compact.
  ASSERT_GE(H5Adelete(dset.get(), "small1"), 0);
  EXPECT_GT(H5O__is_attr_dense_test(dset.get()), 0);
  ASSERT_GE(H5Adelete(dset.get(), "small2"), 0);
  EXPECT_GT(H5O__is_attr_dense_test(dset.get()), 0);

  // Deleting the large attribute leaves nattrs == 0 and removes AINFO.
  ASSERT_GE(H5Adelete(dset.get(), "big"), 0);
  EXPECT_EQ(H5O__is_attr_dense_test(dset.get()), 0);
  H5O_info2_t oinfo{};
  ASSERT_GE(H5Oget_info3(dset.get(), &oinfo, H5O_INFO_NUM_ATTRS), 0);
  EXPECT_EQ(oinfo.num_attrs, 0u);
}

TEST(H5OattributeTest, OpenAlreadyOpenedFindOpenedAcrossObjectsAndFiles) {
  ScopedFile file1 = CreateCoreFileWithLibver(
      "open_attr_1.h5", H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
  ScopedFile file2 = CreateCoreFileWithLibver(
      "open_attr_2.h5", H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file1.valid());
  ASSERT_TRUE(file2.valid());

  ScopedDataset d1 = CreateScalarIntDataset(file1.get(), "d1");
  ScopedDataset d2 = CreateScalarIntDataset(file1.get(), "d2");
  ScopedDataset d_other = CreateScalarIntDataset(file2.get(), "d_other");
  ASSERT_TRUE(d1.valid());
  ASSERT_TRUE(d2.valid());
  ASSERT_TRUE(d_other.valid());

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  // Open distracting attributes across name/addr/file boundaries so
  // H5O__attr_find_opened_attr iterates over mismatches before matching.
  ScopedAttr a_other_name(H5Acreate2(d1.get(), "other", H5T_NATIVE_INT,
                                     scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
  ScopedAttr a_other_obj(H5Acreate2(d2.get(), "target", H5T_NATIVE_INT,
                                    scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
  ScopedAttr a_other_file(H5Acreate2(d_other.get(), "target", H5T_NATIVE_INT,
                                     scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
  ScopedAttr a_target(H5Acreate2(d1.get(), "target", H5T_NATIVE_INT,
                                 scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(a_other_name.valid());
  ASSERT_TRUE(a_other_obj.valid());
  ASSERT_TRUE(a_other_file.valid());
  ASSERT_TRUE(a_target.valid());

  int initial = 17;
  ASSERT_GE(H5Awrite(a_target.get(), H5T_NATIVE_INT, &initial), 0);

  // Re-open by name while held open -> shares internal H5A_shared_t.
  ScopedAttr dup_by_name(H5Aopen(d1.get(), "target", H5P_DEFAULT));
  ASSERT_TRUE(dup_by_name.valid());
  int updated = 55;
  ASSERT_GE(H5Awrite(dup_by_name.get(), H5T_NATIVE_INT, &updated), 0);

  // Re-open by index while held open -> hits H5O__attr_open_by_idx already-open
  // branch as well. ("other" is index 0, "target" is index 1 in H5_INDEX_NAME.)
  ScopedAttr dup_by_idx(H5Aopen_by_idx(d1.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                       1, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dup_by_idx.valid());
  int read_val = 0;
  ASSERT_GE(H5Aread(dup_by_idx.get(), H5T_NATIVE_INT, &read_val), 0);
  EXPECT_EQ(read_val, 55);
}

TEST(H5OattributeTest, WriteAfterObjectHeaderCacheEvictionUpdatesDiskSharedCopy) {
  ScopedFile file = CreateCoreFileWithLibver(
      "evict_write.h5", H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());

  ScopedDataset dset = CreateScalarIntDataset(file.get(), "d");
  ASSERT_TRUE(dset.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  ScopedAttr attr(H5Acreate2(dset.get(), "evict_me", H5T_NATIVE_INT,
                             scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  int v1 = 111;
  ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT, &v1), 0);
  ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_GLOBAL), 0);

  // Expunge the object header from cache while keeping `attr` open. When the
  // object header reloads on H5Awrite, mesg->native->shared != attr->shared.
  {
    ScopedApiContext ctx;
    H5O_loc_t* oloc = H5O_get_loc(dset.get());
    ASSERT_NE(oloc, nullptr);
    ASSERT_GE(H5O__expunge_chunks_test(oloc), 0);
  }

  int v2 = 222;
  ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT, &v2), 0);
  attr.reset();

  ScopedAttr reopened(H5Aopen(dset.get(), "evict_me", H5P_DEFAULT));
  ASSERT_TRUE(reopened.valid());
  int read_back = 0;
  ASSERT_GE(H5Aread(reopened.get(), H5T_NATIVE_INT, &read_back), 0);
  EXPECT_EQ(read_back, 222);
}

TEST(H5OattributeTest, SharedMessageAttributesCreateWriteRenameAndRevert) {
  ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
  ASSERT_TRUE(fcpl.valid());
  ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
  ASSERT_GE(H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_ATTR_FLAG, 10),
            0);

  ScopedFile file = CreateCoreFileWithLibver(
      "sohm_attrs.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST, fcpl.get());
  ASSERT_TRUE(file.valid());

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(
      H5Pset_attr_phase_change(dcpl.get(), /*max_compact=*/3, /*min_dense=*/2),
      0);

  ScopedDataset d1 = CreateScalarIntDataset(file.get(), "d1", dcpl.get());
  ScopedDataset d2 = CreateScalarIntDataset(file.get(), "d2", dcpl.get());
  ASSERT_TRUE(d1.valid());
  ASSERT_TRUE(d2.valid());

  const hsize_t dims[1] = {16};
  ScopedSpace space(H5Screate_simple(1, dims, nullptr));
  ASSERT_TRUE(space.valid());

  // Creating two identical SOHM attributes hits attr_rc > 1 -> H5O__attr_delete.
  {
    ScopedAttr a1(H5Acreate2(d1.get(), "shared_a", H5T_NATIVE_INT, space.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a1.valid());
    ScopedAttr a2(H5Acreate2(d2.get(), "shared_a", H5T_NATIVE_INT, space.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a2.valid());
    ScopedAttr a3(H5Acreate2(d2.get(), "shared_b", H5T_NATIVE_INT, space.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a3.valid());

    // First write creates a new SOHM entry (attr_rc == 1 -> H5O__attr_link).
    std::vector<int> data1(16, 42);
    ASSERT_GE(H5Awrite(a1.get(), H5T_NATIVE_INT, data1.data()), 0);
    // Matching write on d2's attribute shares the updated SOHM entry
    // (attr_rc > 1 branch inside H5O__attr_update_shared).
    ASSERT_GE(H5Awrite(a2.get(), H5T_NATIVE_INT, data1.data()), 0);
  }

  // Rename a shared compact attribute -> exercises H5O__attr_rename_mod_cb
  // H5O_MSG_FLAG_SHARED path.
  ASSERT_GE(H5Arename(d1.get(), "shared_a", "shared_a_ren"), 0);
  EXPECT_GT(H5Aexists(d1.get(), "shared_a_ren"), 0);

  // Add shared attributes on d1 to trigger compact -> dense and then revert
  // back to compact (exercising shared_mesg != 0 reset in remove_update).
  for (int i = 0; i < 3; ++i) {
    const std::string name = absl::StrCat("s_extra_", i);
    ScopedAttr extra(H5Acreate2(d1.get(), name.c_str(), H5T_NATIVE_INT,
                                space.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(extra.valid());
    std::vector<int> vals(16, 42);
    ASSERT_GE(H5Awrite(extra.get(), H5T_NATIVE_INT, vals.data()), 0);
  }
  EXPECT_GT(H5O__is_attr_dense_test(d1.get()), 0);

  ASSERT_GE(H5Adelete(d1.get(), "s_extra_0"), 0);
  ASSERT_GE(H5Adelete(d1.get(), "s_extra_1"), 0);
  ASSERT_GE(H5Adelete(d1.get(), "s_extra_2"), 0);
  // Now nattrs == 1 < min_dense (2) -> reverted to compact!
  EXPECT_EQ(H5O__is_attr_dense_test(d1.get()), 0);

  std::vector<int> verify(16, 0);
  ScopedAttr reopened(H5Aopen(d1.get(), "shared_a_ren", H5P_DEFAULT));
  ASSERT_TRUE(reopened.valid());
  ASSERT_GE(H5Aread(reopened.get(), H5T_NATIVE_INT, verify.data()), 0);
  EXPECT_EQ(verify[0], 42);
}

TEST(H5OattributeTest, DenseRevertWithOpenAndCommittedDatatypeAttributes) {
  ScopedFile file = CreateCoreFileWithLibver(
      "revert_open.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
  ASSERT_TRUE(file.valid());

  // Committed named datatype ensures H5O__attr_link has a shared component to
  // increment when reverting dense -> compact for an unshared attribute.
  ScopedType ctype(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(ctype.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "my_int", ctype.get(), H5P_DEFAULT,
                       H5P_DEFAULT, H5P_DEFAULT),
            0);

  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_TRUE(dcpl.valid());
  ASSERT_GE(
      H5Pset_attr_phase_change(dcpl.get(), /*max_compact=*/3, /*min_dense=*/3),
      0);

  ScopedDataset dset = CreateScalarIntDataset(file.get(), "d", dcpl.get());
  ASSERT_TRUE(dset.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  ScopedAttr a_open(H5Acreate2(dset.get(), "kept_open", ctype.get(),
                               scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(a_open.valid());
  int v_open = 7;
  ASSERT_GE(H5Awrite(a_open.get(), H5T_NATIVE_INT, &v_open), 0);

  {
    ScopedAttr a_closed(H5Acreate2(dset.get(), "kept_closed", ctype.get(),
                                   scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a_closed.valid());
    int v_closed = 8;
    ASSERT_GE(H5Awrite(a_closed.get(), H5T_NATIVE_INT, &v_closed), 0);
  }
  for (int i = 0; i < 2; ++i) {
    const std::string name = absl::StrCat("drop_", i);
    ScopedAttr tmp(H5Acreate2(dset.get(), name.c_str(), H5T_NATIVE_INT,
                              scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(tmp.valid());
  }
  EXPECT_GT(H5O__is_attr_dense_test(dset.get()), 0);

  // Deleting two attributes reduces nattrs to 2 (< min_dense == 3), reverting
  // to compact while `a_open` is held open and `kept_closed` is not open.
  ASSERT_GE(H5Adelete_by_idx(dset.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                             H5P_DEFAULT),
            0);
  ASSERT_GE(H5Adelete_by_name(dset.get(), ".", "drop_1", H5P_DEFAULT), 0);
  EXPECT_EQ(H5O__is_attr_dense_test(dset.get()), 0);

  int check_open = 0;
  ASSERT_GE(H5Aread(a_open.get(), H5T_NATIVE_INT, &check_open), 0);
  EXPECT_EQ(check_open, 7);
  ScopedAttr reopened_closed(H5Aopen(dset.get(), "kept_closed", H5P_DEFAULT));
  ASSERT_TRUE(reopened_closed.valid());
  int check_closed = 0;
  ASSERT_GE(H5Aread(reopened_closed.get(), H5T_NATIVE_INT, &check_closed), 0);
  EXPECT_EQ(check_closed, 8);
}

TEST(H5OattributeTest, CompactRenameIterateRemoveAndV1ObjectHeaderPaths) {
  // Exercise v1 object header (no ainfo message).
  {
    ScopedFile v1 = CreateCoreFileWithLibver(
        "v1_attrs.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
    ASSERT_TRUE(v1.valid());
    ScopedDataset d1 = CreateScalarIntDataset(v1.get(), "d");
    ASSERT_TRUE(d1.valid());
    ScopedSpace scalar(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(scalar.valid());

    for (const char* name : {"b", "a", "c"}) {
      ScopedAttr attr(H5Acreate2(d1.get(), name, H5T_NATIVE_INT, scalar.get(),
                                 H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(attr.valid());
    }

    H5O_info2_t oinfo{};
    ASSERT_GE(H5Oget_info3(d1.get(), &oinfo, H5O_INFO_NUM_ATTRS), 0);
    EXPECT_EQ(oinfo.num_attrs, 3u);
    H5O_native_info_t ninfo{};
    ASSERT_GE(
        H5Oget_native_info(d1.get(), &ninfo, H5O_NATIVE_INFO_META_SIZE), 0);
    EXPECT_EQ(ninfo.meta_size.attr.index_size, 0u);
    EXPECT_EQ(ninfo.meta_size.attr.heap_size, 0u);

    ScopedAttr by_idx(H5Aopen_by_idx(d1.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                     0, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(by_idx.valid());

    // Compact rename same length, different length, and UTF-8 version change.
    ASSERT_GE(H5Arename(d1.get(), "a", "z"), 0);
    ASSERT_GE(H5Arename(d1.get(), "z", "longer_name"), 0);
    ASSERT_GE(H5Arename(d1.get(), "b", "αβ"), 0);
    ASSERT_GE(H5Arename(d1.get(), "αβ", "xy"), 0);

    // Delete by name and by index on v1 object header.
    ASSERT_GE(H5Adelete(d1.get(), "c"), 0);
    ASSERT_GE(H5Adelete_by_idx(d1.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                               H5P_DEFAULT),
              0);
    ASSERT_GE(H5Oget_info3(d1.get(), &oinfo, H5O_INFO_NUM_ATTRS), 0);
    EXPECT_EQ(oinfo.num_attrs, 1u);
  }

  // Exercise v2 untracked compact iteration, skip/stop/error, and bad args.
  ScopedFile v2 = CreateCoreFileWithLibver("v2_compact.h5", H5F_LIBVER_V18,
                                           H5F_LIBVER_LATEST);
  ASSERT_TRUE(v2.valid());
  ScopedDataset d2 = CreateScalarIntDataset(v2.get(), "d2");
  ASSERT_TRUE(d2.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  for (const char* name : {"k1", "k2", "k3"}) {
    ScopedAttr attr(H5Acreate2(d2.get(), name, H5T_NATIVE_INT, scalar.get(),
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
  }

  {
    IterRecord stop_rec;
    stop_rec.stop_after = 2;
    hsize_t idx = 0;
    EXPECT_EQ(H5Aiterate2(d2.get(), H5_INDEX_NAME, H5_ITER_INC, &idx,
                          CollectAttrNames, &stop_rec),
              1);
    EXPECT_EQ(stop_rec.names.size(), 2u);
  }

  {
    ScopedErrorSilence silence;
    IterRecord err_rec;
    err_rec.error_after = 1;
    hsize_t idx = 0;
    EXPECT_LT(H5Aiterate2(d2.get(), H5_INDEX_NAME, H5_ITER_INC, &idx,
                          CollectAttrNames, &err_rec),
              0);

    // Compact skip >= num_attrs error.
    hsize_t bad_skip = 10;
    IterRecord skip_rec;
    EXPECT_LT(H5Aiterate2(d2.get(), H5_INDEX_NAME, H5_ITER_INC, &bad_skip,
                          CollectAttrNames, &skip_rec),
              0);

    // Dense skip >= num_attrs error.
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 1, 1), 0);
    ScopedGroup g_dense(
        H5Gcreate2(v2.get(), "g_dense", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(g_dense.valid());
    ScopedAttr da1(H5Acreate2(g_dense.get(), "d1", H5T_NATIVE_INT, scalar.get(),
                              H5P_DEFAULT, H5P_DEFAULT));
    ScopedAttr da2(H5Acreate2(g_dense.get(), "d2", H5T_NATIVE_INT, scalar.get(),
                              H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(da1.valid());
    ASSERT_TRUE(da2.valid());
    EXPECT_LT(H5Aiterate2(g_dense.get(), H5_INDEX_NAME, H5_ITER_INC, &bad_skip,
                          CollectAttrNames, &skip_rec),
              0);

    // Compact rename/remove errors.
    EXPECT_LT(H5Arename(d2.get(), "k1", "k2"), 0);
    EXPECT_LT(H5Arename(d2.get(), "missing", "newname"), 0);
    EXPECT_LT(H5Adelete(d2.get(), "missing"), 0);
    EXPECT_LT(H5Adelete_by_idx(d2.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 99,
                               H5P_DEFAULT),
              0);

    // Invalid loc_id directly into H5O__attr_iterate.
    H5A_attr_iter_op_t app_op{};
    app_op.op_type = H5A_ATTR_OP_APP2;
    app_op.u.app_op2 = CollectAttrNames;
    ScopedApiContext ctx;
    EXPECT_LT(H5O__attr_iterate(H5I_INVALID_HID, H5_INDEX_NAME, H5_ITER_INC, 0,
                                nullptr, &app_op, &skip_rec),
              0);
  }

  // Delete all compact attrs via delete_by_idx to clean out AINFO (nattrs == 0).
  ASSERT_GE(H5Adelete_by_idx(d2.get(), ".", H5_INDEX_NAME, H5_ITER_DEC, 0,
                             H5P_DEFAULT),
            0);
  ASSERT_GE(H5Adelete_by_idx(d2.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                             H5P_DEFAULT),
            0);
  ASSERT_GE(H5Adelete_by_idx(d2.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                             H5P_DEFAULT),
            0);
  H5O_info2_t oinfo{};
  ASSERT_GE(H5Oget_info3(d2.get(), &oinfo, H5O_INFO_NUM_ATTRS), 0);
  EXPECT_EQ(oinfo.num_attrs, 0u);
}

std::vector<uint8_t> ExtractFileImage(hid_t file_id) {
  if (H5Fflush(file_id, H5F_SCOPE_GLOBAL) < 0) return {};
  const ssize_t size = H5Fget_file_image(file_id, nullptr, 0);
  if (size <= 0) return {};
  std::vector<uint8_t> image(static_cast<size_t>(size));
  if (H5Fget_file_image(file_id, image.data(), image.size()) != size) {
    return {};
  }
  return image;
}

ScopedFile OpenMemoryImage(const std::vector<uint8_t>& image, unsigned flags,
                           const char* name = "mem_attr.h5") {
  ScopedPlist fapl = MakeCoreFapl();
  if (!fapl.valid()) return ScopedFile();
  if (H5Pset_file_image(fapl.get(), const_cast<uint8_t*>(image.data()),
                        image.size()) < 0) {
    return ScopedFile();
  }
  return ScopedFile(H5Fopen(name, flags, fapl.get()));
}

TEST(H5OattributeTest, AdditionalPublicAndDirectInternalErrorPaths) {
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  // 1) Compact missing open, dense callback/delete_by_idx errors, and
  //    open-handle write after background rename/delete (compact & dense).
  {
    ScopedFile file = CreateCoreFileWithLibver(
        "pub_errs.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(file.valid());
    ScopedDataset d_cmp = CreateScalarIntDataset(file.get(), "d_cmp");
    ASSERT_TRUE(d_cmp.valid());
    ScopedAttr c1(H5Acreate2(d_cmp.get(), "c1", H5T_NATIVE_INT, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(c1.valid());

    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 1, 1), 0);
    ScopedGroup g_dns(
        H5Gcreate2(file.get(), "g_dns", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(g_dns.valid());
    ScopedAttr d1(H5Acreate2(g_dns.get(), "d1", H5T_NATIVE_INT, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ScopedAttr d2(H5Acreate2(g_dns.get(), "d2", H5T_NATIVE_INT, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d1.valid());
    ASSERT_TRUE(d2.valid());

    // Delete attributes behind open handles so H5Awrite fails cleanly in both
    // compact (900) and dense (881) storage. Keep a second compact attr so
    // d_cmp retains an attribute for out-of-range checks.
    ScopedAttr c2(H5Acreate2(d_cmp.get(), "c2", H5T_NATIVE_INT, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(c2.valid());
    ASSERT_GE(H5Adelete(d_cmp.get(), "c1"), 0);
    ASSERT_GE(H5Adelete(g_dns.get(), "d1"), 0);

    ScopedErrorSilence silence;
    int val = 123;
    // Compact open missing name (line 493) + out-of-range open_by_idx (579).
    EXPECT_FALSE(
        ScopedAttr(H5Aopen(d_cmp.get(), "no_such_compact", H5P_DEFAULT)).valid());
    EXPECT_FALSE(ScopedAttr(H5Aopen_by_idx(d_cmp.get(), ".", H5_INDEX_NAME,
                                           H5_ITER_INC, 99, H5P_DEFAULT,
                                           H5P_DEFAULT))
                     .valid());
    // Write to deleted compact (900) and dense (881) attribute handles.
    EXPECT_LT(H5Awrite(c1.get(), H5T_NATIVE_INT, &val), 0);
    EXPECT_LT(H5Awrite(d1.get(), H5T_NATIVE_INT, &val), 0);

    // Dense callback failure (1214), open_by_idx out-of-range (579), and
    // delete_by_idx out-of-range (1561).
    IterRecord err_rec;
    err_rec.error_after = 0;
    hsize_t idx = 0;
    EXPECT_LT(H5Aiterate2(g_dns.get(), H5_INDEX_NAME, H5_ITER_INC, &idx,
                          CollectAttrNames, &err_rec),
              0);
    EXPECT_FALSE(ScopedAttr(H5Aopen_by_idx(g_dns.get(), ".", H5_INDEX_NAME,
                                           H5_ITER_INC, 99, H5P_DEFAULT,
                                           H5P_DEFAULT))
                     .valid());
    EXPECT_LT(H5Adelete_by_idx(g_dns.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 99,
                               H5P_DEFAULT),
              0);
  }

  // 2) >64KB attribute rejected in v1 object header (H5O__msg_append_real < 0).
  {
    ScopedFile v1 = CreateCoreFileWithLibver(
        "v1_oversized.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST);
    ASSERT_TRUE(v1.valid());
    ScopedDataset d_v1 = CreateScalarIntDataset(v1.get(), "d_v1");
    ASSERT_TRUE(d_v1.valid());
    const hsize_t big_dims[1] = {70000};
    ScopedSpace big_sp(H5Screate_simple(1, big_dims, nullptr));
    ASSERT_TRUE(big_sp.valid());
    ScopedErrorSilence silence;
    ScopedAttr fail_big(H5Acreate2(d_v1.get(), "too_big_v1", H5T_NATIVE_UINT8,
                                   big_sp.get(), H5P_DEFAULT, H5P_DEFAULT));
    EXPECT_FALSE(fail_big.valid());
  }

  // 3) SOHM-shared compact attribute shrunk below min_mesg_size on rename +
  //    UTF-8 compact attribute rename rejected under downgraded high_bound.
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
    ASSERT_GE(
        H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_ATTR_FLAG, 48), 0);

    ScopedFile file = CreateCoreFileWithLibver(
        "sohm_shrink_and_ver.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST,
        fcpl.get());
    ASSERT_TRUE(file.valid());
    ScopedDataset d = CreateScalarIntDataset(file.get(), "d");
    ASSERT_TRUE(d.valid());

    // Long name puts encoded attr above min_size=48 -> stored in SOHM; renaming
    // to "x" drops below 48 -> H5O__attr_update_shared hits == 0 error path.
    ScopedAttr sh_a(H5Acreate2(d.get(), "shared_long_attr_name_padding_01234",
                               H5T_NATIVE_INT, scalar.get(), H5P_DEFAULT,
                               H5P_DEFAULT));
    ASSERT_TRUE(sh_a.valid());
    int sh_v = 9;
    ASSERT_GE(H5Awrite(sh_a.get(), H5T_NATIVE_INT, &sh_v), 0);

    // UTF-8 compact attribute for version-bound failure on rename.
    ScopedPlist utf8_acpl(H5Pcreate(H5P_ATTRIBUTE_CREATE));
    ASSERT_TRUE(utf8_acpl.valid());
    ASSERT_GE(H5Pset_char_encoding(utf8_acpl.get(), H5T_CSET_UTF8), 0);
    ScopedAttr utf8_a(H5Acreate2(d.get(), "utf8_name", H5T_NATIVE_INT,
                                 scalar.get(), utf8_acpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(utf8_a.valid());

    ScopedErrorSilence silence;
    EXPECT_LT(H5Arename(d.get(), "shared_long_attr_name_padding_01234", "x"), 0);

    auto* f = static_cast<H5F_t*>(H5VL_object(file.get()));
    ASSERT_NE(f, nullptr);
    const H5F_libver_t orig_hi = f->shared->high_bound;
    f->shared->high_bound = H5F_LIBVER_EARLIEST;
    absl::Cleanup restore_hi = [&] { f->shared->high_bound = orig_hi; };
    EXPECT_LT(H5Arename(d.get(), "utf8_name", "utf8_renamed"), 0);
  }

  // 4) Direct internal calls with invalid idx_type, read-only oloc, bad addr,
  //    and invalid sh_loc.type in H5O__attr_update_shared.
  {
    ScopedFile rw = CreateCoreFileWithLibver(
        "ro_bad_loc.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(rw.valid());
    ScopedDataset d_rw = CreateScalarIntDataset(rw.get(), "d");
    ASSERT_TRUE(d_rw.valid());
    ScopedAttr a_rw(H5Acreate2(d_rw.get(), "c1", H5T_NATIVE_INT, scalar.get(),
                               H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(a_rw.valid());
    a_rw.reset();
    d_rw.reset();
    const std::vector<uint8_t> img = ExtractFileImage(rw.get());
    ASSERT_FALSE(img.empty());

    d_rw = ScopedDataset(H5Dopen2(rw.get(), "d", H5P_DEFAULT));
    a_rw = ScopedAttr(H5Aopen(d_rw.get(), "c1", H5P_DEFAULT));
    ASSERT_TRUE(d_rw.valid());
    ASSERT_TRUE(a_rw.valid());

    ScopedFile ro = OpenMemoryImage(img, H5F_ACC_RDONLY);
    ASSERT_TRUE(ro.valid());
    ScopedDataset d_ro(H5Dopen2(ro.get(), "d", H5P_DEFAULT));
    ASSERT_TRUE(d_ro.valid());

    auto* rw_loc = H5O_get_loc(d_rw.get());
    auto* ro_loc = H5O_get_loc(d_ro.get());
    auto* a_native = static_cast<H5A_t*>(H5VL_object_verify(a_rw.get(), H5I_ATTR));
    ASSERT_NE(rw_loc, nullptr);
    ASSERT_NE(ro_loc, nullptr);
    ASSERT_NE(a_native, nullptr);

    H5O_loc_t bad_loc{};
    bad_loc.file = rw_loc->file;
    bad_loc.addr = 0x1234560;

    ScopedApiContext ctx;
    ScopedErrorSilence silence;
    EXPECT_LT(H5O__attr_create(ro_loc, a_native), 0);
    EXPECT_LT(H5O__attr_write(ro_loc, a_native), 0);
    EXPECT_LT(H5O__attr_rename(ro_loc, "c1", "c2"), 0);
    EXPECT_LT(H5O__attr_remove(ro_loc, "c1"), 0);
    EXPECT_LT(H5O__attr_remove_by_idx(ro_loc, H5_INDEX_NAME, H5_ITER_INC, 0), 0);

    EXPECT_EQ(H5O__attr_open_by_name(&bad_loc, "c1"), nullptr);
    EXPECT_EQ(
        H5O__attr_open_by_idx(&bad_loc, H5_INDEX_NAME, H5_ITER_INC, 0),
        nullptr);
    bool ex = false;
    EXPECT_LT(H5O__attr_exists(&bad_loc, "c1", &ex), 0);

    H5O_t* oh = H5O_pin(rw_loc);
    ASSERT_NE(oh, nullptr);
    absl::Cleanup unpin_oh = [&] { H5O_unpin(oh); };
    const H5O_shared_t orig_sh = a_native->sh_loc;
    a_native->sh_loc.type = 99u;
    absl::Cleanup restore_sh = [&] { a_native->sh_loc = orig_sh; };
    EXPECT_LT(H5O__attr_update_shared(rw_loc->file, oh, a_native, nullptr), 0);
  }
}

struct MultiChunkImage {
  std::vector<uint8_t> bytes;
  std::vector<std::string> chk0_attr_names;
  std::string chk1_attr_name;
};

MultiChunkImage BuildMultiChunkImage(const char* fname, H5F_libver_t lo,
                                     H5F_libver_t hi, hid_t scalar_space,
                                     unsigned max_compact = 65,
                                     unsigned min_dense = 65) {
  MultiChunkImage out;
  ScopedFile f = CreateCoreFileWithLibver(fname, lo, hi);
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  if (lo >= H5F_LIBVER_V18 && dcpl.valid()) {
    H5Pset_attr_phase_change(dcpl.get(), max_compact, min_dense);
  }
  ScopedDataset d = CreateScalarIntDataset(
      f.get(), "d", lo >= H5F_LIBVER_V18 ? dcpl.get() : H5P_DEFAULT);
  for (int i = 0; i < 45; ++i) {
    const std::string name = absl::StrCat("m_", i);
    ScopedAttr a(H5Acreate2(d.get(), name.c_str(), H5T_NATIVE_INT, scalar_space,
                            H5P_DEFAULT, H5P_DEFAULT));
    if (i % 15 == 14) {
      const std::string pad_name = absl::StrCat("pad_", i);
      ScopedDataset pad = CreateScalarIntDataset(f.get(), pad_name.c_str());
      H5Fflush(f.get(), H5F_SCOPE_LOCAL);
    }
  }
  if (H5O_loc_t* loc = H5O_get_loc(d.get())) {
    ScopedApiContext ctx;
    if (H5O_t* oh = H5O_pin(loc)) {
      absl::Cleanup unpin = [&] { H5O_unpin(oh); };
      for (unsigned u = 0; u < oh->nmesgs; ++u) {
        if (oh->mesg[u].type == H5O_MSG_ATTR && oh->mesg[u].native != nullptr) {
          const char* attr_name =
              static_cast<const H5A_t*>(oh->mesg[u].native)->shared->name;
          if (oh->mesg[u].chunkno == 0) {
            out.chk0_attr_names.emplace_back(attr_name);
          } else if (out.chk1_attr_name.empty()) {
            out.chk1_attr_name = attr_name;
          }
        }
      }
    }
  }
  d.reset();
  out.bytes = ExtractFileImage(f.get());
  return out;
}

TEST(H5OattributeTest, CorruptAinfoWrapAndMultiChunkSohmFailurePaths) {
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  // 1) Creation-order index wraparound (288) and corrupt AINFO pointers.
  {
    ScopedFile file = CreateCoreFileWithLibver(
        "corrupt_ainfo.h5", H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
    ASSERT_TRUE(file.valid());
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 2, 1), 0);
    ASSERT_GE(H5Pset_attr_creation_order(
                  gcpl.get(), H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED),
              0);
    ScopedGroup g(
        H5Gcreate2(file.get(), "g", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
    for (int i = 0; i < 3; ++i) {
      const std::string name = absl::StrCat("a", i);
      ScopedAttr a(H5Acreate2(g.get(), name.c_str(), H5T_NATIVE_INT,
                              scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(a.valid());
    }
    ScopedAttr open_a0(H5Aopen(g.get(), "a0", H5P_DEFAULT));
    ASSERT_TRUE(open_a0.valid());
    auto* a0_nat =
        static_cast<H5A_t*>(H5VL_object_verify(open_a0.get(), H5I_ATTR));
    ASSERT_NE(a0_nat, nullptr);

    H5O_loc_t* gloc = H5O_get_loc(g.get());
    ASSERT_NE(gloc, nullptr);

    ScopedApiContext ctx;
    ScopedErrorSilence silence;
    H5O_t* oh = H5O_pin(gloc);
    ASSERT_NE(oh, nullptr);
    H5O_ainfo_t orig_ainfo{};
    ASSERT_GT(H5A__get_ainfo(gloc->file, oh, &orig_ainfo), 0);
    absl::Cleanup restore_and_unpin = [&] {
      H5O__msg_write_real(gloc->file, oh, H5O_MSG_AINFO,
                          H5O_MSG_FLAG_DONTSHARE, 0, &orig_ainfo);
      H5O_unpin(oh);
    };

    auto write_ainfo = [&](const H5O_ainfo_t& ai) {
      H5O_ainfo_t copy = ai;
      ASSERT_GE(H5O__msg_write_real(gloc->file, oh, H5O_MSG_AINFO,
                                    H5O_MSG_FLAG_DONTSHARE, 0, &copy),
                0);
    };

    // max_crt_idx == H5O_MAX_CRT_ORDER_IDX -> H5O__attr_create fails at 288.
    H5O_ainfo_t wrap_ai = orig_ainfo;
    wrap_ai.max_crt_idx = H5O_MAX_CRT_ORDER_IDX;
    write_ainfo(wrap_ai);
    EXPECT_FALSE(ScopedAttr(H5Acreate2(g.get(), "wrap", H5T_NATIVE_INT,
                                       scalar.get(), H5P_DEFAULT, H5P_DEFAULT))
                     .valid());

    // Bogus name_bt2_addr -> bh_info fails at 1793.
    H5O_native_info_t ni{};
    H5O_ainfo_t bad_name_bt = orig_ainfo;
    bad_name_bt.name_bt2_addr = 0x999910;
    write_ainfo(bad_name_bt);
    EXPECT_LT(
        H5Oget_native_info(g.get(), &ni, H5O_NATIVE_INFO_META_SIZE), 0);

    // Valid name_bt2_addr + bogus corder_bt2_addr -> bh_info fails at 1804.
    H5O_ainfo_t bad_crd_bt = orig_ainfo;
    bad_crd_bt.corder_bt2_addr = 0x999920;
    write_ainfo(bad_crd_bt);
    EXPECT_LT(
        H5Oget_native_info(g.get(), &ni, H5O_NATIVE_INFO_META_SIZE), 0);

    // Valid B-trees + bogus fheap_addr -> bh_info (1816), exists (1733),
    // create/dense_insert (318).
    H5O_ainfo_t bad_heap = orig_ainfo;
    bad_heap.fheap_addr = 0x999900;
    write_ainfo(bad_heap);
    EXPECT_LT(
        H5Oget_native_info(g.get(), &ni, H5O_NATIVE_INFO_META_SIZE), 0);
    EXPECT_LT(H5Aexists(g.get(), "a0"), 0);
    EXPECT_FALSE(ScopedAttr(H5Acreate2(g.get(), "bad_ins", H5T_NATIVE_INT,
                                       scalar.get(), H5P_DEFAULT, H5P_DEFAULT))
                     .valid());
    {
      const H5O_msg_crt_idx_t orig_crt = a0_nat->shared->crt_idx;
      absl::Cleanup restore_crt = [&] { a0_nat->shared->crt_idx = orig_crt; };
      EXPECT_LT(H5O__attr_create(gloc, a0_nat), 0);
    }

    // nattrs == HSIZET_MAX + bogus name_bt2_addr forces H5A__get_ainfo < 0
    // across every entry point (224,458,874,1111,1197,1476,1554,1637,1726,1787).
    H5O_ainfo_t bad_get = orig_ainfo;
    bad_get.nattrs = HSIZET_MAX;
    bad_get.fheap_addr = 0x999900;
    bad_get.name_bt2_addr = 0x999910;
    write_ainfo(bad_get);

    EXPECT_FALSE(ScopedAttr(H5Acreate2(g.get(), "bad_cr", H5T_NATIVE_INT,
                                       scalar.get(), H5P_DEFAULT, H5P_DEFAULT))
                     .valid());
    EXPECT_LT(H5O__attr_create(gloc, a0_nat), 0);
    EXPECT_FALSE(ScopedAttr(H5Aopen(g.get(), "a0", H5P_DEFAULT)).valid());
    EXPECT_LT(H5O__attr_write(gloc, a0_nat), 0);
    EXPECT_LT(H5Arename(g.get(), "a0", "a0_x"), 0);
    hsize_t it_idx = 0;
    IterRecord it_rec;
    EXPECT_LT(H5Aiterate2(g.get(), H5_INDEX_NAME, H5_ITER_INC, &it_idx,
                          CollectAttrNames, &it_rec),
              0);
    EXPECT_LT(H5Adelete(g.get(), "a0"), 0);
    EXPECT_LT(H5Adelete_by_idx(g.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                               H5P_DEFAULT),
              0);
    H5O_info2_t oi{};
    EXPECT_LT(H5Oget_info3(g.get(), &oi, H5O_INFO_NUM_ATTRS), 0);
    EXPECT_LT(H5Aexists(g.get(), "a0"), 0);
    EXPECT_LT(
        H5Oget_native_info(g.get(), &ni, H5O_NATIVE_INFO_META_SIZE), 0);
  }

  // 2) Dense -> compact revert failing in H5A__dense_delete (1373,1508,1594).
  for (bool by_idx : {false, true}) {
    ScopedFile file = CreateCoreFileWithLibver(
        "revert_fail.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
    ASSERT_TRUE(file.valid());
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 2, 2), 0);
    ScopedGroup g(
        H5Gcreate2(file.get(), "g", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
    for (int i = 0; i < 3; ++i) {
      const std::string name = absl::StrCat("u", i);
      ScopedAttr a(H5Acreate2(g.get(), name.c_str(), H5T_NATIVE_INT,
                              scalar.get(), H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(a.valid());
    }
    ASSERT_GE(H5Adelete(g.get(), "u2"), 0);  // Still dense with nattrs == 2.
    H5O_loc_t* gloc = H5O_get_loc(g.get());
    ASSERT_NE(gloc, nullptr);
    ScopedApiContext ctx;
    ScopedErrorSilence silence;
    H5O_t* oh = H5O_pin(gloc);
    ASSERT_NE(oh, nullptr);
    {
      absl::Cleanup unpin_oh = [&] { H5O_unpin(oh); };
      H5O_ainfo_t ai{};
      ASSERT_GT(H5A__get_ainfo(gloc->file, oh, &ai), 0);
      ai.corder_bt2_addr = 0x999920;  // Ignored on unindexed remove, fails delete.
      ASSERT_GE(H5O__msg_write_real(gloc->file, oh, H5O_MSG_AINFO,
                                    H5O_MSG_FLAG_DONTSHARE, 0, &ai),
                0);
    }
    if (by_idx) {
      EXPECT_LT(H5Adelete_by_idx(g.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                                 H5P_DEFAULT),
                0);
    } else {
      EXPECT_LT(H5Adelete(g.get(), "u0"), 0);
    }
  }

  // 3) Multi-chunk chunk_protect failures (792,896,987,1428,1498,1584) +
  //    v1 H5O_touch_oh failures (373,905,1153,1512,1598).
  {
    // V2 multi-chunk chunk_protect failure on reopened image (chk1 uncached).
    {
      const MultiChunkImage v2_img = BuildMultiChunkImage(
          "v2_mc.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST, scalar.get());
      ASSERT_FALSE(v2_img.bytes.empty());
      ASSERT_FALSE(v2_img.chk1_attr_name.empty());
      ScopedFile v2_re =
          OpenMemoryImage(v2_img.bytes, H5F_ACC_RDWR, "v2_re.h5");
      ASSERT_TRUE(v2_re.valid());
      ScopedDataset d_v2(H5Dopen2(v2_re.get(), "d", H5P_DEFAULT));
      ASSERT_TRUE(d_v2.valid());
      H5O_loc_t* loc2 = H5O_get_loc(d_v2.get());
      ASSERT_NE(loc2, nullptr);

      ScopedApiContext ctx;
      H5O_t* oh = H5O_pin(loc2);
      ASSERT_NE(oh, nullptr);
      bool corrupted_chk1 = false;
      haddr_t orig_chk1 = HADDR_UNDEF;
      absl::Cleanup restore = [&] {
        if (corrupted_chk1) oh->chunk[1].addr = orig_chk1;
        H5O_unpin(oh);
      };
      ASSERT_GT(oh->nchunks, 1u);
      const std::string& chk1_attr_name = v2_img.chk1_attr_name;
      ScopedAttr open_chk1(
          H5Aopen(d_v2.get(), chk1_attr_name.c_str(), H5P_DEFAULT));
      ASSERT_TRUE(open_chk1.valid());

      // Determine index of chk1_attr_name in H5_INDEX_NAME before fault.
      IterRecord all_names;
      hsize_t z = 0;
      ASSERT_GE(H5Aiterate2(d_v2.get(), H5_INDEX_NAME, H5_ITER_INC, &z,
                            CollectAttrNames, &all_names),
                0);
      hsize_t chk1_idx = 0;
      for (size_t i = 0; i < all_names.names.size(); ++i) {
        if (all_names.names[i] == chk1_attr_name) {
          chk1_idx = i;
          break;
        }
      }

      orig_chk1 = oh->chunk[1].addr;
      oh->chunk[1].addr = 0x999900;
      corrupted_chk1 = true;

      ScopedErrorSilence silence;
      int val = 7;
      EXPECT_LT(H5Awrite(open_chk1.get(), H5T_NATIVE_INT, &val), 0);
      EXPECT_LT(H5Arename(d_v2.get(), chk1_attr_name.c_str(), "new_c1"), 0);
      EXPECT_LT(H5Adelete(d_v2.get(), chk1_attr_name.c_str()), 0);
      EXPECT_LT(H5Adelete_by_idx(d_v2.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                 chk1_idx, H5P_DEFAULT),
                0);
    }

    // V1 multi-chunk H5O_touch_oh failures (373, 905, 1153, 1512, 1598).
    {
      const MultiChunkImage v1_img = BuildMultiChunkImage(
          "v1_mc.h5", H5F_LIBVER_EARLIEST, H5F_LIBVER_LATEST, scalar.get());
      ASSERT_FALSE(v1_img.bytes.empty());
      ASSERT_GE(v1_img.chk0_attr_names.size(), 2u);
      ScopedFile v1_re =
          OpenMemoryImage(v1_img.bytes, H5F_ACC_RDWR, "v1_re.h5");
      ASSERT_TRUE(v1_re.valid());
      ScopedDataset d_v1(H5Dopen2(v1_re.get(), "d", H5P_DEFAULT));
      ASSERT_TRUE(d_v1.valid());
      H5O_loc_t* loc1 = H5O_get_loc(d_v1.get());
      ASSERT_NE(loc1, nullptr);

      const std::string& c0_a = v1_img.chk0_attr_names[0];
      const std::string& c0_b = v1_img.chk0_attr_names[1];
      ASSERT_LT(c0_b, "z_n");
      ScopedAttr a0(H5Aopen(d_v1.get(), c0_a.c_str(), H5P_DEFAULT));
      ASSERT_TRUE(a0.valid());

      // Compute H5_INDEX_NAME index of c0_b after c0_a is deleted and "z_n"
      // (after "m_*") is created in its chunk-0 null slot.
      IterRecord v1_names;
      hsize_t z = 0;
      ASSERT_GE(H5Aiterate2(d_v1.get(), H5_INDEX_NAME, H5_ITER_INC, &z,
                            CollectAttrNames, &v1_names),
                0);
      hsize_t c0_b_idx = 0;
      for (const std::string& nm : v1_names.names) {
        if (nm != c0_a && nm < c0_b) {
          ++c0_b_idx;
        }
      }

      ScopedApiContext ctx;
      H5O_t* oh = H5O_pin(loc1);
      ASSERT_NE(oh, nullptr);
      bool corrupted = false;
      int mtime_idx = -1;
      decltype(oh->flags) orig_flags = 0;
      unsigned orig_mchk = 0;
      haddr_t orig_chk1 = HADDR_UNDEF;
      absl::Cleanup restore = [&] {
        if (corrupted) {
          oh->flags = orig_flags;
          oh->mesg[mtime_idx].chunkno = orig_mchk;
          oh->chunk[1].addr = orig_chk1;
        }
        H5O_unpin(oh);
      };
      ASSERT_EQ(oh->version, H5O_VERSION_1);
      ASSERT_GT(oh->nchunks, 1u);
      for (unsigned u = 0; u < oh->nmesgs; ++u) {
        if (oh->mesg[u].type == H5O_MSG_MTIME ||
            oh->mesg[u].type == H5O_MSG_MTIME_NEW) {
          mtime_idx = static_cast<int>(u);
          break;
        }
      }
      ASSERT_GE(mtime_idx, 0);
      orig_flags = oh->flags;
      orig_mchk = oh->mesg[mtime_idx].chunkno;
      orig_chk1 = oh->chunk[1].addr;

      oh->flags |= H5O_HDR_STORE_TIMES;
      oh->mesg[mtime_idx].chunkno = 1;
      oh->chunk[1].addr = 0x999900;
      corrupted = true;

      ScopedErrorSilence silence;
      int val = 9;
      EXPECT_LT(H5Awrite(a0.get(), H5T_NATIVE_INT, &val), 0);
      EXPECT_LT(H5Arename(d_v1.get(), c0_a.c_str(), "m0x"), 0);
      EXPECT_LT(H5Adelete(d_v1.get(), "m0x"), 0);
      EXPECT_FALSE(ScopedAttr(H5Acreate2(d_v1.get(), "z_n", H5T_NATIVE_INT,
                                         scalar.get(), H5P_DEFAULT,
                                         H5P_DEFAULT))
                       .valid());
      EXPECT_LT(H5Adelete_by_idx(d_v1.get(), ".", H5_INDEX_NAME, H5_ITER_INC,
                                 c0_b_idx, H5P_DEFAULT),
                0);
    }
  }

  // 4) SOHM table lookup & update failures (252, 365, 729, 747, 751, 822).
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(H5Pset_shared_mesg_nindexes(fcpl.get(), 1), 0);
    ASSERT_GE(
        H5Pset_shared_mesg_index(fcpl.get(), 0, H5O_SHMESG_ATTR_FLAG, 100), 0);
    ScopedFile f = CreateCoreFileWithLibver(
        "sohm_corrupt.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST, fcpl.get());
    ASSERT_TRUE(f.valid());
    ScopedType cdt(H5Tcopy(H5T_NATIVE_INT));
    ASSERT_TRUE(cdt.valid());
    ASSERT_GE(H5Tcommit2(f.get(), "cdt", cdt.get(), H5P_DEFAULT, H5P_DEFAULT,
                         H5P_DEFAULT),
              0);

    ScopedDataset d = CreateScalarIntDataset(f.get(), "d");
    ASSERT_TRUE(d.valid());
    const hsize_t dims[1] = {40};
    ScopedSpace sp(H5Screate_simple(1, dims, nullptr));
    ASSERT_TRUE(sp.valid());
    ScopedAttr sa(H5Acreate2(d.get(), "sa", H5T_NATIVE_INT, sp.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(sa.valid());
    sa.reset();
    ScopedAttr sc(H5Acreate2(d.get(), "sc", cdt.get(), sp.get(), H5P_DEFAULT,
                             H5P_DEFAULT));
    ASSERT_TRUE(sc.valid());
    sc.reset();
    d.reset();

    ScopedDataset d2 = CreateScalarIntDataset(f.get(), "d2");
    ASSERT_TRUE(d2.valid());
    ScopedAttr t(H5Acreate2(d2.get(), "t", H5T_NATIVE_INT, scalar.get(),
                            H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(t.valid());
    t.reset();
    ScopedAttr sc2(H5Acreate2(d2.get(), "sc", cdt.get(), sp.get(), H5P_DEFAULT,
                              H5P_DEFAULT));
    ASSERT_TRUE(sc2.valid());
    std::vector<int> ones(40, 1);
    ASSERT_GE(H5Awrite(sc2.get(), H5T_NATIVE_INT, ones.data()), 0);
    sc2.reset();
    d2.reset();
    ScopedDataset d3 = CreateScalarIntDataset(f.get(), "d3");
    ASSERT_TRUE(d3.valid());
    d3.reset();
    cdt.reset();

    const std::vector<uint8_t> img = ExtractFileImage(f.get());
    ASSERT_FALSE(img.empty());
    ASSERT_GE(H5Fclose(f.release()), 0);

    ScopedFile re = OpenMemoryImage(img, H5F_ACC_RDWR, "sohm_re.h5");
    ASSERT_TRUE(re.valid());
    ScopedDataset dre(H5Dopen2(re.get(), "d", H5P_DEFAULT));
    ScopedAttr sare(H5Aopen(dre.get(), "sa", H5P_DEFAULT));
    ScopedDataset d2re(H5Dopen2(re.get(), "d2", H5P_DEFAULT));
    ScopedAttr tre(H5Aopen(d2re.get(), "t", H5P_DEFAULT));
    ScopedAttr sc2re(H5Aopen(d2re.get(), "sc", H5P_DEFAULT));
    ScopedDataset d3re(H5Dopen2(re.get(), "d3", H5P_DEFAULT));
    ASSERT_TRUE(dre.valid());
    ASSERT_TRUE(sare.valid());
    ASSERT_TRUE(d2re.valid());
    ASSERT_TRUE(tre.valid());
    ASSERT_TRUE(sc2re.valid());
    ASSERT_TRUE(d3re.valid());

    H5O_loc_t* dre_loc = H5O_get_loc(dre.get());
    H5O_loc_t* d2re_loc = H5O_get_loc(d2re.get());
    H5O_loc_t* d3re_loc = H5O_get_loc(d3re.get());
    ASSERT_NE(dre_loc, nullptr);
    ASSERT_NE(d2re_loc, nullptr);
    ASSERT_NE(d3re_loc, nullptr);
    auto* t_nat = static_cast<H5A_t*>(H5VL_object_verify(tre.get(), H5I_ATTR));
    auto* sare_nat =
        static_cast<H5A_t*>(H5VL_object_verify(sare.get(), H5I_ATTR));
    auto* sc2_nat =
        static_cast<H5A_t*>(H5VL_object_verify(sc2re.get(), H5I_ATTR));
    ASSERT_NE(t_nat, nullptr);
    ASSERT_NE(sare_nat, nullptr);
    ASSERT_NE(sc2_nat, nullptr);
    ASSERT_EQ(t_nat->sh_loc.type, H5O_SHARE_TYPE_UNSHARED);
    ASSERT_EQ(sare_nat->sh_loc.type, H5O_SHARE_TYPE_SOHM);
    ASSERT_EQ(sc2_nat->sh_loc.type, H5O_SHARE_TYPE_SOHM);
    // H5O_shared_t sh_loc is the first field of H5T_t (same convention as
    // H5Oshared.h).
    auto* dt_sh = reinterpret_cast<H5O_shared_t*>(sc2_nat->shared->dt);
    ASSERT_EQ(dt_sh->type, H5O_SHARE_TYPE_COMMITTED);

    auto* fptr = static_cast<H5F_t*>(H5VL_object(re.get()));
    ASSERT_NE(fptr, nullptr);

    ScopedApiContext ctx;
    ScopedErrorSilence silence;
    {
      const haddr_t orig_sohm = fptr->shared->sohm_addr;
      fptr->shared->sohm_addr = 0x999900;
      absl::Cleanup restore = [&] { fptr->shared->sohm_addr = orig_sohm; };

      EXPECT_LT(H5O__attr_create(dre_loc, t_nat), 0);
      std::vector<int> vals(40, 5);
      EXPECT_LT(H5Awrite(sare.get(), H5T_NATIVE_INT, vals.data()), 0);
    }

    // With valid SOHM, force H5SM_delete failure (751).
    {
      H5O_t* oh = H5O_pin(dre_loc);
      ASSERT_NE(oh, nullptr);
      const H5O_shared_t orig_sh = sare_nat->sh_loc;
      int* data_ptr = static_cast<int*>(sare_nat->shared->data);
      const int orig_val0 = data_ptr[0];
      absl::Cleanup restore = [&] {
        sare_nat->sh_loc = orig_sh;
        data_ptr[0] = orig_val0;
        H5O_unpin(oh);
      };
      sare_nat->sh_loc.msg_type_id = H5O_NULL_ID;
      data_ptr[0] = 42;
      EXPECT_LT(H5O__attr_update_shared(fptr, oh, sare_nat, nullptr), 0);
    }

    // Committed datatype with bogus oh_addr hits H5O__attr_link fail in
    // H5O__attr_update_shared (747) and H5O__attr_delete fail on attr_rc > 1
    // in H5O__attr_create (365).
    {
      H5O_t* oh2 = H5O_pin(d2re_loc);
      ASSERT_NE(oh2, nullptr);
      const H5O_shared_t orig_sh = sc2_nat->sh_loc;
      const haddr_t orig_dt_addr = dt_sh->u.loc.oh_addr;
      int* data_ptr = static_cast<int*>(sc2_nat->shared->data);
      std::vector<int> orig_data(data_ptr, data_ptr + 40);
      absl::Cleanup restore = [&] {
        dt_sh->u.loc.oh_addr = orig_dt_addr;
        sc2_nat->sh_loc = orig_sh;
        for (size_t i = 0; i < 40; ++i) data_ptr[i] = orig_data[i];
        H5O_unpin(oh2);
      };

      // Unique value -> attr_rc == 1 -> H5O__attr_link fails at 747.
      data_ptr[0] = 77;
      dt_sh->u.loc.oh_addr = 0x999900;
      EXPECT_LT(H5O__attr_update_shared(fptr, oh2, sc2_nat, nullptr), 0);
      dt_sh->u.loc.oh_addr = orig_dt_addr;

      // With valid dt oh_addr matching SOHM ("sc", all zeros), set
      // oh_cdt->nlink = 0 so H5SM_try_share hits attr_rc == 2 > 1 and then
      // H5O__attr_delete -> H5O_link(cdt, -1) fails at 365.
      ScopedType cdt_re(H5Topen2(re.get(), "cdt", H5P_DEFAULT));
      ASSERT_TRUE(cdt_re.valid());
      H5O_loc_t* cdt_loc = H5O_get_loc(cdt_re.get());
      ASSERT_NE(cdt_loc, nullptr);
      H5O_t* oh_cdt = H5O_pin(cdt_loc);
      ASSERT_NE(oh_cdt, nullptr);
      const unsigned orig_cdt_nlink = oh_cdt->nlink;
      oh_cdt->nlink = 0;
      absl::Cleanup restore_cdt = [&] {
        oh_cdt->nlink = orig_cdt_nlink;
        H5O_unpin(oh_cdt);
      };

      for (size_t i = 0; i < 40; ++i) data_ptr[i] = 0;
      ASSERT_GE(H5O_msg_reset_share(H5O_ATTR_ID, sc2_nat), 0);
      const hsize_t nrefs_before = sc2_nat->shared->nrefs;
      EXPECT_LT(H5O__attr_create(d3re_loc, sc2_nat), 0);
      if (sc2_nat->shared->nrefs > nrefs_before + 1) {
        // Undo the extra caller reference added at line 330 before line 365
        // failed (the message copied into d3re holds the remaining +1 ref).
        sc2_nat->shared->nrefs--;
      }
    }
  }
}

TEST(H5OattributeTest,
     CorruptRawAttrAinfoMoveAndPhaseConversionFailurePaths) {
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  // 1) Compact raw attribute decode failures on reopened dataset
  //    (489, 1134, 1219, 1569, 1747).
  {
    std::vector<uint8_t> img;
    {
      ScopedFile f = CreateCoreFileWithLibver(
          "raw_attr_src.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
      ASSERT_TRUE(f.valid());
      ScopedDataset d = CreateScalarIntDataset(f.get(), "d");
      ASSERT_TRUE(d.valid());
      ScopedAttr a(H5Acreate2(d.get(), "c0", H5T_NATIVE_INT, scalar.get(),
                              H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(a.valid());
      a.reset();
      d.reset();
      img = ExtractFileImage(f.get());
    }
    ASSERT_FALSE(img.empty());

    ScopedFile re = OpenMemoryImage(img, H5F_ACC_RDWR, "raw_attr_re.h5");
    ASSERT_TRUE(re.valid());
    ScopedDataset d_re(H5Dopen2(re.get(), "d", H5P_DEFAULT));
    ASSERT_TRUE(d_re.valid());
    H5O_loc_t* loc = H5O_get_loc(d_re.get());
    ASSERT_NE(loc, nullptr);

    ScopedApiContext ctx;
    H5O_t* oh = H5O_pin(loc);
    ASSERT_NE(oh, nullptr);
    int attr_u = -1;
    for (unsigned u = 0; u < oh->nmesgs; ++u) {
      if (oh->mesg[u].type == H5O_MSG_ATTR) {
        attr_u = static_cast<int>(u);
        break;
      }
    }
    ASSERT_GE(attr_u, 0);
    ASSERT_NE(oh->mesg[attr_u].raw, nullptr);
    ASSERT_EQ(oh->mesg[attr_u].native, nullptr);

    const uint8_t orig_raw0 = oh->mesg[attr_u].raw[0];
    oh->mesg[attr_u].raw[0] = 0xff;
    absl::Cleanup restore = [&] {
      oh->mesg[attr_u].raw[0] = orig_raw0;
      H5O_unpin(oh);
    };

    ScopedErrorSilence silence;
    EXPECT_FALSE(ScopedAttr(H5Aopen(d_re.get(), "c0", H5P_DEFAULT)).valid());
    EXPECT_LT(H5Arename(d_re.get(), "c0", "c1"), 0);
    EXPECT_FALSE(ScopedAttr(H5Aopen_by_idx(d_re.get(), ".", H5_INDEX_NAME,
                                           H5_ITER_INC, 0, H5P_DEFAULT,
                                           H5P_DEFAULT))
                     .valid());
    EXPECT_LT(H5Adelete_by_idx(d_re.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                               H5P_DEFAULT),
              0);
    EXPECT_LT(H5Aexists(d_re.get(), "c0"), 0);
  }

  // 2) Move AINFO to uncached chunk 1 on v2 compact object (304) and on
  //    dense objects with H5Oset_comment (1382, 1389, 1508, 1594).
  {
    const MultiChunkImage img = BuildMultiChunkImage(
        "v2_ainfo_src.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST, scalar.get());
    ASSERT_FALSE(img.bytes.empty());

    ScopedFile re = OpenMemoryImage(img.bytes, H5F_ACC_RDWR, "v2_ainfo_re.h5");
    ASSERT_TRUE(re.valid());
    ScopedDataset d_re(H5Dopen2(re.get(), "d", H5P_DEFAULT));
    ASSERT_TRUE(d_re.valid());
    H5O_loc_t* loc = H5O_get_loc(d_re.get());
    ASSERT_NE(loc, nullptr);

    ScopedApiContext ctx;
    H5O_t* oh = H5O_pin(loc);
    ASSERT_NE(oh, nullptr);
    ASSERT_GT(oh->nchunks, 1u);
    int ainfo_u = -1;
    for (unsigned u = 0; u < oh->nmesgs; ++u) {
      if (oh->mesg[u].type == H5O_MSG_AINFO) {
        ainfo_u = static_cast<int>(u);
        break;
      }
    }
    ASSERT_GE(ainfo_u, 0);

    const unsigned orig_achk = oh->mesg[ainfo_u].chunkno;
    const haddr_t orig_chk1 = oh->chunk[1].addr;
    oh->mesg[ainfo_u].chunkno = 1;
    oh->chunk[1].addr = 0x999900;
    absl::Cleanup restore = [&] {
      oh->mesg[ainfo_u].chunkno = orig_achk;
      oh->chunk[1].addr = orig_chk1;
      H5O_unpin(oh);
    };

    ScopedErrorSilence silence;
    EXPECT_FALSE(ScopedAttr(H5Acreate2(d_re.get(), "c_new", H5T_NATIVE_INT,
                                       scalar.get(), H5P_DEFAULT, H5P_DEFAULT))
                     .valid());
  }
  {
    std::vector<uint8_t> dense_img;
    {
      ScopedFile f = CreateCoreFileWithLibver(
          "dense_ainfo_src.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
      ASSERT_TRUE(f.valid());
      ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
      ASSERT_TRUE(gcpl.valid());
      ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 2, 2), 0);
      ScopedGroup g(
          H5Gcreate2(f.get(), "g", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
      ASSERT_TRUE(g.valid());
      for (const char* nm : {"a0", "a1", "a2", "a3"}) {
        ScopedAttr a(H5Acreate2(g.get(), nm, H5T_NATIVE_INT, scalar.get(),
                                H5P_DEFAULT, H5P_DEFAULT));
        ASSERT_TRUE(a.valid());
      }
      // Large comment spills into chunk 1 on the group header.
      const std::string comment(1024, 'x');
      ASSERT_GE(H5Oset_comment(g.get(), comment.c_str()), 0);
      g.reset();
      dense_img = ExtractFileImage(f.get());
    }
    ASSERT_FALSE(dense_img.empty());

    ScopedFile re =
        OpenMemoryImage(dense_img, H5F_ACC_RDWR, "dense_ainfo_re.h5");
    ASSERT_TRUE(re.valid());
    ScopedGroup g(H5Gopen2(re.get(), "g", H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
    H5O_loc_t* gloc = H5O_get_loc(g.get());
    ASSERT_NE(gloc, nullptr);

    ScopedApiContext ctx;
    H5O_t* oh = H5O_pin(gloc);
    ASSERT_NE(oh, nullptr);
    ASSERT_GT(oh->nchunks, 1u);
    int ainfo_u = -1;
    for (unsigned u = 0; u < oh->nmesgs; ++u) {
      if (oh->mesg[u].type == H5O_MSG_AINFO) {
        ainfo_u = static_cast<int>(u);
        break;
      }
    }
    ASSERT_GE(ainfo_u, 0);

    const unsigned orig_achk = oh->mesg[ainfo_u].chunkno;
    const haddr_t orig_chk1 = oh->chunk[1].addr;
    oh->mesg[ainfo_u].chunkno = 1;
    oh->chunk[1].addr = 0x999900;
    absl::Cleanup restore = [&] {
      oh->mesg[ainfo_u].chunkno = orig_achk;
      oh->chunk[1].addr = orig_chk1;
      H5O_unpin(oh);
    };

    ScopedErrorSilence silence;
    // 4->3 >= min_dense(2): stays dense, fails H5O__msg_write_real(AINFO).
    EXPECT_LT(H5Adelete(g.get(), "a3"), 0);
    // 3->2 >= min_dense(2): stays dense, fails H5O__msg_write_real(AINFO).
    EXPECT_LT(H5Adelete_by_idx(g.get(), ".", H5_INDEX_NAME, H5_ITER_INC, 0,
                               H5P_DEFAULT),
              0);
  }
  {
    std::vector<uint8_t> rm_img;
    {
      ScopedFile f = CreateCoreFileWithLibver(
          "rm_ainfo_src.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST);
      ASSERT_TRUE(f.valid());
      ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
      ASSERT_TRUE(dcpl.valid());
      ASSERT_GE(H5Pset_attr_phase_change(dcpl.get(), 1, 1), 0);
      ScopedDataset d = CreateScalarIntDataset(f.get(), "d", dcpl.get());
      ASSERT_TRUE(d.valid());
      for (const char* nm : {"a0", "a1"}) {
        ScopedAttr a(H5Acreate2(d.get(), nm, H5T_NATIVE_INT, scalar.get(),
                                H5P_DEFAULT, H5P_DEFAULT));
        ASSERT_TRUE(a.valid());
      }
      ASSERT_GE(H5Adelete(d.get(), "a1"), 0);  // Dense with nattrs == 1.

      // Consume free chunk-0 NULL space with a comment so the small 4-byte
      // REFCOUNT message from H5Lcreate_hard lands in chunk 1 (smaller than
      // AINFO so H5O__condense_header tries to move it into AINFO's slot).
      size_t null_sz = 0;
      {
        H5O_loc_t* dloc = H5O_get_loc(d.get());
        ASSERT_NE(dloc, nullptr);
        ScopedApiContext ctx;
        H5O_t* oh = H5O_pin(dloc);
        ASSERT_NE(oh, nullptr);
        absl::Cleanup unpin = [&] { H5O_unpin(oh); };
        for (unsigned u = 0; u < oh->nmesgs; ++u) {
          if (oh->mesg[u].chunkno == 0 && oh->mesg[u].type == H5O_MSG_NULL) {
            null_sz += oh->mesg[u].raw_size;
          }
        }
      }
      if (null_sz >= 8) {
        const std::string pad_comment(null_sz - 4, 'x');
        ASSERT_GE(H5Oset_comment(d.get(), pad_comment.c_str()), 0);
      }
      ASSERT_GE(H5Lcreate_hard(f.get(), "d", f.get(), "d_hard", H5P_DEFAULT,
                               H5P_DEFAULT),
                0);
      d.reset();
      rm_img = ExtractFileImage(f.get());
    }
    ASSERT_FALSE(rm_img.empty());

    ScopedFile re = OpenMemoryImage(rm_img, H5F_ACC_RDWR, "rm_ainfo_re.h5");
    ASSERT_TRUE(re.valid());
    ScopedDataset d(H5Dopen2(re.get(), "d", H5P_DEFAULT));
    ASSERT_TRUE(d.valid());
    H5O_loc_t* dloc = H5O_get_loc(d.get());
    ASSERT_NE(dloc, nullptr);

    ScopedApiContext ctx;
    H5O_t* oh = H5O_pin(dloc);
    ASSERT_NE(oh, nullptr);
    ASSERT_GT(oh->nchunks, 1u);
    int ainfo_chk = -1;
    for (unsigned u = 0; u < oh->nmesgs; ++u) {
      if (oh->mesg[u].type == H5O_MSG_AINFO) {
        ainfo_chk = static_cast<int>(oh->mesg[u].chunkno);
        break;
      }
    }
    ASSERT_EQ(ainfo_chk, 0);

    const haddr_t orig_chk1 = oh->chunk[1].addr;
    // Leave AINFO in chunk 0 so msg_write succeeds, then msg_remove(AINFO)
    // frees chunk 0 space and H5O__condense_header moves REFCOUNT from chunk 1.
    oh->chunk[1].addr = 0x999900;
    absl::Cleanup restore = [&] {
      oh->chunk[1].addr = orig_chk1;
      H5O_unpin(oh);
    };

    ScopedErrorSilence silence;
    EXPECT_LT(H5Adelete(d.get(), "a0"), 0);
  }

  // 3) Compact->dense H5A__dense_create failure via maxaddr cap (265).
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(
        H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_NONE, false,
                                   1),
        0);
    ScopedFile f = CreateCoreFileWithLibver(
        "dense_create_fail.h5", H5F_LIBVER_LATEST, H5F_LIBVER_LATEST,
        fcpl.get());
    ASSERT_TRUE(f.valid());
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 1, 1), 0);
    ScopedGroup g(
        H5Gcreate2(f.get(), "g", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
    ScopedAttr k0(H5Acreate2(g.get(), "k0", H5T_NATIVE_INT, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(k0.valid());

    auto* fptr = static_cast<H5F_t*>(H5VL_object(f.get()));
    ASSERT_NE(fptr, nullptr);
    ASSERT_NE(fptr->shared->lf, nullptr);
    const haddr_t orig_maxaddr = fptr->shared->lf->maxaddr;
    fptr->shared->lf->maxaddr = 64;
    absl::Cleanup restore = [&] { fptr->shared->lf->maxaddr = orig_maxaddr; };

    ScopedErrorSilence silence;
    EXPECT_FALSE(ScopedAttr(H5Acreate2(g.get(), "k1", H5T_NATIVE_INT,
                                       scalar.get(), H5P_DEFAULT, H5P_DEFAULT))
                     .valid());
  }

  // 4) Compact->dense duplicate-name insert failure (169, 276).
  {
    ScopedFile f = CreateCoreFileWithLibver(
        "dense_cvt_dup.h5", H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
    ASSERT_TRUE(f.valid());
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 2, 2), 0);
    ScopedGroup g(
        H5Gcreate2(f.get(), "g", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
    ScopedAttr d0(H5Acreate2(g.get(), "d0", H5T_NATIVE_INT, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ScopedAttr d1(H5Acreate2(g.get(), "d1", H5T_NATIVE_INT, scalar.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(d0.valid());
    ASSERT_TRUE(d1.valid());

    H5O_loc_t* gloc = H5O_get_loc(g.get());
    ASSERT_NE(gloc, nullptr);
    ScopedApiContext ctx;
    H5O_t* oh = H5O_pin(gloc);
    ASSERT_NE(oh, nullptr);
    std::vector<H5A_t*> attrs;
    for (unsigned u = 0; u < oh->nmesgs; ++u) {
      if (oh->mesg[u].type == H5O_MSG_ATTR && oh->mesg[u].native != nullptr) {
        attrs.push_back(static_cast<H5A_t*>(oh->mesg[u].native));
      }
    }
    ASSERT_EQ(attrs.size(), 2u);
    char* orig_name = attrs[1]->shared->name;
    attrs[1]->shared->name = attrs[0]->shared->name;
    absl::Cleanup restore = [&] {
      attrs[1]->shared->name = orig_name;
      H5O_unpin(oh);
    };

    ScopedErrorSilence silence;
    EXPECT_FALSE(ScopedAttr(H5Acreate2(g.get(), "d2", H5T_NATIVE_INT,
                                       scalar.get(), H5P_DEFAULT, H5P_DEFAULT))
                     .valid());
  }

  // 5) Dense->compact H5O__msg_append_real failure for open (1364) and
  //    closed (1368) attributes via H5F_FSPACE_STRATEGY_NONE + maxaddr cap.
  for (bool keep_open : {true, false}) {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(
        H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_NONE, false,
                                   1),
        0);
    ScopedFile f = CreateCoreFileWithLibver(
        keep_open ? "rev_none_open.h5" : "rev_none_closed.h5", H5F_LIBVER_V18,
        H5F_LIBVER_LATEST, fcpl.get());
    ASSERT_TRUE(f.valid());
    ScopedPlist gcpl(H5Pcreate(H5P_GROUP_CREATE));
    ASSERT_TRUE(gcpl.valid());
    ASSERT_GE(H5Pset_attr_phase_change(gcpl.get(), 2, 2), 0);
    ScopedGroup g(
        H5Gcreate2(f.get(), "g", H5P_DEFAULT, gcpl.get(), H5P_DEFAULT));
    ASSERT_TRUE(g.valid());
    for (const char* nm : {"a1", "a2", "a3"}) {
      ScopedAttr a(H5Acreate2(g.get(), nm, H5T_NATIVE_INT, scalar.get(),
                              H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(a.valid());
    }
    const hsize_t big_dims[1] = {1000};
    ScopedSpace big_sp(H5Screate_simple(1, big_dims, nullptr));
    ASSERT_TRUE(big_sp.valid());
    ScopedAttr u0(H5Acreate2(g.get(), "u0", H5T_NATIVE_INT, big_sp.get(),
                             H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(u0.valid());
    if (!keep_open) {
      u0.reset();
    }
    ASSERT_GE(H5Adelete(g.get(), "a1"), 0);
    ASSERT_GE(H5Adelete(g.get(), "a2"), 0);  // Dense with nattrs == 2.
    ScopedDataset blocker = CreateScalarIntDataset(f.get(), "blocker");
    ASSERT_TRUE(blocker.valid());

    auto* fptr = static_cast<H5F_t*>(H5VL_object(f.get()));
    ASSERT_NE(fptr, nullptr);
    ASSERT_NE(fptr->shared->lf, nullptr);
    const haddr_t orig_maxaddr = fptr->shared->lf->maxaddr;
    fptr->shared->lf->maxaddr = 64;
    absl::Cleanup restore = [&] { fptr->shared->lf->maxaddr = orig_maxaddr; };

    ScopedErrorSilence silence;
    EXPECT_LT(H5Adelete(g.get(), "a3"), 0);
  }

  // 6) First H5O__msg_append_real(AINFO) failure via maxaddr cap (300).
  {
    ScopedPlist fcpl(H5Pcreate(H5P_FILE_CREATE));
    ASSERT_TRUE(fcpl.valid());
    ASSERT_GE(
        H5Pset_file_space_strategy(fcpl.get(), H5F_FSPACE_STRATEGY_NONE, false,
                                   1),
        0);
    ScopedFile f = CreateCoreFileWithLibver(
        "new_ainfo_fail.h5", H5F_LIBVER_V18, H5F_LIBVER_LATEST, fcpl.get());
    ASSERT_TRUE(f.valid());
    ScopedDataset d = CreateScalarIntDataset(f.get(), "d");
    ASSERT_TRUE(d.valid());
    // Fill chunk 0's initial free NULL space with a comment before blocking
    // extension, so appending AINFO requires allocating a new chunk.
    size_t null_sz = 0;
    {
      H5O_loc_t* dloc = H5O_get_loc(d.get());
      ASSERT_NE(dloc, nullptr);
      ScopedApiContext ctx;
      H5O_t* oh = H5O_pin(dloc);
      ASSERT_NE(oh, nullptr);
      absl::Cleanup unpin = [&] { H5O_unpin(oh); };
      for (unsigned u = 0; u < oh->nmesgs; ++u) {
        if (oh->mesg[u].chunkno == 0 && oh->mesg[u].type == H5O_MSG_NULL) {
          null_sz += oh->mesg[u].raw_size;
        }
      }
    }
    if (null_sz >= 8) {
      const std::string fill_comment(null_sz - 4, 'x');
      ASSERT_GE(H5Oset_comment(d.get(), fill_comment.c_str()), 0);
    }
    ScopedDataset blocker = CreateScalarIntDataset(f.get(), "blocker");
    ASSERT_TRUE(blocker.valid());

    auto* fptr = static_cast<H5F_t*>(H5VL_object(f.get()));
    ASSERT_NE(fptr, nullptr);
    ASSERT_NE(fptr->shared->lf, nullptr);
    const haddr_t orig_maxaddr = fptr->shared->lf->maxaddr;
    fptr->shared->lf->maxaddr = 64;
    absl::Cleanup restore = [&] { fptr->shared->lf->maxaddr = orig_maxaddr; };

    ScopedErrorSilence silence;
    EXPECT_FALSE(ScopedAttr(H5Acreate2(d.get(), "a0", H5T_NATIVE_INT,
                                       scalar.get(), H5P_DEFAULT, H5P_DEFAULT))
                     .valid());
  }

  // 7) H5O__attr_to_dense_cb -> H5O__release_mesg fail on uncached chk1 (174).
  {
    const MultiChunkImage img =
        BuildMultiChunkImage("v2_mc45_src.h5", H5F_LIBVER_V18,
                             H5F_LIBVER_LATEST, scalar.get(), 45, 2);
    ASSERT_FALSE(img.bytes.empty());

    ScopedFile re = OpenMemoryImage(img.bytes, H5F_ACC_RDWR, "v2_mc45_re.h5");
    ASSERT_TRUE(re.valid());
    ScopedDataset d_re(H5Dopen2(re.get(), "d", H5P_DEFAULT));
    ASSERT_TRUE(d_re.valid());
    H5O_loc_t* loc = H5O_get_loc(d_re.get());
    ASSERT_NE(loc, nullptr);

    ScopedApiContext ctx;
    H5O_t* oh = H5O_pin(loc);
    ASSERT_NE(oh, nullptr);
    ASSERT_GT(oh->nchunks, 1u);
    const haddr_t orig_chk1 = oh->chunk[1].addr;
    oh->chunk[1].addr = 0x999900;
    absl::Cleanup restore = [&] {
      oh->chunk[1].addr = orig_chk1;
      H5O_unpin(oh);
    };

    ScopedErrorSilence silence;
    EXPECT_FALSE(ScopedAttr(H5Acreate2(d_re.get(), "m_45", H5T_NATIVE_INT,
                                       scalar.get(), H5P_DEFAULT, H5P_DEFAULT))
                     .valid());
  }
}

}  // namespace
}  // namespace hdf5
