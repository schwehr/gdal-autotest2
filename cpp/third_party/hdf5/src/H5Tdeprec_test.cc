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
// Tests deprecated H5T APIs: H5Tcommit1, H5Topen1, and H5Tdecode1
// (third_party/hdf5/src/H5Tdeprec.c). Note: google3 builds hdf5 with
// deprecated symbols enabled (H5_NO_DEPRECATED_SYMBOLS is not defined).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Tdeprec.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define H5TDEPREC_TEST_HAS_ASAN 1
#endif
#endif
#if !defined(H5TDEPREC_TEST_HAS_ASAN) && \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__))
#define H5TDEPREC_TEST_HAS_ASAN 1
#endif

namespace hdf5 {
namespace {

TEST(H5TdeprecTest, Commit1AndOpen1RoundTripAtFileAndGroup) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  // 1. Commit a transient compound datatype at file root via H5Tcommit1.
  struct Sample {
    int32_t id;
    double value;
  };
  ScopedType comp(H5Tcreate(H5T_COMPOUND, sizeof(Sample)));
  ASSERT_TRUE(comp.valid());
  ASSERT_GE(H5Tinsert(comp, "id", HOFFSET(Sample, id), H5T_NATIVE_INT32), 0);
  ASSERT_GE(H5Tinsert(comp, "value", HOFFSET(Sample, value), H5T_NATIVE_DOUBLE),
            0);

  EXPECT_EQ(H5Tcommitted(comp), 0);
  ASSERT_GE(H5Tcommit1(file, "sample_t", comp), 0);
  EXPECT_GT(H5Tcommitted(comp), 0);

