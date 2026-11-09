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
// Tests compound datatype creation, member insertion/inspection/reopening,
// packing (nested compounds, arrays, VLENs, read-only members), and internal
// compound helpers (third_party/hdf5/src/H5Tcompound.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tcompound.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5TCOMPOUND_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5T_FRIEND
#if defined(H5_HAVE__FLOAT16) && defined(__cplusplus)
using H5__Float16 = uint16_t;
#endif
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Tpkg.h"
}  // extern "C"
#ifdef H5TCOMPOUND_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TCOMPOUND_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

TEST(H5TcompoundTest, InsertInspectReopenAndInternalHelpers) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  // Commit a member datatype so H5Tget_member_type reopens it cleanly.
  ScopedType committed_memb(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(committed_memb.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/committed_i32", committed_memb.get(),
                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);

  // Variable-length string member distinguishes external vs internal class.
  ScopedType vlen_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(vlen_str.valid());
  ASSERT_GE(H5Tset_size(vlen_str.get(), H5T_VARIABLE), 0);

  // Fixed-length string member.
  ScopedType fixed_str(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(fixed_str.valid());
  ASSERT_GE(H5Tset_size(fixed_str.get(), 8), 0);

  const size_t total_size =
      sizeof(int32_t) + sizeof(double) + sizeof(char*) + 8;
  ScopedType comp(H5Tcreate(H5T_COMPOUND, total_size));
  ASSERT_TRUE(comp.valid());

  size_t off = 0;
  ASSERT_GE(H5Tinsert(comp.get(), "id", off, committed_memb.get()), 0);
  off += sizeof(int32_t);
  ASSERT_GE(H5Tinsert(comp.get(), "val", off, H5T_NATIVE_DOUBLE), 0);
  off += sizeof(double);
  ASSERT_GE(H5Tinsert(comp.get(), "vstr", off, vlen_str.get()), 0);
  off += sizeof(char*);
  ASSERT_GE(H5Tinsert(comp.get(), "fstr", off, fixed_str.get()), 0);

  EXPECT_EQ(H5Tget_nmembers(comp.get()), 4);
  EXPECT_EQ(H5Tget_member_offset(comp.get(), 0), 0);
  EXPECT_EQ(H5Tget_member_offset(comp.get(), 1), sizeof(int32_t));
  EXPECT_EQ(H5Tget_member_class(comp.get(), 0), H5T_INTEGER);
  EXPECT_EQ(H5Tget_member_class(comp.get(), 1), H5T_FLOAT);
  EXPECT_EQ(H5Tget_member_class(comp.get(), 2), H5T_STRING);
  EXPECT_EQ(H5Tget_member_class(comp.get(), 3), H5T_STRING);

  ScopedType got0(H5Tget_member_type(comp.get(), 0));
  ASSERT_TRUE(got0.valid());
  EXPECT_GT(H5Tequal(got0.get(), H5T_NATIVE_INT32), 0);

  ScopedType got1(H5Tget_member_type(comp.get(), 1));
  ASSERT_TRUE(got1.valid());
  EXPECT_GT(H5Tequal(got1.get(), H5T_NATIVE_DOUBLE), 0);

  // Exercise library-private compound getters directly.
  const auto* comp_dt =
      static_cast<const H5T_t*>(H5I_object_verify(comp.get(), H5I_DATATYPE));
  ASSERT_NE(comp_dt, nullptr);
  EXPECT_EQ(H5T_get_member_offset(comp_dt, 1), sizeof(int32_t));
  EXPECT_EQ(H5T__get_member_size(comp_dt, 3), 8);
  H5T_t* copied_m1 = H5T_get_member_type(comp_dt, 1);
  ASSERT_NE(copied_m1, nullptr);
  EXPECT_GE(H5T_close(copied_m1), 0);
}

TEST(H5TcompoundTest, GrowthForceConvAndVersionUpgrade) {
  // Insert > 8 members to trigger repeated nalloc doubling (1->2->4->8->16).
  ScopedType many(H5Tcreate(H5T_COMPOUND, 12 * sizeof(int32_t)));
  ASSERT_TRUE(many.valid());
  for (int i = 0; i < 11; ++i) {
    const std::string name = absl::StrFormat("m%02d", i);
    ASSERT_GE(
        H5Tinsert(many.get(), name.c_str(), i * sizeof(int32_t), H5T_NATIVE_INT32),
        0);
  }
  // Insert a member whose version is newer (H5T_COMPLEX v5 > v1), forcing version upgrade.
  ScopedType cf32(H5Tcomplex_create(H5T_NATIVE_FLOAT));
  ASSERT_TRUE(cf32.valid());
  ASSERT_EQ(H5Tget_size(cf32.get()), sizeof(int32_t) * 2);
  ScopedType upgraded(H5Tcreate(H5T_COMPOUND, 16));
  ASSERT_TRUE(upgraded.valid());
  ASSERT_GE(H5Tinsert(upgraded.get(), "i", 0, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(upgraded.get(), "z", 4, cf32.get()), 0);
  // Insert force_conv VLEN member.
  ScopedType vl(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vl.valid());
  ScopedType with_vl(H5Tcreate(H5T_COMPOUND, 32));
  ASSERT_TRUE(with_vl.valid());
  ASSERT_GE(H5Tinsert(with_vl.get(), "vl", 0, vl.get()), 0);

  const auto* with_vl_dt =
      static_cast<const H5T_t*>(H5I_object_verify(with_vl.get(), H5I_DATATYPE));
  ASSERT_NE(with_vl_dt, nullptr);
  EXPECT_TRUE(with_vl_dt->shared->force_conv);
}

TEST(H5TcompoundTest, PackSortingNestedArraysVlensAndEmpty) {
  // Packing out-of-order members with padding gaps shrinks size and removes gaps.
  ScopedType out_of_order(H5Tcreate(H5T_COMPOUND, 32));
  ASSERT_TRUE(out_of_order.valid());
  ASSERT_GE(H5Tinsert(out_of_order.get(), "second", 16, H5T_NATIVE_INT64), 0);
  ASSERT_GE(H5Tinsert(out_of_order.get(), "first", 4, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tpack(out_of_order.get()), 0);
  EXPECT_EQ(H5Tget_size(out_of_order.get()),
            sizeof(int32_t) + sizeof(int64_t));
  EXPECT_EQ(H5Tget_member_offset(out_of_order.get(), 0), 0);
  EXPECT_EQ(H5Tget_member_offset(out_of_order.get(), 1), sizeof(int32_t));

  // Packing an already-packed compound is a fast no-op.
  EXPECT_GE(H5Tpack(out_of_order.get()), 0);

  // Nested unpacked compound matching outer size exactly (memb_size == size)
  // triggers !H5T__is_packed inside H5T__update_packed.
  ScopedType unpacked_inner(H5Tcreate(H5T_COMPOUND, 16));
  ASSERT_TRUE(unpacked_inner.valid());
  ASSERT_GE(H5Tinsert(unpacked_inner.get(), "a", 4, H5T_NATIVE_INT32), 0);

  ScopedType outer_exact(H5Tcreate(H5T_COMPOUND, 16));
  ASSERT_TRUE(outer_exact.valid());
  ASSERT_GE(H5Tinsert(outer_exact.get(), "inner", 0, unpacked_inner.get()), 0);

  const auto* outer_dt = static_cast<const H5T_t*>(
      H5I_object_verify(outer_exact.get(), H5I_DATATYPE));
  ASSERT_NE(outer_dt, nullptr);
  EXPECT_FALSE(outer_dt->shared->u.compnd.packed);

  ASSERT_GE(H5Tpack(outer_exact.get()), 0);
  EXPECT_EQ(H5Tget_size(outer_exact.get()), sizeof(int32_t));
  EXPECT_TRUE(outer_dt->shared->u.compnd.packed);

  // Array of unpacked compound updates array size when packed.
  const hsize_t arr_dim = 3;
  ScopedType arr_of_unpacked(
      H5Tarray_create2(unpacked_inner.get(), 1, &arr_dim));
  ASSERT_TRUE(arr_of_unpacked.valid());
  EXPECT_EQ(H5Tget_size(arr_of_unpacked.get()), 48);
  ASSERT_GE(H5Tpack(arr_of_unpacked.get()), 0);
  EXPECT_EQ(H5Tget_size(arr_of_unpacked.get()), 3 * sizeof(int32_t));

  // VLEN of unpacked compound packs inner compound without changing VLEN hvl_t size.
  ScopedType vlen_of_unpacked(H5Tvlen_create(unpacked_inner.get()));
  ASSERT_TRUE(vlen_of_unpacked.valid());
  ASSERT_GE(H5Tpack(vlen_of_unpacked.get()), 0);
  EXPECT_EQ(H5Tget_size(vlen_of_unpacked.get()), sizeof(hvl_t));

  // Dataset roundtrip with packed compound.
  struct Record {
    int32_t first;
    int64_t second;
  };
  ScopedType packed_rec(H5Tcreate(H5T_COMPOUND, sizeof(Record) + 16));
  ASSERT_TRUE(packed_rec.valid());
  ASSERT_GE(H5Tinsert(packed_rec.get(), "first", 4, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(packed_rec.get(), "second", 16, H5T_NATIVE_INT64), 0);
  ASSERT_GE(H5Tpack(packed_rec.get()), 0);

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  const hsize_t dset_dims = 2;
  ScopedSpace space(H5Screate_simple(1, &dset_dims, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file.get(), "/recs", packed_rec.get(), space.get(),
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  std::array<uint8_t, 24> in{};
  const int32_t in_first[2] = {11, 22};
  const int64_t in_second[2] = {1001, 2002};
  std::memcpy(in.data(), &in_first[0], sizeof(int32_t));
  std::memcpy(in.data() + sizeof(int32_t), &in_second[0], sizeof(int64_t));
  std::memcpy(in.data() + 12, &in_first[1], sizeof(int32_t));
  std::memcpy(in.data() + 12 + sizeof(int32_t), &in_second[1], sizeof(int64_t));

  std::array<uint8_t, 24> out{};
  ASSERT_GE(H5Dwrite(dset.get(), packed_rec.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     in.data()),
            0);
  ASSERT_GE(H5Dread(dset.get(), packed_rec.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    out.data()),
            0);
  int32_t out_first[2] = {};
  int64_t out_second[2] = {};
  std::memcpy(&out_first[0], out.data(), sizeof(int32_t));
  std::memcpy(&out_second[0], out.data() + sizeof(int32_t), sizeof(int64_t));
  std::memcpy(&out_first[1], out.data() + 12, sizeof(int32_t));
  std::memcpy(&out_second[1], out.data() + 12 + sizeof(int32_t), sizeof(int64_t));
  EXPECT_EQ(out_first[0], 11);
  EXPECT_EQ(out_second[0], 1001);
  EXPECT_EQ(out_first[1], 22);
  EXPECT_EQ(out_second[1], 2002);
}

TEST(H5TcompoundTest, InsertAndPackErrorCases) {
  ScopedErrorSilence silence;

  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());

  ScopedType empty_comp(H5Tcreate(H5T_COMPOUND, 16));
  ASSERT_TRUE(empty_comp.valid());

  // Getter error cases.
  EXPECT_EQ(H5Tget_member_offset(H5I_INVALID_HID, 0), 0);
  EXPECT_EQ(H5Tget_member_offset(H5T_NATIVE_INT, 0), 0);
  EXPECT_EQ(H5Tget_member_offset(space.get(), 0), 0);
  EXPECT_EQ(H5Tget_member_offset(empty_comp.get(), 0), 0);

  EXPECT_EQ(H5Tget_member_class(H5I_INVALID_HID, 0), H5T_NO_CLASS);
  EXPECT_EQ(H5Tget_member_class(H5T_NATIVE_DOUBLE, 0), H5T_NO_CLASS);
  EXPECT_EQ(H5Tget_member_class(space.get(), 0), H5T_NO_CLASS);
  EXPECT_EQ(H5Tget_member_class(empty_comp.get(), 0), H5T_NO_CLASS);

  EXPECT_EQ(H5Tget_member_type(H5I_INVALID_HID, 0), H5I_INVALID_HID);
  EXPECT_EQ(H5Tget_member_type(H5T_NATIVE_FLOAT, 0), H5I_INVALID_HID);
  EXPECT_EQ(H5Tget_member_type(space.get(), 0), H5I_INVALID_HID);
  EXPECT_EQ(H5Tget_member_type(empty_comp.get(), 0), H5I_INVALID_HID);

  // H5Tinsert argument validation errors.
  EXPECT_LT(H5Tinsert(empty_comp.get(), "self", 0, empty_comp.get()), 0);
  EXPECT_LT(H5Tinsert(H5I_INVALID_HID, "a", 0, H5T_NATIVE_INT32), 0);
  EXPECT_LT(H5Tinsert(H5T_NATIVE_INT, "a", 0, H5T_NATIVE_INT32), 0);
  EXPECT_LT(H5Tinsert(space.get(), "a", 0, H5T_NATIVE_INT32), 0);
  EXPECT_LT(H5Tinsert(empty_comp.get(), nullptr, 0, H5T_NATIVE_INT32), 0);
  EXPECT_LT(H5Tinsert(empty_comp.get(), "", 0, H5T_NATIVE_INT32), 0);
  EXPECT_LT(H5Tinsert(empty_comp.get(), "a", 0, H5I_INVALID_HID), 0);
  EXPECT_LT(H5Tinsert(empty_comp.get(), "a", 0, space.get()), 0);

  // Read-only parent rejected.
  ScopedType locked_comp(H5Tcreate(H5T_COMPOUND, 16));
  ASSERT_TRUE(locked_comp.valid());
  ASSERT_GE(H5Tinsert(locked_comp.get(), "x", 4, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tlock(locked_comp.get()), 0);
  EXPECT_LT(H5Tinsert(locked_comp.get(), "y", 8, H5T_NATIVE_INT32), 0);
  EXPECT_LT(H5Tpack(locked_comp.get()), 0);

  // Duplicate name, overlaps in both directions, and past-the-end errors.
  ScopedType overlap_comp(H5Tcreate(H5T_COMPOUND, 16));
  ASSERT_TRUE(overlap_comp.valid());
  ASSERT_GE(H5Tinsert(overlap_comp.get(), "mid", 4, H5T_NATIVE_INT32), 0);
  // Duplicate member name.
  EXPECT_LT(H5Tinsert(overlap_comp.get(), "mid", 8, H5T_NATIVE_INT32), 0);
  // Overlaps from left (offset <= existing.offset && offset + size > existing.offset).
  EXPECT_LT(H5Tinsert(overlap_comp.get(), "left_ov", 2, H5T_NATIVE_INT32), 0);
  // Overlaps from right (existing.offset <= offset && existing.offset + size > offset).
  EXPECT_LT(H5Tinsert(overlap_comp.get(), "right_ov", 6, H5T_NATIVE_INT32), 0);
  // Extends past end of compound type.
  EXPECT_LT(H5Tinsert(overlap_comp.get(), "past_end", 14, H5T_NATIVE_INT32), 0);
  // Exact adjacent placement succeeds.
  EXPECT_GE(H5Tinsert(overlap_comp.get(), "before", 0, H5T_NATIVE_INT32), 0);
  EXPECT_GE(H5Tinsert(overlap_comp.get(), "after", 8, H5T_NATIVE_INT64), 0);

  // H5Tpack non-compound error cases.
  EXPECT_LT(H5Tpack(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Tpack(space.get()), 0);
  EXPECT_LT(H5Tpack(H5T_NATIVE_INT32), 0);
  const hsize_t arr_dim = 2;
  ScopedType int_arr(H5Tarray_create2(H5T_NATIVE_INT32, 1, &arr_dim));
  ASSERT_TRUE(int_arr.valid());
  EXPECT_LT(H5Tpack(int_arr.get()), 0);

  // Read-only nested unpacked compound inside transient outer compound fails H5Tpack
  // both via H5T_ARRAY wrapper (parent fail) and via nested compound (member fail).
  ScopedType ro_arr(H5Tarray_create2(locked_comp.get(), 1, &arr_dim));
  ASSERT_TRUE(ro_arr.valid());
  EXPECT_LT(H5Tpack(ro_arr.get()), 0);

  ScopedType outer_with_ro(H5Tcreate(H5T_COMPOUND, 16));
  ASSERT_TRUE(outer_with_ro.valid());
  ASSERT_GE(H5Tinsert(outer_with_ro.get(), "locked_memb", 0, locked_comp.get()), 0);
  EXPECT_LT(H5Tpack(outer_with_ro.get()), 0);
}

}  // namespace
}  // namespace hdf5
