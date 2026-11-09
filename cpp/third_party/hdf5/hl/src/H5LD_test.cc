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
// Tests HDF5 High-Level Dataset Watch APIs (third_party/hdf5/hl/src/H5LD.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/src/H5LD.c
//   https://github.com/HDFGroup/hdf5/blob/develop/hl/test/test_ld.c

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/h5_test_util.h"
#include "third_party/hdf5/hl/src/H5LDpublic.h"
#include "third_party/hdf5/hl/src/hdf5_hl.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

struct InnerRecord {
  double x;
  std::array<int32_t, 2> vec;
};

struct OuterRecord {
  int32_t a;
  InnerRecord inner;
  int16_t tail;
};

struct EscapedRecord {
  int32_t dot_field;
  double comma_field;
};

ScopedDatatype CreateOuterCompoundType() {
  constexpr std::array<hsize_t, 1> kVecDims = {2};
  ScopedDatatype vec_tid(
      H5Tarray_create2(H5T_NATIVE_INT32, 1, kVecDims.data()));
  if (!vec_tid.valid()) {
    return ScopedDatatype();
  }

  ScopedDatatype inner_tid(H5Tcreate(H5T_COMPOUND, sizeof(InnerRecord)));
  if (!inner_tid.valid()) {
    return ScopedDatatype();
  }
  if (H5Tinsert(inner_tid, "x", offsetof(InnerRecord, x), H5T_NATIVE_DOUBLE) <
          0 ||
      H5Tinsert(inner_tid, "vec", offsetof(InnerRecord, vec), vec_tid) < 0) {
    return ScopedDatatype();
  }

  ScopedDatatype outer_tid(H5Tcreate(H5T_COMPOUND, sizeof(OuterRecord)));
  if (!outer_tid.valid()) {
    return ScopedDatatype();
  }
  if (H5Tinsert(outer_tid, "a", offsetof(OuterRecord, a), H5T_NATIVE_INT32) <
          0 ||
      H5Tinsert(outer_tid, "inner", offsetof(OuterRecord, inner), inner_tid) <
          0 ||
      H5Tinsert(outer_tid, "tail", offsetof(OuterRecord, tail),
                H5T_NATIVE_INT16) < 0) {
    return ScopedDatatype();
  }
  return outer_tid;
}

