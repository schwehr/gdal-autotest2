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
// Tests enumeration datatype creation, member insertion/reallocation,
// member value retrieval, and bidirectional name/value lookups
// (third_party/hdf5/src/H5Tenum.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tenum.c

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

struct EnumEntry {
  const char* name;
  int32_t value;
};

constexpr std::array<EnumEntry, 5> kUnsortedEntries = {{
    {"DELTA", 40},
    {"ALPHA", 10},
    {"ECHO", 50},
    {"BRAVO", 20},
    {"CHARLIE", 30},
}};

TEST(H5TenumTest, CreateEnumFromIntegerBasesAndRejectInvalidBases) {
  // Valid integer bases of varying widths and byte orders.
  const std::array<hid_t, 4> valid_bases = {
      H5T_NATIVE_INT8,
      H5T_NATIVE_INT32,
      H5T_STD_U16BE,
      H5T_STD_I64LE,
  };
  for (hid_t base : valid_bases) {
    ScopedType enum_dt(H5Tenum_create(base));
    ASSERT_TRUE(enum_dt.valid());
    EXPECT_EQ(H5Tget_class(enum_dt), H5T_ENUM);
    EXPECT_EQ(H5Tget_size(enum_dt), H5Tget_size(base));
    ScopedType super(H5Tget_super(enum_dt));
    ASSERT_TRUE(super.valid());
    EXPECT_GT(H5Tequal(super, base), 0);
  }

  ScopedErrorSilence silence;
  EXPECT_EQ(H5Tenum_create(H5I_INVALID_HID), H5I_INVALID_HID);
  EXPECT_EQ(H5Tenum_create(H5T_NATIVE_FLOAT), H5I_INVALID_HID);
  EXPECT_EQ(H5Tenum_create(H5T_C_S1), H5I_INVALID_HID);

  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  EXPECT_EQ(H5Tenum_create(file), H5I_INVALID_HID);
}