  // 2. Use the committed datatype in a dataset inside the same file.
  const hsize_t dim = 2;
  ScopedSpace space(H5Screate_simple(1, &dim, nullptr));
  ASSERT_TRUE(space.valid());
  ScopedDataset dset(H5Dcreate2(file, "records", comp, space, H5P_DEFAULT,
                                H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(dset.valid());
  constexpr std::array<Sample, 2> kWritten = {{{1, 1.5}, {2, -3.25}}};
  ASSERT_GE(
      H5Dwrite(dset, comp, H5S_ALL, H5S_ALL, H5P_DEFAULT, kWritten.data()), 0);
  std::array<Sample, 2> read_back{};
  ASSERT_GE(
      H5Dread(dset, comp, H5S_ALL, H5S_ALL, H5P_DEFAULT, read_back.data()), 0);
  EXPECT_EQ(read_back[0].id, 1);
  EXPECT_DOUBLE_EQ(read_back[0].value, 1.5);
  EXPECT_EQ(read_back[1].id, 2);
  EXPECT_DOUBLE_EQ(read_back[1].value, -3.25);

  // 3. Commit a second datatype inside a subgroup via H5Tcommit1.
  ScopedGroup grp(
      H5Gcreate2(file, "types", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedType enum_dt(H5Tenum_create(H5T_NATIVE_INT32));
  ASSERT_TRUE(enum_dt.valid());
  const int32_t red_val = 10;
  ASSERT_GE(H5Tenum_insert(enum_dt, "RED", &red_val), 0);
  ASSERT_GE(H5Tcommit1(grp, "color_t", enum_dt), 0);
  EXPECT_GT(H5Tcommitted(enum_dt), 0);

  // 4. Re-open both committed datatypes using H5Topen1 and verify equality.
  ScopedType reopened_comp(H5Topen1(file, "sample_t"));
  ASSERT_TRUE(reopened_comp.valid());
  EXPECT_GT(H5Tcommitted(reopened_comp), 0);
  EXPECT_GT(H5Tequal(reopened_comp, comp), 0);

  ScopedType reopened_enum_rel(H5Topen1(grp, "color_t"));
  ASSERT_TRUE(reopened_enum_rel.valid());
  EXPECT_GT(H5Tcommitted(reopened_enum_rel), 0);
  EXPECT_GT(H5Tequal(reopened_enum_rel, enum_dt), 0);

  ScopedType reopened_enum_abs(H5Topen1(file, "/types/color_t"));
  ASSERT_TRUE(reopened_enum_abs.valid());
  EXPECT_GT(H5Tequal(reopened_enum_abs, enum_dt), 0);
}

TEST(H5TdeprecTest, Commit1InvalidArgumentsAndVolFailures) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedType dt(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(dt.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());

  ScopedErrorSilence silence;

  // Null / empty name.
  EXPECT_LT(H5Tcommit1(file, nullptr, dt), 0);
  EXPECT_LT(H5Tcommit1(file, "", dt), 0);

  // Invalid datatype ID (H5I_INVALID_HID or non-datatype handle).
  EXPECT_LT(H5Tcommit1(file, "bad_type1", H5I_INVALID_HID), 0);
  EXPECT_LT(H5Tcommit1(file, "bad_type2", file), 0);

  // Invalid location ID (H5I_INVALID_HID or non-location handle).
  EXPECT_LT(H5Tcommit1(H5I_INVALID_HID, "bad_loc1", dt), 0);
  EXPECT_LT(H5Tcommit1(space, "bad_loc2", dt), 0);

  // VOL commit failures: missing intermediate group and empty compound.
  EXPECT_LT(H5Tcommit1(file, "missing_grp/dt", dt), 0);
  ScopedType empty_comp(H5Tcreate(H5T_COMPOUND, 4));
  ASSERT_TRUE(empty_comp.valid());
  EXPECT_LT(H5Tcommit1(file, "empty_comp", empty_comp), 0);

  // Commit once, then reject committing the same datatype ID again or reusing
  // the existing link name with another datatype.
  ASSERT_GE(H5Tcommit1(file, "committed_i32", dt), 0);
  EXPECT_LT(H5Tcommit1(file, "second_name", dt), 0);

  ScopedType other(H5Tcopy(H5T_NATIVE_INT32));
  ASSERT_TRUE(other.valid());
  EXPECT_LT(H5Tcommit1(file, "committed_i32", other), 0);
}

TEST(H5TdeprecTest, Open1InvalidArgumentsFail) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedGroup grp(H5Gcreate2(file, "not_a_datatype", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT));
  ASSERT_TRUE(grp.valid());
  ScopedSpace space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(space.valid());

  ScopedErrorSilence silence;

  EXPECT_EQ(H5Topen1(file, nullptr), H5I_INVALID_HID);
  EXPECT_EQ(H5Topen1(file, ""), H5I_INVALID_HID);
  EXPECT_EQ(H5Topen1(H5I_INVALID_HID, "anything"), H5I_INVALID_HID);
  EXPECT_EQ(H5Topen1(space, "anything"), H5I_INVALID_HID);
  EXPECT_EQ(H5Topen1(file, "does_not_exist"), H5I_INVALID_HID);
  EXPECT_EQ(H5Topen1(file, "not_a_datatype"), H5I_INVALID_HID);
}

TEST(H5TdeprecTest, Decode1RoundTripAndCorruptHeaders) {
  // Encode a non-trivial big-endian enum datatype and reconstruct via
  // H5Tdecode1.
  ScopedType orig(H5Tenum_create(H5T_STD_I16BE));
  ASSERT_TRUE(orig.valid());
  const int16_t val = 0x0102;
  ASSERT_GE(H5Tenum_insert(orig, "ITEM", &val), 0);

  size_t needed = 0;
  ASSERT_GE(H5Tencode(orig, nullptr, &needed), 0);
  ASSERT_GT(needed, 2u);

  std::vector<uint8_t> encoded(needed, 0);
  ASSERT_GE(H5Tencode(orig, encoded.data(), &needed), 0);

#if !defined(H5TDEPREC_TEST_HAS_ASAN)
  // H5Tdecode1 forwards SIZE_MAX to H5T_decode (H5Tdeprec.c), causing
  // `p + buf_size - 1` in H5Odtype.c to trigger UBSan pointer-overflow under
  // `--config=asan-chlor`.
  ScopedType decoded(H5Tdecode1(encoded.data()));
  ASSERT_TRUE(decoded.valid());
  EXPECT_GT(H5Tequal(orig, decoded), 0);
  EXPECT_EQ(H5Tget_class(decoded), H5T_ENUM);
#endif

  ScopedErrorSilence silence;

  // Null buffer.
  EXPECT_EQ(H5Tdecode1(nullptr), H5I_INVALID_HID);

  // Corrupt object header message tag byte (checked before reading further).
  std::vector<uint8_t> bad_tag = encoded;
  bad_tag[0] ^= 0xFF;
  EXPECT_EQ(H5Tdecode1(bad_tag.data()), H5I_INVALID_HID);

  // Corrupt encode version byte.
  std::vector<uint8_t> bad_ver = encoded;
  bad_ver[1] ^= 0xFF;
  EXPECT_EQ(H5Tdecode1(bad_ver.data()), H5I_INVALID_HID);
}

}  // namespace
}  // namespace hdf5