TEST(H5ldTest, GetDsetDimsAndTypeSizeSuccessAndErrors) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  // 1. Create a 2D chunked extendible scalar dataset.
  constexpr std::array<hsize_t, 2> kInitDims = {2, 3};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, H5S_UNLIMITED};
  constexpr std::array<hsize_t, 2> kChunkDims = {2, 3};
  ScopedDataspace space(H5Screate_simple(2, kInitDims.data(), kMaxDims.data()));
  ASSERT_GE(space.get(), 0);
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(dcpl.get(), 0);
  ASSERT_GE(H5Pset_chunk(dcpl, 2, kChunkDims.data()), 0);

  ScopedDataset scalar_dset(H5Dcreate2(file, "scalar", H5T_NATIVE_INT32, space,
                                       H5P_DEFAULT, dcpl, H5P_DEFAULT));
  ASSERT_GE(scalar_dset.get(), 0);

  std::array<hsize_t, 2> cur_dims = {0, 0};
  EXPECT_GE(H5LDget_dset_dims(scalar_dset, cur_dims.data()), 0);
  EXPECT_THAT(cur_dims, ElementsAre(2u, 3u));
  EXPECT_EQ(sizeof(int32_t), H5LDget_dset_type_size(scalar_dset, nullptr));

  // 2. Query full size, individual, nested, and multi-field sizes on a compound
  //    dataset.
  ScopedDatatype outer_tid = CreateOuterCompoundType();
  ASSERT_GE(outer_tid.get(), 0);
  ScopedDataset cmpd_dset(H5Dcreate2(file, "compound", outer_tid, space,
                                     H5P_DEFAULT, dcpl, H5P_DEFAULT));
  ASSERT_GE(cmpd_dset.get(), 0);

  EXPECT_EQ(H5Tget_size(outer_tid), H5LDget_dset_type_size(cmpd_dset, nullptr));
  EXPECT_EQ(sizeof(int32_t), H5LDget_dset_type_size(cmpd_dset, "a"));
  EXPECT_EQ(sizeof(double), H5LDget_dset_type_size(cmpd_dset, "inner.x"));
  EXPECT_EQ(sizeof(int32_t) + sizeof(double) + sizeof(int16_t),
            H5LDget_dset_type_size(cmpd_dset, "a,inner.x,tail"));

  // 3. Verify backslash-escaped separator characters ('.' and ',') in field
  //    names.
  ScopedDatatype esc_tid(H5Tcreate(H5T_COMPOUND, sizeof(EscapedRecord)));
  ASSERT_GE(esc_tid.get(), 0);
  ASSERT_GE(H5Tinsert(esc_tid, "a.b", offsetof(EscapedRecord, dot_field),
                      H5T_NATIVE_INT32),
            0);
  ASSERT_GE(H5Tinsert(esc_tid, "c,d", offsetof(EscapedRecord, comma_field),
                      H5T_NATIVE_DOUBLE),
            0);
  ScopedDataset esc_dset(H5Dcreate2(file, "escaped", esc_tid, space,
                                    H5P_DEFAULT, dcpl, H5P_DEFAULT));
  ASSERT_GE(esc_dset.get(), 0);
  EXPECT_EQ(sizeof(int32_t) + sizeof(double),
            H5LDget_dset_type_size(esc_dset, "a\\.b,c\\,d"));

  // 4. Negative paths (never pass "" to H5LDget_dset_type_size because
  //    upstream has active assert(fields && *fields)).
  ScopedErrorSilence silence;
  EXPECT_LT(H5LDget_dset_dims(scalar_dset, nullptr), 0);
  EXPECT_LT(H5LDget_dset_dims(H5I_INVALID_HID, cur_dims.data()), 0);
  EXPECT_EQ(0u, H5LDget_dset_type_size(H5I_INVALID_HID, nullptr));
  EXPECT_EQ(0u, H5LDget_dset_type_size(scalar_dset, "a"));
  EXPECT_EQ(0u, H5LDget_dset_type_size(cmpd_dset, ","));
  EXPECT_EQ(0u, H5LDget_dset_type_size(cmpd_dset, "a,"));
  EXPECT_EQ(0u, H5LDget_dset_type_size(cmpd_dset, ".a"));
  EXPECT_EQ(0u, H5LDget_dset_type_size(cmpd_dset, "a."));
  EXPECT_EQ(0u, H5LDget_dset_type_size(cmpd_dset, "a\\"));
  EXPECT_EQ(0u, H5LDget_dset_type_size(cmpd_dset, "missing"));
  EXPECT_EQ(0u, H5LDget_dset_type_size(cmpd_dset, "inner.missing"));
  EXPECT_EQ(0u, H5LDget_dset_type_size(cmpd_dset, "a.sub"));
}

