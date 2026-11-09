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
// Tests compound and enumeration member queries (H5Tget_nmembers,
// H5Tget_member_name, H5Tget_member_index) and internal value/name sorting
// helpers (H5T__sort_value, H5T__sort_name) (third_party/hdf5/src/H5Tfields.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tfields.c

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` compiles with -DNDEBUG; match its H5C/H5O/H5T struct layout across
// the prelude and internal headers.
#ifndef NDEBUG
#define H5TFIELDS_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#if defined(H5_HAVE__FLOAT16) && defined(__cplusplus)
// H5private.h skips typedef'ing H5__Float16 in C++, yet H5Tpkg.h uses it to
// declare unused internal +/-Inf globals under H5_HAVE__FLOAT16.
typedef uint16_t H5__Float16;
#endif
extern "C" {
#define H5T_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Tpkg.h"
}  // extern "C"
#ifdef H5TFIELDS_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5TFIELDS_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

std::optional<std::string> ReadAndFreeMemberName(hid_t type_id,
                                                 unsigned index) {
  char* raw = H5Tget_member_name(type_id, index);
  if (!raw) {
    return std::nullopt;
  }
  absl::Cleanup free_raw = [raw] { H5free_memory(raw); };
  return std::string(raw);
}

TEST(H5TfieldsTest, CompoundMemberCountNameAndIndex) {
  ScopedType comp(H5Tcreate(H5T_COMPOUND, 16));
  ASSERT_TRUE(comp.valid());
  EXPECT_EQ(H5Tget_nmembers(comp), 0);

  ASSERT_GE(H5Tinsert(comp, "second", 8, H5T_NATIVE_DOUBLE), 0);
  ASSERT_GE(H5Tinsert(comp, "first", 0, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(comp, "third", 4, H5T_NATIVE_FLOAT), 0);

  EXPECT_EQ(H5Tget_nmembers(comp), 3);
  EXPECT_EQ(ReadAndFreeMemberName(comp, 0), "second");
  EXPECT_EQ(ReadAndFreeMemberName(comp, 1), "first");
  EXPECT_EQ(ReadAndFreeMemberName(comp, 2), "third");

  EXPECT_EQ(H5Tget_member_index(comp, "second"), 0);
  EXPECT_EQ(H5Tget_member_index(comp, "first"), 1);
  EXPECT_EQ(H5Tget_member_index(comp, "third"), 2);

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Tget_member_name(comp, 3), nullptr);
  EXPECT_EQ(H5Tget_member_index(comp, "missing"), -1);
  EXPECT_EQ(H5Tget_member_index(comp, ""), -1);
}

TEST(H5TfieldsTest, EnumMemberCountNameAndIndex) {
  ScopedType enum_dt(H5Tenum_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(enum_dt.valid());
  EXPECT_EQ(H5Tget_nmembers(enum_dt), 0);

  const int32_t v0 = 100;
  const int32_t v1 = 10;
  const int32_t v2 = 50;
  ASSERT_GE(H5Tenum_insert(enum_dt, "HUNDRED", &v0), 0);
  ASSERT_GE(H5Tenum_insert(enum_dt, "TEN", &v1), 0);
  ASSERT_GE(H5Tenum_insert(enum_dt, "FIFTY", &v2), 0);

  EXPECT_EQ(H5Tget_nmembers(enum_dt), 3);
  EXPECT_EQ(ReadAndFreeMemberName(enum_dt, 0), "HUNDRED");
  EXPECT_EQ(ReadAndFreeMemberName(enum_dt, 1), "TEN");
  EXPECT_EQ(ReadAndFreeMemberName(enum_dt, 2), "FIFTY");

  EXPECT_EQ(H5Tget_member_index(enum_dt, "HUNDRED"), 0);
  EXPECT_EQ(H5Tget_member_index(enum_dt, "TEN"), 1);
  EXPECT_EQ(H5Tget_member_index(enum_dt, "FIFTY"), 2);

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Tget_member_name(enum_dt, 3), nullptr);
  EXPECT_EQ(H5Tget_member_index(enum_dt, "ZERO"), -1);
}

TEST(H5TfieldsTest, UnsupportedClassesAndInvalidIdsFail) {
  ScopedType str_dt(H5Tcopy(H5T_C_S1));
  ASSERT_TRUE(str_dt.valid());
  ScopedType opaque_dt(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(opaque_dt.valid());
  const hsize_t dim = 2;
  ScopedType arr_dt(H5Tarray_create2(H5T_NATIVE_INT32, 1, &dim));
  ASSERT_TRUE(arr_dt.valid());
  ScopedType vlen_dt(H5Tvlen_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(vlen_dt.valid());
  ScopedType cplx_dt(H5Tcomplex_create(H5T_NATIVE_FLOAT));
  ASSERT_TRUE(cplx_dt.valid());

  const std::array<hid_t, 10> non_field_types = {
      H5T_NATIVE_INT32, H5T_NATIVE_DOUBLE, H5T_UNIX_D32LE, str_dt.get(),
      H5T_STD_B8LE,     opaque_dt.get(),   H5T_STD_REF,    vlen_dt.get(),
      arr_dt.get(),     cplx_dt.get(),
  };

  ScopedErrorSilence silence;

  EXPECT_LT(H5Tget_nmembers(H5I_INVALID_HID), 0);
  EXPECT_EQ(H5Tget_member_name(H5I_INVALID_HID, 0), nullptr);
  EXPECT_LT(H5Tget_member_index(H5I_INVALID_HID, "field"), 0);

  for (hid_t dt_id : non_field_types) {
    EXPECT_LT(H5Tget_nmembers(dt_id), 0);
    EXPECT_EQ(H5Tget_member_name(dt_id, 0), nullptr);
    EXPECT_LT(H5Tget_member_index(dt_id, "field"), 0);
  }
}

TEST(H5TfieldsTest, SortCompoundByValueAndNameWithMapAndShortCircuit) {
  // 1. Insert compound fields out of both offset order and name order.
  ScopedType comp(H5Tcreate(H5T_COMPOUND, 16));
  ASSERT_TRUE(comp.valid());
  ASSERT_GE(H5Tinsert(comp, "gamma", 8, H5T_NATIVE_INT64), 0);
  ASSERT_GE(H5Tinsert(comp, "alpha", 0, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(comp, "beta", 4, H5T_NATIVE_FLOAT), 0);

  auto* dt = static_cast<H5T_t*>(H5I_object_verify(comp, H5I_DATATYPE));
  ASSERT_NE(dt, nullptr);

  // 2. Sort by value (offset) with a non-null parallel index map.
  std::array<int, 3> map_val = {100, 101, 102};
  ASSERT_GE(H5T__sort_value(dt, map_val.data()), 0);
  EXPECT_EQ(ReadAndFreeMemberName(comp, 0), "alpha");
  EXPECT_EQ(ReadAndFreeMemberName(comp, 1), "beta");
  EXPECT_EQ(ReadAndFreeMemberName(comp, 2), "gamma");
  EXPECT_THAT(map_val, ElementsAre(101, 102, 100));

  // Second call is a short-circuited no-op since already sorted by value.
  std::array<int, 3> untouched_map = {1, 2, 3};
  ASSERT_GE(H5T__sort_value(dt, untouched_map.data()), 0);
  EXPECT_THAT(untouched_map, ElementsAre(1, 2, 3));

  // 3. Re-sort the same compound by name with a parallel map, then check
  // already-sorted short-circuit.
  // Offsets were: alpha(0), beta(4), gamma(8); re-test a non-sorted-by-name
  // ordering by building a second compound where offset order != name order.
  ScopedType comp2(H5Tcreate(H5T_COMPOUND, 16));
  ASSERT_TRUE(comp2.valid());
  ASSERT_GE(H5Tinsert(comp2, "zeta", 0, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(comp2, "alpha", 4, H5T_NATIVE_FLOAT), 0);
  ASSERT_GE(H5Tinsert(comp2, "mu", 8, H5T_NATIVE_INT64), 0);
  auto* dt2 = static_cast<H5T_t*>(H5I_object_verify(comp2, H5I_DATATYPE));
  ASSERT_NE(dt2, nullptr);

  std::array<int, 3> map_name = {10, 20, 30};
  ASSERT_GE(H5T__sort_name(dt2, map_name.data()), 0);
  EXPECT_EQ(ReadAndFreeMemberName(comp2, 0), "alpha");
  EXPECT_EQ(ReadAndFreeMemberName(comp2, 1), "mu");
  EXPECT_EQ(ReadAndFreeMemberName(comp2, 2), "zeta");
  EXPECT_THAT(map_name, ElementsAre(20, 30, 10));

  // Second H5T__sort_name short-circuits.
  ASSERT_GE(H5T__sort_name(dt2, nullptr), 0);

  // 4. Pre-sorted compound takes the no-swap early exit on first pass.
  ScopedType presorted(H5Tcreate(H5T_COMPOUND, 8));
  ASSERT_TRUE(presorted.valid());
  ASSERT_GE(H5Tinsert(presorted, "a", 0, H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(presorted, "b", 4, H5T_NATIVE_INT32), 0);
  auto* dt_pre =
      static_cast<H5T_t*>(H5I_object_verify(presorted, H5I_DATATYPE));
  ASSERT_NE(dt_pre, nullptr);
  ASSERT_GE(H5T__sort_value(dt_pre, nullptr), 0);
  ASSERT_GE(H5T__sort_name(dt_pre, nullptr), 0);
}

TEST(H5TfieldsTest, SortEnumByValueAndNameWithMapAndShortCircuit) {
  // Use 4-byte native integers with small values [10, 20, 30] so raw byte
  // memcmp order is deterministic and unsorted on insertion.
  ScopedType enum_dt(H5Tenum_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(enum_dt.valid());
  const int32_t v_zulu = 30;
  const int32_t v_alpha = 10;
  const int32_t v_mike = 20;
  ASSERT_GE(H5Tenum_insert(enum_dt, "ZULU", &v_zulu), 0);
  ASSERT_GE(H5Tenum_insert(enum_dt, "ALPHA", &v_alpha), 0);
  ASSERT_GE(H5Tenum_insert(enum_dt, "MIKE", &v_mike), 0);

  auto* dt = static_cast<H5T_t*>(H5I_object_verify(enum_dt, H5I_DATATYPE));
  ASSERT_NE(dt, nullptr);

  // 1. Sort enum by value with a non-null map -> ALPHA(10), MIKE(20), ZULU(30).
  std::array<int, 3> map_val = {0, 1, 2};
  ASSERT_GE(H5T__sort_value(dt, map_val.data()), 0);
  EXPECT_EQ(ReadAndFreeMemberName(enum_dt, 0), "ALPHA");
  EXPECT_EQ(ReadAndFreeMemberName(enum_dt, 1), "MIKE");
  EXPECT_EQ(ReadAndFreeMemberName(enum_dt, 2), "ZULU");
  EXPECT_THAT(map_val, ElementsAre(1, 2, 0));

  // Second value-sort short-circuits.
  ASSERT_GE(H5T__sort_value(dt, nullptr), 0);

  // 2. Sort a fresh enum by name with a non-null map where value order differs
  // from alphabetical order.
  ScopedType enum_dt2(H5Tenum_create(H5T_NATIVE_UINT8));
  ASSERT_TRUE(enum_dt2.valid());
  const uint8_t u0 = 1;
  const uint8_t u1 = 2;
  const uint8_t u2 = 3;
  ASSERT_GE(H5Tenum_insert(enum_dt2, "ZULU", &u0), 0);
  ASSERT_GE(H5Tenum_insert(enum_dt2, "MIKE", &u1), 0);
  ASSERT_GE(H5Tenum_insert(enum_dt2, "ALPHA", &u2), 0);

  auto* dt2 = static_cast<H5T_t*>(H5I_object_verify(enum_dt2, H5I_DATATYPE));
  ASSERT_NE(dt2, nullptr);

  std::array<int, 3> map_name = {10, 20, 30};
  ASSERT_GE(H5T__sort_name(dt2, map_name.data()), 0);
  EXPECT_EQ(ReadAndFreeMemberName(enum_dt2, 0), "ALPHA");
  EXPECT_EQ(ReadAndFreeMemberName(enum_dt2, 1), "MIKE");
  EXPECT_EQ(ReadAndFreeMemberName(enum_dt2, 2), "ZULU");
  EXPECT_THAT(map_name, ElementsAre(30, 20, 10));

  uint8_t got = 0;
  ASSERT_GE(H5Tget_member_value(enum_dt2, 0, &got), 0);
  EXPECT_EQ(got, 3u);
  ASSERT_GE(H5Tget_member_value(enum_dt2, 2, &got), 0);
  EXPECT_EQ(got, 1u);

  // Second name-sort short-circuits.
  ASSERT_GE(H5T__sort_name(dt2, nullptr), 0);

  // 3. Pre-sorted enum exercises the no-swap path on first sort pass.
  ScopedType presorted(H5Tenum_create(H5T_NATIVE_UINT8));
  ASSERT_TRUE(presorted.valid());
  ASSERT_GE(H5Tenum_insert(presorted, "A", &u0), 0);
  ASSERT_GE(H5Tenum_insert(presorted, "B", &u1), 0);
  auto* dt_pre =
      static_cast<H5T_t*>(H5I_object_verify(presorted, H5I_DATATYPE));
  ASSERT_NE(dt_pre, nullptr);
  ASSERT_GE(H5T__sort_value(dt_pre, nullptr), 0);
  ASSERT_GE(H5T__sort_name(dt_pre, nullptr), 0);
}

}  // namespace
}  // namespace hdf5
