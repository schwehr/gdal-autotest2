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
// Tests reference datatype location tracking, memory/disk conversions,
// compatibility object/region reference conversions, background blob deletion,
// and reference reclamation (third_party/hdf5/src/H5Tref.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tref.c

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` compiles with -DNDEBUG; define NDEBUG around the internal prelude and
// headers so struct layouts (e.g., H5T_t, H5F_t, H5VL_object_t) match `:hdf5`.
#ifndef NDEBUG
#define H5TREF_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
// H5private.h skips typedef'ing H5__Float16 under __cplusplus, whereas H5Tpkg.h
// declares extern H5__Float16 constants under #ifdef H5_HAVE__FLOAT16.
// Temporarily undef H5_HAVE__FLOAT16 around H5Tpkg.h; no struct layouts depend
// on it.
#ifdef H5_HAVE__FLOAT16
#undef H5_HAVE__FLOAT16
#define H5TREF_TEST_RESTORE_FLOAT16 1
#endif
extern "C" {
#define H5T_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Tpkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5TREF_TEST_RESTORE_FLOAT16
#define H5_HAVE__FLOAT16 1
#undef H5TREF_TEST_RESTORE_FLOAT16
#endif
#ifdef H5TREF_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TREF_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

void CreateTargetObjects(hid_t file, hid_t* out_region_space) {
  ScopedGroup grp(H5Gcreate2(file, "/grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());

  constexpr std::array<hsize_t, 2> kDims = {4, 4};
  ScopedSpace space(H5Screate_simple(2, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  ScopedDataset dset(H5Dcreate2(file, "/dset", H5T_NATIVE_INT32, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  std::array<int32_t, 16> data{};
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<int32_t>(i);
  }
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, data.data()), 0);

  ScopedSpace attr_space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(attr_space.valid());
  ScopedAttr attr(H5Acreate2(dset, "tag", H5T_NATIVE_INT32, attr_space, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());
  int32_t attr_val = 42;
  ASSERT_GE(H5Awrite(attr, H5T_NATIVE_INT32, &attr_val), 0);

  constexpr std::array<hsize_t, 2> kStart = {1, 1};
  constexpr std::array<hsize_t, 2> kCount = {2, 2};
  ASSERT_GE(H5Sselect_hyperslab(space, H5S_SELECT_SET, kStart.data(), nullptr, kCount.data(), nullptr), 0);
  *out_region_space = space.release();
}

TEST(H5TrefTest, InternalReferencesRoundTripInDatasetAndAttribute) {
  // 1. Create target group, dataset, attribute, and region selection.
  ScopedFile file = CreateInMemoryFile("ref_internal.h5");
  ASSERT_TRUE(file.valid());
  hid_t raw_reg_space = H5I_INVALID_HID;
  CreateTargetObjects(file, &raw_reg_space);
  ScopedSpace reg_space(raw_reg_space);
  ASSERT_TRUE(reg_space.valid());

  // 2. Build null, object, region, and attribute references.
  std::array<H5R_ref_t, 4> refs{};
  absl::Cleanup destroy_refs = [&refs] {
    for (auto& r : refs) {
      H5Rdestroy(&r);
    }
  };
  ASSERT_GE(H5Rcreate_object(file, "/grp", H5P_DEFAULT, &refs[1]), 0);
  ASSERT_GE(H5Rcreate_region(file, "/dset", reg_space, H5P_DEFAULT, &refs[2]), 0);
  ASSERT_GE(H5Rcreate_attr(file, "/dset", "tag", H5P_DEFAULT, &refs[3]), 0);

  constexpr std::array<hsize_t, 1> kRefDims = {4};
  ScopedSpace ref_space(H5Screate_simple(1, kRefDims.data(), nullptr));
  ASSERT_TRUE(ref_space.valid());

  // 3. Write and read back via dataset (triggers mem->disk and reverse disk->mem passes).
  ScopedDataset ref_dset(H5Dcreate2(file, "/refs", H5T_STD_REF, ref_space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ref_dset.valid());
  ASSERT_GE(H5Dwrite(ref_dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT, refs.data()), 0);

  // Also write to an attribute on /grp.
  ScopedGroup grp(H5Gopen2(file, "/grp", H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedAttr ref_attr(H5Acreate2(grp, "refs_attr", H5T_STD_REF, ref_space, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ref_attr.valid());
  ASSERT_GE(H5Awrite(ref_attr, H5T_STD_REF, refs.data()), 0);

  std::array<H5R_ref_t, 4> read_dset_refs{};
  absl::Cleanup reclaim_dset_refs = [&] {
    H5Treclaim(H5T_STD_REF, ref_space, H5P_DEFAULT, read_dset_refs.data());
  };
  ASSERT_GE(H5Dread(ref_dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT, read_dset_refs.data()), 0);

  const H5R_ref_t zero_ref{};
  EXPECT_EQ(std::memcmp(&read_dset_refs[0], &zero_ref, sizeof(zero_ref)), 0);
  EXPECT_EQ(H5Rget_type(&read_dset_refs[1]), H5R_OBJECT2);
  EXPECT_EQ(H5Rget_type(&read_dset_refs[2]), H5R_DATASET_REGION2);
  EXPECT_EQ(H5Rget_type(&read_dset_refs[3]), H5R_ATTR);
  EXPECT_GT(H5Requal(&refs[1], &read_dset_refs[1]), 0);
  EXPECT_GT(H5Requal(&refs[3], &read_dset_refs[3]), 0);
  {
    ScopedSpace opened_reg(H5Ropen_region(&read_dset_refs[2], H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(opened_reg.valid());
    EXPECT_EQ(H5Sget_select_npoints(opened_reg), 4);
  }

  std::array<H5R_ref_t, 4> read_attr_refs{};
  absl::Cleanup reclaim_attr_refs = [&] {
    H5Treclaim(H5T_STD_REF, ref_space, H5P_DEFAULT, read_attr_refs.data());
  };
  ASSERT_GE(H5Aread(ref_attr, H5T_STD_REF, read_attr_refs.data()), 0);
  EXPECT_EQ(std::memcmp(&read_attr_refs[0], &zero_ref, sizeof(zero_ref)), 0);
  EXPECT_EQ(H5Rget_type(&read_attr_refs[2]), H5R_DATASET_REGION2);
  EXPECT_GT(H5Requal(&refs[3], &read_attr_refs[3]), 0);
  {
    ScopedSpace opened_attr_reg(H5Ropen_region(&read_attr_refs[2], H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(opened_attr_reg.valid());
    EXPECT_EQ(H5Sget_select_npoints(opened_attr_reg), 4);
  }
}

TEST(H5TrefTest, ExternalReferencesShortAndLongFilenames) {
  // 1. Create a short-named source file and a long-named (>255 chars) source file
  //    to exercise both the static and dynamic filename buffers in H5Tref.c.
  ScopedFile short_src = CreateInMemoryFile("short_src.h5");
  ASSERT_TRUE(short_src.valid());
  hid_t raw_short_reg = H5I_INVALID_HID;
  CreateTargetObjects(short_src, &raw_short_reg);
  ScopedSpace short_reg(raw_short_reg);
  ASSERT_TRUE(short_reg.valid());

  const std::string long_name = std::string(280, 'r') + ".h5";
  ScopedFile long_src = CreateInMemoryFile(long_name.c_str());
  ASSERT_TRUE(long_src.valid());
  hid_t raw_long_reg = H5I_INVALID_HID;
  CreateTargetObjects(long_src, &raw_long_reg);
  ScopedSpace long_reg(raw_long_reg);
  ASSERT_TRUE(long_reg.valid());

  ScopedFile dst_file = CreateInMemoryFile("ext_dst.h5");
  ASSERT_TRUE(dst_file.valid());

  // 2. Build external object, region, and attribute references.
  std::array<H5R_ref_t, 4> ext_refs{};
  absl::Cleanup destroy_ext = [&ext_refs] {
    for (auto& r : ext_refs) {
      H5Rdestroy(&r);
    }
  };
  ASSERT_GE(H5Rcreate_object(short_src, "/grp", H5P_DEFAULT, &ext_refs[0]), 0);
  ASSERT_GE(H5Rcreate_region(long_src, "/dset", long_reg, H5P_DEFAULT, &ext_refs[1]), 0);
  ASSERT_GE(H5Rcreate_attr(long_src, "/dset", "tag", H5P_DEFAULT, &ext_refs[2]), 0);
  ASSERT_GE(H5Rcreate_object(long_src, "/grp", H5P_DEFAULT, &ext_refs[3]), 0);

  constexpr std::array<hsize_t, 1> kDims = {4};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(dst_file, "/ext_refs", H5T_STD_REF, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ASSERT_GE(H5Dwrite(dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT, ext_refs.data()), 0);

  // 3. Read external references back and verify stored filenames.
  std::array<H5R_ref_t, 4> read_ext{};
  absl::Cleanup destroy_read_ext = [&read_ext] {
    for (auto& r : read_ext) {
      H5Rdestroy(&r);
    }
  };
  ASSERT_GE(H5Dread(dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT, read_ext.data()), 0);

  EXPECT_EQ(H5Rget_type(&read_ext[0]), H5R_OBJECT2);
  EXPECT_EQ(H5Rget_type(&read_ext[1]), H5R_DATASET_REGION2);
  EXPECT_EQ(H5Rget_type(&read_ext[2]), H5R_ATTR);
  EXPECT_EQ(H5Rget_type(&read_ext[3]), H5R_OBJECT2);

  std::array<char, 32> short_buf{};
  ASSERT_GT(H5Rget_file_name(&read_ext[0], short_buf.data(), short_buf.size()), 0);
  EXPECT_STREQ(short_buf.data(), "short_src.h5");

  std::string long_buf(long_name.size() + 1, '\0');
  ASSERT_GT(H5Rget_file_name(&read_ext[1], long_buf.data(), long_buf.size()), 0);
  EXPECT_STREQ(long_buf.c_str(), long_name.c_str());

  // 4. Re-writing a decoded external ref whose loc_id is H5I_INVALID_HID into
  //    another file fails cleanly in H5T__ref_mem_getsize.
  ScopedErrorSilence silence;
  ScopedFile dst_file2 = CreateInMemoryFile("ext_dst2.h5");
  ASSERT_TRUE(dst_file2.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedDataset dset2(H5Dcreate2(dst_file2, "/bad_ref", H5T_STD_REF, scalar, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset2.valid());
  EXPECT_LT(H5Dwrite(dset2, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT, &read_ext[0]), 0);
}

TEST(H5TrefTest, CompoundOverwriteTriggersBackgroundBlobDeletion) {
  struct CompoundRef {
    int32_t tag;
    H5R_ref_t ref;
  };

  ScopedFile file = CreateInMemoryFile("ref_compound_bg.h5");
  ASSERT_TRUE(file.valid());
  hid_t raw_reg = H5I_INVALID_HID;
  CreateTargetObjects(file, &raw_reg);
  ScopedSpace reg_space(raw_reg);
  ASSERT_TRUE(reg_space.valid());

  // Build memory and file compound types containing a reference field.
  ScopedType mem_comp(H5Tcreate(H5T_COMPOUND, sizeof(CompoundRef)));
  ASSERT_TRUE(mem_comp.valid());
  ASSERT_GE(H5Tinsert(mem_comp, "tag", HOFFSET(CompoundRef, tag), H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(mem_comp, "ref", HOFFSET(CompoundRef, ref), H5T_STD_REF), 0);

  const size_t disk_ref_size = H5Tget_size(H5T_STD_I32LE);
  ScopedType file_comp(H5Tcreate(H5T_COMPOUND, disk_ref_size + H5Tget_size(H5T_STD_REF)));
  ASSERT_TRUE(file_comp.valid());
  ASSERT_GE(H5Tinsert(file_comp, "tag", 0, H5T_STD_I32LE), 0);
  ASSERT_GE(H5Tinsert(file_comp, "ref", disk_ref_size, H5T_STD_REF), 0);

  H5R_ref_t reg_ref{};
  H5R_ref_t attr_ref{};
  absl::Cleanup destroy_src = [&] {
    H5Rdestroy(&reg_ref);
    H5Rdestroy(&attr_ref);
  };
  ASSERT_GE(H5Rcreate_region(file, "/dset", reg_space, H5P_DEFAULT, &reg_ref), 0);
  ASSERT_GE(H5Rcreate_attr(file, "/dset", "tag", H5P_DEFAULT, &attr_ref), 0);

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedGroup grp(H5Gopen2(file, "/grp", H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedAttr attr(H5Acreate2(grp, "comp_ref", file_comp, scalar, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(attr.valid());

  // 1. Write non-null region reference (stores blob on disk).
  CompoundRef row{};
  row.tag = 1;
  ASSERT_GE(H5Rcopy(&reg_ref, &row.ref), 0);
  ASSERT_GE(H5Awrite(attr, mem_comp, &row), 0);
  H5Rdestroy(&row.ref);

  // 2. Overwrite with attribute reference -> H5T__ref_disk_write receives non-null bg_buf
  //    and deletes the previous region blob.
  row.tag = 2;
  std::memset(&row.ref, 0, sizeof(row.ref));
  ASSERT_GE(H5Rcopy(&attr_ref, &row.ref), 0);
  ASSERT_GE(H5Awrite(attr, mem_comp, &row), 0);
  H5Rdestroy(&row.ref);

  // 3. Overwrite with null reference -> H5T__ref_disk_setnull receives non-null bg_buf
  //    and deletes the previous attribute blob.
  row.tag = 3;
  std::memset(&row.ref, 0, sizeof(row.ref));
  ASSERT_GE(H5Awrite(attr, mem_comp, &row), 0);

  CompoundRef read_back{};
  absl::Cleanup reclaim_comp = [&] {
    H5Treclaim(mem_comp, scalar, H5P_DEFAULT, &read_back);
  };
  ASSERT_GE(H5Aread(attr, mem_comp, &read_back), 0);
  EXPECT_EQ(read_back.tag, 3);
  const H5R_ref_t zero_ref{};
  EXPECT_EQ(std::memcmp(&read_back.ref, &zero_ref, sizeof(zero_ref)), 0);
}

TEST(H5TrefTest, DeprecatedObjectAndRegionReferencesReadAsStdRef) {
  ScopedFile file = CreateInMemoryFile("ref_compat.h5");
  ASSERT_TRUE(file.valid());
  hid_t raw_reg = H5I_INVALID_HID;
  CreateTargetObjects(file, &raw_reg);
  ScopedSpace reg_space(raw_reg);
  ASSERT_TRUE(reg_space.valid());

  constexpr std::array<hsize_t, 1> kDims = {2};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  // 1. Legacy H5T_STD_REF_OBJ (nil + valid) read back as H5T_STD_REF.
  std::array<hobj_ref_t, 2> obj_refs{};
  ASSERT_GE(H5Rcreate(&obj_refs[1], file, "/grp", H5R_OBJECT, -1), 0);
  ScopedDataset obj_dset(
      H5Dcreate2(file, "/old_obj", H5T_STD_REF_OBJ, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(obj_dset.valid());
  ASSERT_GE(H5Dwrite(obj_dset, H5T_STD_REF_OBJ, H5S_ALL, H5S_ALL, H5P_DEFAULT, obj_refs.data()), 0);

  std::array<H5R_ref_t, 2> new_obj_refs{};
  absl::Cleanup reclaim_obj = [&] {
    H5Treclaim(H5T_STD_REF, space, H5P_DEFAULT, new_obj_refs.data());
  };
  ASSERT_GE(H5Dread(obj_dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT, new_obj_refs.data()), 0);
  const H5R_ref_t zero_ref{};
  EXPECT_EQ(std::memcmp(&new_obj_refs[0], &zero_ref, sizeof(zero_ref)), 0);
  EXPECT_EQ(H5Rget_type(&new_obj_refs[1]), H5R_OBJECT2);
  ScopedObject opened_grp(H5Ropen_object(&new_obj_refs[1], H5P_DEFAULT, H5P_DEFAULT));
  EXPECT_TRUE(opened_grp.valid());

  // Reclaim on non-opaque H5T_STD_REF_OBJ is a safe no-op.
  EXPECT_GE(H5Treclaim(H5T_STD_REF_OBJ, space, H5P_DEFAULT, obj_refs.data()), 0);

  // 2. Legacy H5T_STD_REF_DSETREG (nil + valid) read back as H5T_STD_REF.
  std::array<hdset_reg_ref_t, 2> reg_refs{};
  ASSERT_GE(H5Rcreate(&reg_refs[1], file, "/dset", H5R_DATASET_REGION, reg_space), 0);
  ScopedDataset reg_dset(
      H5Dcreate2(file, "/old_reg", H5T_STD_REF_DSETREG, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(reg_dset.valid());
  ASSERT_GE(H5Dwrite(reg_dset, H5T_STD_REF_DSETREG, H5S_ALL, H5S_ALL, H5P_DEFAULT, reg_refs.data()), 0);

  std::array<H5R_ref_t, 2> new_reg_refs{};
  absl::Cleanup reclaim_reg = [&] {
    H5Treclaim(H5T_STD_REF, space, H5P_DEFAULT, new_reg_refs.data());
  };
  ASSERT_GE(H5Dread(reg_dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT, new_reg_refs.data()), 0);
  EXPECT_EQ(std::memcmp(&new_reg_refs[0], &zero_ref, sizeof(zero_ref)), 0);
  EXPECT_EQ(H5Rget_type(&new_reg_refs[1]), H5R_DATASET_REGION2);
  ScopedSpace opened_reg(H5Ropen_region(&new_reg_refs[1], H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(opened_reg.valid());
  EXPECT_EQ(H5Sget_select_npoints(opened_reg), 4);
}

TEST(H5TrefTest, InMemoryConvertAndVlenOfReferences) {
  ScopedFile file = CreateInMemoryFile("ref_vlen.h5");
  ASSERT_TRUE(file.valid());
  hid_t raw_reg = H5I_INVALID_HID;
  CreateTargetObjects(file, &raw_reg);
  ScopedSpace reg_space(raw_reg);
  ASSERT_TRUE(reg_space.valid());

  H5R_ref_t obj_ref{};
  H5R_ref_t attr_ref{};
  absl::Cleanup destroy_refs = [&] {
    H5Rdestroy(&obj_ref);
    H5Rdestroy(&attr_ref);
  };
  ASSERT_GE(H5Rcreate_object(file, "/grp", H5P_DEFAULT, &obj_ref), 0);
  ASSERT_GE(H5Rcreate_attr(file, "/dset", "tag", H5P_DEFAULT, &attr_ref), 0);

  // 1. In-memory H5Tconvert exercises dst_file == NULL and src_file == NULL paths.
  H5R_ref_t conv_ref{};
  ASSERT_GE(H5Rcopy(&attr_ref, &conv_ref), 0);
  absl::Cleanup destroy_conv = [&] { H5Rdestroy(&conv_ref); };
  EXPECT_GE(H5Tconvert(H5T_STD_REF, H5T_STD_REF, 1, &conv_ref, nullptr, H5P_DEFAULT), 0);
  EXPECT_GT(H5Requal(&conv_ref, &attr_ref), 0);

  // 2. VLEN sequence of H5T_STD_REF round-trips through a dataset.
  ScopedType vlen_ref_type(H5Tvlen_create(H5T_STD_REF));
  ASSERT_TRUE(vlen_ref_type.valid());
  std::array<H5R_ref_t, 2> seq{};
  ASSERT_GE(H5Rcopy(&obj_ref, &seq[0]), 0);
  ASSERT_GE(H5Rcopy(&attr_ref, &seq[1]), 0);
  absl::Cleanup destroy_seq = [&] {
    for (auto& r : seq) {
      H5Rdestroy(&r);
    }
  };
  hvl_t write_vl{.len = seq.size(), .p = seq.data()};

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedDataset dset(
      H5Dcreate2(file, "/vl_refs", vlen_ref_type, scalar, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  ASSERT_GE(H5Dwrite(dset, vlen_ref_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, &write_vl), 0);

  hvl_t read_vl{};
  absl::Cleanup reclaim_vl = [&] {
    H5Treclaim(vlen_ref_type, scalar, H5P_DEFAULT, &read_vl);
  };
  ASSERT_GE(H5Dread(dset, vlen_ref_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, &read_vl), 0);
  ASSERT_EQ(read_vl.len, 2u);
  const auto* read_seq = static_cast<const H5R_ref_t*>(read_vl.p);
  EXPECT_GT(H5Requal(&read_seq[0], &obj_ref), 0);
  EXPECT_GT(H5Requal(&read_seq[1], &attr_ref), 0);
}

TEST(H5TrefTest, InternalSetLocAndErrorPaths) {
  ScopedFile file = CreateInMemoryFile("ref_internal_api.h5");
  ASSERT_TRUE(file.valid());
  H5VL_object_t* file_vol = H5VL_vol_object(file.get());
  ASSERT_NE(file_vol, nullptr);

  // Push an HDF5 API context so internal VOL calls (e.g. H5VL_file_get and
  // H5VL_blob_*) can set up their wrapper state cleanly.
  H5CX_node_t api_ctx{};
  ASSERT_GE(H5CX_push(&api_ctx), 0);
  absl::Cleanup pop_cx = [] { H5CX_pop(false); };

  // 1. Test H5T__ref_set_loc transitions for H5T_STD_REF, H5T_STD_REF_OBJ, H5T_STD_REF_DSETREG.
  for (hid_t base_id : {H5T_STD_REF, H5T_STD_REF_OBJ, H5T_STD_REF_DSETREG}) {
    ScopedType dt_id(H5Tcopy(base_id));
    ASSERT_TRUE(dt_id.valid());
    H5T_t* dt = static_cast<H5T_t*>(H5I_object_verify(dt_id.get(), H5I_DATATYPE));
    ASSERT_NE(dt, nullptr);

    // Same location returns false (0).
    EXPECT_EQ(H5T__ref_set_loc(dt, nullptr, H5T_LOC_MEMORY), 0);

    // Transition MEMORY -> DISK acquires owned_vol_obj and sets disk class.
    EXPECT_GT(H5T__ref_set_loc(dt, file_vol, H5T_LOC_DISK), 0);
    EXPECT_NE(dt->shared->owned_vol_obj, nullptr);
    EXPECT_NE(dt->shared->u.atomic.u.r.cls, nullptr);

    // Transition DISK -> MEMORY releases owned_vol_obj.
    EXPECT_GT(H5T__ref_set_loc(dt, nullptr, H5T_LOC_MEMORY), 0);
    EXPECT_EQ(dt->shared->owned_vol_obj, nullptr);

    // Transition to H5T_LOC_BADLOC resets file and class pointers.
    EXPECT_GT(H5T__ref_set_loc(dt, nullptr, H5T_LOC_BADLOC), 0);
    EXPECT_EQ(dt->shared->u.atomic.u.r.cls, nullptr);

    // Restore H5T_LOC_MEMORY before closing.
    EXPECT_GT(H5T__ref_set_loc(dt, nullptr, H5T_LOC_MEMORY), 0);
  }

  // 2. Test H5T__ref_set_loc failure branches with ScopedErrorSilence.
  ScopedErrorSilence silence;
  ScopedType dt_id(H5Tcopy(H5T_STD_REF));
  ASSERT_TRUE(dt_id.valid());
  H5T_t* dt = static_cast<H5T_t*>(H5I_object_verify(dt_id.get(), H5I_DATATYPE));
  ASSERT_NE(dt, nullptr);

  EXPECT_LT(H5T__ref_set_loc(dt, nullptr, H5T_LOC_MAXLOC), 0);

  // Non-opaque reference with H5R_OBJECT2 fails in H5T_LOC_MEMORY.
  dt->shared->u.atomic.u.r.loc = H5T_LOC_BADLOC;
  dt->shared->u.atomic.u.r.opaque = false;
  dt->shared->u.atomic.u.r.rtype = H5R_OBJECT2;
  EXPECT_LT(H5T__ref_set_loc(dt, nullptr, H5T_LOC_MEMORY), 0);
  dt->shared->u.atomic.u.r.opaque = true;
  ASSERT_GE(H5T__ref_set_loc(dt, nullptr, H5T_LOC_MEMORY), 0);

  // 3. Test disk/mem class callback error branches directly.
  ScopedType disk_dt_id(H5Tcopy(H5T_STD_REF));
  ASSERT_TRUE(disk_dt_id.valid());
  H5T_t* disk_dt = static_cast<H5T_t*>(H5I_object_verify(disk_dt_id.get(), H5I_DATATYPE));
  ASSERT_NE(disk_dt, nullptr);
  ASSERT_GT(H5T__ref_set_loc(disk_dt, file_vol, H5T_LOC_DISK), 0);
  const H5T_ref_class_t* disk_cls = disk_dt->shared->u.atomic.u.r.cls;
  ASSERT_NE(disk_cls, nullptr);

  std::array<uint8_t, 32> bad_disk_buf{};
  bool dst_copy = false;
  bad_disk_buf[0] = static_cast<uint8_t>(H5R_MAXTYPE);
  EXPECT_EQ(disk_cls->getsize(file_vol, bad_disk_buf.data(), disk_dt->shared->size, nullptr, &dst_copy), 0u);

  const H5T_ref_class_t* mem_cls = dt->shared->u.atomic.u.r.cls;
  ASSERT_NE(mem_cls, nullptr);
  H5R_ref_t out_ref{};
  std::array<uint8_t, 8> bad_encoded{};
  bad_encoded[0] = static_cast<uint8_t>(H5R_MAXTYPE);
  EXPECT_LT(mem_cls->write(file_vol, bad_encoded.data(), bad_encoded.size(), H5R_OBJECT2, nullptr, &out_ref,
                           sizeof(out_ref), nullptr),
            0);

  // Blob size mismatch in H5T__ref_disk_read returns negative error.
  std::array<uint8_t, 4> payload = {1, 2, 3, 4};
  std::array<uint8_t, 32> disk_blob{};
  ASSERT_GE(disk_cls->write(nullptr, payload.data(), payload.size(), H5R_ATTR, file_vol, disk_blob.data(),
                            disk_dt->shared->size, nullptr),
            0);
  std::array<uint8_t, 16> wrong_read_buf{};
  EXPECT_LT(disk_cls->read(file_vol, disk_blob.data(), disk_dt->shared->size, nullptr, wrong_read_buf.data(),
                           wrong_read_buf.size()),
            0);
  // 4. Test mem_cls->write (H5R_DATASET_REGION2, H5R_MAXTYPE), mem_cls->read on
  //    a decoded external reference, and compat disk read decode failures.
  hid_t raw_reg = H5I_INVALID_HID;
  CreateTargetObjects(file, &raw_reg);
  ScopedSpace reg_space(raw_reg);
  ASSERT_TRUE(reg_space.valid());
  H5R_ref_t reg_ref{};
  absl::Cleanup destroy_reg_ref = [&] { H5Rdestroy(&reg_ref); };
  ASSERT_GE(H5Rcreate_region(file, "/dset", reg_space, H5P_DEFAULT, &reg_ref), 0);

  const size_t enc_reg_len = mem_cls->getsize(nullptr, &reg_ref, sizeof(reg_ref), file_vol, &dst_copy);
  ASSERT_GT(enc_reg_len, 0u);
  std::vector<uint8_t> enc_reg(enc_reg_len, 0);
  ASSERT_GE(mem_cls->read(nullptr, &reg_ref, sizeof(reg_ref), file_vol, enc_reg.data(), enc_reg.size()), 0);

  H5R_ref_t decoded_reg_ref{};
  absl::Cleanup destroy_decoded_reg = [&] { H5Rdestroy(&decoded_reg_ref); };
  EXPECT_GE(mem_cls->write(file_vol, enc_reg.data(), enc_reg.size(), H5R_DATASET_REGION2, nullptr,
                           &decoded_reg_ref, sizeof(decoded_reg_ref), nullptr),
            0);
  EXPECT_EQ(H5Rget_type(&decoded_reg_ref), H5R_DATASET_REGION2);

  H5R_ref_t bad_type_ref{};
  EXPECT_LT(mem_cls->write(file_vol, enc_reg.data(), enc_reg.size(), H5R_MAXTYPE, nullptr, &bad_type_ref,
                           sizeof(bad_type_ref), nullptr),
            0);

  // Decoded external reference has loc_id == H5I_INVALID_HID -> mem_cls->read fails.
  ScopedFile ext_holder = CreateInMemoryFile("ref_ext_holder.h5");
  ASSERT_TRUE(ext_holder.valid());
  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  ScopedDataset ext_dset(
      H5Dcreate2(ext_holder, "/ext", H5T_STD_REF, scalar, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(ext_dset.valid());
  ASSERT_GE(H5Dwrite(ext_dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT, &reg_ref), 0);
  H5R_ref_t decoded_ext_ref{};
  absl::Cleanup destroy_decoded_ext = [&] { H5Rdestroy(&decoded_ext_ref); };
  ASSERT_GE(H5Dread(ext_dset, H5T_STD_REF, H5S_ALL, H5S_ALL, H5P_DEFAULT, &decoded_ext_ref), 0);
  std::array<uint8_t, 64> tmp_read_buf{};
  EXPECT_EQ(mem_cls->read(nullptr, &decoded_ext_ref, sizeof(decoded_ext_ref), file_vol, tmp_read_buf.data(),
                          tmp_read_buf.size()),
            0);

  // Compat H5T_STD_REF_OBJ and H5T_STD_REF_DSETREG disk decode error paths.
  std::array<uint8_t, 32> zero_src{};
  {
    ScopedType obj_dt_id(H5Tcopy(H5T_STD_REF_OBJ));
    ASSERT_TRUE(obj_dt_id.valid());
    H5T_t* obj_dt = static_cast<H5T_t*>(H5I_object_verify(obj_dt_id.get(), H5I_DATATYPE));
    ASSERT_NE(obj_dt, nullptr);
    ASSERT_GT(H5T__ref_set_loc(obj_dt, file_vol, H5T_LOC_DISK), 0);
    absl::Cleanup restore_obj_loc = [&] { H5T__ref_set_loc(obj_dt, nullptr, H5T_LOC_MEMORY); };
    H5O_token_t out_tok{};
    EXPECT_LT(obj_dt->shared->u.atomic.u.r.cls->read(file_vol, zero_src.data(), 0, nullptr, &out_tok,
                                                     sizeof(out_tok)),
              0);
  }
  {
    ScopedType reg_dt_id(H5Tcopy(H5T_STD_REF_DSETREG));
    ASSERT_TRUE(reg_dt_id.valid());
    H5T_t* reg_dt = static_cast<H5T_t*>(H5I_object_verify(reg_dt_id.get(), H5I_DATATYPE));
    ASSERT_NE(reg_dt, nullptr);
    ASSERT_GT(H5T__ref_set_loc(reg_dt, file_vol, H5T_LOC_DISK), 0);
    absl::Cleanup restore_reg_loc = [&] { H5T__ref_set_loc(reg_dt, nullptr, H5T_LOC_MEMORY); };
    const size_t reg_mem_sz =
        reg_dt->shared->u.atomic.u.r.cls->getsize(file_vol, zero_src.data(), 0, nullptr, &dst_copy);
    ASSERT_GT(reg_mem_sz, 0u);
    std::vector<uint8_t> reg_mem_out(reg_mem_sz, 0);
    EXPECT_LT(reg_dt->shared->u.atomic.u.r.cls->read(file_vol, zero_src.data(), 0, nullptr,
                                                     reg_mem_out.data(), reg_mem_out.size()),
              0);
  }
  EXPECT_GE(H5T__ref_set_loc(disk_dt, nullptr, H5T_LOC_MEMORY), 0);
  EXPECT_THAT(payload, ElementsAre(1, 2, 3, 4));
}

struct StubVolState {
  const H5VL_class_t* cls_ptr = nullptr;
  bool fail_file_get_cont_info = false;
  bool fail_file_get_name = false;
  bool long_name_fail_second = false;
  bool huge_name = false;
  bool fail_get_conn_cls = false;
  int get_conn_cls_fail_after = -1;
  bool fail_blob_specific = false;
  bool fail_blob_put = false;
  bool fail_object_get_file = true;
};

const H5VL_class_t* GetStubVolClass();

void* StubFileCreate(const char* /*name*/, unsigned /*flags*/, hid_t /*fcpl_id*/, hid_t /*fapl_id*/,
                     hid_t /*dxpl_id*/, void** /*req*/) {
  auto* state = new StubVolState{};
  state->cls_ptr = GetStubVolClass();
  return state;
}

herr_t StubFileGet(void* obj, H5VL_file_get_args_t* args, hid_t /*dxpl_id*/, void** /*req*/) {
  auto* state = static_cast<StubVolState*>(obj);
  if (args->op_type == H5VL_FILE_GET_CONT_INFO) {
    if (state->fail_file_get_cont_info) {
      return -1;
    }
    args->args.get_cont_info.info->feature_flags = 0;
    args->args.get_cont_info.info->token_size = sizeof(haddr_t);
    args->args.get_cont_info.info->blob_id_size = 8;
    return 0;
  }
  if (args->op_type == H5VL_FILE_GET_NAME) {
    const size_t buf_sz = args->args.get_name.buf_size;
    char* buf = args->args.get_name.buf;
    size_t* out_len = args->args.get_name.file_name_len;
    if (buf_sz <= 256) {
      if (state->fail_file_get_name) {
        return -1;
      }
      if (state->huge_name) {
        *out_len = 70000;
        if (buf != nullptr && buf_sz > 0) {
          buf[0] = '\0';
        }
        return 0;
      }
      if (state->long_name_fail_second) {
        *out_len = 300;
        if (buf != nullptr && buf_sz > 0) {
          buf[0] = '\0';
        }
        return 0;
      }
    } else {
      if (state->long_name_fail_second) {
        return -1;
      }
      if (state->huge_name) {
        constexpr size_t kHugeLen = 70000;
        *out_len = kHugeLen;
        if (buf != nullptr && buf_sz > 0) {
          const size_t n = std::min(kHugeLen, buf_sz - 1);
          std::memset(buf, 'n', n);
          buf[n] = '\0';
        }
        return 0;
      }
    }
    constexpr char kName[] = "stub.h5";
    *out_len = sizeof(kName) - 1;
    if (buf != nullptr && buf_sz > 0) {
      const size_t n = std::min(sizeof(kName) - 1, buf_sz - 1);
      std::memcpy(buf, kName, n);
      buf[n] = '\0';
    }
    return 0;
  }
  return -1;
}

herr_t StubFileSpecific(void* /*obj*/, H5VL_file_specific_args_t* args, hid_t /*dxpl_id*/, void** /*req*/) {
  if (args->op_type == H5VL_FILE_IS_EQUAL) {
    *args->args.is_equal.same_file = false;
    return 0;
  }
  return -1;
}

herr_t StubFileClose(void* file, hid_t /*dxpl_id*/, void** /*req*/) {
  delete static_cast<StubVolState*>(file);
  return 0;
}

herr_t StubObjectGet(void* obj, const H5VL_loc_params_t* /*loc_params*/, H5VL_object_get_args_t* args,
                     hid_t /*dxpl_id*/, void** /*req*/) {
  auto* state = static_cast<StubVolState*>(obj);
  if (args->op_type == H5VL_OBJECT_GET_FILE && !state->fail_object_get_file) {
    *args->args.get_file.file = obj;
    return 0;
  }
  return -1;
}

herr_t StubGetConnCls(void* obj, H5VL_get_conn_lvl_t /*lvl*/, const H5VL_class_t** conn_cls) {
  auto* state = static_cast<StubVolState*>(obj);
  if (state->fail_get_conn_cls) {
    return -1;
  }
  if (state->get_conn_cls_fail_after >= 0) {
    if (state->get_conn_cls_fail_after == 0) {
      return -1;
    }
    --state->get_conn_cls_fail_after;
  }
  *conn_cls = state->cls_ptr;
  return 0;
}

herr_t StubOptQuery(void* /*obj*/, H5VL_subclass_t /*cls*/, int /*opt_type*/, uint64_t* flags) {
  *flags = 0;
  return 0;
}

herr_t StubBlobPut(void* obj, const void* /*buf*/, size_t /*size*/, void* /*blob_id*/, void* /*ctx*/) {
  auto* state = static_cast<StubVolState*>(obj);
  return state->fail_blob_put ? -1 : 0;
}

herr_t StubBlobGet(void* /*obj*/, const void* /*blob_id*/, void* /*buf*/, size_t /*size*/, void* /*ctx*/) {
  return -1;
}

herr_t StubBlobSpecific(void* obj, void* /*blob_id*/, H5VL_blob_specific_args_t* /*args*/) {
  auto* state = static_cast<StubVolState*>(obj);
  return state->fail_blob_specific ? -1 : 0;
}

const H5VL_class_t* GetStubVolClass() {
  static const H5VL_class_t kCls = [] {
    H5VL_class_t cls{};
    cls.version = H5VL_VERSION;
    cls.value = 505;
    cls.name = "hdf5_h5tref_stub_vol";
    cls.conn_version = 1;
    cls.cap_flags = H5VL_CAP_FLAG_NONE;
    cls.file_cls.create = &StubFileCreate;
    cls.file_cls.get = &StubFileGet;
    cls.file_cls.specific = &StubFileSpecific;
    cls.file_cls.close = &StubFileClose;
    cls.object_cls.get = &StubObjectGet;
    cls.introspect_cls.get_conn_cls = &StubGetConnCls;
    cls.introspect_cls.opt_query = &StubOptQuery;
    cls.blob_cls.put = &StubBlobPut;
    cls.blob_cls.get = &StubBlobGet;
    cls.blob_cls.specific = &StubBlobSpecific;
    return cls;
  }();
  return &kCls;
}

TEST(H5TrefTest, NonNativeVolAndFailurePaths) {
  const hid_t vol_id = H5VLregister_connector(GetStubVolClass(), H5P_DEFAULT);
  ASSERT_GE(vol_id, 0);
  absl::Cleanup unregister_vol = [vol_id] { H5VLunregister_connector(vol_id); };

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_vol(fapl, vol_id, nullptr), 0);

  ScopedFile native_file = CreateInMemoryFile("ref_vol_native.h5");
  ASSERT_TRUE(native_file.valid());
  hid_t raw_reg = H5I_INVALID_HID;
  CreateTargetObjects(native_file, &raw_reg);
  ScopedSpace reg_space(raw_reg);
  ASSERT_TRUE(reg_space.valid());

  H5R_ref_t obj_ref{};
  H5R_ref_t reg_ref{};
  absl::Cleanup destroy_native_refs = [&] {
    H5Rdestroy(&obj_ref);
    H5Rdestroy(&reg_ref);
  };
  ASSERT_GE(H5Rcreate_object(native_file, "/grp", H5P_DEFAULT, &obj_ref), 0);
  ASSERT_GE(H5Rcreate_region(native_file, "/dset", reg_space, H5P_DEFAULT, &reg_ref), 0);

  ScopedFile stub_file(H5Fcreate("stub_vol_file", H5F_ACC_TRUNC, H5P_DEFAULT, fapl));
  ASSERT_TRUE(stub_file.valid());

  H5VL_object_t* native_vol = H5VL_vol_object(native_file.get());
  ASSERT_NE(native_vol, nullptr);
  H5VL_object_t* stub_vol = H5VL_vol_object(stub_file.get());
  ASSERT_NE(stub_vol, nullptr);
  auto* state = static_cast<StubVolState*>(H5VL_object_data(stub_vol));
  ASSERT_NE(state, nullptr);

  ScopedType mem_dt_id(H5Tcopy(H5T_STD_REF));
  ASSERT_TRUE(mem_dt_id.valid());
  H5T_t* mem_dt = static_cast<H5T_t*>(H5I_object_verify(mem_dt_id.get(), H5I_DATATYPE));
  ASSERT_NE(mem_dt, nullptr);
  const H5T_ref_class_t* mem_cls = mem_dt->shared->u.atomic.u.r.cls;
  ASSERT_NE(mem_cls, nullptr);

  ScopedType disk_dt_id(H5Tcopy(H5T_STD_REF));
  ASSERT_TRUE(disk_dt_id.valid());
  H5T_t* disk_dt = static_cast<H5T_t*>(H5I_object_verify(disk_dt_id.get(), H5I_DATATYPE));
  ASSERT_NE(disk_dt, nullptr);

  H5CX_node_t api_ctx{};
  ASSERT_GE(H5CX_push(&api_ctx), 0);
  absl::Cleanup pop_cx = [] { H5CX_pop(false); };
  ScopedErrorSilence silence;

  // 1. H5T__ref_set_loc fails when container info cannot be retrieved.
  state->fail_file_get_cont_info = true;
  EXPECT_LT(H5T__ref_set_loc(disk_dt, stub_vol, H5T_LOC_DISK), 0);
  state->fail_file_get_cont_info = false;
  ASSERT_GE(H5T__ref_set_loc(disk_dt, nullptr, H5T_LOC_MEMORY), 0);

  ASSERT_GT(H5T__ref_set_loc(disk_dt, stub_vol, H5T_LOC_DISK), 0);
  absl::Cleanup restore_disk_loc = [&] { H5T__ref_set_loc(disk_dt, nullptr, H5T_LOC_MEMORY); };
  const H5T_ref_class_t* disk_cls = disk_dt->shared->u.atomic.u.r.cls;
  ASSERT_NE(disk_cls, nullptr);

  // 2. Non-native destination VOL sets NULL libver bounds for H5R_DATASET_REGION2.
  bool dst_copy = false;
  const size_t reg_ext_len = mem_cls->getsize(nullptr, &reg_ref, sizeof(reg_ref), stub_vol, &dst_copy);
  ASSERT_GT(reg_ext_len, 0u);
  std::vector<uint8_t> reg_buf(reg_ext_len, 0);
  EXPECT_GE(mem_cls->read(nullptr, &reg_ref, sizeof(reg_ref), stub_vol, reg_buf.data(), reg_buf.size()), 0);

  // 3. Connector class lookup failures in mem_cls->getsize and mem_cls->read.
  state->fail_get_conn_cls = true;
  EXPECT_EQ(mem_cls->getsize(nullptr, &reg_ref, sizeof(reg_ref), stub_vol, &dst_copy), 0u);
  EXPECT_LT(mem_cls->read(nullptr, &reg_ref, sizeof(reg_ref), stub_vol, reg_buf.data(), reg_buf.size()), 0);
  state->fail_get_conn_cls = false;

  state->get_conn_cls_fail_after = 1;
  EXPECT_EQ(mem_cls->getsize(nullptr, &reg_ref, sizeof(reg_ref), stub_vol, &dst_copy), 0u);
  state->get_conn_cls_fail_after = 1;
  // Upstream H5T__ref_mem_read mistakenly uses ret=0 in HGOTO_ERROR for H5VL_object_is_native.
  EXPECT_EQ(mem_cls->read(nullptr, &reg_ref, sizeof(reg_ref), stub_vol, reg_buf.data(), reg_buf.size()), 0);
  state->get_conn_cls_fail_after = -1;

  // 4. Encode an internal object reference to test H5F_get_file_id, file name errors, and huge-name encode errors.
  const size_t obj_int_len = mem_cls->getsize(nullptr, &obj_ref, sizeof(obj_ref), native_vol, &dst_copy);
  ASSERT_GT(obj_int_len, 0u);
  std::vector<uint8_t> enc_obj(obj_int_len, 0);
  ASSERT_GE(mem_cls->read(nullptr, &obj_ref, sizeof(obj_ref), native_vol, enc_obj.data(), enc_obj.size()), 0);

  state->fail_object_get_file = true;
  H5R_ref_t bad_loc_ref{};
  EXPECT_LT(mem_cls->write(stub_vol, enc_obj.data(), enc_obj.size(), H5R_OBJECT2, nullptr, &bad_loc_ref,
                           sizeof(bad_loc_ref), nullptr),
            0);

  state->fail_object_get_file = false;
  {
    H5R_ref_t stub_ref{};
    ASSERT_GE(mem_cls->write(stub_vol, enc_obj.data(), enc_obj.size(), H5R_OBJECT2, nullptr, &stub_ref,
                             sizeof(stub_ref), nullptr),
              0);
    absl::Cleanup destroy_stub_ref = [&] { H5Rdestroy(&stub_ref); };

    std::array<uint8_t, 64> tmp_buf{};
    // Upstream H5T__ref_mem_read returns 0 on H5VL_file_get errors before H5R__encode.
    state->fail_file_get_name = true;
    EXPECT_EQ(mem_cls->getsize(nullptr, &stub_ref, sizeof(stub_ref), native_vol, &dst_copy), 0u);
    EXPECT_EQ(mem_cls->read(nullptr, &stub_ref, sizeof(stub_ref), native_vol, tmp_buf.data(), tmp_buf.size()),
              0);
    state->fail_file_get_name = false;

    state->long_name_fail_second = true;
    EXPECT_EQ(mem_cls->getsize(nullptr, &stub_ref, sizeof(stub_ref), native_vol, &dst_copy), 0u);
    EXPECT_EQ(mem_cls->read(nullptr, &stub_ref, sizeof(stub_ref), native_vol, tmp_buf.data(), tmp_buf.size()),
              0);
    state->long_name_fail_second = false;

    state->huge_name = true;
    EXPECT_EQ(mem_cls->getsize(nullptr, &stub_ref, sizeof(stub_ref), native_vol, &dst_copy), 0u);
    EXPECT_LT(mem_cls->read(nullptr, &stub_ref, sizeof(stub_ref), native_vol, tmp_buf.data(), tmp_buf.size()),
              0);
    state->huge_name = false;
  }

  // 5. Blob put/specific failures on disk_cls callbacks.
  state->fail_blob_specific = true;
  state->fail_blob_put = true;
  std::array<uint8_t, 32> zero_disk{};
  std::array<uint8_t, 32> out_disk{};
  std::array<uint8_t, 32> bg_disk{};
  std::array<uint8_t, 8> payload{1, 2, 3, 4, 5, 6, 7, 8};
  bool isnull = false;

  EXPECT_LT(disk_cls->isnull(stub_vol, zero_disk.data(), &isnull), 0);
  EXPECT_LT(disk_cls->setnull(stub_vol, out_disk.data(), bg_disk.data()), 0);
  EXPECT_LT(disk_cls->setnull(stub_vol, out_disk.data(), nullptr), 0);
  EXPECT_LT(disk_cls->write(nullptr, payload.data(), payload.size(), H5R_ATTR, stub_vol, out_disk.data(),
                            disk_dt->shared->size, bg_disk.data()),
            0);
  EXPECT_LT(disk_cls->write(nullptr, payload.data(), payload.size(), H5R_ATTR, stub_vol, out_disk.data(),
                            disk_dt->shared->size, nullptr),
            0);
}

}  // namespace
}  // namespace hdf5