TEST(H5ldTest, GetDsetElmtsScalarOneAndTwoChangedDimensions) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  constexpr std::array<hsize_t, 2> kDims0 = {2, 2};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, H5S_UNLIMITED};
  constexpr std::array<hsize_t, 2> kChunkDims = {4, 4};
  ScopedDataspace space(H5Screate_simple(2, kDims0.data(), kMaxDims.data()));
  ASSERT_GE(space.get(), 0);
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(dcpl.get(), 0);
  ASSERT_GE(H5Pset_chunk(dcpl, 2, kChunkDims.data()), 0);

  ScopedDataset dset(H5Dcreate2(file, "grid", H5T_NATIVE_INT32, space,
                                H5P_DEFAULT, dcpl, H5P_DEFAULT));
  ASSERT_GE(dset.get(), 0);

  // Initial 2x2:
  //   100, 101
  //   110, 111
  constexpr std::array<int32_t, 4> kInitData = {100, 101, 110, 111};
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     kInitData.data()),
            0);

  // 1. Grow along 1 axis (2x2 -> 2x3): exercises ctr == 1 branch.
  constexpr std::array<hsize_t, 2> kDims1 = {2, 3};
  ASSERT_GE(H5Dset_extent(dset, kDims1.data()), 0);
  constexpr std::array<int32_t, 6> kData1 = {100, 101, 102, 110, 111, 112};
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     kData1.data()),
            0);

  std::vector<int32_t> delta1(2, -999);
  ASSERT_GE(H5LDget_dset_elmts(dset, kDims0.data(), kDims1.data(), nullptr,
                               delta1.data()),
            0);
  EXPECT_THAT(delta1, ElementsAre(102, 112));

  // 2. Grow along both axes (2x3 -> 3x4): exercises ctr >= 2 (H5S_SELECT_NOTB)
  //    branch selecting the L-shaped border of 6 new elements.
  constexpr std::array<hsize_t, 2> kDims2 = {3, 4};
  ASSERT_GE(H5Dset_extent(dset, kDims2.data()), 0);
  constexpr std::array<int32_t, 12> kData2 = {100, 101, 102, 103, 110, 111,
                                              112, 113, 120, 121, 122, 123};
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     kData2.data()),
            0);

  std::vector<int32_t> delta2(6, -999);
  ASSERT_GE(H5LDget_dset_elmts(dset, kDims1.data(), kDims2.data(), nullptr,
                               delta2.data()),
            0);
  EXPECT_THAT(delta2, ElementsAre(103, 113, 120, 121, 122, 123));

  // 3. One axis grows while the other shrinks (prev = {3, 4} -> cur = {4, 2}):
  //    MIN(prev_dims[1], cur_dims[1]) = 2 selects 1 row x 2 cols = 2 elements.
  constexpr std::array<hsize_t, 2> kDims3 = {4, 2};
  ASSERT_GE(H5Dset_extent(dset, kDims3.data()), 0);
  constexpr std::array<int32_t, 8> kData3 = {100, 101, 110, 111,
                                             120, 121, 130, 131};
  ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                     kData3.data()),
            0);

  std::vector<int32_t> delta3(2, -999);
  ASSERT_GE(H5LDget_dset_elmts(dset, kDims2.data(), kDims3.data(), nullptr,
                               delta3.data()),
            0);
  EXPECT_THAT(delta3, ElementsAre(130, 131));

  // 4. Negative parameter and geometry checks.
  ScopedErrorSilence silence;
  EXPECT_LT(
      H5LDget_dset_elmts(dset, nullptr, kDims3.data(), nullptr, delta3.data()),
      0);
  EXPECT_LT(
      H5LDget_dset_elmts(dset, kDims2.data(), nullptr, nullptr, delta3.data()),
      0);
  EXPECT_LT(
      H5LDget_dset_elmts(dset, kDims2.data(), kDims3.data(), nullptr, nullptr),
      0);
  EXPECT_LT(H5LDget_dset_elmts(H5I_INVALID_HID, kDims2.data(), kDims3.data(),
                               nullptr, delta3.data()),
            0);
  // cur_dims <= prev_dims across all dimensions -> ctr == 0.
  EXPECT_LT(H5LDget_dset_elmts(dset, kDims3.data(), kDims3.data(), nullptr,
                               delta3.data()),
            0);
  // Selection has 0 elements (prev={1,0}, cur={2,0}) -> snum_elmts == 0.
  constexpr std::array<hsize_t, 2> kZeroPrev = {1, 0};
  constexpr std::array<hsize_t, 2> kZeroCur = {2, 0};
  EXPECT_LT(H5LDget_dset_elmts(dset, kZeroPrev.data(), kZeroCur.data(), nullptr,
                               delta3.data()),
            0);
  // Selection exceeds current extent (prev={0,0}, cur={10,2}) -> H5Dread fails.
  constexpr std::array<hsize_t, 2> kZeroDims = {0, 0};
  constexpr std::array<hsize_t, 2> kOobDims = {10, 2};
  std::vector<int32_t> large_buf(20, 0);
  EXPECT_LT(H5LDget_dset_elmts(dset, kZeroDims.data(), kOobDims.data(), nullptr,
                               large_buf.data()),
            0);
  // Non-compound dataset with non-null fields fails.
  EXPECT_LT(H5LDget_dset_elmts(dset, kDims2.data(), kDims3.data(), "a",
                               delta3.data()),
            0);
}