TEST(H5TenumTest, InsertGrowTableBeyondInitialCapacityAndGetMemberValue) {
  ScopedType enum_dt(H5Tenum_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(enum_dt.valid());

  // Initial allocation is 32 slots in H5T__enum_insert; inserting 70 members
  // triggers both the initial allocation (0 -> 32) and growth reallocs
  // (32 -> 64 -> 128).
  constexpr unsigned kMemberCount = 70;
  for (unsigned i = 0; i < kMemberCount; ++i) {
    const std::string name = "MEMBER_" + std::to_string(i);
    const int32_t value = static_cast<int32_t>(i * 3 - 50);
    ASSERT_GE(H5Tenum_insert(enum_dt, name.c_str(), &value), 0);
  }
  EXPECT_EQ(H5Tget_nmembers(enum_dt), static_cast<int>(kMemberCount));

  for (unsigned i = 0; i < kMemberCount; ++i) {
    int32_t got = 0;
    ASSERT_GE(H5Tget_member_value(enum_dt, i, &got), 0);
    EXPECT_EQ(got, static_cast<int32_t>(i * 3 - 50));
  }
}

TEST(H5TenumTest, InsertAndGetMemberValueInvalidArgumentsFail) {
  ScopedType enum_dt(H5Tenum_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(enum_dt.valid());

  ScopedErrorSilence silence;

  // Querying an empty enum out of bounds fails.
  int32_t out_val = -1;
  EXPECT_LT(H5Tget_member_value(enum_dt, 0, &out_val), 0);

  const int32_t v1 = 7;
  ASSERT_GE(H5Tenum_insert(enum_dt, "SEVEN", &v1), 0);

  // H5Tenum_insert error paths.
  EXPECT_LT(H5Tenum_insert(H5I_INVALID_HID, "A", &v1), 0);
  EXPECT_LT(H5Tenum_insert(H5T_NATIVE_INT32, "A", &v1), 0);
  EXPECT_LT(H5Tenum_insert(enum_dt, nullptr, &v1), 0);
  EXPECT_LT(H5Tenum_insert(enum_dt, "", &v1), 0);
  EXPECT_LT(H5Tenum_insert(enum_dt, "NO_VAL", nullptr), 0);

  // Duplicate name and duplicate value must both be rejected.
  const int32_t v2 = 8;
  EXPECT_LT(H5Tenum_insert(enum_dt, "SEVEN", &v2), 0);
  EXPECT_LT(H5Tenum_insert(enum_dt, "OTHER_SEVEN", &v1), 0);

  // H5Tget_member_value error paths.
  EXPECT_LT(H5Tget_member_value(H5I_INVALID_HID, 0, &out_val), 0);
  EXPECT_LT(H5Tget_member_value(H5T_NATIVE_INT32, 0, &out_val), 0);
  EXPECT_LT(H5Tget_member_value(enum_dt, 1, &out_val), 0);
  EXPECT_LT(H5Tget_member_value(enum_dt, 0, nullptr), 0);
}

TEST(H5TenumTest, NameofAndValueofBinarySearchAndErrorPaths) {
  ScopedType empty_enum(H5Tenum_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(empty_enum.valid());

  ScopedType enum_dt(H5Tenum_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(enum_dt.valid());
  for (const EnumEntry& entry : kUnsortedEntries) {
    ASSERT_GE(H5Tenum_insert(enum_dt, entry.name, &entry.value), 0);
  }

  // Successful lookups across all entries (covers cmp < 0, cmp > 0, cmp == 0
  // in both H5T__enum_nameof and H5T__enum_valueof).
  for (const EnumEntry& entry : kUnsortedEntries) {
    char buf[32]{};
    ASSERT_GE(H5Tenum_nameof(enum_dt, &entry.value, buf, sizeof(buf)), 0);
    EXPECT_STREQ(buf, entry.name);

    int32_t val = -1;
    ASSERT_GE(H5Tenum_valueof(enum_dt, entry.name, &val), 0);
    EXPECT_EQ(val, entry.value);
  }

  // Exact-fit buffer (strlen + 1) succeeds.
  constexpr char kCharlie[] = "CHARLIE";
  std::array<char, sizeof(kCharlie)> exact_buf{};
  ASSERT_GE(H5Tenum_nameof(enum_dt, &kUnsortedEntries[4].value,
                           exact_buf.data(), exact_buf.size()),
            0);
  EXPECT_STREQ(exact_buf.data(), "CHARLIE");

  ScopedErrorSilence silence;

  // Truncation when buffer omits space for NUL terminator (size == strlen) or
  // is even smaller (size == 0 / size == 3).
  std::array<char, 7> tight_buf{};
  EXPECT_LT(H5Tenum_nameof(enum_dt, &kUnsortedEntries[4].value,
                           tight_buf.data(), tight_buf.size()),
            0);
  EXPECT_EQ((std::string_view(tight_buf.data(), tight_buf.size())), "CHARLIE");

  std::array<char, 3> tiny_buf{};
  EXPECT_LT(H5Tenum_nameof(enum_dt, &kUnsortedEntries[4].value, tiny_buf.data(),
                           tiny_buf.size()),
            0);
  EXPECT_EQ((std::string_view(tiny_buf.data(), tiny_buf.size())), "CHA");

  char zero_buf[2] = {'X', '\0'};
  EXPECT_LT(H5Tenum_nameof(enum_dt, &kUnsortedEntries[1].value, zero_buf, 0),
            0);
  EXPECT_EQ(zero_buf[0], 'X');

  // Empty enum lookups fail; nameof clears first byte when size > 0.
  char name_buf[16] = {'Z', 'Z', '\0'};
  const int32_t probe_val = 10;
  int32_t out_val = 0;
  EXPECT_LT(H5Tenum_nameof(empty_enum, &probe_val, name_buf, sizeof(name_buf)),
            0);
  EXPECT_EQ(name_buf[0], '\0');
  EXPECT_LT(H5Tenum_valueof(empty_enum, "ALPHA", &out_val), 0);

  // Absent values (below min, between elements, above max) and absent names.
  for (int32_t missing : {0, 25, 99}) {
    name_buf[0] = 'Q';
    EXPECT_LT(H5Tenum_nameof(enum_dt, &missing, name_buf, sizeof(name_buf)), 0);
    EXPECT_EQ(name_buf[0], '\0');
  }
  for (const char* missing_name : {"AAA", "BETA", "ZULU"}) {
    EXPECT_LT(H5Tenum_valueof(enum_dt, missing_name, &out_val), 0);
  }

  // Invalid type and null argument checks for H5Tenum_nameof.
  EXPECT_LT(
      H5Tenum_nameof(H5I_INVALID_HID, &probe_val, name_buf, sizeof(name_buf)),
      0);
  EXPECT_LT(
      H5Tenum_nameof(H5T_NATIVE_INT32, &probe_val, name_buf, sizeof(name_buf)),
      0);
  EXPECT_LT(H5Tenum_nameof(enum_dt, nullptr, name_buf, sizeof(name_buf)), 0);
  EXPECT_LT(H5Tenum_nameof(enum_dt, &probe_val, nullptr, 0), 0);

  // Invalid type and null/empty argument checks for H5Tenum_valueof.
  EXPECT_LT(H5Tenum_valueof(H5I_INVALID_HID, "ALPHA", &out_val), 0);
  EXPECT_LT(H5Tenum_valueof(H5T_NATIVE_INT32, "ALPHA", &out_val), 0);
  EXPECT_LT(H5Tenum_valueof(enum_dt, nullptr, &out_val), 0);
  EXPECT_LT(H5Tenum_valueof(enum_dt, "", &out_val), 0);
  EXPECT_LT(H5Tenum_valueof(enum_dt, "ALPHA", nullptr), 0);
}

TEST(H5TenumTest, DatasetRoundTripWithBigEndianEnum) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  // 1. Create a 2-byte big-endian enum and populate raw big-endian values.
  ScopedType file_enum(H5Tenum_create(H5T_STD_U16BE));
  ASSERT_TRUE(file_enum.valid());
  const std::array<uint8_t, 2> raw_low = {0x00, 0x01};
  const std::array<uint8_t, 2> raw_high = {0x01, 0x00};
  ASSERT_GE(H5Tenum_insert(file_enum, "LOW", raw_low.data()), 0);
  ASSERT_GE(H5Tenum_insert(file_enum, "HIGH", raw_high.data()), 0);

  // 2. Write a dataset using native uint16 memory enum converted to big-endian
  // disk enum.
  ScopedType mem_enum(H5Tenum_create(H5T_NATIVE_UINT16));
  ASSERT_TRUE(mem_enum.valid());
  const uint16_t mem_low = 1;
  const uint16_t mem_high = 256;
  ASSERT_GE(H5Tenum_insert(mem_enum, "LOW", &mem_low), 0);
  ASSERT_GE(H5Tenum_insert(mem_enum, "HIGH", &mem_high), 0);

  const hsize_t dims = 3;
  ScopedSpace space(H5Screate_simple(1, &dims, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file, "enum_dset", file_enum, space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());

  const std::array<uint16_t, 3> input = {mem_high, mem_low, mem_high};
  ASSERT_GE(
      H5Dwrite(dset, mem_enum, H5S_ALL, H5S_ALL, H5P_DEFAULT, input.data()), 0);

  std::array<uint16_t, 3> output = {0, 0, 0};
  ASSERT_GE(
      H5Dread(dset, mem_enum, H5S_ALL, H5S_ALL, H5P_DEFAULT, output.data()), 0);
  EXPECT_THAT(output, ElementsAre(mem_high, mem_low, mem_high));
}

}  // namespace
}  // namespace hdf5