TEST(H5ldTest, GetDsetElmtsExtractsNestedCompoundFields) {
  ScopedFile file = CreateCoreFile();
  ASSERT_GE(file.get(), 0);

  ScopedDatatype outer_tid = CreateOuterCompoundType();
  ASSERT_GE(outer_tid.get(), 0);

  constexpr std::array<hsize_t, 1> kInitDims = {1};
  constexpr std::array<hsize_t, 1> kMaxDims = {H5S_UNLIMITED};
  constexpr std::array<hsize_t, 1> kChunkDims = {4};
  ScopedDataspace space(H5Screate_simple(1, kInitDims.data(), kMaxDims.data()));
  ASSERT_GE(space.get(), 0);
  ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
  ASSERT_GE(dcpl.get(), 0);
  ASSERT_GE(H5Pset_chunk(dcpl, 1, kChunkDims.data()), 0);

  ScopedDataset dset(H5Dcreate2(file, "records", outer_tid, space, H5P_DEFAULT,
                                dcpl, H5P_DEFAULT));
  ASSERT_GE(dset.get(), 0);

  constexpr std::array<hsize_t, 1> kExtendedDims = {3};
  ASSERT_GE(H5Dset_extent(dset, kExtendedDims.data()), 0);
  constexpr std::array<OuterRecord, 3> kRecords = {{
      {.a = 1, .inner = {.x = 1.5, .vec = {10, 11}}, .tail = 100},
      {.a = 2, .inner = {.x = 2.5, .vec = {20, 21}}, .tail = 200},
      {.a = 3, .inner = {.x = 3.5, .vec = {30, 31}}, .tail = 300},
  }};
  ASSERT_GE(
      H5Dwrite(dset, outer_tid, H5S_ALL, H5S_ALL, H5P_DEFAULT, kRecords.data()),
      0);

  // Read only newly appended elements [1..3) and extract packed "a,inner.x".
  const size_t elem_size = H5LDget_dset_type_size(dset, "a,inner.x");
  ASSERT_EQ(sizeof(int32_t) + sizeof(double), elem_size);
  std::vector<uint8_t> raw(2 * elem_size, 0);
  ASSERT_GE(H5LDget_dset_elmts(dset, kInitDims.data(), kExtendedDims.data(),
                               "a,inner.x", raw.data()),
            0);

  for (size_t i = 0; i < 2; ++i) {
    int32_t actual_a = 0;
    double actual_x = 0.0;
    const size_t base = i * elem_size;
    std::memcpy(&actual_a, raw.data() + base, sizeof(actual_a));
    std::memcpy(&actual_x, raw.data() + base + sizeof(actual_a),
                sizeof(actual_x));
    EXPECT_EQ(kRecords[i + 1].a, actual_a);
    EXPECT_DOUBLE_EQ(kRecords[i + 1].inner.x, actual_x);
  }

  ScopedErrorSilence silence;
  EXPECT_LT(H5LDget_dset_elmts(dset, kInitDims.data(), kExtendedDims.data(),
                               "missing", raw.data()),
            0);
  constexpr std::array<hsize_t, 1> kZero = {0};
  constexpr std::array<hsize_t, 1> kOob = {10};
  std::vector<OuterRecord> big(10);
  EXPECT_LT(
      H5LDget_dset_elmts(dset, kZero.data(), kOob.data(), "a", big.data()), 0);
}

}  // namespace
}  // namespace hdf5

// TODO(schwehr): Remaining uncovered lines in H5LD.c are mid-parse
// calloc/malloc OOM branches and defensive internal HDF5 failure guards.
