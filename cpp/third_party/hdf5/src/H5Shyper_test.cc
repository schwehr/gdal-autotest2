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
// Tests HDF5 hyperslab selection creation, boolean combination, rebuilding,
// span construction, block/bounds/shape queries, offset adjustment,
// intersection/projection, unlimited-dimension clipping helpers, selection
// serialization/deserialization (v1/v2/v3), and selection iterators
// (third_party/hdf5/src/H5Shyper.c).
//
// In default non-MPI Linux google3 builds, H5_HAVE_PARALLEL, H5S_TESTING, and
// H5S_HYPER_DEBUG are disabled.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Shyper.c

// Match :hdf5's -DNDEBUG struct layout across internal headers before
// including hdf5_internal_prelude.h and H5Spkg.h.
#ifndef NDEBUG
#define H5SHYPER_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5S_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
#include "third_party/hdf5/src/H5Tprivate.h"
}  // extern "C"
#ifdef H5SHYPER_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5SHYPER_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

using ::testing::ElementsAre;

using ScopedSelIterId = ScopedH5Id<H5Ssel_iter_close>;

struct InternalSpaceDeleter {
  void operator()(H5S_t* s) const {
    if (s != nullptr) {
      H5S_close(s);
    }
  }
};
using UniqueInternalSpace = std::unique_ptr<H5S_t, InternalSpaceDeleter>;

class ScopedSelIter {
 public:
  ScopedSelIter() = default;
  ~ScopedSelIter() { reset(); }

  ScopedSelIter(const ScopedSelIter&) = delete;
  ScopedSelIter& operator=(const ScopedSelIter&) = delete;

  herr_t init(H5S_t* space, size_t elmt_size, unsigned flags = 0) {
    reset();
    herr_t rc = H5S_select_iter_init(&iter_, space, elmt_size, flags);
    if (rc >= 0) {
      initialized_ = true;
    }
    return rc;
  }

  void reset() {
    if (initialized_) {
      H5S_select_iter_release(&iter_);
      initialized_ = false;
    }
  }

  H5S_sel_iter_t* get() { return &iter_; }
  const H5S_sel_iter_t* get() const { return &iter_; }

 private:
  H5S_sel_iter_t iter_{};
  bool initialized_ = false;
};

class ScopedApiContext {
 public:
  ScopedApiContext() {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
    }
  }
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

H5S_t* UnwrapSpace(hid_t space_id) {
  return static_cast<H5S_t*>(H5I_object_verify(space_id, H5I_DATASPACE));
}

ScopedSpace MakeSimpleSpace(std::span<const hsize_t> dims,
                            std::span<const hsize_t> maxdims = {}) {
  return ScopedSpace(H5Screate_simple(
      static_cast<int>(dims.size()), dims.data(),
      maxdims.empty() ? nullptr : maxdims.data()));
}

// Collects all byte sequences from a public selection iterator.
std::vector<std::pair<hsize_t, size_t>> CollectSeqList(hid_t space_id,
                                                       size_t elmt_size,
                                                       unsigned flags,
                                                       size_t maxseq,
                                                       size_t maxelmts) {
  std::vector<std::pair<hsize_t, size_t>> result;
  ScopedSelIterId iter(H5Ssel_iter_create(space_id, elmt_size, flags));
  if (!iter.valid()) {
    ADD_FAILURE() << "H5Ssel_iter_create failed";
    return result;
  }
  std::vector<hsize_t> off(maxseq);
  std::vector<size_t> len(maxseq);
  for (;;) {
    size_t nseq = 0;
    size_t nelmts = 0;
    if (H5Ssel_iter_get_seq_list(iter.get(), maxseq, maxelmts, &nseq, &nelmts,
                                 off.data(), len.data()) < 0) {
      ADD_FAILURE() << "H5Ssel_iter_get_seq_list failed";
      break;
    }
    if (nseq == 0) {
      EXPECT_EQ(nelmts, 0u);
      break;
    }
    for (size_t i = 0; i < nseq; ++i) {
      result.emplace_back(off[i], len[i]);
    }
  }
  return result;
}

TEST(H5ShyperTest, SelectHyperslabErrorHandlingAndValidation) {
  ScopedErrorSilence silence;

  constexpr std::array<hsize_t, 2> kDims = {10, 12};
  constexpr std::array<hsize_t, 2> kStart = {0, 0};
  constexpr std::array<hsize_t, 2> kStrideZero = {0, 2};
  constexpr std::array<hsize_t, 2> kStrideSmall = {2, 3};
  constexpr std::array<hsize_t, 2> kCountTwo = {2, 2};
  constexpr std::array<hsize_t, 2> kBlockLarge = {3, 4};

  ScopedSpace space = MakeSimpleSpace(kDims);
  ASSERT_TRUE(space.valid());

  EXPECT_LT(H5Sselect_hyperslab(H5I_INVALID_HID, H5S_SELECT_SET,
                                kStart.data(), nullptr, kCountTwo.data(),
                                nullptr),
            0);

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());
  EXPECT_LT(H5Sselect_hyperslab(scalar.get(), H5S_SELECT_SET, kStart.data(),
                                nullptr, kCountTwo.data(), nullptr),
            0);

  ScopedSpace null_space(H5Screate(H5S_NULL));
  ASSERT_TRUE(null_space.valid());
  EXPECT_LT(H5Sselect_hyperslab(null_space.get(), H5S_SELECT_SET, kStart.data(),
                                nullptr, kCountTwo.data(), nullptr),
            0);

  EXPECT_LT(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, nullptr, nullptr,
                                kCountTwo.data(), nullptr),
            0);
  EXPECT_LT(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, kStart.data(),
                                nullptr, nullptr, nullptr),
            0);
  EXPECT_LT(H5Sselect_hyperslab(space.get(), H5S_SELECT_NOOP, kStart.data(),
                                nullptr, kCountTwo.data(), nullptr),
            0);
  EXPECT_LT(H5Sselect_hyperslab(space.get(), H5S_SELECT_INVALID, kStart.data(),
                                nullptr, kCountTwo.data(), nullptr),
            0);
  EXPECT_LT(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, kStart.data(),
                                kStrideZero.data(), kCountTwo.data(), nullptr),
            0);
  // Overlapping blocks when count > 1 and stride < block.
  EXPECT_LT(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, kStart.data(),
                                kStrideSmall.data(), kCountTwo.data(),
                                kBlockLarge.data()),
            0);

  // Operations other than SET fail when applied to a point selection.
  constexpr std::array<hsize_t, 2> kPt = {1, 1};
  ASSERT_GE(
      H5Sselect_elements(space.get(), H5S_SELECT_SET, 1, kPt.data()), 0);
  EXPECT_LT(H5Sselect_hyperslab(space.get(), H5S_SELECT_OR, kStart.data(),
                                nullptr, kCountTwo.data(), nullptr),
            0);
  // SET replaces the point selection.
  EXPECT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, kStart.data(),
                                nullptr, kCountTwo.data(), nullptr),
            0);
  EXPECT_EQ(H5Sget_select_npoints(space.get()), 4);
}

TEST(H5ShyperTest, SelectHyperslabDefaultsMergingAndZeroSized) {
  constexpr std::array<hsize_t, 2> kDims = {10, 12};
  ScopedSpace space = MakeSimpleSpace(kDims);
  ASSERT_TRUE(space.valid());

  // NULL stride and NULL block use default 1s.
  constexpr std::array<hsize_t, 2> kStart = {1, 2};
  constexpr std::array<hsize_t, 2> kCount = {3, 4};
  ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, kStart.data(),
                                nullptr, kCount.data(), nullptr),
            0);
  EXPECT_EQ(H5Sget_select_npoints(space.get()), 12);

  // Contiguous block merge when stride == block (both block == 1 and block > 1).
  constexpr std::array<hsize_t, 2> kStrideMerge = {1, 2};
  constexpr std::array<hsize_t, 2> kCountMerge = {4, 3};
  constexpr std::array<hsize_t, 2> kBlockMerge = {1, 2};
  ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, kStart.data(),
                                kStrideMerge.data(), kCountMerge.data(),
                                kBlockMerge.data()),
            0);
  EXPECT_GT(H5Sis_regular_hyperslab(space.get()), 0);
  std::array<hsize_t, 2> reg_start{};
  std::array<hsize_t, 2> reg_stride{};
  std::array<hsize_t, 2> reg_count{};
  std::array<hsize_t, 2> reg_block{};
  ASSERT_GE(H5Sget_regular_hyperslab(space.get(), reg_start.data(),
                                     reg_stride.data(), reg_count.data(),
                                     reg_block.data()),
            0);
  EXPECT_THAT(reg_count, ElementsAre(4, 3));
  EXPECT_THAT(reg_block, ElementsAre(1, 2));
  const H5S_t* space_ptr = UnwrapSpace(space.get());
  EXPECT_EQ(space_ptr->select.sel_info.hslab->diminfo.opt[0].count, 1u);
  EXPECT_EQ(space_ptr->select.sel_info.hslab->diminfo.opt[0].block, 4u);
  EXPECT_EQ(space_ptr->select.sel_info.hslab->diminfo.opt[1].count, 1u);
  EXPECT_EQ(space_ptr->select.sel_info.hslab->diminfo.opt[1].block, 6u);

  // Zero-sized hyperslabs under all operators on regular selections.
  constexpr std::array<hsize_t, 2> kZeroCount = {0, 3};
  ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_OR, kStart.data(),
                                nullptr, kZeroCount.data(), nullptr),
            0);
  EXPECT_EQ(H5Sget_select_npoints(space.get()), 24);
  ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_XOR, kStart.data(),
                                nullptr, kZeroCount.data(), nullptr),
            0);
  EXPECT_EQ(H5Sget_select_npoints(space.get()), 24);
  ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_NOTB, kStart.data(),
                                nullptr, kZeroCount.data(), nullptr),
            0);
  EXPECT_EQ(H5Sget_select_npoints(space.get()), 24);

  for (H5S_seloper_t op : {H5S_SELECT_SET, H5S_SELECT_AND, H5S_SELECT_NOTA}) {
    ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_SET, kStart.data(),
                                  nullptr, kCount.data(), nullptr),
              0);
    constexpr std::array<hsize_t, 2> kZeroBlock = {1, 0};
    ASSERT_GE(H5Sselect_hyperslab(space.get(), op, kStart.data(), nullptr,
                                  kCount.data(), kZeroBlock.data()),
              0);
    EXPECT_EQ(H5Sget_select_type(space.get()), H5S_SEL_NONE);
  }
}

TEST(H5ShyperTest, SelectHyperslabOnNoneAndAllSelections) {
  constexpr std::array<hsize_t, 2> kDims = {6, 8};
  constexpr std::array<hsize_t, 2> kStart = {1, 2};
  constexpr std::array<hsize_t, 2> kCount = {2, 3};
  ScopedSpace space = MakeSimpleSpace(kDims);
  ASSERT_TRUE(space.valid());

  // Operations on H5S_SEL_NONE.
  for (H5S_seloper_t op : {H5S_SELECT_AND, H5S_SELECT_NOTB}) {
    ASSERT_GE(H5Sselect_none(space.get()), 0);
    ASSERT_GE(H5Sselect_hyperslab(space.get(), op, kStart.data(), nullptr,
                                  kCount.data(), nullptr),
              0);
    EXPECT_EQ(H5Sget_select_type(space.get()), H5S_SEL_NONE);
  }
  for (H5S_seloper_t op :
       {H5S_SELECT_SET, H5S_SELECT_OR, H5S_SELECT_XOR, H5S_SELECT_NOTA}) {
    ASSERT_GE(H5Sselect_none(space.get()), 0);
    ASSERT_GE(H5Sselect_hyperslab(space.get(), op, kStart.data(), nullptr,
                                  kCount.data(), nullptr),
              0);
    EXPECT_EQ(H5Sget_select_type(space.get()), H5S_SEL_HYPERSLABS);
    EXPECT_EQ(H5Sget_select_npoints(space.get()), 6);
  }

  // Operations on H5S_SEL_ALL.
  ASSERT_GE(H5Sselect_all(space.get()), 0);
  ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_OR, kStart.data(),
                                nullptr, kCount.data(), nullptr),
            0);
  EXPECT_EQ(H5Sget_select_type(space.get()), H5S_SEL_ALL);

  ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_AND, kStart.data(),
                                nullptr, kCount.data(), nullptr),
            0);
  EXPECT_EQ(H5Sget_select_type(space.get()), H5S_SEL_HYPERSLABS);
  EXPECT_EQ(H5Sget_select_npoints(space.get()), 6);

  ASSERT_GE(H5Sselect_all(space.get()), 0);
  ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_XOR, kStart.data(),
                                nullptr, kCount.data(), nullptr),
            0);
  EXPECT_EQ(H5Sget_select_type(space.get()), H5S_SEL_HYPERSLABS);
  EXPECT_EQ(H5Sget_select_npoints(space.get()), 42);

  ASSERT_GE(H5Sselect_all(space.get()), 0);
  ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_NOTB, kStart.data(),
                                nullptr, kCount.data(), nullptr),
            0);
  EXPECT_EQ(H5Sget_select_type(space.get()), H5S_SEL_HYPERSLABS);
  EXPECT_EQ(H5Sget_select_npoints(space.get()), 42);

  ASSERT_GE(H5Sselect_all(space.get()), 0);
  ASSERT_GE(H5Sselect_hyperslab(space.get(), H5S_SELECT_NOTA, kStart.data(),
                                nullptr, kCount.data(), nullptr),
            0);
  EXPECT_EQ(H5Sget_select_type(space.get()), H5S_SEL_NONE);
}

TEST(H5ShyperTest, BooleanSpanOpsAndIntervalOverlapCases) {
  // Exercise all six interval relationships in H5S__hyper_clip_spans and
  // H5S__hyper_merge_spans_helper across 1D and 3D spans.
  constexpr std::array<hsize_t, 1> kDims1d = {100};
  ScopedSpace s1d = MakeSimpleSpace(kDims1d);
  ASSERT_TRUE(s1d.valid());

  // Disjoint spans coalescing and enclosing intervals in 1D.
  constexpr hsize_t s_a = 10, c_a = 10;  // [10..19]
  constexpr hsize_t s_b = 30, c_b = 10;  // [30..39]
  constexpr hsize_t s_c = 50, c_c = 10;  // [50..59]
  ASSERT_GE(
      H5Sselect_hyperslab(s1d.get(), H5S_SELECT_SET, &s_a, nullptr, &c_a,
                          nullptr),
      0);
  ASSERT_GE(
      H5Sselect_hyperslab(s1d.get(), H5S_SELECT_OR, &s_b, nullptr, &c_b,
                          nullptr),
      0);
  ASSERT_GE(
      H5Sselect_hyperslab(s1d.get(), H5S_SELECT_OR, &s_c, nullptr, &c_c,
                          nullptr),
      0);
  EXPECT_EQ(H5Sget_select_npoints(s1d.get()), 30);

  // Span covering: before a[0], enclosing a[0], bridging a[1], ending inside
  // a[2], and extending after a[2].
  constexpr hsize_t s_mid = 5, c_mid = 50;  // [5..54]
  ScopedSpace s_xor(H5Scopy(s1d.get()));
  ASSERT_TRUE(s_xor.valid());
  ASSERT_GE(H5Sselect_hyperslab(s_xor.get(), H5S_SELECT_XOR, &s_mid, nullptr,
                                &c_mid, nullptr),
            0);
  // [5..9] (5) + [20..29] (10) + [40..49] (10) + [55..59] (5) = 30
  EXPECT_EQ(H5Sget_select_npoints(s_xor.get()), 30);

  ScopedSpace s_notb(H5Scopy(s1d.get()));
  ASSERT_TRUE(s_notb.valid());
  ASSERT_GE(H5Sselect_hyperslab(s_notb.get(), H5S_SELECT_NOTB, &s_mid, nullptr,
                                &c_mid, nullptr),
            0);
  // [55..59] = 5
  EXPECT_EQ(H5Sget_select_npoints(s_notb.get()), 5);

  ScopedSpace s_nota(H5Scopy(s1d.get()));
  ASSERT_TRUE(s_nota.valid());
  ASSERT_GE(H5Sselect_hyperslab(s_nota.get(), H5S_SELECT_NOTA, &s_mid, nullptr,
                                &c_mid, nullptr),
            0);
  // [5..9] + [20..29] + [40..49] = 25
  EXPECT_EQ(H5Sget_select_npoints(s_nota.get()), 25);

  // Inside / exact-high / tail-recover cases in 3D.
  constexpr std::array<hsize_t, 3> kDims3d = {20, 20, 20};
  ScopedSpace s3d = MakeSimpleSpace(kDims3d);
  ASSERT_TRUE(s3d.valid());

  constexpr std::array<hsize_t, 3> kStart3a = {2, 2, 2};
  constexpr std::array<hsize_t, 3> kStride3a = {4, 4, 4};
  constexpr std::array<hsize_t, 3> kCount3a = {3, 3, 3};
  constexpr std::array<hsize_t, 3> kBlock3a = {2, 2, 2};
  ASSERT_GE(H5Sselect_hyperslab(s3d.get(), H5S_SELECT_SET, kStart3a.data(),
                                kStride3a.data(), kCount3a.data(),
                                kBlock3a.data()),
            0);
  // Multi-block regular AND with single block:
  // 1) Completely disjoint -> NONE.
  {
    ScopedSpace tmp(H5Scopy(s3d.get()));
    constexpr std::array<hsize_t, 3> kDisjointStart = {15, 15, 15};
    constexpr std::array<hsize_t, 3> kOnes = {1, 1, 1};
    constexpr std::array<hsize_t, 3> kBox = {2, 2, 2};
    ASSERT_GE(H5Sselect_hyperslab(tmp.get(), H5S_SELECT_AND,
                                  kDisjointStart.data(), nullptr, kOnes.data(),
                                  kBox.data()),
              0);
    EXPECT_EQ(H5Sget_select_type(tmp.get()), H5S_SEL_NONE);
  }
  // 2) Lands inside gap between strided blocks -> NONE.
  {
    ScopedSpace tmp(H5Scopy(s3d.get()));
    constexpr std::array<hsize_t, 3> kGapStart = {4, 2, 2};
    constexpr std::array<hsize_t, 3> kOnes = {1, 1, 1};
    constexpr std::array<hsize_t, 3> kBox = {2, 2, 2};
    ASSERT_GE(H5Sselect_hyperslab(tmp.get(), H5S_SELECT_AND, kGapStart.data(),
                                  nullptr, kOnes.data(), kBox.data()),
              0);
    EXPECT_EQ(H5Sget_select_type(tmp.get()), H5S_SEL_NONE);
  }
  // 3) Full block alignment preserving regularity.
  {
    ScopedSpace tmp(H5Scopy(s3d.get()));
    constexpr std::array<hsize_t, 3> kAlignedStart = {2, 2, 2};
    constexpr std::array<hsize_t, 3> kOnes = {1, 1, 1};
    constexpr std::array<hsize_t, 3> kAlignedBox = {7, 7, 7};
    ASSERT_GE(H5Sselect_hyperslab(tmp.get(), H5S_SELECT_AND,
                                  kAlignedStart.data(), nullptr, kOnes.data(),
                                  kAlignedBox.data()),
              0);
    EXPECT_GT(H5Sis_regular_hyperslab(tmp.get()), 0);
    EXPECT_EQ(H5Sget_select_npoints(tmp.get()), 64);
  }
  // 4) Partial first & last block overlap inside strided selection -> clips.
  {
    ScopedSpace tmp(H5Scopy(s3d.get()));
    constexpr std::array<hsize_t, 3> kPartialStart = {3, 3, 3};
    constexpr std::array<hsize_t, 3> kOnes = {1, 1, 1};
    constexpr std::array<hsize_t, 3> kPartialBox = {8, 8, 8};
    ASSERT_GE(H5Sselect_hyperslab(tmp.get(), H5S_SELECT_AND,
                                  kPartialStart.data(), nullptr, kOnes.data(),
                                  kPartialBox.data()),
              0);
    EXPECT_EQ(H5Sget_select_npoints(tmp.get()), 4 * 4 * 4);
  }

  // Irregular 3D XOR / OR / AND / NOTB / NOTA with partial overlapping blocks.
  constexpr std::array<hsize_t, 3> kStart3b = {3, 1, 3};
  constexpr std::array<hsize_t, 3> kOnes = {1, 1, 1};
  constexpr std::array<hsize_t, 3> kBlock3b = {9, 10, 9};
  ScopedSpace s3d_xor(H5Scopy(s3d.get()));
  ASSERT_GE(H5Sselect_hyperslab(s3d_xor.get(), H5S_SELECT_XOR, kStart3b.data(),
                                nullptr, kOnes.data(), kBlock3b.data()),
            0);
  EXPECT_GT(H5Sget_select_npoints(s3d_xor.get()), 0);

  // Single-block AND on single-block selection (overlapping & disjoint).
  ScopedSpace single = MakeSimpleSpace(kDims3d);
  constexpr std::array<hsize_t, 3> kBoxA = {4, 4, 4};
  constexpr std::array<hsize_t, 3> kStartB = {6, 6, 6};
  constexpr std::array<hsize_t, 3> kBoxB = {5, 5, 5};
  ASSERT_GE(H5Sselect_hyperslab(single.get(), H5S_SELECT_SET, kStart3a.data(),
                                nullptr, kOnes.data(), kBoxA.data()),
            0);
  ASSERT_GE(H5Sselect_hyperslab(single.get(), H5S_SELECT_AND, kStartB.data(),
                                nullptr, kOnes.data(), kBoxB.data()),
            0);
  EXPECT_EQ(H5Sget_select_type(single.get()), H5S_SEL_NONE);

  ASSERT_GE(H5Sselect_hyperslab(single.get(), H5S_SELECT_SET, kStart3a.data(),
                                nullptr, kOnes.data(), kBoxA.data()),
            0);
  constexpr std::array<hsize_t, 3> kStartOverlap = {3, 3, 3};
  ASSERT_GE(H5Sselect_hyperslab(single.get(), H5S_SELECT_AND,
                                kStartOverlap.data(), nullptr, kOnes.data(),
                                kBoxB.data()),
            0);
  EXPECT_EQ(H5Sget_select_npoints(single.get()), 27);
}

TEST(H5ShyperTest, UpdateDiminfoAndRebuildRegularHyperslabs) {
  constexpr std::array<hsize_t, 2> kDims = {30, 30};
  constexpr std::array<hsize_t, 2> kOnes = {1, 1};

  // 1. Overlapping count==1 OR merges diminfo directly.
  ScopedSpace s1 = MakeSimpleSpace(kDims);
  constexpr std::array<hsize_t, 2> kS1 = {2, 4};
  constexpr std::array<hsize_t, 2> kB1 = {5, 6};
  constexpr std::array<hsize_t, 2> kS2 = {4, 4};
  constexpr std::array<hsize_t, 2> kB2 = {6, 6};
  ASSERT_GE(H5Sselect_hyperslab(s1.get(), H5S_SELECT_SET, kS1.data(), nullptr,
                                kOnes.data(), kB1.data()),
            0);
  ASSERT_GE(H5Sselect_hyperslab(s1.get(), H5S_SELECT_OR, kS2.data(), nullptr,
                                kOnes.data(), kB2.data()),
            0);
  EXPECT_GT(H5Sis_regular_hyperslab(s1.get()), 0);
  EXPECT_EQ(H5Sget_select_npoints(s1.get()), 8 * 6);

  // 2. Overlapping equal-block count==1 XOR creates 2 strided blocks cleanly.
  ScopedSpace s2 = MakeSimpleSpace(kDims);
  constexpr std::array<hsize_t, 2> kXS1 = {2, 4};
  constexpr std::array<hsize_t, 2> kXS2 = {4, 4};
  constexpr std::array<hsize_t, 2> kXB = {4, 6};
  ASSERT_GE(H5Sselect_hyperslab(s2.get(), H5S_SELECT_SET, kXS1.data(), nullptr,
                                kOnes.data(), kXB.data()),
            0);
  ASSERT_GE(H5Sselect_hyperslab(s2.get(), H5S_SELECT_XOR, kXS2.data(), nullptr,
                                kOnes.data(), kXB.data()),
            0);
  EXPECT_GT(H5Sis_regular_hyperslab(s2.get()), 0);
  EXPECT_EQ(H5Sget_select_npoints(s2.get()), 24);

  // 3. Adjacent count==1 OR merges bordering blocks; reversed order too.
  ScopedSpace s3 = MakeSimpleSpace(kDims);
  constexpr std::array<hsize_t, 2> kAS2 = {8, 4};
  constexpr std::array<hsize_t, 2> kAS1 = {2, 4};
  constexpr std::array<hsize_t, 2> kAB = {6, 6};
  ASSERT_GE(H5Sselect_hyperslab(s3.get(), H5S_SELECT_SET, kAS2.data(), nullptr,
                                kOnes.data(), kAB.data()),
            0);
  ASSERT_GE(H5Sselect_hyperslab(s3.get(), H5S_SELECT_OR, kAS1.data(), nullptr,
                                kOnes.data(), kAB.data()),
            0);
  EXPECT_GT(H5Sis_regular_hyperslab(s3.get()), 0);
  EXPECT_EQ(H5Sget_select_npoints(s3.get()), 12 * 6);

  // 4. Two disjoint equal blocks become regular stride selection.
  ScopedSpace s4 = MakeSimpleSpace(kDims);
  constexpr std::array<hsize_t, 2> kDS2 = {10, 4};
  ASSERT_GE(H5Sselect_hyperslab(s4.get(), H5S_SELECT_SET, kAS1.data(), nullptr,
                                kOnes.data(), kAB.data()),
            0);
  ASSERT_GE(H5Sselect_hyperslab(s4.get(), H5S_SELECT_OR, kDS2.data(), nullptr,
                                kOnes.data(), kAB.data()),
            0);
  EXPECT_GT(H5Sis_regular_hyperslab(s4.get()), 0);
  EXPECT_EQ(H5Sget_select_npoints(s4.get()), 72);

  // 5. Append third equal-stride block with NOTB/rebuild paths -> H5S__hyper_rebuild!
  ScopedSpace s5 = MakeSimpleSpace(kDims);
  constexpr std::array<hsize_t, 2> kStride = {4, 1};
  constexpr std::array<hsize_t, 2> kCnt3 = {3, 1};
  constexpr std::array<hsize_t, 2> kBlk3 = {2, 5};
  ASSERT_GE(H5Sselect_hyperslab(s5.get(), H5S_SELECT_SET, kAS1.data(),
                                kStride.data(), kCnt3.data(), kBlk3.data()),
            0);
  // Punch out middle row [6..7], then punch out differently sized range -> rebuild fail,
  // OR add back exact regular block [14..15] via NOTB + OR to trigger rebuild success!
  constexpr std::array<hsize_t, 2> kMidRm = {6, 4};
  constexpr std::array<hsize_t, 2> kMidBlk = {2, 5};
  ASSERT_GE(H5Sselect_hyperslab(s5.get(), H5S_SELECT_NOTB, kMidRm.data(),
                                nullptr, kOnes.data(), kMidBlk.data()),
            0);
  // Now [2..3] and [10..11] exist: NOTB cleared diminfo_valid, so rebuild makes it regular!
  EXPECT_GT(H5Sis_regular_hyperslab(s5.get()), 0);

  // Three spans with unequal strides fail rebuild.
  ScopedSpace s6 = MakeSimpleSpace(kDims);
  constexpr std::array<hsize_t, 2> kR1 = {1, 1};
  constexpr std::array<hsize_t, 2> kR2 = {5, 1};
  constexpr std::array<hsize_t, 2> kR3 = {12, 1};
  constexpr std::array<hsize_t, 2> kRB = {2, 3};
  ASSERT_GE(H5Sselect_hyperslab(s6.get(), H5S_SELECT_SET, kR1.data(), nullptr,
                                kOnes.data(), kRB.data()),
            0);
  ASSERT_GE(H5Sselect_hyperslab(s6.get(), H5S_SELECT_OR, kR2.data(), nullptr,
                                kOnes.data(), kRB.data()),
            0);
  ASSERT_GE(H5Sselect_hyperslab(s6.get(), H5S_SELECT_OR, kR3.data(), nullptr,
                                kOnes.data(), kRB.data()),
            0);
  EXPECT_EQ(H5Sis_regular_hyperslab(s6.get()), 0);

  // Two spans with unequal block widths fail rebuild.
  ScopedSpace s7 = MakeSimpleSpace(kDims);
  constexpr std::array<hsize_t, 2> kRBWide = {3, 3};
  ASSERT_GE(H5Sselect_hyperslab(s7.get(), H5S_SELECT_SET, kR1.data(), nullptr,
                                kOnes.data(), kRB.data()),
            0);
  ASSERT_GE(H5Sselect_hyperslab(s7.get(), H5S_SELECT_OR, kR2.data(), nullptr,
                                kOnes.data(), kRBWide.data()),
            0);
  EXPECT_EQ(H5Sis_regular_hyperslab(s7.get()), 0);

  // Unequal down spans (2 rows with different col spans) fail rebuild.
  ScopedSpace s8 = MakeSimpleSpace(kDims);
  constexpr std::array<hsize_t, 2> kC2 = {5, 10};
  ASSERT_GE(H5Sselect_hyperslab(s8.get(), H5S_SELECT_SET, kR1.data(), nullptr,
                                kOnes.data(), kRB.data()),
            0);
  ASSERT_GE(H5Sselect_hyperslab(s8.get(), H5S_SELECT_OR, kC2.data(), nullptr,
                                kOnes.data(), kRB.data()),
            0);
  EXPECT_EQ(H5Sis_regular_hyperslab(s8.get()), 0);

  // Count > 1 update_diminfo edge cases: mismatched phase, stride mismatch,
  // border XOR, and >1 non-identical dimension.
  ScopedSpace s9 = MakeSimpleSpace(kDims);
  constexpr std::array<hsize_t, 2> kA9 = {0, 0};
  constexpr std::array<hsize_t, 2> kS9 = {4, 1};
  constexpr std::array<hsize_t, 2> kCnt9 = {2, 1};
  constexpr std::array<hsize_t, 2> kBlk9 = {2, 4};
  ASSERT_GE(H5Sselect_hyperslab(s9.get(), H5S_SELECT_SET, kA9.data(),
                                kS9.data(), kCnt9.data(), kBlk9.data()),
            0);
  // Bordering XOR with matching stride: [8..9] + [12..13] -> count 4.
  constexpr std::array<hsize_t, 2> kA9b = {8, 0};
  ASSERT_GE(H5Sselect_hyperslab(s9.get(), H5S_SELECT_XOR, kA9b.data(),
                                kS9.data(), kCnt9.data(), kBlk9.data()),
            0);
  EXPECT_GT(H5Sis_regular_hyperslab(s9.get()), 0);

  // Overlapping count > 1 OR extends existing regular count.
  constexpr std::array<hsize_t, 2> kA9c = {12, 0};
  ASSERT_GE(H5Sselect_hyperslab(s9.get(), H5S_SELECT_OR, kA9c.data(),
                                kS9.data(), kCnt9.data(), kBlk9.data()),
            0);
  EXPECT_GT(H5Sis_regular_hyperslab(s9.get()), 0);
  EXPECT_EQ(H5Sget_select_npoints(s9.get()), 5 * 2 * 4);

  // Querying regular hyperslab with NULL outputs or on invalid/irregular spaces.
  ScopedErrorSilence silence;
  std::array<hsize_t, 2> reg_start{};
  EXPECT_GE(H5Sget_regular_hyperslab(s9.get(), nullptr, nullptr, nullptr,
                                     nullptr),
            0);
  EXPECT_LT(H5Sget_regular_hyperslab(s8.get(), reg_start.data(), nullptr,
                                     nullptr, nullptr),
            0);
  EXPECT_LT(H5Sis_regular_hyperslab(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Sget_regular_hyperslab(H5I_INVALID_HID, reg_start.data(),
                                     nullptr, nullptr, nullptr),
            0);
  ScopedSpace s_none = MakeSimpleSpace(kDims);
  ASSERT_GE(H5Sselect_none(s_none.get()), 0);
  EXPECT_LT(H5Sget_regular_hyperslab(s_none.get(), reg_start.data(), nullptr,
                                     nullptr, nullptr),
            0);
}

TEST(H5ShyperTest, CombineHyperslabSelectAndModifySelect) {
  constexpr std::array<hsize_t, 2> kDims = {16, 16};
  constexpr std::array<hsize_t, 2> kOnes = {1, 1};
  ScopedSpace s1 = MakeSimpleSpace(kDims);
  ScopedSpace s2 = MakeSimpleSpace(kDims);
  ASSERT_TRUE(s1.valid() && s2.valid());

  // Regular base [0..3, 0..3].
  constexpr std::array<hsize_t, 2> kStart1 = {0, 0};
  constexpr std::array<hsize_t, 2> kBlock1 = {4, 4};
  ASSERT_GE(H5Sselect_hyperslab(s1.get(), H5S_SELECT_SET, kStart1.data(),
                                nullptr, kOnes.data(), kBlock1.data()),
            0);

  // Combine with zero-sized slab (AND/NOTA -> none, OR/XOR/NOTB -> copy).
  constexpr std::array<hsize_t, 2> kZero = {0, 4};
  for (H5S_seloper_t op : {H5S_SELECT_AND, H5S_SELECT_NOTA}) {
    ScopedSpace out(H5Scombine_hyperslab(s1.get(), op, kStart1.data(), nullptr,
                                         kZero.data(), nullptr));
    ASSERT_TRUE(out.valid());
    EXPECT_EQ(H5Sget_select_type(out.get()), H5S_SEL_NONE);
  }
  for (H5S_seloper_t op : {H5S_SELECT_OR, H5S_SELECT_XOR, H5S_SELECT_NOTB}) {
    ScopedSpace out(H5Scombine_hyperslab(s1.get(), op, kStart1.data(), nullptr,
                                         kZero.data(), nullptr));
    ASSERT_TRUE(out.valid());
    EXPECT_EQ(H5Sget_select_npoints(out.get()), 16);
  }

  // Disjoint fast path in H5S_combine_hyperslab (both with and without span_lst).
  constexpr std::array<hsize_t, 2> kDisjointStart = {8, 8};
  constexpr std::array<hsize_t, 2> kDisjointBlock = {4, 4};
  for (H5S_seloper_t op : {H5S_SELECT_OR, H5S_SELECT_XOR, H5S_SELECT_AND,
                           H5S_SELECT_NOTA, H5S_SELECT_NOTB}) {
    ScopedSpace out(H5Scombine_hyperslab(s1.get(), op, kDisjointStart.data(),
                                         nullptr, kOnes.data(),
                                         kDisjointBlock.data()));
    ASSERT_TRUE(out.valid());
    if (op == H5S_SELECT_AND || op == H5S_SELECT_NOTB) {
      EXPECT_EQ(H5Sget_select_npoints(out.get()), 0);
    } else if (op == H5S_SELECT_NOTA) {
      EXPECT_EQ(H5Sget_select_npoints(out.get()), 16);
    } else {
      EXPECT_EQ(H5Sget_select_npoints(out.get()), 32);
    }
  }

  // Irregular s1 (has span_lst) with disjoint H5Scombine_hyperslab NOTB/OR.
  constexpr std::array<hsize_t, 2> kExtraStart = {1, 5};
  constexpr std::array<hsize_t, 2> kExtraBlock = {2, 2};
  ASSERT_GE(H5Sselect_hyperslab(s1.get(), H5S_SELECT_OR, kExtraStart.data(),
                                nullptr, kOnes.data(), kExtraBlock.data()),
            0);
  {
    ScopedSpace out_notb(H5Scombine_hyperslab(s1.get(), H5S_SELECT_NOTB,
                                              kDisjointStart.data(), nullptr,
                                              kOnes.data(),
                                              kDisjointBlock.data()));
    ASSERT_TRUE(out_notb.valid());
    EXPECT_EQ(H5Sget_select_npoints(out_notb.get()), 20);

    ScopedSpace out_or(H5Scombine_hyperslab(s1.get(), H5S_SELECT_OR,
                                            kDisjointStart.data(), nullptr,
                                            kOnes.data(),
                                            kDisjointBlock.data()));
    ASSERT_TRUE(out_or.valid());
    EXPECT_EQ(H5Sget_select_npoints(out_or.get()), 36);
  }

  // Overlapping H5Scombine_hyperslab path.
  constexpr std::array<hsize_t, 2> kOverlapStart = {2, 2};
  ScopedSpace combined(H5Scombine_hyperslab(s1.get(), H5S_SELECT_OR,
                                            kOverlapStart.data(), nullptr,
                                            kOnes.data(), kBlock1.data()));
  ASSERT_TRUE(combined.valid());

  // H5Scombine_select & H5Smodify_select across regular and irregular spaces.
  ASSERT_GE(H5Sselect_hyperslab(s2.get(), H5S_SELECT_SET, kOverlapStart.data(),
                                nullptr, kOnes.data(), kBlock1.data()),
            0);
  for (H5S_seloper_t op : {H5S_SELECT_OR, H5S_SELECT_AND, H5S_SELECT_XOR,
                           H5S_SELECT_NOTB, H5S_SELECT_NOTA}) {
    ScopedSpace cs(H5Scombine_select(s1.get(), op, s2.get()));
    ASSERT_TRUE(cs.valid());
    ScopedSpace ms(H5Scopy(s1.get()));
    ASSERT_GE(H5Smodify_select(ms.get(), op, s2.get()), 0);
    EXPECT_EQ(H5Sget_select_npoints(cs.get()), H5Sget_select_npoints(ms.get()));
  }

  // Combine_select when intersection is completely empty -> H5S_SEL_NONE.
  constexpr std::array<hsize_t, 2> kFarStart = {10, 10};
  ASSERT_GE(H5Sselect_hyperslab(s2.get(), H5S_SELECT_SET, kFarStart.data(),
                                nullptr, kOnes.data(), kBlock1.data()),
            0);
  ScopedSpace empty_comb(H5Scombine_select(s1.get(), H5S_SELECT_AND, s2.get()));
  ASSERT_TRUE(empty_comb.valid());
  EXPECT_EQ(H5Sget_select_type(empty_comb.get()), H5S_SEL_NONE);

  // H5S_select_subtract internal entry point.
  {
    ScopedSpace sub_dst(H5Scopy(s1.get()));
    EXPECT_GE(H5S_select_subtract(UnwrapSpace(sub_dst.get()),
                                  UnwrapSpace(combined.get())),
              0);
    EXPECT_EQ(H5Sget_select_type(sub_dst.get()), H5S_SEL_NONE);
  }

  // Error handling for combine_hyperslab / combine_select / modify_select.
  ScopedErrorSilence silence;
  EXPECT_EQ(H5Scombine_hyperslab(H5I_INVALID_HID, H5S_SELECT_SET,
                                 kStart1.data(), nullptr, kOnes.data(),
                                 nullptr),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Scombine_hyperslab(s1.get(), H5S_SELECT_SET, nullptr, nullptr,
                                 kOnes.data(), nullptr),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Scombine_hyperslab(s1.get(), H5S_SELECT_NOOP, kStart1.data(),
                                 nullptr, kOnes.data(), nullptr),
            H5I_INVALID_HID);

  constexpr std::array<hsize_t, 1> kDims1d = {8};
  ScopedSpace s_rank_mismatch = MakeSimpleSpace(kDims1d);
  constexpr hsize_t kS1d = 0, kC1d = 2;
  ASSERT_GE(H5Sselect_hyperslab(s_rank_mismatch.get(), H5S_SELECT_SET, &kS1d,
                                nullptr, &kC1d, nullptr),
            0);
  EXPECT_EQ(
      H5Scombine_select(s1.get(), H5S_SELECT_OR, s_rank_mismatch.get()),
      H5I_INVALID_HID);
  EXPECT_LT(
      H5Smodify_select(s1.get(), H5S_SELECT_OR, s_rank_mismatch.get()), 0);
  EXPECT_EQ(H5Scombine_select(s1.get(), H5S_SELECT_SET, s2.get()),
            H5I_INVALID_HID);
  EXPECT_LT(H5Smodify_select(s1.get(), H5S_SELECT_SET, s2.get()), 0);

  ScopedSpace s_none = MakeSimpleSpace(kDims);
  ASSERT_GE(H5Sselect_none(s_none.get()), 0);
  EXPECT_EQ(H5Scombine_select(s1.get(), H5S_SELECT_OR, s_none.get()),
            H5I_INVALID_HID);
  EXPECT_LT(H5Smodify_select(s_none.get(), H5S_SELECT_OR, s1.get()), 0);
  EXPECT_EQ(H5Scombine_select(H5I_INVALID_HID, H5S_SELECT_OR, s2.get()),
            H5I_INVALID_HID);
  EXPECT_EQ(H5Scombine_select(s1.get(), H5S_SELECT_OR, H5I_INVALID_HID),
            H5I_INVALID_HID);
  EXPECT_LT(H5Smodify_select(H5I_INVALID_HID, H5S_SELECT_OR, s2.get()), 0);
  EXPECT_LT(H5Smodify_select(s1.get(), H5S_SELECT_OR, H5I_INVALID_HID), 0);
}

TEST(H5ShyperTest, UnlimitedHyperslabsAndClipHelpers) {
  constexpr std::array<hsize_t, 2> kDims = {10, 8};
  constexpr std::array<hsize_t, 2> kMaxDims = {H5S_UNLIMITED, 8};
  constexpr std::array<hsize_t, 2> kStart = {2, 1};
  constexpr std::array<hsize_t, 2> kStrideUnlim = {4, 1};
  constexpr std::array<hsize_t, 2> kCountUnlim = {H5S_UNLIMITED, 1};
  constexpr std::array<hsize_t, 2> kBlockFixed = {2, 4};
  constexpr std::array<hsize_t, 2> kOnes = {1, 1};

  ScopedSpace unlim_cnt = MakeSimpleSpace(kDims, kMaxDims);
  ASSERT_TRUE(unlim_cnt.valid());
  ASSERT_GE(H5Sselect_hyperslab(unlim_cnt.get(), H5S_SELECT_SET, kStart.data(),
                                kStrideUnlim.data(), kCountUnlim.data(),
                                kBlockFixed.data()),
            0);

  // Basic unlimited queries.
  H5S_t* unlim_ptr = UnwrapSpace(unlim_cnt.get());
  EXPECT_EQ(H5S_get_select_unlim_dim(unlim_ptr), 0);
  hsize_t slice_nelem = 0;
  ASSERT_GE(H5S_get_select_num_elem_non_unlim(unlim_ptr, &slice_nelem), 0);
  EXPECT_EQ(slice_nelem, 4u);
  EXPECT_EQ(H5Sselect_valid(unlim_cnt.get()), 0);

  std::array<hsize_t, 2> bstart{};
  std::array<hsize_t, 2> bend{};
  ASSERT_GE(H5Sget_select_bounds(unlim_cnt.get(), bstart.data(), bend.data()),
            0);
  EXPECT_THAT(bstart, ElementsAre(2, 1));
  EXPECT_THAT(bend, ElementsAre(H5S_UNLIMITED, 4));

  // Clip helper APIs (match_space must be non-null; H5S_SEL_NONE yields 0 slices).
  ScopedSpace match_none = MakeSimpleSpace(kDims, kMaxDims);
  ASSERT_GE(H5Sselect_none(match_none.get()), 0);
  EXPECT_EQ(H5S_hyper_get_clip_extent(unlim_ptr, UnwrapSpace(match_none.get()),
                                      false),
            0u);
  EXPECT_EQ(
      H5S_hyper_get_clip_extent(unlim_ptr, UnwrapSpace(match_none.get()), true),
      2u);

  ScopedSpace match_fixed = MakeSimpleSpace(kDims, kMaxDims);
  constexpr std::array<hsize_t, 2> kMatchStart = {1, 0};
  constexpr std::array<hsize_t, 2> kMatchBlock = {9, 8};
  ASSERT_GE(H5Sselect_hyperslab(match_fixed.get(), H5S_SELECT_SET,
                                kMatchStart.data(), nullptr, kOnes.data(),
                                kMatchBlock.data()),
            0);
  EXPECT_EQ(H5S_hyper_get_clip_extent(unlim_ptr, UnwrapSpace(match_fixed.get()),
                                      false),
            36u);
  EXPECT_EQ(
      H5S_hyper_get_clip_extent(unlim_ptr, UnwrapSpace(match_fixed.get()), true),
      38u);

  // Unlimited block selection (count = 1, block = {UNLIMITED, 4}).
  constexpr std::array<hsize_t, 2> kBlockUnlim = {H5S_UNLIMITED, 4};
  ScopedSpace unlim_blk = MakeSimpleSpace(kDims, kMaxDims);
  ASSERT_GE(H5Sselect_hyperslab(unlim_blk.get(), H5S_SELECT_SET, kStart.data(),
                                nullptr, kOnes.data(), kBlockUnlim.data()),
            0);
  EXPECT_EQ(H5S_hyper_get_clip_extent(UnwrapSpace(unlim_blk.get()),
                                      UnwrapSpace(match_none.get()), true),
            2u);
  EXPECT_EQ(H5S_hyper_get_clip_extent(UnwrapSpace(unlim_blk.get()),
                                      UnwrapSpace(match_fixed.get()), false),
            20u);
  EXPECT_EQ(H5S_hyper_get_clip_extent_match(
                unlim_ptr, UnwrapSpace(unlim_blk.get()), 11, true),
            19u);
  EXPECT_EQ(H5S_hyper_get_clip_extent_match(
                unlim_ptr, UnwrapSpace(unlim_blk.get()), 0, false),
            0u);
  EXPECT_EQ(H5S_hyper_get_clip_extent_match(UnwrapSpace(unlim_blk.get()),
                                            unlim_ptr, 12, false),
            8u);
  EXPECT_EQ(H5S_hyper_get_clip_extent_match(UnwrapSpace(unlim_blk.get()),
                                            unlim_ptr, 12, true),
            8u);
  // Partial last block in match_space (10..10 out of 10..11).
  EXPECT_EQ(H5S_hyper_get_clip_extent_match(UnwrapSpace(unlim_blk.get()),
                                            unlim_ptr, 11, false),
            7u);

  // get_unlim_block and get_first_inc_block.
  for (hsize_t idx = 0; idx < 3; ++idx) {
    UniqueInternalSpace blk(H5S_hyper_get_unlim_block(unlim_ptr, idx));
    ASSERT_NE(blk, nullptr);
    EXPECT_EQ(H5S_get_select_npoints(blk.get()), 8u);
  }
  {
    UniqueInternalSpace blk_u(
        H5S_hyper_get_unlim_block(UnwrapSpace(unlim_blk.get()), 0));
    ASSERT_NE(blk_u, nullptr);
  }
  bool partial = false;
  EXPECT_EQ(H5S_hyper_get_first_inc_block(unlim_ptr, 1, &partial), 0u);
  EXPECT_FALSE(partial);
  EXPECT_EQ(H5S_hyper_get_first_inc_block(unlim_ptr, 7, &partial), 1u);
  EXPECT_TRUE(partial);
  EXPECT_EQ(H5S_hyper_get_first_inc_block(unlim_ptr, 10, nullptr), 2u);

  // Clipping unlimited count selections at various bounds.
  {
    ScopedSpace c_none(H5Scopy(unlim_cnt.get()));
    ASSERT_GE(H5S_hyper_clip_unlim(UnwrapSpace(c_none.get()), 2), 0);
    EXPECT_EQ(H5Sget_select_type(c_none.get()), H5S_SEL_NONE);

    ScopedSpace c_exact(H5Scopy(unlim_cnt.get()));
    ASSERT_GE(H5S_hyper_clip_unlim(UnwrapSpace(c_exact.get()), 8), 0);
    EXPECT_EQ(H5S_get_select_unlim_dim(UnwrapSpace(c_exact.get())), -1);
    EXPECT_EQ(H5Sget_select_npoints(c_exact.get()), 16);

    // Partial last block triggers irregular compound clip!
    ScopedSpace c_partial(H5Scopy(unlim_cnt.get()));
    ASSERT_GE(H5S_hyper_clip_unlim(UnwrapSpace(c_partial.get()), 7), 0);
    EXPECT_EQ(H5Sget_select_npoints(c_partial.get()), 12);
  }
  {
    // Clip unlimited-block selection.
    ScopedSpace c_blk(H5Scopy(unlim_blk.get()));
    ASSERT_GE(H5S_hyper_clip_unlim(UnwrapSpace(c_blk.get()), 1), 0);
    EXPECT_EQ(H5Sget_select_type(c_blk.get()), H5S_SEL_NONE);

    ScopedSpace c_blk2(H5Scopy(unlim_blk.get()));
    ASSERT_GE(H5S_hyper_clip_unlim(UnwrapSpace(c_blk2.get()), 6), 0);
    EXPECT_EQ(H5Sget_select_npoints(c_blk2.get()), 16);
  }

  // Combining fixed selection with unlimited selection (AND and NOTB) and
  // modifying unlimited selection with fixed selection (AND and NOTA).
  {
    ScopedSpace mod_unlim(H5Scopy(unlim_cnt.get()));
    constexpr std::array<hsize_t, 2> kFixedClip = {3, 2};
    constexpr std::array<hsize_t, 2> kFixedBlk = {6, 4};
    ASSERT_GE(H5Sselect_hyperslab(mod_unlim.get(), H5S_SELECT_AND,
                                  kFixedClip.data(), nullptr, kOnes.data(),
                                  kFixedBlk.data()),
              0);
    EXPECT_EQ(H5S_get_select_unlim_dim(UnwrapSpace(mod_unlim.get())), -1);
    EXPECT_EQ(H5Sget_select_npoints(mod_unlim.get()), 9);

    ScopedSpace fixed_first = MakeSimpleSpace(kDims, kMaxDims);
    ASSERT_GE(H5Sselect_hyperslab(fixed_first.get(), H5S_SELECT_SET,
                                  kFixedClip.data(), nullptr, kOnes.data(),
                                  kFixedBlk.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(fixed_first.get(), H5S_SELECT_NOTB,
                                  kStart.data(), kStrideUnlim.data(),
                                  kCountUnlim.data(), kBlockFixed.data()),
              0);
    EXPECT_GT(H5Sget_select_npoints(fixed_first.get()), 0);
  }

  // Error paths for unlimited selections.
  ScopedErrorSilence silence;
  constexpr std::array<hsize_t, 2> kMaxTwo = {H5S_UNLIMITED, H5S_UNLIMITED};
  ScopedSpace s_err = MakeSimpleSpace(kDims, kMaxTwo);
  constexpr std::array<hsize_t, 2> kBothUnlim = {H5S_UNLIMITED, H5S_UNLIMITED};
  EXPECT_LT(H5Sselect_hyperslab(s_err.get(), H5S_SELECT_SET, kStart.data(),
                                nullptr, kBothUnlim.data(), kOnes.data()),
            0);
  EXPECT_LT(H5Sselect_hyperslab(s_err.get(), H5S_SELECT_SET, kStart.data(),
                                nullptr, kCountUnlim.data(), kBlockUnlim.data()),
            0);
  EXPECT_LT(H5Sselect_hyperslab(s_err.get(), H5S_SELECT_SET, kStart.data(),
                                nullptr, kCountUnlim.data(), kCountUnlim.data()),
            0);
  EXPECT_LT(H5Sselect_hyperslab(unlim_cnt.get(), H5S_SELECT_OR, kStart.data(),
                                nullptr, kOnes.data(), kBlockFixed.data()),
            0);
  EXPECT_LT(H5Sselect_hyperslab(unlim_cnt.get(), H5S_SELECT_AND, kStart.data(),
                                kStrideUnlim.data(), kCountUnlim.data(),
                                kBlockFixed.data()),
            0);
  EXPECT_LT(H5Sselect_hyperslab(match_fixed.get(), H5S_SELECT_OR, kStart.data(),
                                kStrideUnlim.data(), kCountUnlim.data(),
                                kBlockFixed.data()),
            0);
  EXPECT_LT(H5Sselect_hyperslab(match_fixed.get(), H5S_SELECT_AND,
                                kStart.data(), kStrideUnlim.data(),
                                kCountUnlim.data(), kCountUnlim.data()),
            0);
  EXPECT_LT(H5Sget_select_hyper_nblocks(unlim_cnt.get()), 0);
  std::array<hsize_t, 4> blist{};
  EXPECT_LT(H5Sget_select_hyper_blocklist(unlim_cnt.get(), 0, 1, blist.data()),
            0);
  EXPECT_LT(H5S_get_select_num_elem_non_unlim(UnwrapSpace(match_fixed.get()),
                                              &slice_nelem),
            0);
}

TEST(H5ShyperTest, BlocklistBoundsOffsetContiguousAndSingleQueries) {
  constexpr std::array<hsize_t, 3> kDims = {10, 12, 14};
  constexpr std::array<hsize_t, 3> kOnes = {1, 1, 1};
  ScopedSpace reg = MakeSimpleSpace(kDims);
  ASSERT_TRUE(reg.valid());

  // Multi-block regular 3D: 2 x 2 x 2 = 8 blocks of size 2x2x2.
  constexpr std::array<hsize_t, 3> kStart = {1, 2, 3};
  constexpr std::array<hsize_t, 3> kStride = {4, 4, 4};
  constexpr std::array<hsize_t, 3> kCount = {2, 2, 2};
  constexpr std::array<hsize_t, 3> kBlock = {2, 2, 2};
  ASSERT_GE(H5Sselect_hyperslab(reg.get(), H5S_SELECT_SET, kStart.data(),
                                kStride.data(), kCount.data(), kBlock.data()),
            0);

  EXPECT_EQ(H5Sget_select_hyper_nblocks(reg.get()), 8);
  EXPECT_EQ(H5S_select_is_contiguous(UnwrapSpace(reg.get())), 0);
  EXPECT_EQ(H5S_select_is_single(UnwrapSpace(reg.get())), 0);
  EXPECT_GT(H5Sselect_valid(reg.get()), 0);

  std::vector<hsize_t> blocks(6 * 4, 0);
  EXPECT_GE(H5Sget_select_hyper_blocklist(reg.get(), 2, 0, blocks.data()), 0);
  ASSERT_GE(H5Sget_select_hyper_blocklist(reg.get(), 3, 4, blocks.data()), 0);
  // Block 3 in row-major order is (i=0, j=1, k=1) -> [1, 6, 7] .. [2, 7, 8].
  EXPECT_EQ(blocks[0], 1u);
  EXPECT_EQ(blocks[1], 6u);
  EXPECT_EQ(blocks[2], 7u);
  EXPECT_EQ(blocks[3], 2u);
  EXPECT_EQ(blocks[4], 7u);
  EXPECT_EQ(blocks[5], 8u);

  // Offset changes bounds, valid check, and linear offset.
  constexpr std::array<hssize_t, 3> kPosOff = {1, 1, 1};
  ASSERT_GE(H5Soffset_simple(reg.get(), kPosOff.data()), 0);
  EXPECT_GT(H5Sselect_valid(reg.get()), 0);
  std::array<hsize_t, 3> b_start{};
  std::array<hsize_t, 3> b_end{};
  ASSERT_GE(H5Sget_select_bounds(reg.get(), b_start.data(), b_end.data()), 0);
  EXPECT_THAT(b_start, ElementsAre(2, 3, 4));
  EXPECT_THAT(b_end, ElementsAre(7, 8, 9));

  hsize_t lin_off = 0;
  ASSERT_GE(H5S_get_select_offset(UnwrapSpace(reg.get()), &lin_off), 0);
  EXPECT_EQ(lin_off, 2u * 12 * 14 + 3u * 14 + 4u);

  // Contiguous regular checks: large_contiguous vs small_contiguous.
  ScopedSpace contig = MakeSimpleSpace(kDims);
  constexpr std::array<hsize_t, 3> kC1Start = {1, 0, 0};
  constexpr std::array<hsize_t, 3> kC1Block = {4, 12, 14};
  ASSERT_GE(H5Sselect_hyperslab(contig.get(), H5S_SELECT_SET, kC1Start.data(),
                                nullptr, kOnes.data(), kC1Block.data()),
            0);
  EXPECT_GT(H5S_select_is_contiguous(UnwrapSpace(contig.get())), 0);
  EXPECT_GT(H5S_select_is_single(UnwrapSpace(contig.get())), 0);

  constexpr std::array<hsize_t, 3> kC2Start = {1, 2, 0};
  constexpr std::array<hsize_t, 3> kC2Block = {1, 1, 14};
  ASSERT_GE(H5Sselect_hyperslab(contig.get(), H5S_SELECT_SET, kC2Start.data(),
                                nullptr, kOnes.data(), kC2Block.data()),
            0);
  EXPECT_GT(H5S_select_is_contiguous(UnwrapSpace(contig.get())), 0);
  EXPECT_GT(H5S_select_is_single(UnwrapSpace(contig.get())), 0);

  // Irregular hyperslab is_contiguous: large_contiguous and small_contiguous.
  {
    ScopedSpace irreg_lc = MakeSimpleSpace(kDims);
    constexpr std::array<hsize_t, 3> kA = {1, 0, 0};
    constexpr std::array<hsize_t, 3> kB = {2, 12, 14};
    constexpr std::array<hsize_t, 3> kC = {2, 0, 0};
    ASSERT_GE(H5Sselect_hyperslab(irreg_lc.get(), H5S_SELECT_SET, kA.data(),
                                  nullptr, kOnes.data(), kB.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(irreg_lc.get(), H5S_SELECT_OR, kC.data(),
                                  nullptr, kOnes.data(), kB.data()),
              0);
    EXPECT_GT(H5S_select_is_contiguous(UnwrapSpace(irreg_lc.get())), 0);
    EXPECT_GT(H5S_select_is_single(UnwrapSpace(irreg_lc.get())), 0);

    ScopedSpace irreg_sc = MakeSimpleSpace(kDims);
    constexpr std::array<hsize_t, 3> kSA = {1, 2, 0};
    constexpr std::array<hsize_t, 3> kSB = {1, 1, 6};
    constexpr std::array<hsize_t, 3> kSC = {1, 2, 6};
    constexpr std::array<hsize_t, 3> kSD = {1, 1, 4};
    ASSERT_GE(H5Sselect_hyperslab(irreg_sc.get(), H5S_SELECT_SET, kSA.data(),
                                  nullptr, kOnes.data(), kSB.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(irreg_sc.get(), H5S_SELECT_OR, kSC.data(),
                                  nullptr, kOnes.data(), kSD.data()),
              0);
    EXPECT_GT(H5S_select_is_contiguous(UnwrapSpace(irreg_sc.get())), 0);
    EXPECT_GT(H5S_select_is_single(UnwrapSpace(irreg_sc.get())), 0);
  }

  // Irregular hyperslab queries: nblocks, blocklist with startblock > 0,
  // bounds, offset, non-contiguous, is_single == 0.
  constexpr std::array<hsize_t, 3> kIrregStart = {0, 0, 0};
  constexpr std::array<hsize_t, 3> kIrregBlock = {1, 1, 1};
  ASSERT_GE(H5Sselect_hyperslab(contig.get(), H5S_SELECT_OR, kIrregStart.data(),
                                nullptr, kOnes.data(), kIrregBlock.data()),
            0);
  EXPECT_EQ(H5S_select_is_contiguous(UnwrapSpace(contig.get())), 0);
  EXPECT_EQ(H5S_select_is_single(UnwrapSpace(contig.get())), 0);
  EXPECT_EQ(H5Sget_select_hyper_nblocks(contig.get()), 2);
  std::vector<hsize_t> iblocks(6);
  ASSERT_GE(H5Sget_select_hyper_blocklist(contig.get(), 1, 1, iblocks.data()),
            0);
  EXPECT_THAT(iblocks, ElementsAre(1, 2, 0, 1, 2, 13));
  constexpr std::array<hssize_t, 3> kPosOffInBounds = {1, 1, 0};
  ASSERT_GE(H5Soffset_simple(contig.get(), kPosOffInBounds.data()), 0);
  ASSERT_GE(H5Sget_select_bounds(contig.get(), b_start.data(), b_end.data()),
            0);
  ASSERT_GE(H5S_get_select_offset(UnwrapSpace(contig.get()), &lin_off), 0);
  EXPECT_GT(H5Sselect_valid(contig.get()), 0);

  // Negative/overflow offset error handling in valid, bounds, offset.
  ScopedErrorSilence silence;
  constexpr std::array<hssize_t, 3> kNegOff = {-5, 0, 0};
  ASSERT_GE(H5Soffset_simple(reg.get(), kNegOff.data()), 0);
  EXPECT_EQ(H5Sselect_valid(reg.get()), 0);
  EXPECT_LT(H5Sget_select_bounds(reg.get(), b_start.data(), b_end.data()), 0);
  EXPECT_LT(H5S_get_select_offset(UnwrapSpace(reg.get()), &lin_off), 0);

  ASSERT_GE(H5Soffset_simple(contig.get(), kNegOff.data()), 0);
  EXPECT_EQ(H5Sselect_valid(contig.get()), 0);
  EXPECT_LT(H5Sget_select_bounds(contig.get(), b_start.data(), b_end.data()), 0);
  EXPECT_LT(H5S_get_select_offset(UnwrapSpace(contig.get()), &lin_off), 0);

  constexpr std::array<hssize_t, 3> kTooBigOff = {9, 0, 0};
  ASSERT_GE(H5Soffset_simple(reg.get(), kTooBigOff.data()), 0);
  EXPECT_EQ(H5Sselect_valid(reg.get()), 0);

  EXPECT_LT(H5Sget_select_hyper_nblocks(H5I_INVALID_HID), 0);
  EXPECT_LT(H5Sget_select_hyper_blocklist(reg.get(), 0, 1, nullptr), 0);
  EXPECT_LT(H5Sget_select_hyper_blocklist(H5I_INVALID_HID, 0, 1, iblocks.data()),
            0);
  ScopedSpace pall = MakeSimpleSpace(kDims);
  EXPECT_LT(H5Sget_select_hyper_nblocks(pall.get()), 0);
  EXPECT_LT(H5Sget_select_hyper_blocklist(pall.get(), 0, 1, iblocks.data()), 0);
}

TEST(H5ShyperTest, ShapeSameAndBlockIntersection) {
  constexpr std::array<hsize_t, 2> kDims2 = {10, 10};
  constexpr std::array<hsize_t, 3> kDims3 = {5, 10, 10};
  constexpr std::array<hsize_t, 2> kOnes2 = {1, 1};
  constexpr std::array<hsize_t, 3> kOnes3 = {1, 1, 1};

  ScopedSpace r2a = MakeSimpleSpace(kDims2);
  ScopedSpace r2b = MakeSimpleSpace(kDims2);
  ScopedSpace r3 = MakeSimpleSpace(kDims3);
  ASSERT_TRUE(r2a.valid() && r2b.valid() && r3.valid());

  // Regular vs regular same & different rank.
  constexpr std::array<hsize_t, 2> kS2a = {0, 0};
  constexpr std::array<hsize_t, 2> kSt2 = {3, 3};
  constexpr std::array<hsize_t, 2> kC2 = {2, 2};
  constexpr std::array<hsize_t, 2> kB2 = {2, 2};
  ASSERT_GE(H5Sselect_hyperslab(r2a.get(), H5S_SELECT_SET, kS2a.data(),
                                kSt2.data(), kC2.data(), kB2.data()),
            0);

  constexpr std::array<hsize_t, 2> kS2b = {2, 3};
  ASSERT_GE(H5Sselect_hyperslab(r2b.get(), H5S_SELECT_SET, kS2b.data(),
                                kSt2.data(), kC2.data(), kB2.data()),
            0);
  EXPECT_GT(H5Sselect_shape_same(r2a.get(), r2b.get()), 0);

  // Shifted irregular + shifted irregular across matching & different ranks.
  constexpr std::array<hsize_t, 2> kPt = {1, 1};
  ASSERT_GE(H5Sselect_hyperslab(r2a.get(), H5S_SELECT_OR, kPt.data(), nullptr,
                                kOnes2.data(), kOnes2.data()),
            0);
  constexpr std::array<hsize_t, 2> kPt2 = {3, 4};
  ASSERT_GE(H5Sselect_hyperslab(r2b.get(), H5S_SELECT_OR, kPt2.data(), nullptr,
                                kOnes2.data(), kOnes2.data()),
            0);
  EXPECT_GT(H5Sselect_shape_same(r2a.get(), r2b.get()), 0);

  constexpr std::array<hsize_t, 3> kS3 = {2, 1, 2};
  constexpr std::array<hsize_t, 3> kSt3 = {1, 3, 3};
  constexpr std::array<hsize_t, 3> kC3 = {1, 2, 2};
  constexpr std::array<hsize_t, 3> kB3 = {1, 2, 2};
  ASSERT_GE(H5Sselect_hyperslab(r3.get(), H5S_SELECT_SET, kS3.data(),
                                kSt3.data(), kC3.data(), kB3.data()),
            0);
  constexpr std::array<hsize_t, 3> kPt3 = {2, 2, 3};
  ASSERT_GE(H5Sselect_hyperslab(r3.get(), H5S_SELECT_OR, kPt3.data(), nullptr,
                                kOnes3.data(), kOnes3.data()),
            0);
  EXPECT_GT(H5Sselect_shape_same(r3.get(), r2a.get()), 0);

  // Mismatched extra rank thickness or unequal count/blocks -> false.
  constexpr std::array<hsize_t, 3> kS3Wide = {2, 0, 0};
  constexpr std::array<hsize_t, 3> kB3Wide = {2, 1, 1};
  ScopedSpace r3w = MakeSimpleSpace(kDims3);
  ASSERT_GE(H5Sselect_hyperslab(r3w.get(), H5S_SELECT_SET, kS3Wide.data(),
                                nullptr, kOnes3.data(), kB3Wide.data()),
            0);
  EXPECT_EQ(H5Sselect_shape_same(r3w.get(), r2a.get()), 0);

  // Hyperslab vs Point selection triggers H5S__hyper_iter_block /
  // iter_has_next_block / iter_next_block inside H5S_select_shape_same!
  ScopedSpace pts = MakeSimpleSpace(kDims2);
  constexpr std::array<hsize_t, 2 * 16> kCoords = {
      0, 0, 0, 1, 0, 3, 0, 4, 1, 0, 1, 1, 1, 3, 1, 4,
      3, 0, 3, 1, 3, 3, 3, 4, 4, 0, 4, 1, 4, 3, 4, 4,
  };
  ASSERT_GE(H5Sselect_elements(pts.get(), H5S_SELECT_SET, 16, kCoords.data()),
            0);
  ScopedSpace reg_pts_shape = MakeSimpleSpace(kDims2);
  ASSERT_GE(H5Sselect_hyperslab(reg_pts_shape.get(), H5S_SELECT_SET,
                                kS2a.data(), kSt2.data(), kC2.data(),
                                kB2.data()),
            0);
  EXPECT_EQ(H5Sselect_shape_same(reg_pts_shape.get(), pts.get()), 0);
  EXPECT_EQ(H5Sselect_shape_same(r2a.get(), pts.get()), 0);

  // Matching block-by-block hyperslab vs points for true via block iterator:
  // 2 isolated 1x1 blocks vs 2 points!
  ScopedSpace reg2 = MakeSimpleSpace(kDims2);
  ASSERT_GE(H5Sselect_hyperslab(reg2.get(), H5S_SELECT_SET, kS2a.data(),
                                kSt2.data(), kC2.data(), kOnes2.data()),
            0);
  ScopedSpace pts4 = MakeSimpleSpace(kDims2);
  constexpr std::array<hsize_t, 8> kCoords4 = {0, 0, 0, 3, 3, 0, 3, 3};
  ASSERT_GE(H5Sselect_elements(pts4.get(), H5S_SELECT_SET, 4, kCoords4.data()),
            0);
  EXPECT_GT(H5Sselect_shape_same(reg2.get(), pts4.get()), 0);

  // Irregular 1x1 blocks vs points!
  ScopedSpace irreg4 = MakeSimpleSpace(kDims2);
  constexpr std::array<hsize_t, 2> kA = {1, 1};
  constexpr std::array<hsize_t, 2> kB = {4, 5};
  ASSERT_GE(H5Sselect_hyperslab(irreg4.get(), H5S_SELECT_SET, kA.data(),
                                nullptr, kOnes2.data(), kOnes2.data()),
            0);
  ASSERT_GE(H5Sselect_hyperslab(irreg4.get(), H5S_SELECT_OR, kB.data(),
                                nullptr, kOnes2.data(), kOnes2.data()),
            0);
  ScopedSpace pts2 = MakeSimpleSpace(kDims2);
  constexpr std::array<hsize_t, 4> kCoords2 = {1, 1, 4, 5};
  ASSERT_GE(H5Sselect_elements(pts2.get(), H5S_SELECT_SET, 2, kCoords2.data()),
            0);
  EXPECT_GT(H5Sselect_shape_same(irreg4.get(), pts2.get()), 0);

  // H5Sselect_intersect_block tests: strided gap miss, strided hit, irregular
  // hit/miss, single block.
  constexpr std::array<hsize_t, 2> kGapBlkStart = {2, 2};
  constexpr std::array<hsize_t, 2> kGapBlkEnd = {2, 2};
  EXPECT_EQ(
      H5Sselect_intersect_block(reg_pts_shape.get(), kGapBlkStart.data(),
                                kGapBlkEnd.data()),
      0);
  constexpr std::array<hsize_t, 2> kHitBlkStart = {2, 2};
  constexpr std::array<hsize_t, 2> kHitBlkEnd = {3, 3};
  EXPECT_GT(
      H5Sselect_intersect_block(reg_pts_shape.get(), kHitBlkStart.data(),
                                kHitBlkEnd.data()),
      0);
  EXPECT_EQ(H5Sselect_intersect_block(r2a.get(), kGapBlkStart.data(),
                                      kGapBlkEnd.data()),
            0);
  EXPECT_GT(H5Sselect_intersect_block(r2a.get(), kHitBlkStart.data(),
                                      kHitBlkEnd.data()),
            0);
}

TEST(H5ShyperTest, AddSpanElementAdjustNormalizeProject) {
  constexpr std::array<hsize_t, 3> kDims = {10, 10, 10};
  ScopedSpace space = MakeSimpleSpace(kDims);
  ASSERT_TRUE(space.valid());
  ASSERT_GE(H5Sselect_none(space.get()), 0);
  H5S_t* sptr = UnwrapSpace(space.get());

  // Build 2x2x2 planes row-by-row using H5S_hyper_add_span_element to exercise
  // tail merging, down-span reuse, and sibling span merging.
  for (hsize_t i : {2, 3, 6}) {
    for (hsize_t j : {1, 2}) {
      for (hsize_t k : {4, 5, 6}) {
        const std::array<hsize_t, 3> pt = {i, j, k};
        ASSERT_GE(H5S_hyper_add_span_element(sptr, 3, pt.data()), 0);
      }
    }
  }
  EXPECT_EQ(H5Sget_select_npoints(space.get()), 18);

  // Add an out-of-order lower row/column to trigger mid/merge span insertion
  // when diminfo_valid is false.
  constexpr std::array<hsize_t, 3> kNewPt = {3, 3, 5};
  ASSERT_GE(H5S_hyper_add_span_element(sptr, 3, kNewPt.data()), 0);
  EXPECT_EQ(H5Sget_select_npoints(space.get()), 19);

  // Set offset, normalize, adjust_u, adjust_s, denormalize.
  constexpr std::array<hssize_t, 3> kOff = {1, 2, 1};
  ASSERT_GE(H5Soffset_simple(space.get(), kOff.data()), 0);
  std::array<hssize_t, 3> saved_off{};
  EXPECT_GT(H5S_hyper_normalize_offset(sptr, saved_off.data()), 0);
  EXPECT_THAT(saved_off, ElementsAre(1, 2, 1));
  ASSERT_GE(H5S_hyper_denormalize_offset(sptr, saved_off.data()), 0);

  constexpr std::array<hsize_t, 3> kAdjU = {1, 1, 1};
  ASSERT_GE(H5S_select_adjust_u(sptr, kAdjU.data()), 0);

  constexpr std::array<hssize_t, 3> kAdjS = {1, 0, 1};
  ASSERT_GE(H5Sselect_adjust(space.get(), kAdjS.data()), 0);

  // Invalid negative adjustment below low_bounds.
  {
    ScopedErrorSilence silence;
    constexpr std::array<hssize_t, 3> kTooBigAdj = {10, 0, 0};
    EXPECT_LT(H5Sselect_adjust(space.get(), kTooBigAdj.data()), 0);
  }

  // Projection: construct_projection up (3->4) and down (4->2, 3->2, 3->0) for
  // both regular and irregular selections.
  {
    ScopedSpace reg_proj_src = MakeSimpleSpace(kDims);
    constexpr std::array<hsize_t, 3> kS1 = {2, 2, 3};
    constexpr std::array<hsize_t, 3> kSt1 = {1, 3, 3};
    constexpr std::array<hsize_t, 3> kC1 = {1, 2, 2};
    constexpr std::array<hsize_t, 3> kB1 = {1, 2, 2};
    ASSERT_GE(H5Sselect_hyperslab(reg_proj_src.get(), H5S_SELECT_SET,
                                  kS1.data(), kSt1.data(), kC1.data(),
                                  kB1.data()),
              0);
    ASSERT_GE(H5Soffset_simple(reg_proj_src.get(), kOff.data()), 0);
    H5S_t* proj_up_raw = nullptr;
    ptrdiff_t adj = -1;
    ASSERT_GE(H5S_select_construct_projection(
                  UnwrapSpace(reg_proj_src.get()), &proj_up_raw, 4, 4, &adj),
              0);
    UniqueInternalSpace proj_up(proj_up_raw);
    EXPECT_EQ(adj, 0);

    H5S_t* proj_dn_raw = nullptr;
    ASSERT_GE(H5S_select_construct_projection(
                  UnwrapSpace(reg_proj_src.get()), &proj_dn_raw, 2, 4, &adj),
              0);
    UniqueInternalSpace proj_dn(proj_dn_raw);
    EXPECT_EQ(adj, 800);

    // Irregular projection up and down.
    constexpr std::array<hsize_t, 3> kS2 = {2, 5, 5};
    constexpr std::array<hsize_t, 3> kOnes = {1, 1, 1};
    ASSERT_GE(H5Sselect_hyperslab(reg_proj_src.get(), H5S_SELECT_OR, kS2.data(),
                                  nullptr, kOnes.data(), kOnes.data()),
              0);
    H5S_t* iproj_up_raw = nullptr;
    ASSERT_GE(H5S_select_construct_projection(
                  UnwrapSpace(reg_proj_src.get()), &iproj_up_raw, 4, 4, &adj),
              0);
    UniqueInternalSpace iproj_up(iproj_up_raw);

    H5S_t* iproj_dn_raw = nullptr;
    ASSERT_GE(H5S_select_construct_projection(
                  UnwrapSpace(reg_proj_src.get()), &iproj_dn_raw, 2, 4, &adj),
              0);
    UniqueInternalSpace iproj_dn(iproj_dn_raw);

    // Single-element scalar projection for regular and irregular.
    ScopedSpace single_el = MakeSimpleSpace(kDims);
    constexpr std::array<hsize_t, 3> kSinglePt = {2, 3, 4};
    ASSERT_GE(H5Sselect_hyperslab(single_el.get(), H5S_SELECT_SET,
                                  kSinglePt.data(), nullptr, kOnes.data(),
                                  kOnes.data()),
              0);
    H5S_t* s_scalar_raw = nullptr;
    ASSERT_GE(H5S_select_construct_projection(
                  UnwrapSpace(single_el.get()), &s_scalar_raw, 0, 8, &adj),
              0);
    UniqueInternalSpace s_scalar(s_scalar_raw);

    // Force irregular 1-element span tree using add_span_element.
    ScopedSpace isingle = MakeSimpleSpace(kDims);
    ASSERT_GE(H5Sselect_none(isingle.get()), 0);
    ASSERT_GE(
        H5S_hyper_add_span_element(UnwrapSpace(isingle.get()), 3, kSinglePt.data()),
        0);
    H5S_t* i_scalar_raw = nullptr;
    ASSERT_GE(H5S_select_construct_projection(
                  UnwrapSpace(isingle.get()), &i_scalar_raw, 0, 8, &adj),
              0);
    UniqueInternalSpace i_scalar(i_scalar_raw);
  }
}

TEST(H5ShyperTest, ProjectIntersectionRegularIrregularAndMultiBatch) {
  constexpr std::array<hsize_t, 3> kSrcDims = {16, 16, 16};
  constexpr std::array<hsize_t, 2> kDstDims = {32, 32};
  constexpr std::array<hsize_t, 3> kOnes3 = {1, 1, 1};
  constexpr std::array<hsize_t, 2> kOnes2 = {1, 1};

  // 1. Regular src -> Regular & Irregular dst with pre-gap, overlap, post-gap,
  //    count > 1 clean-bitmap combinations.
  ScopedSpace src = MakeSimpleSpace(kSrcDims);
  ScopedSpace dst = MakeSimpleSpace(kDstDims);
  ScopedSpace isect = MakeSimpleSpace(kSrcDims);
  ASSERT_TRUE(src.valid() && dst.valid() && isect.valid());

  // Src: 2x4x4 strided (start={1,1,1}, stride={3,3,3}, count={2,3,3}, block={2,2,2})
  // Total = 2*3*3 * 8 = 144 elements.
  constexpr std::array<hsize_t, 3> kSrcStart = {1, 1, 1};
  constexpr std::array<hsize_t, 3> kSrcStride = {3, 3, 3};
  constexpr std::array<hsize_t, 3> kSrcCount = {2, 3, 3};
  constexpr std::array<hsize_t, 3> kSrcBlock = {2, 2, 2};
  ASSERT_GE(H5Sselect_hyperslab(src.get(), H5S_SELECT_SET, kSrcStart.data(),
                                kSrcStride.data(), kSrcCount.data(),
                                kSrcBlock.data()),
            0);

  // Dst: 12 x 12 block (144 elements) made irregular by ORing two abutting pieces.
  constexpr std::array<hsize_t, 2> kDstStart1 = {2, 2};
  constexpr std::array<hsize_t, 2> kDstBlk1 = {6, 12};
  constexpr std::array<hsize_t, 2> kDstStart2 = {10, 2};
  ASSERT_GE(H5Sselect_hyperslab(dst.get(), H5S_SELECT_SET, kDstStart1.data(),
                                nullptr, kOnes2.data(), kDstBlk1.data()),
            0);
  ASSERT_GE(H5Sselect_hyperslab(dst.get(), H5S_SELECT_OR, kDstStart2.data(),
                                nullptr, kOnes2.data(), kDstBlk1.data()),
            0);

  // Src intersect: hits middle + strided subset + tail gap.
  constexpr std::array<hsize_t, 3> kIS = {2, 2, 0};
  constexpr std::array<hsize_t, 3> kISt = {1, 2, 3};
  constexpr std::array<hsize_t, 3> kIC = {1, 3, 3};
  constexpr std::array<hsize_t, 3> kIB = {4, 1, 2};
  ASSERT_GE(H5Sselect_hyperslab(isect.get(), H5S_SELECT_SET, kIS.data(),
                                kISt.data(), kIC.data(), kIB.data()),
            0);

  ScopedSpace proj(
      H5Sselect_project_intersection(src.get(), dst.get(), isect.get()));
  ASSERT_TRUE(proj.valid());
  EXPECT_GT(H5Sget_select_npoints(proj.get()), 0);

  // Same with share_space = true via internal API!
  {
    H5S_t* raw_proj = nullptr;
    ASSERT_GE(H5S_select_project_intersection(UnwrapSpace(src.get()),
                                              UnwrapSpace(dst.get()),
                                              UnwrapSpace(isect.get()),
                                              &raw_proj, /*share_space=*/true),
              0);
    UniqueInternalSpace sproj(raw_proj);
    EXPECT_EQ(H5S_get_select_npoints(sproj.get()),
              H5Sget_select_npoints(proj.get()));
  }

  // Disjoint src_intersect -> H5S_SEL_NONE result.
  constexpr std::array<hsize_t, 3> kDisj = {12, 12, 12};
  ASSERT_GE(H5Sselect_hyperslab(isect.get(), H5S_SELECT_SET, kDisj.data(),
                                nullptr, kOnes3.data(), kOnes3.data()),
            0);
  ScopedSpace empty_proj(
      H5Sselect_project_intersection(src.get(), dst.get(), isect.get()));
  ASSERT_TRUE(empty_proj.valid());
  EXPECT_EQ(H5Sget_select_type(empty_proj.get()), H5S_SEL_NONE);

  // H5S_SEL_ALL as src_space & dst_space + irregular large (>256 span seqs) to
  // test multiple seq buffers in H5S__hyper_proj_int_iterate!
  constexpr std::array<hsize_t, 2> kBigSrc = {20, 40};
  constexpr std::array<hsize_t, 2> kBigDst = {20, 40};
  ScopedSpace b_src = MakeSimpleSpace(kBigSrc);
  ScopedSpace b_dst = MakeSimpleSpace(kBigDst);
  ScopedSpace b_isect = MakeSimpleSpace(kBigSrc);
  // Every other element selected in column dim across 20 rows => 400 spans!
  constexpr std::array<hsize_t, 2> kZero2 = {0, 0};
  constexpr std::array<hsize_t, 2> kBSt = {1, 2};
  constexpr std::array<hsize_t, 2> kBC = {20, 20};
  ASSERT_GE(H5Sselect_hyperslab(b_isect.get(), H5S_SELECT_SET, kZero2.data(),
                                kBSt.data(), kBC.data(), kOnes2.data()),
            0);
  ScopedSpace b_proj(
      H5Sselect_project_intersection(b_src.get(), b_dst.get(), b_isect.get()));
  ASSERT_TRUE(b_proj.valid());
  EXPECT_EQ(H5Sget_select_npoints(b_proj.get()), 400);

  // Also test dst=ALL with irregular src having >256 spans.
  ScopedSpace b_src_irreg = MakeSimpleSpace(kBigSrc);
  ASSERT_GE(H5Sselect_hyperslab(b_src_irreg.get(), H5S_SELECT_SET,
                                kZero2.data(), kBSt.data(), kBC.data(),
                                kOnes2.data()),
            0);
  constexpr std::array<hsize_t, 2> kSmallMod = {0, 1};
  ASSERT_GE(H5Sselect_hyperslab(b_src_irreg.get(), H5S_SELECT_OR,
                                kSmallMod.data(), nullptr, kOnes2.data(),
                                kOnes2.data()),
            0);
  // Use 1D dst matching 401 elements!
  constexpr std::array<hsize_t, 1> kDst1dDims = {500};
  ScopedSpace dst1d = MakeSimpleSpace(kDst1dDims);
  constexpr hsize_t kZ1 = 0, kO1 = 1, kN1 = 401;
  ASSERT_GE(H5Sselect_hyperslab(dst1d.get(), H5S_SELECT_SET, &kZ1, nullptr,
                                &kO1, &kN1),
            0);
  ScopedSpace b_proj2(H5Sselect_project_intersection(
      b_src_irreg.get(), dst1d.get(), b_isect.get()));
  ASSERT_TRUE(b_proj2.valid());
  EXPECT_EQ(H5Sget_select_npoints(b_proj2.get()), 400);
}

TEST(H5ShyperTest, SerializeDeserializeAllVersionsAndWidths) {
  constexpr std::array<H5F_libver_t, 3> kBounds = {
      H5F_LIBVER_EARLIEST, H5F_LIBVER_V110, H5F_LIBVER_V112};
  constexpr std::array<hsize_t, 2> kDimsHuge = {1ULL << 36, 1ULL << 36};
  constexpr std::array<hsize_t, 2> kOnes = {1, 1};

  // Encode/decode regular & irregular selections across V1, V2, V3 and 2/4/8B.
  struct WidthCase {
    hsize_t start;
    hsize_t stride;
    hsize_t count;
    hsize_t block;
    H5F_libver_t min_ver;
  };
  constexpr auto kWidthCases = std::to_array<WidthCase>({
      {10, 5, 3, 2, H5F_LIBVER_EARLIEST},           // <= UINT16_MAX (V1/V2/V3 2B)
      {70000, 10, 2, 3, H5F_LIBVER_EARLIEST},       // > UINT16_MAX (4B)
      {1ULL << 34, 4, 2, 2, H5F_LIBVER_V110},       // > UINT32_MAX (8B)
  });

  for (const WidthCase& wc : kWidthCases) {
    ScopedSpace reg = MakeSimpleSpace(kDimsHuge);
    const std::array<hsize_t, 2> st = {wc.start, 10};
    const std::array<hsize_t, 2> str = {wc.stride, 3};
    const std::array<hsize_t, 2> cnt = {wc.count, 2};
    const std::array<hsize_t, 2> blk = {wc.block, 2};
    ASSERT_GE(H5Sselect_hyperslab(reg.get(), H5S_SELECT_SET, st.data(),
                                  str.data(), cnt.data(), blk.data()),
              0);

    // Also make an irregular counterpart.
    ScopedSpace irreg(H5Scopy(reg.get()));
    const std::array<hsize_t, 2> extra = {wc.start + 1, 20};
    ASSERT_GE(H5Sselect_hyperslab(irreg.get(), H5S_SELECT_OR, extra.data(),
                                  nullptr, kOnes.data(), kOnes.data()),
              0);

    // Helper decoding H5Sencode2's selection payload with an exact finite size
    // to avoid H5Sdecode's upstream SIZE_MAX pointer-overflow in H5Sselect.c:528.
    auto decode_encoded_sel = [](hid_t template_sid,
                                 const std::vector<uint8_t>& buf) -> ScopedSpace {
      if (buf.size() < 7) {
        return {};
      }
      const size_t extent_size = static_cast<size_t>(buf[3]) |
                                 (static_cast<size_t>(buf[4]) << 8) |
                                 (static_cast<size_t>(buf[5]) << 16) |
                                 (static_cast<size_t>(buf[6]) << 24);
      const size_t sel_off = 7 + extent_size;
      if (sel_off >= buf.size()) {
        return {};
      }
      ScopedSpace dec(H5Scopy(template_sid));
      if (!dec.valid() || H5Sselect_none(dec.get()) < 0) {
        return {};
      }
      H5S_t* dec_ptr = UnwrapSpace(dec.get());
      const uint8_t* sel_p = buf.data() + sel_off;
      if (H5S_select_deserialize(&dec_ptr, &sel_p, buf.size() - sel_off) < 0) {
        return {};
      }
      return dec;
    };

    for (H5F_libver_t lb : kBounds) {
      if (lb < wc.min_ver) {
        continue;
      }
      ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_GE(H5Pset_libver_bounds(fapl.get(), lb, H5F_LIBVER_LATEST), 0);
      for (hid_t sid : {reg.get(), irreg.get()}) {
        size_t nalloc = 0;
        ASSERT_GE(H5Sencode2(sid, nullptr, &nalloc, fapl.get()), 0);
        std::vector<uint8_t> buf(nalloc);
        ASSERT_GE(H5Sencode2(sid, buf.data(), &nalloc, fapl.get()), 0);
        ScopedSpace dec = decode_encoded_sel(sid, buf);
        ASSERT_TRUE(dec.valid());
        EXPECT_EQ(H5Sget_select_npoints(dec.get()), H5Sget_select_npoints(sid));
      }
    }
  }

  // Encode/decode unlimited hyperslabs under V2 and V3 (2B, 4B, 8B), plus V1 fail.
  auto decode_unlim_sel = [](hid_t template_sid,
                             const std::vector<uint8_t>& buf) -> ScopedSpace {
    if (buf.size() < 7) {
      return {};
    }
    const size_t extent_size = static_cast<size_t>(buf[3]) |
                               (static_cast<size_t>(buf[4]) << 8) |
                               (static_cast<size_t>(buf[5]) << 16) |
                               (static_cast<size_t>(buf[6]) << 24);
    const size_t sel_off = 7 + extent_size;
    if (sel_off >= buf.size()) {
      return {};
    }
    ScopedSpace dec(H5Scopy(template_sid));
    if (!dec.valid() || H5Sselect_none(dec.get()) < 0) {
      return {};
    }
    H5S_t* dec_ptr = UnwrapSpace(dec.get());
    const uint8_t* sel_p = buf.data() + sel_off;
    if (H5S_select_deserialize(&dec_ptr, &sel_p, buf.size() - sel_off) < 0) {
      return {};
    }
    return dec;
  };
  constexpr std::array<hsize_t, 2> kUnlimDims = {10, 10};
  constexpr std::array<hsize_t, 2> kUnlimMax = {H5S_UNLIMITED, H5S_UNLIMITED};
  for (hsize_t start_coord : {hsize_t{2}, hsize_t{70000}, hsize_t{1ULL << 34}}) {
    ScopedSpace unlim = MakeSimpleSpace(kUnlimDims, kUnlimMax);
    const std::array<hsize_t, 2> ust = {start_coord, 1};
    const std::array<hsize_t, 2> ustr = {5, 3};
    const std::array<hsize_t, 2> ucnt = {H5S_UNLIMITED, 2};
    const std::array<hsize_t, 2> ublk = {2, 2};
    ASSERT_GE(H5Sselect_hyperslab(unlim.get(), H5S_SELECT_SET, ust.data(),
                                  ustr.data(), ucnt.data(), ublk.data()),
              0);

    for (H5F_libver_t lb : {H5F_LIBVER_V110, H5F_LIBVER_V112}) {
      ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_GE(H5Pset_libver_bounds(fapl.get(), lb, H5F_LIBVER_LATEST), 0);
      size_t nalloc = 0;
      ASSERT_GE(H5Sencode2(unlim.get(), nullptr, &nalloc, fapl.get()), 0);
      std::vector<uint8_t> buf(nalloc);
      ASSERT_GE(H5Sencode2(unlim.get(), buf.data(), &nalloc, fapl.get()), 0);
      ScopedSpace dec = decode_unlim_sel(unlim.get(), buf);
      ASSERT_TRUE(dec.valid());
      EXPECT_EQ(H5S_get_select_unlim_dim(UnwrapSpace(dec.get())), 0);
    }

    // Also test unlimited block encoding under V2 and V3.
    ScopedSpace unlim_blk = MakeSimpleSpace(kUnlimDims, kUnlimMax);
    const std::array<hsize_t, 2> bcnt = {1, 2};
    const std::array<hsize_t, 2> bblk = {H5S_UNLIMITED, 2};
    ASSERT_GE(H5Sselect_hyperslab(unlim_blk.get(), H5S_SELECT_SET, ust.data(),
                                  ustr.data(), bcnt.data(), bblk.data()),
              0);
    for (H5F_libver_t lb : {H5F_LIBVER_V110, H5F_LIBVER_V112}) {
      ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
      ASSERT_GE(H5Pset_libver_bounds(fapl.get(), lb, H5F_LIBVER_LATEST), 0);
      size_t nalloc = 0;
      ASSERT_GE(H5Sencode2(unlim_blk.get(), nullptr, &nalloc, fapl.get()), 0);
      std::vector<uint8_t> buf(nalloc);
      ASSERT_GE(H5Sencode2(unlim_blk.get(), buf.data(), &nalloc, fapl.get()),
                0);
      ScopedSpace dec = decode_unlim_sel(unlim_blk.get(), buf);
      ASSERT_TRUE(dec.valid());
      EXPECT_EQ(H5S_get_select_unlim_dim(UnwrapSpace(dec.get())), 0);
    }
  }

  // Test H5S_select_serial_size / H5S_select_serialize / H5S_select_deserialize
  // directly: normal, *space != NULL, rank mismatch, and truncations.
  ScopedSpace s_ser = MakeSimpleSpace(kUnlimDims);
  constexpr std::array<hsize_t, 2> kSS = {1, 2};
  constexpr std::array<hsize_t, 2> kSC = {2, 2};
  ASSERT_GE(H5Sselect_hyperslab(s_ser.get(), H5S_SELECT_SET, kSS.data(),
                                nullptr, kSC.data(), nullptr),
            0);
  {
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_V112, H5F_LIBVER_LATEST),
        0);
    ScopedApiContext ctx;
    ASSERT_TRUE(ctx.valid());
    hid_t apl_id = fapl.get();
    ASSERT_GE(
        H5CX_set_apl(&apl_id, H5P_CLS_FACC, H5I_INVALID_HID, /*is_coll=*/false),
        0);
    hssize_t sz = H5S_SELECT_SERIAL_SIZE(UnwrapSpace(s_ser.get()));
    ASSERT_GT(sz, 0);
    std::vector<uint8_t> raw(static_cast<size_t>(sz));
    uint8_t* p_out = raw.data();
    ASSERT_GE(H5S_SELECT_SERIALIZE(UnwrapSpace(s_ser.get()), &p_out), 0);

    // Deserialize into existing matching-rank dataspace.
    ScopedSpace target = MakeSimpleSpace(kUnlimDims);
    H5S_t* target_ptr = UnwrapSpace(target.get());
    const uint8_t* p_in = raw.data();
    ASSERT_GE(H5S_select_deserialize(&target_ptr, &p_in, raw.size()), 0);
    EXPECT_EQ(H5Sget_select_npoints(target.get()), 4);

    // Error paths: mismatched rank on *space != NULL, corrupt versions/flags,
    // and truncations at every byte boundary >= 5!
    ScopedErrorSilence silence;
    constexpr std::array<hsize_t, 1> kDims1d = {10};
    ScopedSpace mismatch = MakeSimpleSpace(kDims1d);
    H5S_t* mismatch_ptr = UnwrapSpace(mismatch.get());
    p_in = raw.data();
    EXPECT_LT(H5S_select_deserialize(&mismatch_ptr, &p_in, raw.size()), 0);

    for (size_t len = 5; len < raw.size(); ++len) {
      ScopedSpace t2 = MakeSimpleSpace(kUnlimDims);
      H5S_t* t2_ptr = UnwrapSpace(t2.get());
      p_in = raw.data();
      EXPECT_LT(H5S_select_deserialize(&t2_ptr, &p_in, len), 0);
    }

    // Corrupt version = 0, version = 99, invalid flag = 0x80, bad enc_size = 1.
    for (uint8_t bad_ver : {uint8_t{0}, uint8_t{99}}) {
      std::vector<uint8_t> corrupt = raw;
      corrupt[4] = bad_ver;
      ScopedSpace t3 = MakeSimpleSpace(kUnlimDims);
      H5S_t* t3_ptr = UnwrapSpace(t3.get());
      p_in = corrupt.data();
      EXPECT_LT(H5S_select_deserialize(&t3_ptr, &p_in, corrupt.size()), 0);
    }
    {
      std::vector<uint8_t> corrupt = raw;
      corrupt[8] = 0x80;  // Invalid flags byte in V3 header.
      ScopedSpace t3 = MakeSimpleSpace(kUnlimDims);
      H5S_t* t3_ptr = UnwrapSpace(t3.get());
      p_in = corrupt.data();
      EXPECT_LT(H5S_select_deserialize(&t3_ptr, &p_in, corrupt.size()), 0);
    }
    {
      std::vector<uint8_t> corrupt = raw;
      corrupt[9] = 0x01;  // Invalid enc_size byte in V3 header.
      ScopedSpace t3 = MakeSimpleSpace(kUnlimDims);
      H5S_t* t3_ptr = UnwrapSpace(t3.get());
      p_in = corrupt.data();
      EXPECT_LT(H5S_select_deserialize(&t3_ptr, &p_in, corrupt.size()), 0);
    }
  }

  // Libver high_bound too low errors when serializing V2/V3-only selections.
  ScopedErrorSilence silence;
  {
    ScopedSpace huge = MakeSimpleSpace(kDimsHuge);
    const std::array<hsize_t, 2> st = {1ULL << 34, 1};
    const std::array<hsize_t, 2> cnt = {2, 2};
    ASSERT_GE(H5Sselect_hyperslab(huge.get(), H5S_SELECT_SET, st.data(),
                                  nullptr, cnt.data(), nullptr),
              0);
    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(
        H5Pset_libver_bounds(fapl.get(), H5F_LIBVER_EARLIEST, H5F_LIBVER_V18),
        0);
    size_t nalloc = 0;
    EXPECT_LT(H5Sencode2(huge.get(), nullptr, &nalloc, fapl.get()), 0);
  }
}

TEST(H5ShyperTest, IteratorsSeqListSingleOptGenAndDirectMethods) {
  // 1. Single-block 1D and 3D (skip_dim == 0 and skip_dim > 0) with small
  //    maxseq/maxelmts triggering partial remainders and offset resumes.
  {
    constexpr std::array<hsize_t, 3> kDims = {4, 6, 8};
    ScopedSpace s = MakeSimpleSpace(kDims);
    constexpr std::array<hsize_t, 3> kStart = {1, 2, 1};
    constexpr std::array<hsize_t, 3> kOnes = {1, 1, 1};
    constexpr std::array<hsize_t, 3> kBlock = {2, 3, 5};
    ASSERT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_SET, kStart.data(),
                                  nullptr, kOnes.data(), kBlock.data()),
              0);
    // Element-by-element prime chunking forces partial block resume!
    auto seqs = CollectSeqList(s.get(), 4, 0, /*maxseq=*/2, /*maxelmts=*/3);
    size_t total_bytes = 0;
    for (const auto& [off, len] : seqs) {
      total_bytes += len;
    }
    EXPECT_EQ(total_bytes, 2u * 3u * 5u * 4u);
  }
  {
    // Non-flattened single block where fastest dim block == 1 (skip_dim == 0).
    constexpr std::array<hsize_t, 3> kDims = {4, 6, 8};
    ScopedSpace s = MakeSimpleSpace(kDims);
    constexpr std::array<hsize_t, 3> kStart = {0, 1, 2};
    constexpr std::array<hsize_t, 3> kOnes = {1, 1, 1};
    constexpr std::array<hsize_t, 3> kBlock = {3, 4, 1};
    ASSERT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_SET, kStart.data(),
                                  nullptr, kOnes.data(), kBlock.data()),
              0);
    auto seqs = CollectSeqList(s.get(), 4, 0, /*maxseq=*/3, /*maxelmts=*/5);
    size_t total_bytes = 0;
    for (const auto& [off, len] : seqs) {
      total_bytes += len;
    }
    EXPECT_EQ(total_bytes, 12u * 4u);
  }

  // 2. Optimized strided (count = 1..11) to exercise all Duff's device unroll
  //    counts (0..7) and flattened/non-flattened wrapping with offset != 0.
  for (hsize_t fast_cnt : {1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 6ULL, 7ULL, 8ULL,
                           10ULL}) {
    constexpr std::array<hsize_t, 3> kDims = {3, 4, 30};
    ScopedSpace s = MakeSimpleSpace(kDims);
    const std::array<hsize_t, 3> kStart = {0, 0, 1};
    const std::array<hsize_t, 3> kStride = {1, 2, 2};
    const std::array<hsize_t, 3> kCnt = {2, 2, fast_cnt};
    const std::array<hsize_t, 3> kBlk = {1, 1, 1};
    ASSERT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_SET, kStart.data(),
                                  kStride.data(), kCnt.data(), kBlk.data()),
              0);
    constexpr std::array<hssize_t, 3> kOff = {0, 1, 1};
    ASSERT_GE(H5Soffset_simple(s.get(), kOff.data()), 0);
    auto seqs = CollectSeqList(s.get(), 8, 0, /*maxseq=*/3, /*maxelmts=*/5);
    size_t total_bytes = 0;
    for (const auto& [off, len] : seqs) {
      total_bytes += len;
    }
    EXPECT_EQ(total_bytes, 4u * fast_cnt * 8u);
  }

  // 3. Irregular 3D iterator with & without H5S_SEL_ITER_SHARE_WITH_DATASPACE
  //    and prime chunking to force partial span resumes & dim backtracking.
  {
    constexpr std::array<hsize_t, 3> kDims = {6, 6, 8};
    ScopedSpace s = MakeSimpleSpace(kDims);
    constexpr std::array<hsize_t, 3> kS1 = {0, 1, 1};
    constexpr std::array<hsize_t, 3> kSt1 = {2, 2, 3};
    constexpr std::array<hsize_t, 3> kC1 = {2, 2, 2};
    constexpr std::array<hsize_t, 3> kB1 = {1, 2, 2};
    ASSERT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_SET, kS1.data(),
                                  kSt1.data(), kC1.data(), kB1.data()),
              0);
    constexpr std::array<hsize_t, 3> kS2 = {1, 0, 2};
    constexpr std::array<hsize_t, 3> kOnes = {1, 1, 1};
    constexpr std::array<hsize_t, 3> kB2 = {2, 3, 4};
    ASSERT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_OR, kS2.data(), nullptr,
                                  kOnes.data(), kB2.data()),
              0);

    for (unsigned flags : {0u, unsigned{H5S_SEL_ITER_SHARE_WITH_DATASPACE}}) {
      auto seqs = CollectSeqList(s.get(), 4, flags, /*maxseq=*/2, /*maxelmts=*/3);
      size_t total_bytes = 0;
      for (const auto& [off, len] : seqs) {
        total_bytes += len;
      }
      EXPECT_EQ(total_bytes,
                static_cast<size_t>(H5Sget_select_npoints(s.get())) * 4u);
    }

    // Exercise H5S_select_iterate and H5S_select_fill on buffer.
    const hsize_t total_elems = kDims[0] * kDims[1] * kDims[2];
    std::vector<uint32_t> buf(total_elems, 0);
    constexpr uint32_t kFill = 0xA5A55A5Au;
    H5T_t* native_u32 =
        static_cast<H5T_t*>(H5I_object_verify(H5T_NATIVE_UINT32, H5I_DATATYPE));
    ASSERT_NE(native_u32, nullptr);
    ASSERT_GE(
        H5S_select_fill(&kFill, sizeof(kFill), UnwrapSpace(s.get()), buf.data()),
        0);

    size_t visited = 0;
    H5S_sel_iter_op_t op{};
    op.op_type = H5S_SEL_ITER_OP_LIB;
    op.u.lib_op = [](void* elem, const H5T_t* /*type*/, unsigned /*ndim*/,
                     const hsize_t* /*point*/, void* op_data) -> herr_t {
      EXPECT_EQ(*static_cast<uint32_t*>(elem), 0xA5A55A5Au);
      ++(*static_cast<size_t*>(op_data));
      return 0;
    };
    ASSERT_GE(H5S_select_iterate(buf.data(), native_u32, UnwrapSpace(s.get()),
                                 &op, &visited),
              0);
    EXPECT_EQ(visited, static_cast<size_t>(H5Sget_select_npoints(s.get())));
  }

  // 4. Direct internal iterator methods (coords, nelmts, next, block,
  //    has_next_block, next_block) on flattened regular (count==1 and count>1),
  //    unflattened regular (elmt_size=0), and irregular trees.
  constexpr std::array<hsize_t, 3> kIterDims = {4, 5, 6};
  {
    // Flattened count == 1 regular.
    ScopedSpace s_flat1 = MakeSimpleSpace(kIterDims);
    constexpr std::array<hsize_t, 3> kStart = {1, 0, 0};
    constexpr std::array<hsize_t, 3> kOnes = {1, 1, 1};
    constexpr std::array<hsize_t, 3> kBlk = {2, 5, 6};
    ASSERT_GE(H5Sselect_hyperslab(s_flat1.get(), H5S_SELECT_SET, kStart.data(),
                                  nullptr, kOnes.data(), kBlk.data()),
              0);
    ScopedSelIter it;
    ASSERT_GE(it.init(UnwrapSpace(s_flat1.get()), /*elmt_size=*/4, 0), 0);
    std::array<hsize_t, 3> coords{};
    ASSERT_GE(H5S_select_iter_coords(it.get(), coords.data()), 0);
    EXPECT_THAT(coords, ElementsAre(1, 0, 0));
    EXPECT_EQ(H5S_select_iter_nelmts(it.get()), 60u);
    ASSERT_GE(H5S_select_iter_next(it.get(), 7), 0);
    ASSERT_GE(H5S_select_iter_coords(it.get(), coords.data()), 0);
    EXPECT_EQ(H5S_select_iter_nelmts(it.get()), 53u);
    EXPECT_EQ(it.get()->type->iter_has_next_block(it.get()), 0);
  }
  {
    // Flattened count > 1 regular and unflattened (elmt_size == 0) regular.
    ScopedSpace s_flat_cnt = MakeSimpleSpace(kIterDims);
    constexpr std::array<hsize_t, 3> kStart = {0, 0, 0};
    constexpr std::array<hsize_t, 3> kStride = {2, 1, 1};
    constexpr std::array<hsize_t, 3> kCount = {2, 1, 1};
    constexpr std::array<hsize_t, 3> kBlk = {1, 5, 6};
    ASSERT_GE(H5Sselect_hyperslab(s_flat_cnt.get(), H5S_SELECT_SET,
                                  kStart.data(), kStride.data(), kCount.data(),
                                  kBlk.data()),
              0);
    for (size_t elsz : {size_t{0}, size_t{4}}) {
      ScopedSelIter it;
      ASSERT_GE(it.init(UnwrapSpace(s_flat_cnt.get()), elsz, 0), 0);
      std::array<hsize_t, 3> coords{};
      ASSERT_GE(H5S_select_iter_coords(it.get(), coords.data()), 0);
      ASSERT_GE(H5S_select_iter_next(it.get(), 17), 0);
      ASSERT_GE(H5S_select_iter_coords(it.get(), coords.data()), 0);
      ASSERT_GE(H5S_select_iter_next(it.get(), 15), 0);
      ASSERT_GE(H5S_select_iter_coords(it.get(), coords.data()), 0);

      // Re-init and walk all 10 1D row blocks cleanly via iter_block /
      // iter_next_block / iter_has_next_block.
      ASSERT_GE(it.init(UnwrapSpace(s_flat_cnt.get()), 0, 0), 0);
      int row_blocks = 0;
      for (;;) {
        std::array<hsize_t, 3> b_st{};
        std::array<hsize_t, 3> b_en{};
        ASSERT_GE(
            it.get()->type->iter_block(it.get(), b_st.data(), b_en.data()), 0);
        ++row_blocks;
        if (it.get()->type->iter_has_next_block(it.get()) <= 0) {
          break;
        }
        ASSERT_GE(it.get()->type->iter_next_block(it.get()), 0);
      }
      // Note: H5S__hyper_iter_has_next_block becomes false upon reaching (2,0,0).
      EXPECT_GE(row_blocks, 6);
    }

    // Multi-dim block carry with block == 1 in fastest dim, count > 1 in slow dims.
    ScopedSpace s_blk_walk = MakeSimpleSpace(kIterDims);
    constexpr std::array<hsize_t, 3> kZero = {0, 0, 0};
    constexpr std::array<hsize_t, 3> kWStride = {2, 3, 1};
    constexpr std::array<hsize_t, 3> kWCount = {2, 2, 1};
    constexpr std::array<hsize_t, 3> kWBlk = {1, 2, 3};
    ASSERT_GE(H5Sselect_hyperslab(s_blk_walk.get(), H5S_SELECT_SET,
                                  kZero.data(), kWStride.data(),
                                  kWCount.data(), kWBlk.data()),
              0);
    ScopedSelIter bw_it;
    ASSERT_GE(bw_it.init(UnwrapSpace(s_blk_walk.get()), 0, 0), 0);
    int walk_steps = 0;
    for (;;) {
      std::array<hsize_t, 3> b_st{};
      std::array<hsize_t, 3> b_en{};
      ASSERT_GE(
          bw_it.get()->type->iter_block(bw_it.get(), b_st.data(), b_en.data()),
          0);
      ++walk_steps;
      if (bw_it.get()->type->iter_has_next_block(bw_it.get()) <= 0) {
        break;
      }
      ASSERT_GE(bw_it.get()->type->iter_next_block(bw_it.get()), 0);
    }
    EXPECT_GE(walk_steps, 4);
  }
  {
    // Irregular multi-step iter_next (both nelem == 1 and nelem > 1) and block walk.
    ScopedSpace s_ir = MakeSimpleSpace(kIterDims);
    constexpr std::array<hsize_t, 3> kS1 = {0, 0, 0};
    constexpr std::array<hsize_t, 3> kS2 = {2, 1, 2};
    constexpr std::array<hsize_t, 3> kOnes = {1, 1, 1};
    constexpr std::array<hsize_t, 3> kB1 = {1, 2, 3};
    constexpr std::array<hsize_t, 3> kB2 = {2, 2, 2};
    ASSERT_GE(H5Sselect_hyperslab(s_ir.get(), H5S_SELECT_SET, kS1.data(),
                                  nullptr, kOnes.data(), kB1.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(s_ir.get(), H5S_SELECT_OR, kS2.data(),
                                  nullptr, kOnes.data(), kB2.data()),
              0);
    ScopedSelIter it;
    ASSERT_GE(it.init(UnwrapSpace(s_ir.get()), 4, 0), 0);
    std::array<hsize_t, 3> coords{};
    ASSERT_GE(H5S_select_iter_coords(it.get(), coords.data()), 0);
    ASSERT_GE(H5S_select_iter_next(it.get(), 4), 0);  // Jump across row/span!
    ASSERT_GE(H5S_select_iter_coords(it.get(), coords.data()), 0);
    ASSERT_GE(H5S_select_iter_next(it.get(), 5), 0);
    ASSERT_GE(H5S_select_iter_coords(it.get(), coords.data()), 0);

    ASSERT_GE(it.init(UnwrapSpace(s_ir.get()), 0, 0), 0);
    int block_count = 0;
    for (;;) {
      std::array<hsize_t, 3> b_st{};
      std::array<hsize_t, 3> b_en{};
      ASSERT_GE(it.get()->type->iter_block(it.get(), b_st.data(), b_en.data()),
                0);
      ++block_count;
      if (it.get()->type->iter_has_next_block(it.get()) <= 0) {
        break;
      }
      ASSERT_GE(it.get()->type->iter_next_block(it.get()), 0);
    }
    EXPECT_GE(block_count, 2);
  }
}

TEST(H5ShyperTest, SerializeNullSpaceIrregularAndAllWidthTruncations) {
  constexpr std::array<hsize_t, 2> kDimsHuge = {1ULL << 36, 1ULL << 36};
  constexpr std::array<hsize_t, 2> kOnes = {1, 1};

  struct CaseSpec {
    hsize_t start;
    hsize_t stride;
    hsize_t count;
    hsize_t block;
    H5F_libver_t lb;
    H5F_libver_t hb;
  };
  constexpr auto kCases = std::to_array<CaseSpec>({
      {5, 4, 2, 2, H5F_LIBVER_EARLIEST, H5F_LIBVER_V18},
      {5, 4, 2, 2, H5F_LIBVER_V110, H5F_LIBVER_V110},
      {5, 4, 2, 2, H5F_LIBVER_V112, H5F_LIBVER_LATEST},       // V3 2B
      {70000, 10, 2, 3, H5F_LIBVER_V112, H5F_LIBVER_LATEST},  // V3 4B
      {1ULL << 34, 4, 2, 2, H5F_LIBVER_V110, H5F_LIBVER_LATEST},
      {1ULL << 34, 4, 2, 2, H5F_LIBVER_V112, H5F_LIBVER_LATEST},  // V3 8B
  });

  for (const CaseSpec& cs : kCases) {
    ScopedSpace reg = MakeSimpleSpace(kDimsHuge);
    const std::array<hsize_t, 2> st = {cs.start, 3};
    const std::array<hsize_t, 2> str = {cs.stride, 5};
    const std::array<hsize_t, 2> cnt = {cs.count, 2};
    const std::array<hsize_t, 2> blk = {cs.block, 2};
    ASSERT_GE(H5Sselect_hyperslab(reg.get(), H5S_SELECT_SET, st.data(),
                                  str.data(), cnt.data(), blk.data()),
              0);

    ScopedSpace irreg(H5Scopy(reg.get()));
    const std::array<hsize_t, 2> extra = {cs.start + 1, 20};
    ASSERT_GE(H5Sselect_hyperslab(irreg.get(), H5S_SELECT_OR, extra.data(),
                                  nullptr, kOnes.data(), kOnes.data()),
              0);

    ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_GE(H5Pset_libver_bounds(fapl.get(), cs.lb, cs.hb), 0);

    for (hid_t sid : {reg.get(), irreg.get()}) {
      ScopedApiContext ctx;
      ASSERT_TRUE(ctx.valid());
      hid_t apl_id = fapl.get();
      ASSERT_GE(H5CX_set_apl(&apl_id, H5P_CLS_FACC, H5I_INVALID_HID, false),
                0);

      hssize_t sz = H5S_SELECT_SERIAL_SIZE(UnwrapSpace(sid));
      ASSERT_GT(sz, 0);
      std::vector<uint8_t> raw(static_cast<size_t>(sz));
      uint8_t* p_out = raw.data();
      ASSERT_GE(H5S_SELECT_SERIALIZE(UnwrapSpace(sid), &p_out), 0);
      EXPECT_EQ(static_cast<size_t>(p_out - raw.data()), raw.size());

      // Deserialize with *space == nullptr (allocates new space internally).
      H5S_t* raw_space = nullptr;
      const uint8_t* p_in = raw.data();
      ASSERT_GE(H5S_select_deserialize(&raw_space, &p_in, raw.size()), 0);
      UniqueInternalSpace owned(raw_space);
      ASSERT_NE(owned, nullptr);
      EXPECT_EQ(H5S_get_select_npoints(owned.get()), H5Sget_select_npoints(sid));

      // Truncations into *space == nullptr cover cleanup and overflow checks.
      ScopedErrorSilence silence;
      for (size_t trunc_len = 5; trunc_len < raw.size(); ++trunc_len) {
        H5S_t* fail_space = nullptr;
        const uint8_t* tp = raw.data();
        EXPECT_LT(H5S_select_deserialize(&fail_space, &tp, trunc_len), 0);
        EXPECT_EQ(fail_space, nullptr);
      }

      // Corrupt enc_size = 0x01 in V3 buffers (both regular and irregular).
      if (cs.lb >= H5F_LIBVER_V112) {
        std::vector<uint8_t> bad = raw;
        bad[9] = 0x01;
        ScopedSpace target = MakeSimpleSpace(kDimsHuge);
        H5S_t* tptr = UnwrapSpace(target.get());
        const uint8_t* tp = bad.data();
        EXPECT_LT(H5S_select_deserialize(&tptr, &tp, bad.size()), 0);
      }
    }
  }

  // Error checks when bounds exceed V1/V18 limit.
  ScopedErrorSilence silence;
  ScopedSpace s_big_irreg = MakeSimpleSpace(kDimsHuge);
  constexpr std::array<hsize_t, 2> kBigS = {1ULL << 34, 1};
  constexpr std::array<hsize_t, 2> kBigC = {2, 2};
  ASSERT_GE(H5Sselect_hyperslab(s_big_irreg.get(), H5S_SELECT_SET,
                                kBigS.data(), nullptr, kBigC.data(), nullptr),
            0);
  constexpr std::array<hsize_t, 2> kBigExtra = {(1ULL << 34) + 5, 10};
  ASSERT_GE(H5Sselect_hyperslab(s_big_irreg.get(), H5S_SELECT_OR,
                                kBigExtra.data(), nullptr, kOnes.data(),
                                kOnes.data()),
            0);
  ScopedPlist fapl_v1(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_GE(H5Pset_libver_bounds(fapl_v1.get(), H5F_LIBVER_EARLIEST,
                                 H5F_LIBVER_V18),
            0);
  size_t nalloc = 0;
  // Regular or irregular with > 2^32 bound under EARLIEST..V18 -> hits 3652-3654.
  EXPECT_LT(H5Sencode2(s_big_irreg.get(), nullptr, &nalloc, fapl_v1.get()), 0);

  // Regular with > 2^32 block count under EARLIEST..V18 -> hits 3649-3651.
  ScopedSpace s_big_cnt = MakeSimpleSpace(kDimsHuge);
  constexpr std::array<hsize_t, 2> kZero = {0, 0};
  constexpr std::array<hsize_t, 2> kBigStride = {2, 2};
  constexpr std::array<hsize_t, 2> kBigCount = {70000, 70000};
  ASSERT_GE(H5Sselect_hyperslab(s_big_cnt.get(), H5S_SELECT_SET, kZero.data(),
                                kBigStride.data(), kBigCount.data(), nullptr),
            0);
  EXPECT_LT(H5Sencode2(s_big_cnt.get(), nullptr, &nalloc, fapl_v1.get()), 0);

  // Unlimited selection with small coords under EARLIEST..V18 -> hits 3655-3657.
  constexpr std::array<hsize_t, 2> kUnlimMax = {H5S_UNLIMITED, 8};
  constexpr std::array<hsize_t, 2> kSmallDims = {4, 8};
  ScopedSpace s_unlim(
      H5Screate_simple(2, kSmallDims.data(), kUnlimMax.data()));
  constexpr std::array<hsize_t, 2> kUS = {2, 1};
  constexpr std::array<hsize_t, 2> kUSt = {4, 1};
  constexpr std::array<hsize_t, 2> kUC = {H5S_UNLIMITED, 1};
  constexpr std::array<hsize_t, 2> kUB = {2, 4};
  ASSERT_GE(H5Sselect_hyperslab(s_unlim.get(), H5S_SELECT_SET, kUS.data(),
                                kUSt.data(), kUC.data(), kUB.data()),
            0);
  EXPECT_LT(H5Sencode2(s_unlim.get(), nullptr, &nalloc, fapl_v1.get()), 0);
}

TEST(H5ShyperTest, IrregularShapeSameAndBlockIntersectionDeep) {
  constexpr std::array<hsize_t, 2> kDims2 = {12, 12};
  constexpr std::array<hsize_t, 3> kDims3 = {8, 12, 12};
  constexpr std::array<hsize_t, 2> kOnes2 = {1, 1};
  constexpr std::array<hsize_t, 3> kOnes3 = {1, 1, 1};

  // Construct genuinely non-rebuildable L-shaped 2D irregular selections.
  auto make_l_2d = [&](hsize_t r0, hsize_t c0) {
    ScopedSpace s = MakeSimpleSpace(kDims2);
    const std::array<hsize_t, 2> s1 = {r0, c0};
    const std::array<hsize_t, 2> b1 = {2, 3};
    const std::array<hsize_t, 2> s2 = {r0 + 2, c0};
    const std::array<hsize_t, 2> b2 = {1, 1};
    EXPECT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_SET, s1.data(), nullptr,
                                  kOnes2.data(), b1.data()),
              0);
    EXPECT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_OR, s2.data(), nullptr,
                                  kOnes2.data(), b2.data()),
              0);
    return s;
  };

  ScopedSpace l2_base = make_l_2d(1, 2);
  ScopedSpace l2_same = make_l_2d(1, 2);
  ScopedSpace l2_shift = make_l_2d(4, 5);
  ScopedSpace l2_shift_slow = make_l_2d(4, 2);

  EXPECT_GT(H5Sselect_shape_same(l2_base.get(), l2_same.get()), 0);
  EXPECT_GT(H5Sselect_shape_same(l2_base.get(), l2_shift.get()), 0);
  EXPECT_GT(H5Sselect_shape_same(l2_base.get(), l2_shift_slow.get()), 0);

  // Mismatched irregular shapes in spans_shape_same_helper:
  // (a) Different span high in fast dim (bounds differ or span len differs).
  ScopedSpace l2_diff_hi = MakeSimpleSpace(kDims2);
  {
    constexpr std::array<hsize_t, 2> s1 = {1, 2};
    constexpr std::array<hsize_t, 2> b1 = {2, 4};
    constexpr std::array<hsize_t, 2> s2 = {3, 2};
    constexpr std::array<hsize_t, 2> b2 = {1, 1};
    ASSERT_GE(H5Sselect_hyperslab(l2_diff_hi.get(), H5S_SELECT_SET, s1.data(),
                                  nullptr, kOnes2.data(), b1.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(l2_diff_hi.get(), H5S_SELECT_OR, s2.data(),
                                  nullptr, kOnes2.data(), b2.data()),
              0);
  }
  EXPECT_EQ(H5Sselect_shape_same(l2_base.get(), l2_diff_hi.get()), 0);

  // (b) Same outer bounds and total count, different middle span (2 spans vs 1).
  auto make_same_bounds_diff_mid = [&](bool split_mid) {
    ScopedSpace s = MakeSimpleSpace(kDims2);
    constexpr std::array<hsize_t, 2> s1 = {1, 1};
    constexpr std::array<hsize_t, 2> b1 = {1, 5};
    constexpr std::array<hsize_t, 2> s2 = {4, 1};
    constexpr std::array<hsize_t, 2> b2 = {1, 5};
    EXPECT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_SET, s1.data(), nullptr,
                                  kOnes2.data(), b1.data()),
              0);
    EXPECT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_OR, s2.data(), nullptr,
                                  kOnes2.data(), b2.data()),
              0);
    if (split_mid) {
      constexpr std::array<hsize_t, 2> sm1 = {2, 1};
      constexpr std::array<hsize_t, 2> sm2 = {2, 4};
      EXPECT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_OR, sm1.data(),
                                    nullptr, kOnes2.data(), kOnes2.data()),
                0);
      EXPECT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_OR, sm2.data(),
                                    nullptr, kOnes2.data(), kOnes2.data()),
                0);
    } else {
      constexpr std::array<hsize_t, 2> sm = {2, 2};
      constexpr std::array<hsize_t, 2> bm = {1, 2};
      EXPECT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_OR, sm.data(), nullptr,
                                    kOnes2.data(), bm.data()),
                0);
    }
    return s;
  };
  ScopedSpace s_split = make_same_bounds_diff_mid(true);
  ScopedSpace s_solid = make_same_bounds_diff_mid(false);
  EXPECT_EQ(H5Sselect_shape_same(s_split.get(), s_solid.get()), 0);

  // Shifted ONLY in dim 0 of 3D so rest_zeros hits H5S__hyper_cmp_spans.
  auto make_l_3d = [&](hsize_t d0, hsize_t d0_len, hsize_t r0, hsize_t c0,
                       bool add_second_d0) {
    ScopedSpace s = MakeSimpleSpace(kDims3);
    const std::array<hsize_t, 3> s1 = {d0, r0, c0};
    const std::array<hsize_t, 3> b1 = {d0_len, 2, 3};
    const std::array<hsize_t, 3> s2 = {d0, r0 + 2, c0};
    const std::array<hsize_t, 3> b2 = {d0_len, 1, 1};
    EXPECT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_SET, s1.data(), nullptr,
                                  kOnes3.data(), b1.data()),
              0);
    EXPECT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_OR, s2.data(), nullptr,
                                  kOnes3.data(), b2.data()),
              0);
    if (add_second_d0) {
      const std::array<hsize_t, 3> s3 = {d0 + 3, r0, c0};
      const std::array<hsize_t, 3> b3 = {1, 1, 1};
      EXPECT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_OR, s3.data(),
                                    nullptr, kOnes3.data(), b3.data()),
                0);
    }
    return s;
  };
  ScopedSpace l3_a = make_l_3d(1, 1, 1, 2, false);
  ScopedSpace l3_b = make_l_3d(4, 1, 1, 2, false);
  ScopedSpace l3_c = make_l_3d(4, 2, 1, 2, false);
  ScopedSpace l3_d = make_l_3d(1, 1, 1, 2, true);
  // 3D irregular slice thickness 1 vs 2D irregular -> true!
  EXPECT_GT(H5Sselect_shape_same(l3_a.get(), l2_base.get()), 0);
  // 3D shifted only in dim 0 vs 3D -> hits rest_zeros cmp_spans!
  EXPECT_GT(H5Sselect_shape_same(l3_a.get(), l3_b.get()), 0);
  // 3D vs 2D where extra dim has thickness 2 or multiple spans -> false!
  EXPECT_EQ(H5Sselect_shape_same(l3_c.get(), l2_base.get()), 0);
  EXPECT_EQ(H5Sselect_shape_same(l3_d.get(), l2_base.get()), 0);

  // Regular-in-disguise (diminfo_valid == NO) created by add_span_element.
  // Use strided coordinates {2,4},{2,6},{4,4},{4,6} (stride=2 > block=1) so
  // H5S__hyper_rebuild recovers stride={2,2}, count={2,2}, block={1,1}.
  ScopedSpace reg_disguise = MakeSimpleSpace(kDims2);
  ASSERT_GE(H5Sselect_none(reg_disguise.get()), 0);
  for (hsize_t r : {hsize_t{2}, hsize_t{4}}) {
    for (hsize_t c : {hsize_t{4}, hsize_t{6}}) {
      const std::array<hsize_t, 2> pt = {r, c};
      ASSERT_GE(
          H5S_hyper_add_span_element(UnwrapSpace(reg_disguise.get()), 2,
                                     pt.data()),
          0);
    }
  }
  ScopedSpace reg_2x2 = MakeSimpleSpace(kDims2);
  constexpr std::array<hsize_t, 2> kRegStart = {2, 4};
  constexpr std::array<hsize_t, 2> k2x2 = {2, 2};
  ASSERT_GE(H5Sselect_hyperslab(reg_2x2.get(), H5S_SELECT_SET, kRegStart.data(),
                                nullptr, kOnes2.data(), k2x2.data()),
            0);
  ScopedSpace reg_grid_2d = MakeSimpleSpace(kDims2);
  constexpr std::array<hsize_t, 2> kGridSt = {2, 2};
  constexpr std::array<hsize_t, 2> kGridCnt = {2, 2};
  ASSERT_GE(H5Sselect_hyperslab(reg_grid_2d.get(), H5S_SELECT_SET,
                                kRegStart.data(), kGridSt.data(),
                                kGridCnt.data(), kOnes2.data()),
            0);
  // Compare reg_grid_2d (space1 regular) vs reg_disguise (space2 rebuilds -> true!).
  EXPECT_GT(H5Sselect_shape_same(reg_grid_2d.get(), reg_disguise.get()), 0);
  // 3D strided slice rebuilds dim0={1,1,1,1}, dim1..2={2/4,2,2,1} -> matches 2D!
  ScopedSpace reg_disguise_3d = MakeSimpleSpace(kDims3);
  ASSERT_GE(H5Sselect_none(reg_disguise_3d.get()), 0);
  for (hsize_t r : {hsize_t{2}, hsize_t{4}}) {
    for (hsize_t c : {hsize_t{4}, hsize_t{6}}) {
      const std::array<hsize_t, 3> pt = {1, r, c};
      ASSERT_GE(
          H5S_hyper_add_span_element(UnwrapSpace(reg_disguise_3d.get()), 3,
                                     pt.data()),
          0);
    }
  }
  EXPECT_GT(H5Sselect_shape_same(reg_disguise_3d.get(), reg_grid_2d.get()), 0);

  // Also trigger depth-1 sibling merge (6043-6065) and sibling share (6068-6078)
  // in add_span_element via consecutive rows followed by a new row/gap.
  ScopedSpace add_merge_share = MakeSimpleSpace(kDims3);
  ASSERT_GE(H5Sselect_none(add_merge_share.get()), 0);
  const std::array<std::array<hsize_t, 3>, 9> kPts = {{
      {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, {0, 1, 1},
      {0, 2, 0}, {0, 2, 1}, {0, 4, 0}, {0, 4, 1},
      {0, 5, 0},
  }};
  for (const auto& pt : kPts) {
    ASSERT_GE(
        H5S_hyper_add_span_element(UnwrapSpace(add_merge_share.get()), 3,
                                   pt.data()),
        0);
  }
  EXPECT_EQ(H5Sget_select_npoints(add_merge_share.get()), 9);

  // Also test diminfo_valid == NO rebuild inside intersect_block!
  ScopedSpace reg_disguise2 = MakeSimpleSpace(kDims2);
  ASSERT_GE(H5Sselect_none(reg_disguise2.get()), 0);
  for (hsize_t r : {hsize_t{2}, hsize_t{3}}) {
    for (hsize_t c : {hsize_t{4}, hsize_t{5}}) {
      const std::array<hsize_t, 2> pt = {r, c};
      ASSERT_GE(
          H5S_hyper_add_span_element(UnwrapSpace(reg_disguise2.get()), 2,
                                     pt.data()),
          0);
    }
  }
  constexpr std::array<hsize_t, 2> kHit22 = {3, 4};
  EXPECT_GT(H5Sselect_intersect_block(reg_disguise2.get(), kHit22.data(),
                                      kHit22.data()),
            0);
  // Single block regular intersect + strided count == 1 in one dim.
  EXPECT_GT(
      H5Sselect_intersect_block(reg_2x2.get(), kHit22.data(), kHit22.data()),
      0);
  ScopedSpace s_1stride = MakeSimpleSpace(kDims2);
  constexpr std::array<hsize_t, 2> kSt1s = {1, 3};
  constexpr std::array<hsize_t, 2> kCn1s = {1, 2};
  constexpr std::array<hsize_t, 2> kBk1s = {4, 2};
  ASSERT_GE(H5Sselect_hyperslab(s_1stride.get(), H5S_SELECT_SET, kOnes2.data(),
                                kSt1s.data(), kCn1s.data(), kBk1s.data()),
            0);
  constexpr std::array<hsize_t, 2> kGapMissS = {2, 3};
  EXPECT_EQ(H5Sselect_intersect_block(s_1stride.get(), kGapMissS.data(),
                                      kGapMissS.data()),
            0);

  // Genuine irregular 3D intersect_block: DAG shared rows, skip curr<start,
  // stop curr>end, down-span miss then later hit!
  ScopedSpace irreg3 = MakeSimpleSpace(kDims3);
  constexpr std::array<hsize_t, 3> kI1S = {1, 1, 2};
  constexpr std::array<hsize_t, 3> kI1B = {1, 2, 2};
  constexpr std::array<hsize_t, 3> kI2S = {3, 1, 2};
  constexpr std::array<hsize_t, 3> kI3S = {3, 5, 5};
  constexpr std::array<hsize_t, 3> kI4S = {5, 1, 2};
  ASSERT_GE(H5Sselect_hyperslab(irreg3.get(), H5S_SELECT_SET, kI1S.data(),
                                nullptr, kOnes3.data(), kI1B.data()),
            0);
  ASSERT_GE(H5Sselect_hyperslab(irreg3.get(), H5S_SELECT_OR, kI2S.data(),
                                nullptr, kOnes3.data(), kI1B.data()),
            0);
  ASSERT_GE(H5Sselect_hyperslab(irreg3.get(), H5S_SELECT_OR, kI3S.data(),
                                nullptr, kOnes3.data(), kI1B.data()),
            0);
  ASSERT_GE(H5Sselect_hyperslab(irreg3.get(), H5S_SELECT_OR, kI4S.data(),
                                nullptr, kOnes3.data(), kOnes3.data()),
            0);
  // Out of bounds block -> false.
  constexpr std::array<hsize_t, 3> kOut = {0, 0, 0};
  EXPECT_EQ(H5Sselect_intersect_block(irreg3.get(), kOut.data(), kOut.data()),
            0);
  // Spans dim 0 gap [2..2] (past curr 1, before curr 3 -> curr->low > end).
  constexpr std::array<hsize_t, 3> kMidGap = {2, 1, 2};
  EXPECT_EQ(
      H5Sselect_intersect_block(irreg3.get(), kMidGap.data(), kMidGap.data()),
      0);
  // Matches dim 0 [3], misses in down span (y=4), stops in dim 0 [4].
  constexpr std::array<hsize_t, 3> kDownMissS = {3, 4, 2};
  constexpr std::array<hsize_t, 3> kDownMissE = {3, 4, 3};
  EXPECT_EQ(H5Sselect_intersect_block(irreg3.get(), kDownMissS.data(),
                                      kDownMissE.data()),
            0);
  // Matches dim 0 [1..3], hits down span on dim 0 [3] y=[5..6] z=[5..6].
  constexpr std::array<hsize_t, 3> kDownHitS = {1, 5, 5};
  constexpr std::array<hsize_t, 3> kDownHitE = {3, 6, 6};
  EXPECT_GT(H5Sselect_intersect_block(irreg3.get(), kDownHitS.data(),
                                      kDownHitE.data()),
            0);
}

TEST(H5ShyperTest, BooleanClipMergeUpdateDiminfoAndCombineDisjoint) {
  constexpr std::array<hsize_t, 2> kDims2 = {24, 24};
  constexpr std::array<hsize_t, 2> kOnes2 = {1, 1};
  constexpr std::array<hsize_t, 3> kDims3 = {12, 12, 12};

  // 1. All 4 interval overlap cases in dim 0 WITH distinct overlapping down
  //    spans in dim 1/2 across OR, XOR, AND, NOTB, NOTA to cover
  //    H5S__hyper_clip_spans down recursion AND H5S__hyper_merge_spans_helper!
  struct Pair2D {
    std::array<hsize_t, 2> a_s, a_b;
    std::array<hsize_t, 2> a2_s, a2_b;
    std::array<hsize_t, 2> b_s, b_b;
    std::array<hsize_t, 2> b2_s, b2_b;
  };
  constexpr auto kPairs = std::to_array<Pair2D>({
      // Case 1: A left-overlaps B in dim 0, partial down overlap + trailing B.
      {{1, 1}, {5, 5}, {12, 2}, {2, 2}, {3, 3}, {6, 5}, {16, 4}, {2, 3}},
      // Case 2: A contains B in dim 0, B has trailing high + diff down.
      {{1, 2}, {8, 5}, {14, 1}, {2, 2}, {3, 4}, {3, 5}, {20, 3}, {2, 2}},
      // Case 3: A inside B in dim 0, B extends above & below + diff down.
      {{3, 4}, {3, 5}, {18, 2}, {1, 2}, {1, 2}, {8, 5}, {14, 5}, {1, 2}},
      // Case 4: A right-overlaps B in dim 0, + equal-down + trailing A.
      {{4, 3}, {6, 5}, {15, 3}, {2, 2}, {1, 1}, {5, 5}, {15, 3}, {2, 2}},
  });

  for (const Pair2D& p : kPairs) {
    ScopedSpace sa = MakeSimpleSpace(kDims2);
    ScopedSpace sb = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_hyperslab(sa.get(), H5S_SELECT_SET, p.a_s.data(),
                                  nullptr, kOnes2.data(), p.a_b.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(sa.get(), H5S_SELECT_OR, p.a2_s.data(),
                                  nullptr, kOnes2.data(), p.a2_b.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(sb.get(), H5S_SELECT_SET, p.b_s.data(),
                                  nullptr, kOnes2.data(), p.b_b.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(sb.get(), H5S_SELECT_OR, p.b2_s.data(),
                                  nullptr, kOnes2.data(), p.b2_b.data()),
              0);

    for (H5S_seloper_t op : {H5S_SELECT_OR, H5S_SELECT_XOR, H5S_SELECT_AND,
                             H5S_SELECT_NOTB, H5S_SELECT_NOTA}) {
      ScopedSpace comb(H5Scombine_select(sa.get(), op, sb.get()));
      ASSERT_TRUE(comb.valid());
      ScopedSpace mod(H5Scopy(sa.get()));
      ASSERT_GE(H5Smodify_select(mod.get(), op, sb.get()), 0);
      EXPECT_EQ(H5Sget_select_npoints(mod.get()),
                H5Sget_select_npoints(comb.get()));
    }
  }

  // Merge spans where a_not_b and b_not_a have overlapping dim-0 ranges with
  // matching AND mismatching down trees + tail leftovers (via XOR!).
  {
    ScopedSpace xa = MakeSimpleSpace(kDims2);
    ScopedSpace xb = MakeSimpleSpace(kDims2);
    constexpr std::array<hsize_t, 2> kA1S = {2, 1}, kA1B = {6, 4};
    constexpr std::array<hsize_t, 2> kA2S = {12, 1}, kA2B = {2, 6};
    constexpr std::array<hsize_t, 2> kB1S = {4, 3}, kB1B = {6, 5};
    constexpr std::array<hsize_t, 2> kB2S = {10, 4}, kB2B = {6, 2};
    ASSERT_GE(H5Sselect_hyperslab(xa.get(), H5S_SELECT_SET, kA1S.data(),
                                  nullptr, kOnes2.data(), kA1B.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(xa.get(), H5S_SELECT_OR, kA2S.data(), nullptr,
                                  kOnes2.data(), kA2B.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(xb.get(), H5S_SELECT_SET, kB1S.data(),
                                  nullptr, kOnes2.data(), kB1B.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(xb.get(), H5S_SELECT_OR, kB2S.data(), nullptr,
                                  kOnes2.data(), kB2B.data()),
              0);
    ScopedSpace x_res(H5Scombine_select(xa.get(), H5S_SELECT_XOR, xb.get()));
    ASSERT_TRUE(x_res.valid());
    EXPECT_GT(H5Sget_select_npoints(x_res.get()), 0);
    ScopedSpace x_mod(H5Scopy(xa.get()));
    ASSERT_GE(H5Smodify_select(x_mod.get(), H5S_SELECT_OR, xb.get()), 0);
    EXPECT_GT(H5Sget_select_npoints(x_mod.get()), 0);
  }

  // 2. Completely disjoint H5Scombine_select, H5Smodify_select, and
  //    H5Scombine_hyperslab (regular and irregular + zero-block + error).
  {
    ScopedSpace d_reg1 = MakeSimpleSpace(kDims2);
    constexpr std::array<hsize_t, 2> kD1S = {1, 1}, kD1B = {2, 2};
    ASSERT_GE(H5Sselect_hyperslab(d_reg1.get(), H5S_SELECT_SET, kD1S.data(),
                                  nullptr, kOnes2.data(), kD1B.data()),
              0);

    ScopedSpace d_reg2 = MakeSimpleSpace(kDims2);
    constexpr std::array<hsize_t, 2> kD2S = {8, 8}, kD2B = {2, 3};
    ASSERT_GE(H5Sselect_hyperslab(d_reg2.get(), H5S_SELECT_SET, kD2S.data(),
                                  nullptr, kOnes2.data(), kD2B.data()),
              0);

    ScopedSpace d_irreg = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_hyperslab(d_irreg.get(), H5S_SELECT_SET, kD1S.data(),
                                  nullptr, kOnes2.data(), kD1B.data()),
              0);
    constexpr std::array<hsize_t, 2> kD1Ex = {4, 5};
    ASSERT_GE(H5Sselect_hyperslab(d_irreg.get(), H5S_SELECT_OR, kD1Ex.data(),
                                  nullptr, kOnes2.data(), kOnes2.data()),
              0);

    ScopedSpace d_irreg2 = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_hyperslab(d_irreg2.get(), H5S_SELECT_SET, kD2S.data(),
                                  nullptr, kOnes2.data(), kD2B.data()),
              0);
    constexpr std::array<hsize_t, 2> kD2Ex = {12, 1};
    ASSERT_GE(H5Sselect_hyperslab(d_irreg2.get(), H5S_SELECT_OR, kD2Ex.data(),
                                  nullptr, kOnes2.data(), kOnes2.data()),
              0);

    for (H5S_seloper_t op : {H5S_SELECT_OR, H5S_SELECT_XOR, H5S_SELECT_AND,
                             H5S_SELECT_NOTB, H5S_SELECT_NOTA}) {
      // Disjoint H5Scombine_select (irreg op irreg -> fill_in_new_space new=true).
      ScopedSpace ci(H5Scombine_select(d_irreg.get(), op, d_irreg2.get()));
      ASSERT_TRUE(ci.valid());
      // Disjoint H5Smodify_select (irreg op irreg -> fill_in_new_space new=false).
      ScopedSpace mi(H5Scopy(d_irreg.get()));
      ASSERT_GE(H5Smodify_select(mi.get(), op, d_irreg2.get()), 0);
      EXPECT_EQ(H5Sget_select_npoints(mi.get()),
                H5Sget_select_npoints(ci.get()));

      // Disjoint H5Scombine_hyperslab on both regular and irregular old_space.
      ScopedSpace chr(H5Scombine_hyperslab(d_reg1.get(), op, kD2S.data(),
                                           nullptr, kOnes2.data(), kD2B.data()));
      ASSERT_TRUE(chr.valid());
      ScopedSpace chi(H5Scombine_hyperslab(d_irreg.get(), op, kD2S.data(),
                                           nullptr, kOnes2.data(), kD2B.data()));
      ASSERT_TRUE(chi.valid());

      // Zero-sized block in H5Scombine_hyperslab for every op.
      constexpr std::array<hsize_t, 2> kZeroB = {0, 1};
      ScopedSpace chz(H5Scombine_hyperslab(d_reg1.get(), op, kD2S.data(),
                                           nullptr, kOnes2.data(), kZeroB.data()));
      ASSERT_TRUE(chz.valid());
    }

    // Overlapping block error in H5Scombine_hyperslab.
    ScopedErrorSilence silence;
    constexpr std::array<hsize_t, 2> kBadC = {2, 1}, kBadB = {2, 1};
    EXPECT_EQ(H5Scombine_hyperslab(d_reg1.get(), H5S_SELECT_OR, kD2S.data(),
                                   nullptr, kBadC.data(), kBadB.data()),
              H5I_INVALID_HID);
  }

  // 3. Exercise H5S__hyper_update_diminfo fallback/success branches.
  {
    ScopedSpace ud = MakeSimpleSpace(kDims2);
    // Two non-identical dims -> sets diminfo_valid = NO.
    constexpr std::array<hsize_t, 2> kS1 = {0, 0}, kB1 = {2, 2};
    constexpr std::array<hsize_t, 2> kS2 = {1, 1}, kB2 = {2, 2};
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_SET, kS1.data(), nullptr,
                                  kOnes2.data(), kB1.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_OR, kS2.data(), nullptr,
                                  kOnes2.data(), kB2.data()),
              0);

    // Differing strides with count > 1.
    constexpr std::array<hsize_t, 2> kStA = {1, 3}, kCA = {1, 2};
    constexpr std::array<hsize_t, 2> kStB = {1, 4}, kCB = {1, 2};
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_SET, kS1.data(),
                                  kStA.data(), kCA.data(), kOnes2.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_OR, kS1.data(),
                                  kStB.data(), kCB.data(), kOnes2.data()),
              0);

    // tmp.count == 1 + new.count > 1 on matching phase.
    constexpr std::array<hsize_t, 2> kS3 = {0, 3};
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_SET, kS1.data(), nullptr,
                                  kOnes2.data(), kOnes2.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_OR, kS3.data(),
                                  kStA.data(), kCA.data(), kOnes2.data()),
              0);

    // XOR on overlapping count==1 with differing blocks -> impossible.
    constexpr std::array<hsize_t, 2> kSX = {0, 1}, kBX = {1, 3};
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_SET, kS1.data(), nullptr,
                                  kOnes2.data(), kB1.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_XOR, kSX.data(), nullptr,
                                  kOnes2.data(), kBX.data()),
              0);

    // Distinct count==1 with differing blocks -> diminfo NO.
    constexpr std::array<hsize_t, 2> kSDist = {0, 5};
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_SET, kS1.data(), nullptr,
                                  kOnes2.data(), kB1.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_OR, kSDist.data(),
                                  nullptr, kOnes2.data(), kBX.data()),
              0);

    // Strided count>1 with differing block, differing phase, or gap/non-border XOR.
    constexpr std::array<hsize_t, 2> kSt3 = {1, 4}, kC3 = {1, 2}, kB3 = {1, 2};
    constexpr std::array<hsize_t, 2> kSPhase = {0, 1};
    constexpr std::array<hsize_t, 2> SGap = {0, 12};
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_SET, kS1.data(),
                                  kSt3.data(), kC3.data(), kB3.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_OR, kS1.data(),
                                  kSt3.data(), kC3.data(), kOnes2.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_SET, kS1.data(),
                                  kSt3.data(), kC3.data(), kB3.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_OR, kSPhase.data(),
                                  kSt3.data(), kC3.data(), kB3.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_SET, kS1.data(),
                                  kSt3.data(), kC3.data(), kB3.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_OR, SGap.data(),
                                  kSt3.data(), kC3.data(), kB3.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_SET, kS1.data(),
                                  kSt3.data(), kC3.data(), kB3.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(ud.get(), H5S_SELECT_XOR, SGap.data(),
                                  kSt3.data(), kC3.data(), kB3.data()),
              0);
  }

  // 4. H5S_hyper_add_span_element sibling merging + shared down-tree reuse +
  //    high-bound growth across levels!
  {
    ScopedSpace sp = MakeSimpleSpace(kDims3);
    ASSERT_GE(H5Sselect_none(sp.get()), 0);
    H5S_t* sptr = UnwrapSpace(sp.get());
    // Under dim0 = 1:
    //   dim1 = 1: [4, 6]
    //   dim1 = 2: [4, 6] -> matches sibling [1..1], merges into [1..2]!
    //   dim1 = 4: [4, 6] -> disjoint from [1..2], shares down tree [4, 6]!
    //   dim1 = 5: [8]    -> grows high_bounds[2] across dim 0!
    // And add dim0 = 1, dim1 = 1..2 with [4,5] -> prev_down_tail_span merge!
    for (const auto& pt : std::to_array<std::array<hsize_t, 3>>({
             {1, 1, 4},
             {1, 1, 6},
             {1, 2, 4},
             {1, 2, 6},
             {1, 4, 4},
             {1, 4, 6},
             {1, 5, 8},
             {3, 1, 4},
             {3, 1, 6},
         })) {
      ASSERT_GE(H5S_hyper_add_span_element(sptr, 3, pt.data()), 0);
    }
    EXPECT_EQ(H5Sget_select_npoints(sp.get()), 9);
  }
}

TEST(H5ShyperTest, ProjectIntersectionMultiDimSkipAndCleanBitmap) {
  constexpr std::array<hsize_t, 3> kDims3 = {12, 12, 12};
  constexpr std::array<hsize_t, 3> kOnes3 = {1, 1, 1};

  // Multi-span 3D irregular dst_space:
  // dim0: [1..2] and [5..7] (5 rows)
  // dim1: [1..2] and [6..7] (4 cols per row -> wait: strided 2x2x4 below makes 160)
  // Let's make 144 points in dst_3d: dim0={1..2, 6..7} (4), dim1={1..3, 6..8} (6),
  // dim2={1..3, 7..9} (6) => 4 * 6 * 6 = 144 points!
  ScopedSpace dst3 = MakeSimpleSpace(kDims3);
  constexpr std::array<hsize_t, 3> kDS1 = {1, 1, 1}, kDSt = {5, 5, 6};
  constexpr std::array<hsize_t, 3> kDC = {2, 2, 2}, kDB = {2, 3, 3};
  ASSERT_GE(H5Sselect_hyperslab(dst3.get(), H5S_SELECT_SET, kDS1.data(),
                                kDSt.data(), kDC.data(), kDB.data()),
            0);
  // Make dst3 genuinely irregular by adding two non-grid points and adjusting
  // block so total is 144! Actually ORing [1,1,1] stride {5,5,6} count {2,2,2}
  // block {2,3,3} with replacing last block by {6,6,1}->{6,6,3}:
  // Easier: use XOR of two points or OR two overlapping pieces then match npoints!
  // Let dst3 have 2*2*2 = 8 3D blocks (count={2,2,2}) = 144 elements, which STILL
  // has 2 spans in depth 0, 2 spans in depth 1, 2 spans in depth 2 in its span tree!
  // To make span trees NOT share the same down pointer across dim 0, tweak one block:
  constexpr std::array<hsize_t, 3> kTweaks = {1, 6, 4};  // 3 elements
  constexpr std::array<hsize_t, 3> kTweakB = {1, 1, 3};
  ASSERT_GE(H5Sselect_hyperslab(dst3.get(), H5S_SELECT_OR, kTweaks.data(),
                                nullptr, kOnes3.data(), kTweakB.data()),
            0);
  // Now dst3 has 144 + 3 = 147 elements, with multi-span tree at ALL 3 depths!

  // src3: 3 * 7 * 7 = 147 elements (start={0,0,0}, block={3,7,7}).
  ScopedSpace src3 = MakeSimpleSpace(kDims3);
  constexpr std::array<hsize_t, 3> kZ3 = {0, 0, 0};
  constexpr std::array<hsize_t, 3> kSrcB = {3, 7, 7};
  ASSERT_GE(H5Sselect_hyperslab(src3.get(), H5S_SELECT_SET, kZ3.data(), nullptr,
                                kOnes3.data(), kSrcB.data()),
            0);

  // src_intersect creates alternating skips and runs that cross EVERY depth boundary
  // in dst3 (skipping into depth 2, matching across depth 1 & depth 0 span ends,
  // skipping again into another depth 2 span, and ending mid-span!).
  ScopedSpace isect3 = MakeSimpleSpace(kDims3);
  constexpr std::array<hsize_t, 3> kI1 = {0, 3, 2}, kIB1 = {1, 4, 4};  // 16 elems after skip 23
  constexpr std::array<hsize_t, 3> kI2 = {1, 1, 1}, kIB2 = {2, 5, 5};  // 50 elems after more skip
  ASSERT_GE(H5Sselect_hyperslab(isect3.get(), H5S_SELECT_SET, kI1.data(),
                                nullptr, kOnes3.data(), kIB1.data()),
            0);
  ASSERT_GE(H5Sselect_hyperslab(isect3.get(), H5S_SELECT_OR, kI2.data(),
                                nullptr, kOnes3.data(), kIB2.data()),
            0);

  for (bool share : {false, true}) {
    H5S_t* raw_proj = nullptr;
    ASSERT_GE(H5S_select_project_intersection(
                  UnwrapSpace(src3.get()), UnwrapSpace(dst3.get()),
                  UnwrapSpace(isect3.get()), &raw_proj, share),
              0);
    UniqueInternalSpace p(raw_proj);
    EXPECT_GT(H5S_get_select_npoints(p.get()), 0);
  }

  // Exercise ps_clean_bitmap Cases 2 & 3 and trailing ss_span skips:
  // src_clean: count = {3, 3, 2}, stride = {4, 4, 5}, block = {1, 1, 4} (3*3*8 = 72).
  // dst_clean: 72 elements.
  ScopedSpace src_cln = MakeSimpleSpace(kDims3);
  constexpr std::array<hsize_t, 3> kSCSt = {4, 4, 5}, kSCC = {3, 3, 2},
                                   kSCB = {1, 1, 4};
  ASSERT_GE(H5Sselect_hyperslab(src_cln.get(), H5S_SELECT_SET, kZ3.data(),
                                kSCSt.data(), kSCC.data(), kSCB.data()),
            0);
  ScopedSpace dst_cln = MakeSimpleSpace(kDims3);
  constexpr std::array<hsize_t, 3> kDstClnB = {1, 6, 12};
  ASSERT_GE(H5Sselect_hyperslab(dst_cln.get(), H5S_SELECT_SET, kZ3.data(),
                                nullptr, kOnes3.data(), kDstClnB.data()),
            0);

  // Intersect only middle-dim slice [4, 4..8, 1..3] so:
  // - depth 0 u=0 [0] completely missed (Case 2 / depth>0 skip add)
  // - depth 1 u=0 [4,0] hits skip at 0 then matches [1..3] (Case 3: skip + nelem, clean!)
  // - trailing ss_span left unvisited in depth 0/1/2 (lines 11555-11586).
  ScopedSpace isect_cln = MakeSimpleSpace(kDims3);
  constexpr std::array<hsize_t, 3> kICS = {4, 4, 1}, kICB = {1, 5, 3};
  ASSERT_GE(H5Sselect_hyperslab(isect_cln.get(), H5S_SELECT_SET, kICS.data(),
                                nullptr, kOnes3.data(), kICB.data()),
            0);
  ScopedSpace proj_cln(H5Sselect_project_intersection(
      src_cln.get(), dst_cln.get(), isect_cln.get()));
  ASSERT_TRUE(proj_cln.valid());
  EXPECT_EQ(H5Sget_select_npoints(proj_cln.get()), 6);
}

TEST(H5ShyperTest, FlattenedIteratorsSingleOptAndIrregEdgeCases) {
  // 1. 4D flattened with non-flattened slower dimensions (last_dim_flattened=false)
  //    covers lines 664-669, 837-846, 1219, 1905-1912, 2344-2351, 2617-2621!
  constexpr std::array<hsize_t, 4> kDims4 = {4, 5, 6, 8};
  {
    // Strided 4D where dim 3 is flattened into dim 2, leaving dims 0 & 1 unflattened.
    ScopedSpace s4_str = MakeSimpleSpace(kDims4);
    constexpr std::array<hsize_t, 4> kS = {1, 1, 0, 0};
    constexpr std::array<hsize_t, 4> kSt = {2, 3, 2, 1};
    constexpr std::array<hsize_t, 4> kC = {2, 2, 2, 1};
    constexpr std::array<hsize_t, 4> kB = {1, 2, 1, 8};
    ASSERT_GE(H5Sselect_hyperslab(s4_str.get(), H5S_SELECT_SET, kS.data(),
                                  kSt.data(), kC.data(), kB.data()),
              0);
    auto seqs = CollectSeqList(s4_str.get(), 4, 0, /*maxseq=*/3, /*maxelmts=*/7);
    size_t total = 0;
    for (const auto& [off, len] : seqs) {
      total += len;
    }
    EXPECT_EQ(total, 128u * 4u);

    // Also run non-opt 'gen' path via maxseq=1/maxelmts=1 and elmt_size=4
    // direct iter_coords & iter_next_block on flattened!
    ScopedSelIter it;
    ASSERT_GE(it.init(UnwrapSpace(s4_str.get()), /*elmt_size=*/4, 0), 0);
    std::array<hsize_t, 4> coords{};
    ASSERT_GE(H5S_select_iter_coords(it.get(), coords.data()), 0);
    EXPECT_THAT(coords, ElementsAre(1, 1, 0, 0));
    ASSERT_GE(H5S_select_iter_next(it.get(), 9), 0);
    ASSERT_GE(H5S_select_iter_coords(it.get(), coords.data()), 0);
    ASSERT_GE(it.get()->type->iter_next_block(it.get()), 0);
  }
  {
    // Single-block 4D where dim 3 is flattened (single path with iter_rank < rank).
    ScopedSpace s4_single = MakeSimpleSpace(kDims4);
    constexpr std::array<hsize_t, 4> kS = {1, 1, 0, 0};
    constexpr std::array<hsize_t, 4> kC = {1, 1, 1, 1};
    constexpr std::array<hsize_t, 4> kB = {2, 3, 1, 8};
    ASSERT_GE(H5Sselect_hyperslab(s4_single.get(), H5S_SELECT_SET, kS.data(),
                                  nullptr, kC.data(), kB.data()),
              0);
    auto seqs = CollectSeqList(s4_single.get(), 4, 0, /*maxseq=*/4, /*maxelmts=*/5);
    size_t total = 0;
    for (const auto& [off, len] : seqs) {
      total += len;
    }
    EXPECT_EQ(total, 48u * 4u);
  }

  // 2. Single-block 1D (0 == fast_dim) and 2D (0 == skip_dim).
  {
    constexpr std::array<hsize_t, 1> kDims1 = {16};
    ScopedSpace s1 = MakeSimpleSpace(kDims1);
    constexpr hsize_t kS = 3, kC = 1, kB = 7;
    ASSERT_GE(H5Sselect_hyperslab(s1.get(), H5S_SELECT_SET, &kS, nullptr, &kC,
                                  &kB),
              0);
    auto seqs = CollectSeqList(s1.get(), 4, 0, 4, 16);
    EXPECT_EQ(seqs.size(), 1u);
    EXPECT_EQ(seqs[0].second, 7u * 4u);
  }
  {
    constexpr std::array<hsize_t, 2> kDims2 = {8, 10};
    ScopedSpace s2 = MakeSimpleSpace(kDims2);
    constexpr std::array<hsize_t, 2> kS = {1, 2}, kOnes = {1, 1}, kB = {4, 5};
    ASSERT_GE(H5Sselect_hyperslab(s2.get(), H5S_SELECT_SET, kS.data(), nullptr,
                                  kOnes.data(), kB.data()),
              0);
    auto seqs = CollectSeqList(s2.get(), 4, 0, /*maxseq=*/10, /*maxelmts=*/20);
    size_t total = 0;
    for (const auto& [off, len] : seqs) {
      total += len;
    }
    EXPECT_EQ(total, 20u * 4u);
  }

  // 3. Opt strided partial leftover block (lines 2236-2251): io_left ends mid-block!
  {
    constexpr std::array<hsize_t, 2> kDims2 = {4, 20};
    ScopedSpace s_opt = MakeSimpleSpace(kDims2);
    constexpr std::array<hsize_t, 2> kZ = {0, 0}, kSt = {2, 5}, kC = {2, 3},
                                     kB = {1, 4};
    ASSERT_GE(H5Sselect_hyperslab(s_opt.get(), H5S_SELECT_SET, kZ.data(),
                                  kSt.data(), kC.data(), kB.data()),
              0);
    auto seqs = CollectSeqList(s_opt.get(), 4, 0, /*maxseq=*/10, /*maxelmts=*/10);
    size_t total = 0;
    for (const auto& [off, len] : seqs) {
      total += len;
    }
    EXPECT_EQ(total, 24u * 4u);
  }

  // 4. Irregular 'gen' end-of-span resume (1510-1518), coalescing (1641, 1664),
  //    and maxseq == 1 termination (1677).
  {
    constexpr std::array<hsize_t, 2> kDims2 = {4, 8};
    ScopedSpace s_gen = MakeSimpleSpace(kDims2);
    constexpr std::array<hsize_t, 2> kS1 = {0, 0}, kOnes = {1, 1}, kB1 = {2, 8};
    constexpr std::array<hsize_t, 2> kS2 = {2, 1}, kB2 = {1, 2};
    constexpr std::array<hsize_t, 2> kS3 = {2, 5}, kB3 = {1, 2};
    ASSERT_GE(H5Sselect_hyperslab(s_gen.get(), H5S_SELECT_SET, kS1.data(),
                                  nullptr, kOnes.data(), kB1.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(s_gen.get(), H5S_SELECT_OR, kS2.data(),
                                  nullptr, kOnes.data(), kB2.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(s_gen.get(), H5S_SELECT_OR, kS3.data(),
                                  nullptr, kOnes.data(), kB3.data()),
              0);
    // Row 0..1 are contiguous -> coalesces spans! Row 2 has two spans -> resume!
    auto seqs1 = CollectSeqList(s_gen.get(), 4, 0, /*maxseq=*/1, /*maxelmts=*/18);
    size_t total1 = 0;
    for (const auto& [off, len] : seqs1) {
      total1 += len;
    }
    EXPECT_EQ(total1, 20u * 4u);

    auto seqs2 = CollectSeqList(s_gen.get(), 4, 0, /*maxseq=*/4, /*maxelmts=*/2);
    size_t total2 = 0;
    for (const auto& [off, len] : seqs2) {
      total2 += len;
    }
    EXPECT_EQ(total2, 20u * 4u);
  }
}

TEST(H5ShyperTest, DeepMergeClipCombineAndShapeSameEdges) {
  ScopedErrorSilence silence;

  // 1. H5S__hyper_is_contiguous on non-contiguous regular + irregular span trees
  //    (lines 5205-5313).
  {
    constexpr std::array<hsize_t, 2> kDims2 = {8, 8};
    ScopedSpace reg_nc = MakeSimpleSpace(kDims2);
    constexpr std::array<hsize_t, 2> kS = {1, 1}, kOnes2 = {1, 1}, kB = {2, 4};
    ASSERT_GE(H5Sselect_hyperslab(reg_nc.get(), H5S_SELECT_SET, kS.data(),
                                  nullptr, kOnes2.data(), kB.data()),
              0);
    EXPECT_EQ(H5S_select_is_contiguous(UnwrapSpace(reg_nc.get())), 0);

    constexpr std::array<hsize_t, 3> kDims3 = {4, 4, 4};
    constexpr std::array<hsize_t, 3> kOnes3 = {1, 1, 1};
    // Irregular multi-span in dim 1 -> false at 5250 / 5290.
    ScopedSpace ir_multi = MakeSimpleSpace(kDims3);
    constexpr std::array<hsize_t, 3> kZ3 = {0, 0, 0}, kRowFull = {1, 1, 4};
    constexpr std::array<hsize_t, 3> kGapRow = {0, 2, 0};
    ASSERT_GE(H5Sselect_hyperslab(ir_multi.get(), H5S_SELECT_SET, kZ3.data(),
                                  nullptr, kOnes3.data(), kRowFull.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(ir_multi.get(), H5S_SELECT_OR, kGapRow.data(),
                                  nullptr, kOnes3.data(), kRowFull.data()),
              0);
    EXPECT_EQ(H5S_select_is_contiguous(UnwrapSpace(ir_multi.get())), 0);

    // Irregular single partial dim-1 via add_span_element -> false at 5257/5296.
    ScopedSpace ir_part = MakeSimpleSpace(kDims3);
    ASSERT_GE(H5Sselect_none(ir_part.get()), 0);
    for (hsize_t i = 0; i < 2; ++i) {
      for (hsize_t j = 0; j < 2; ++j) {
        for (hsize_t k = 0; k < 4; ++k) {
          const std::array<hsize_t, 3> pt = {i, j, k};
          ASSERT_GE(
              H5S_hyper_add_span_element(UnwrapSpace(ir_part.get()), 3, pt.data()),
              0);
        }
      }
    }
    EXPECT_EQ(H5S_select_is_contiguous(UnwrapSpace(ir_part.get())), 0);

    // Large-contiguous irregular (diminfo_valid != YES) -> true at 5263-5313!
    ScopedSpace ir_large = MakeSimpleSpace(kDims3);
    constexpr std::array<hsize_t, 3> kLargeB = {2, 4, 4}, kExtraS = {2, 0, 0};
    ASSERT_GE(H5Sselect_hyperslab(ir_large.get(), H5S_SELECT_SET, kZ3.data(),
                                  nullptr, kOnes3.data(), kLargeB.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(ir_large.get(), H5S_SELECT_OR, kExtraS.data(),
                                  nullptr, kOnes3.data(), kOnes3.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(ir_large.get(), H5S_SELECT_NOTB,
                                  kExtraS.data(), nullptr, kOnes3.data(),
                                  kOnes3.data()),
              0);
    EXPECT_GT(H5S_select_is_contiguous(UnwrapSpace(ir_large.get())), 0);

    // Small-contiguous irregular (diminfo_valid == NO) -> true at 5302-5313!
    ScopedSpace ir_small = MakeSimpleSpace(kDims3);
    ASSERT_GE(H5Sselect_none(ir_small.get()), 0);
    for (hsize_t k = 1; k < 4; ++k) {
      const std::array<hsize_t, 3> pt = {1, 2, k};
      ASSERT_GE(
          H5S_hyper_add_span_element(UnwrapSpace(ir_small.get()), 3, pt.data()),
          0);
    }
    EXPECT_GT(H5S_select_is_contiguous(UnwrapSpace(ir_small.get())), 0);
  }

  // 2. H5S_select_adjust_u & H5S_select_adjust_s on regular diminfo_valid==YES
  //    (lines 6568-6577, 7103-7115), plus blocklist wrap (4787) and unlim bounds
  //    with offset (4938).
  {
    constexpr std::array<hsize_t, 2> kDims2 = {12, 12};
    ScopedSpace reg = MakeSimpleSpace(kDims2);
    constexpr std::array<hsize_t, 2> kS = {4, 5}, kSt = {3, 3}, kC = {2, 2},
                                     kB = {2, 2};
    ASSERT_GE(H5Sselect_hyperslab(reg.get(), H5S_SELECT_SET, kS.data(),
                                  kSt.data(), kC.data(), kB.data()),
              0);
    // Request more blocks than 4 to trigger temp_dim == 0 -> done = true (4787).
    std::array<hsize_t, 20> blks{};
    EXPECT_GE(H5Sget_select_hyper_blocklist(reg.get(), 0, 5, blks.data()), 0);

    constexpr std::array<hsize_t, 2> kAdjU = {2, 3};
    ASSERT_GE(H5S_select_adjust_u(UnwrapSpace(reg.get()), kAdjU.data()), 0);
    constexpr std::array<hssize_t, 2> kAdjS = {-1, 1};
    ASSERT_GE(H5S_select_adjust_s(UnwrapSpace(reg.get()), kAdjS.data()), 0);
    std::array<hsize_t, 2> lo{}, hi{};
    ASSERT_GE(H5Sget_select_bounds(reg.get(), lo.data(), hi.data()), 0);
    EXPECT_THAT(lo, ElementsAre(3, 1));

    // Unlimited + non-zero offset -> H5Sget_select_bounds sets end = H5S_UNLIMITED (4938).
    constexpr std::array<hsize_t, 2> kUDims = {12, 8},
                                     kUMax = {H5S_UNLIMITED, 8};
    ScopedSpace u_off = MakeSimpleSpace(kUDims, kUMax);
    constexpr std::array<hsize_t, 2> kUS = {2, 1}, kUC = {H5S_UNLIMITED, 1},
                                     kUB = {1, 2};
    ASSERT_GE(H5Sselect_hyperslab(u_off.get(), H5S_SELECT_SET, kUS.data(),
                                  nullptr, kUC.data(), kUB.data()),
              0);
    constexpr std::array<hssize_t, 2> kOff = {1, 1};
    ASSERT_GE(H5Soffset_simple(u_off.get(), kOff.data()), 0);
    ASSERT_GE(H5Sget_select_bounds(u_off.get(), lo.data(), hi.data()), 0);
    EXPECT_EQ(hi[0], H5S_UNLIMITED);
  }

  // 3. H5S__hyper_spans_shape_same_helper mismatches (5480, 5482, 5503, 5508,
  //    5515, 5523) + intersect_block exhaustion (6319) + leaf gap (6022-6024).
  {
    constexpr std::array<hsize_t, 2> kDims2 = {12, 12};
    constexpr std::array<hsize_t, 2> kOnes2 = {1, 1};
    auto add_rect = [&](hid_t s, hsize_t r0, hsize_t r1, hsize_t c0,
                        hsize_t c1) {
      const std::array<hsize_t, 2> st = {r0, c0};
      const std::array<hsize_t, 2> bk = {r1 - r0 + 1, c1 - c0 + 1};
      const H5S_seloper_t op =
          (H5Sget_select_type(s) == H5S_SEL_ALL ||
           H5Sget_select_npoints(s) == 0)
              ? H5S_SELECT_SET
              : H5S_SELECT_OR;
      EXPECT_GE(
          H5Sselect_hyperslab(s, op, st.data(), nullptr, kOnes2.data(), bk.data()),
          0);
    };

    // Case 5480 & 5523: outer bounds match (rows 0..1, cols 0..4 vs rows 1..2,
    // cols 0..4? Make col head shifted +1 so offset[1]=+1, row 1 cols [0..4]
    // fails low_bounds[0]+1 != 0 at 5480).
    ScopedSpace s5480_a = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_none(s5480_a.get()), 0);
    add_rect(s5480_a.get(), 0, 0, 2, 3);
    add_rect(s5480_a.get(), 1, 1, 0, 4);
    ScopedSpace s5480_b = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_none(s5480_b.get()), 0);
    add_rect(s5480_b.get(), 1, 1, 3, 4);
    add_rect(s5480_b.get(), 2, 2, 0, 4);
    EXPECT_EQ(H5Sselect_shape_same(s5480_a.get(), s5480_b.get()), 0);

    // Case 5482: same outer 3-row box (width 3 x 6), row0 gives offset=(1,1),
    // row1 low matches (1+1==2) but high disagrees (3+1 != 5)!
    ScopedSpace s5482_a = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_none(s5482_a.get()), 0);
    add_rect(s5482_a.get(), 0, 0, 2, 3);
    add_rect(s5482_a.get(), 1, 1, 1, 3);
    add_rect(s5482_a.get(), 2, 2, 0, 5);
    ScopedSpace s5482_b = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_none(s5482_b.get()), 0);
    add_rect(s5482_b.get(), 1, 1, 3, 4);
    add_rect(s5482_b.get(), 2, 2, 2, 5);
    add_rect(s5482_b.get(), 3, 3, 1, 6);
    EXPECT_EQ(H5Sselect_shape_same(s5482_a.get(), s5482_b.get()), 0);

    // Case 5503: row1 down has same low/high bounds [1..4] vs [2..5] (offset+1),
    // A has 2 spans ([1..1],[4..4]), B has 1 span ([2..5]) -> span2 == NULL!
    ScopedSpace s5503_a = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_none(s5503_a.get()), 0);
    add_rect(s5503_a.get(), 0, 0, 2, 3);
    add_rect(s5503_a.get(), 1, 1, 1, 1);
    add_rect(s5503_a.get(), 1, 1, 4, 4);
    add_rect(s5503_a.get(), 2, 2, 0, 5);
    ScopedSpace s5503_b = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_none(s5503_b.get()), 0);
    add_rect(s5503_b.get(), 1, 1, 3, 4);
    add_rect(s5503_b.get(), 2, 2, 2, 5);
    add_rect(s5503_b.get(), 3, 3, 1, 6);
    EXPECT_EQ(H5Sselect_shape_same(s5503_a.get(), s5503_b.get()), 0);

    // Case 5508: both have 2 spans in row1 with same low/high [1..6] vs [2..7],
    // but first span high differs ([1..2] vs [2..4]).
    ScopedSpace s5508_a = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_none(s5508_a.get()), 0);
    add_rect(s5508_a.get(), 0, 0, 2, 3);
    add_rect(s5508_a.get(), 1, 1, 1, 2);
    add_rect(s5508_a.get(), 1, 1, 6, 6);
    add_rect(s5508_a.get(), 2, 2, 0, 7);
    ScopedSpace s5508_b = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_none(s5508_b.get()), 0);
    add_rect(s5508_b.get(), 1, 1, 3, 4);
    add_rect(s5508_b.get(), 2, 2, 2, 4);
    add_rect(s5508_b.get(), 2, 2, 7, 7);
    add_rect(s5508_b.get(), 3, 3, 1, 8);
    EXPECT_EQ(H5Sselect_shape_same(s5508_a.get(), s5508_b.get()), 0);

    // Case 5515: rest_zeros[0] == true (offset = (1, 0)), row1 differs -> cmp_spans false!
    ScopedSpace s5515_a = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_none(s5515_a.get()), 0);
    add_rect(s5515_a.get(), 0, 0, 0, 3);
    add_rect(s5515_a.get(), 1, 1, 0, 1);
    ScopedSpace s5515_b = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_none(s5515_b.get()), 0);
    add_rect(s5515_b.get(), 1, 1, 0, 3);
    add_rect(s5515_b.get(), 2, 2, 0, 2);
    EXPECT_EQ(H5Sselect_shape_same(s5515_a.get(), s5515_b.get()), 0);

    // Intersect block exhaustion reaching op_gen write (6319).
    constexpr std::array<hsize_t, 2> kMissLo = {0, 4}, kMissHi = {1, 5};
    EXPECT_EQ(H5Sselect_intersect_block(s5515_a.get(), kMissLo.data(),
                                        kMissHi.data()),
              0);

    // Add_span_element leaf growth gap (6022-6024).
    ScopedSpace leaf_gap = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_none(leaf_gap.get()), 0);
    for (const std::array<hsize_t, 2> pt :
         {std::array<hsize_t, 2>{0, 0}, std::array<hsize_t, 2>{0, 2},
          std::array<hsize_t, 2>{0, 3}}) {
      ASSERT_GE(
          H5S_hyper_add_span_element(UnwrapSpace(leaf_gap.get()), 2, pt.data()),
          0);
    }
    EXPECT_EQ(H5Sget_select_npoints(leaf_gap.get()), 3);
  }

  // 4. H5S__hyper_clip_spans AND cases (7656, 7764) & 3D multi-span
  //    H5S__hyper_merge_spans_helper overlap permutations (8125-8365).
  {
    constexpr std::array<hsize_t, 1> kDims1 = {20};
    ScopedSpace a1 = MakeSimpleSpace(kDims1);
    ScopedSpace a2 = MakeSimpleSpace(kDims1);
    const hsize_t a1_s = 2, a1_b = 10, b1_s = 5, b1_b = 4, c1_s = 14, c1_b = 1;
    const hsize_t one = 1;
    // Make both irregular multi-span, then AND: a contains b (7656) & b inside a (7764).
    ASSERT_GE(H5Sselect_hyperslab(a1.get(), H5S_SELECT_SET, &a1_s, nullptr, &one, &a1_b), 0);
    ASSERT_GE(H5Sselect_hyperslab(a1.get(), H5S_SELECT_OR, &c1_s, nullptr, &one, &c1_b), 0);
    ASSERT_GE(H5Sselect_hyperslab(a2.get(), H5S_SELECT_SET, &b1_s, nullptr, &one, &b1_b), 0);
    const hsize_t c2_s = 16, c2_b = 1;
    ASSERT_GE(H5Sselect_hyperslab(a2.get(), H5S_SELECT_OR, &c2_s, nullptr, &one, &c2_b), 0);
    ScopedSpace ab(H5Scombine_select(a1.get(), H5S_SELECT_AND, a2.get()));
    ScopedSpace ba(H5Scombine_select(a2.get(), H5S_SELECT_AND, a1.get()));
    EXPECT_EQ(H5Sget_select_npoints(ab.get()), 4);
    EXPECT_EQ(H5Sget_select_npoints(ba.get()), 4);

    // 3D OR / XOR forcing merge_spans_helper across all 4 overlap cases with
    // both equal and distinct 'down' trees and trailing spans on A & B.
    constexpr std::array<hsize_t, 3> kDims3 = {20, 12, 12};
    constexpr std::array<hsize_t, 3> kOnes3 = {1, 1, 1};
    auto make_slab = [&](hsize_t z0, hsize_t z1, hsize_t y0, hsize_t y1,
                         hsize_t x0, hsize_t x1) {
      ScopedSpace s = MakeSimpleSpace(kDims3);
      const std::array<hsize_t, 3> st = {z0, y0, x0};
      const std::array<hsize_t, 3> bk = {z1 - z0 + 1, y1 - y0 + 1, x1 - x0 + 1};
      EXPECT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_SET, st.data(), nullptr,
                                    kOnes3.data(), bk.data()),
                0);
      return s;
    };
    auto or_slab = [&](hid_t s, hsize_t z0, hsize_t z1, hsize_t y0, hsize_t y1,
                       hsize_t x0, hsize_t x1) {
      const std::array<hsize_t, 3> st = {z0, y0, x0};
      const std::array<hsize_t, 3> bk = {z1 - z0 + 1, y1 - y0 + 1, x1 - x0 + 1};
      EXPECT_GE(H5Sselect_hyperslab(s, H5S_SELECT_OR, st.data(), nullptr,
                                    kOnes3.data(), bk.data()),
                0);
    };
    ScopedSpace m3_a = make_slab(2, 5, 2, 6, 1, 4);
    or_slab(m3_a.get(), 7, 11, 2, 6, 1, 4);
    or_slab(m3_a.get(), 13, 16, 2, 6, 1, 4);
    or_slab(m3_a.get(), 18, 19, 0, 2, 0, 2);

    ScopedSpace m3_b = make_slab(0, 3, 1, 3, 5, 7);   // overlaps lower/upper
    or_slab(m3_b.get(), 4, 8, 2, 6, 1, 4);            // equal down on [4..5],[7..8]
    or_slab(m3_b.get(), 9, 10, 4, 8, 5, 8);           // inside [7..11], diff down
    or_slab(m3_b.get(), 12, 17, 1, 4, 6, 9);          // encloses [13..16]!

    ScopedSpace or_ab(H5Scombine_select(m3_a.get(), H5S_SELECT_OR, m3_b.get()));
    ScopedSpace or_ba(H5Scombine_select(m3_b.get(), H5S_SELECT_OR, m3_a.get()));
    ScopedSpace xor_ab(H5Scombine_select(m3_a.get(), H5S_SELECT_XOR, m3_b.get()));
    EXPECT_EQ(H5Sget_select_npoints(or_ab.get()),
              H5Sget_select_npoints(or_ba.get()));
    EXPECT_GT(H5Sget_select_npoints(xor_ab.get()), 0);
  }

  // 5. H5S__hyper_regular_and_single_block (9858, 9910), H5S_select_hyperslab
  //    (10040-10054, 10101-10108, 10140-10257), H5S_combine_hyperslab (10419-10550).
  {
    constexpr std::array<hsize_t, 2> kDims2 = {20, 20};
    constexpr std::array<hsize_t, 2> kOnes2 = {1, 1};
    // 9858 & 9910: strided dim0 + non-strided dim1 AND single block overlapping
    // start/end!
    ScopedSpace rs = MakeSimpleSpace(kDims2);
    constexpr std::array<hsize_t, 2> kRS = {2, 2}, kRSt = {4, 1}, kRC = {3, 1},
                                     kRB = {2, 6};
    ASSERT_GE(H5Sselect_hyperslab(rs.get(), H5S_SELECT_SET, kRS.data(),
                                  kRSt.data(), kRC.data(), kRB.data()),
              0);
    constexpr std::array<hsize_t, 2> kAndS = {0, 3}, kAndB = {3, 2};
    ASSERT_GE(H5Sselect_hyperslab(rs.get(), H5S_SELECT_AND, kAndS.data(),
                                  nullptr, kOnes2.data(), kAndB.data()),
              0);
    EXPECT_EQ(H5Sget_select_npoints(rs.get()), 2);

    // Negative / invalid op paths on H5S_select_hyperslab & H5S_combine_hyperslab.
    ScopedSpace s_err = MakeSimpleSpace(kDims2);
    constexpr std::array<hsize_t, 2> kZero2 = {0, 0}, kZeroC = {0, 1};
    EXPECT_LT(H5S_select_hyperslab(UnwrapSpace(s_err.get()), H5S_SELECT_APPEND,
                                   kZero2.data(), nullptr, kZeroC.data(), nullptr),
              0);
    ASSERT_GE(H5Sselect_none(s_err.get()), 0);
    EXPECT_LT(H5S_select_hyperslab(UnwrapSpace(s_err.get()), H5S_SELECT_APPEND,
                                   kZero2.data(), nullptr, kOnes2.data(), nullptr),
              0);
    ASSERT_GE(H5Sselect_all(s_err.get()), 0);
    EXPECT_LT(H5S_select_hyperslab(UnwrapSpace(s_err.get()), H5S_SELECT_APPEND,
                                   kZero2.data(), nullptr, kOnes2.data(), nullptr),
              0);
    // H5S_SEL_ALL + NOTB / XOR / NOTA.
    ScopedSpace s_all1 = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_all(s_all1.get()), 0);
    ASSERT_GE(H5Sselect_hyperslab(s_all1.get(), H5S_SELECT_NOTB, kRS.data(),
                                  nullptr, kOnes2.data(), kAndB.data()),
              0);
    EXPECT_EQ(H5Sget_select_npoints(s_all1.get()), 400 - 6);
    ASSERT_GE(H5Sselect_all(s_all1.get()), 0);
    ASSERT_GE(H5Sselect_hyperslab(s_all1.get(), H5S_SELECT_NOTA, kRS.data(),
                                  nullptr, kOnes2.data(), kAndB.data()),
              0);
    EXPECT_EQ(H5Sget_select_type(s_all1.get()), H5S_SEL_NONE);

    // Point selection + hyperslab OR -> error (10200-10209).
    ScopedSpace s_pts = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_elements(s_pts.get(), H5S_SELECT_SET, 1, kZero2.data()), 0);
    EXPECT_LT(H5Sselect_hyperslab(s_pts.get(), H5S_SELECT_OR, kZero2.data(),
                                  nullptr, kOnes2.data(), kOnes2.data()),
              0);

    // Unlimited negative & finite-vs-unlim clipping (10048-10054, 10100-10108,
    // 10232-10257).
    constexpr std::array<hsize_t, 2> kUMax2 = {H5S_UNLIMITED, H5S_UNLIMITED};
    ScopedSpace s_u2 = MakeSimpleSpace(kDims2, kUMax2);
    EXPECT_LT(H5Sselect_hyperslab(s_u2.get(), H5S_SELECT_SET, kZero2.data(),
                                  nullptr, kUMax2.data(), nullptr),
              0);
    constexpr std::array<hsize_t, 2> kCntB1 = {H5S_UNLIMITED, 1};
    EXPECT_LT(H5Sselect_hyperslab(s_u2.get(), H5S_SELECT_SET, kZero2.data(),
                                  nullptr, kCntB1.data(), kCntB1.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(s_u2.get(), H5S_SELECT_SET, kZero2.data(),
                                  nullptr, kCntB1.data(), nullptr),
              0);
    EXPECT_LT(H5Sselect_hyperslab(s_u2.get(), H5S_SELECT_OR, kZero2.data(),
                                  nullptr, kOnes2.data(), nullptr),
              0);
    EXPECT_LT(H5Sselect_hyperslab(s_u2.get(), H5S_SELECT_AND, kZero2.data(),
                                  nullptr, kCntB1.data(), nullptr),
              0);
    // Finite regular combined with unlimited AND -> clips unlimited & succeeds!
    ScopedSpace fin = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_hyperslab(fin.get(), H5S_SELECT_SET, kRS.data(),
                                  nullptr, kOnes2.data(), kRB.data()),
              0);
    constexpr std::array<hsize_t, 2> kUStart = {0, 2};
    EXPECT_GE(H5Sselect_hyperslab(fin.get(), H5S_SELECT_AND, kUStart.data(),
                                  nullptr, kCntB1.data(), nullptr),
              0);
    EXPECT_EQ(H5Sget_select_npoints(fin.get()), 2);
    ScopedSpace fin2 = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_hyperslab(fin2.get(), H5S_SELECT_SET, kRS.data(),
                                  nullptr, kOnes2.data(), kRB.data()),
              0);
    EXPECT_LT(H5Sselect_hyperslab(fin2.get(), H5S_SELECT_OR, kZero2.data(),
                                  nullptr, kCntB1.data(), nullptr),
              0);
    constexpr std::array<hsize_t, 2> kUBig = {1, 4};
    EXPECT_GE(H5Sselect_hyperslab(fin2.get(), H5S_SELECT_NOTB, kZero2.data(),
                                  nullptr, kCntB1.data(), kUBig.data()),
              0);

    // H5S_combine_hyperslab zero-sized (10424-10448), overlap error (10419),
    // and disjoint span_lst != NULL / == NULL (10485-10549).
    ScopedSpace base_reg = MakeSimpleSpace(kDims2);
    constexpr std::array<hsize_t, 2> kBS = {1, 1}, kBB = {2, 3};
    ASSERT_GE(H5Sselect_hyperslab(base_reg.get(), H5S_SELECT_SET, kBS.data(),
                                  nullptr, kOnes2.data(), kBB.data()),
              0);
    H5S_t* out = nullptr;
    constexpr std::array<hsize_t, 2> kBadSt = {1, 1}, kBadC = {2, 2}, kBadB = {2, 2};
    EXPECT_LT(H5S_combine_hyperslab(UnwrapSpace(base_reg.get()), H5S_SELECT_OR,
                                    kBS.data(), kBadSt.data(), kBadC.data(),
                                    kBadB.data(), &out),
              0);
    if (out) {
      H5S_close(out);
      out = nullptr;
    }
    for (H5S_seloper_t zop : {H5S_SELECT_AND, H5S_SELECT_OR, H5S_SELECT_APPEND}) {
      H5S_t* zout = nullptr;
      herr_t rc = H5S_combine_hyperslab(UnwrapSpace(base_reg.get()), zop,
                                        kZero2.data(), nullptr, kZeroC.data(),
                                        nullptr, &zout);
      if (zop == H5S_SELECT_APPEND) {
        EXPECT_LT(rc, 0);
      } else {
        EXPECT_GE(rc, 0);
      }
      if (zout) {
        H5S_close(zout);
      }
    }
    // Disjoint combine on regular (span_lst == NULL) with NOTB, NOTA, XOR, AND, APPEND.
    constexpr std::array<hsize_t, 2> kFarS = {10, 10};
    for (H5S_seloper_t rop :
         {H5S_SELECT_NOTB, H5S_SELECT_NOTA, H5S_SELECT_XOR, H5S_SELECT_AND}) {
      H5S_t* rout = nullptr;
      ASSERT_GE(H5S_combine_hyperslab(UnwrapSpace(base_reg.get()), rop,
                                      kFarS.data(), nullptr, kOnes2.data(),
                                      kBB.data(), &rout),
                0);
      if (rout) {
        H5S_close(rout);
      }
    }
    H5S_t* dout = nullptr;
    EXPECT_LT(H5S_combine_hyperslab(UnwrapSpace(base_reg.get()), H5S_SELECT_APPEND,
                                    kFarS.data(), nullptr, kOnes2.data(),
                                    kBB.data(), &dout),
              0);
    if (dout) {
      H5S_close(dout);
      dout = nullptr;
    }
    // Disjoint combine when old_space HAS span_lst != NULL.
    ScopedSpace base_spans = MakeSimpleSpace(kDims2);
    ASSERT_GE(H5Sselect_hyperslab(base_spans.get(), H5S_SELECT_SET, kBS.data(),
                                  nullptr, kOnes2.data(), kBB.data()),
              0);
    constexpr std::array<hsize_t, 2> kExtraS = {1, 6};
    ASSERT_GE(H5Sselect_hyperslab(base_spans.get(), H5S_SELECT_OR, kExtraS.data(),
                                  nullptr, kOnes2.data(), kOnes2.data()),
              0);
    for (H5S_seloper_t dop : {H5S_SELECT_OR, H5S_SELECT_AND, H5S_SELECT_NOTB}) {
      H5S_t* sout = nullptr;
      ASSERT_GE(H5S_combine_hyperslab(UnwrapSpace(base_spans.get()), dop,
                                      kFarS.data(), nullptr, kOnes2.data(),
                                      kBB.data(), &sout),
                0);
      if (sout) {
        H5S_close(sout);
      }
    }
  }
}

TEST(H5ShyperTest, SeqListDuffResumeCoalesceAndProjectIntersectDeep) {
  // 1. Flattened full-rank iterator -> iter_coords u < 0 (line 822).
  {
    constexpr std::array<hsize_t, 2> kDims2 = {4, 8};
    ScopedSpace sf = MakeSimpleSpace(kDims2);
    constexpr std::array<hsize_t, 2> kZ = {0, 0}, kO = {1, 1}, kB = {2, 8};
    ASSERT_GE(H5Sselect_hyperslab(sf.get(), H5S_SELECT_SET, kZ.data(), nullptr,
                                  kO.data(), kB.data()),
              0);
    ScopedSelIter it;
    ASSERT_GE(it.init(UnwrapSpace(sf.get()), /*elmt_size=*/4, 0), 0);
    std::array<hsize_t, 2> coords{};
    ASSERT_GE(H5S_select_iter_coords(it.get(), coords.data()), 0);
    EXPECT_THAT(coords, ElementsAre(0, 0));
  }

  // 2. Duff's device in get_seq_list_opt (lines 2121-2138) for count % 8 == 0..7.
  {
    constexpr std::array<hsize_t, 2> kDims2 = {2, 34};
    for (hsize_t c = 1; c <= 16; ++c) {
      ScopedSpace sd = MakeSimpleSpace(kDims2);
      const std::array<hsize_t, 2> st = {0, 0}, str = {1, 2}, cnt = {2, c},
                                   blk = {1, 1};
      ASSERT_GE(H5Sselect_hyperslab(sd.get(), H5S_SELECT_SET, st.data(),
                                    str.data(), cnt.data(), blk.data()),
                0);
      auto seqs = CollectSeqList(sd.get(), 4, 0, /*maxseq=*/64, /*maxelmts=*/64);
      EXPECT_EQ(seqs.size(), static_cast<size_t>(2 * c));
    }
  }

  // 3. Irregular 'gen' mid-loop next fast-dim span (1510-1518) and across-row
  //    span coalescing (1641, 1664).
  {
    constexpr std::array<hsize_t, 2> kDims2 = {4, 8};
    ScopedSpace sg = MakeSimpleSpace(kDims2);
    constexpr std::array<hsize_t, 2> kS1 = {0, 0}, kO = {1, 1}, kB1 = {1, 2};
    constexpr std::array<hsize_t, 2> kS2 = {0, 4}, kB2 = {1, 2};
    ASSERT_GE(H5Sselect_hyperslab(sg.get(), H5S_SELECT_SET, kS1.data(), nullptr,
                                  kO.data(), kB1.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(sg.get(), H5S_SELECT_OR, kS2.data(), nullptr,
                                  kO.data(), kB2.data()),
              0);
    // maxelmts = 1 stops mid-[0..1], next call hits end of [0..1] -> curr_span->next [4..5] (1510-1518)!
    auto seqs = CollectSeqList(sg.get(), 4, 0, /*maxseq=*/8, /*maxelmts=*/1);
    EXPECT_EQ(seqs.size(), 4u);

    // Adjacent across row boundary: row 0 [0..7], row 1 [0..3], row 2 [2..4].
    ScopedSpace sc = MakeSimpleSpace(kDims2);
    constexpr std::array<hsize_t, 2> kR0 = {0, 0}, kRB0 = {1, 8};
    constexpr std::array<hsize_t, 2> kR1 = {1, 0}, kRB1 = {1, 4};
    constexpr std::array<hsize_t, 2> kR2 = {2, 2}, kRB2 = {1, 3};
    ASSERT_GE(H5Sselect_hyperslab(sc.get(), H5S_SELECT_SET, kR0.data(), nullptr,
                                  kO.data(), kRB0.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(sc.get(), H5S_SELECT_OR, kR1.data(), nullptr,
                                  kO.data(), kRB1.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(sc.get(), H5S_SELECT_OR, kR2.data(), nullptr,
                                  kO.data(), kRB2.data()),
              0);
    // maxelmts=10: trims row 1 onto row 0 seq (line 1641).
    auto s1 = CollectSeqList(sc.get(), 4, 0, /*maxseq=*/8, /*maxelmts=*/10);
    EXPECT_EQ(s1.size(), 3u);
    // maxelmts=20: absorbs entire row 1 onto row 0 seq (line 1664).
    auto s2 = CollectSeqList(sc.get(), 4, 0, /*maxseq=*/8, /*maxelmts=*/20);
    EXPECT_EQ(s2.size(), 2u);
  }

  // 4. H5S__hyper_project_intersection deep 3D build_proj (11130-11345) &
  //    ps_clean_bitmap Cases 2 & 3 + trailing skip (11555-11624).
  {
    constexpr std::array<hsize_t, 3> kDstDims = {12, 6, 4};
    constexpr std::array<hsize_t, 3> kO3 = {1, 1, 1};
    ScopedSpace dst = MakeSimpleSpace(kDstDims);
    constexpr std::array<hsize_t, 3> kD1S = {0, 0, 0}, kD1B = {4, 3, 2};
    constexpr std::array<hsize_t, 3> kD2S = {5, 0, 0}, kD2B = {4, 3, 2};
    constexpr std::array<hsize_t, 3> kD3S = {10, 0, 0}, kD3B = {2, 1, 2};
    constexpr std::array<hsize_t, 3> kD4S = {10, 2, 0};
    constexpr std::array<hsize_t, 3> kD5S = {10, 4, 0};
    ASSERT_GE(H5Sselect_hyperslab(dst.get(), H5S_SELECT_SET, kD1S.data(),
                                  nullptr, kO3.data(), kD1B.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(dst.get(), H5S_SELECT_OR, kD2S.data(),
                                  nullptr, kO3.data(), kD2B.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(dst.get(), H5S_SELECT_OR, kD3S.data(),
                                  nullptr, kO3.data(), kD3B.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(dst.get(), H5S_SELECT_OR, kD4S.data(),
                                  nullptr, kO3.data(), kD3B.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(dst.get(), H5S_SELECT_OR, kD5S.data(),
                                  nullptr, kO3.data(), kD3B.data()),
              0);
    ASSERT_EQ(H5Sget_select_npoints(dst.get()), 60);

    constexpr hsize_t kSrcLen = 60;
    ScopedSpace src(H5Screate_simple(1, &kSrcLen, nullptr));
    ASSERT_GE(H5Sselect_all(src.get()), 0);
    ScopedSpace sis(H5Screate_simple(1, &kSrcLen, nullptr));
    const hsize_t i1_s = 0, i1_b = 16, i2_s = 20, i2_b = 2, i3_s = 22,
                  i3_b = 31, one = 1;
    ASSERT_GE(H5Sselect_hyperslab(sis.get(), H5S_SELECT_SET, &i1_s, nullptr, &one, &i1_b), 0);
    ASSERT_GE(H5Sselect_hyperslab(sis.get(), H5S_SELECT_OR, &i2_s, nullptr, &one, &i2_b), 0);
    ASSERT_GE(H5Sselect_hyperslab(sis.get(), H5S_SELECT_OR, &i3_s, nullptr, &one, &i3_b), 0);

    for (bool share : {true, false}) {
      H5S_t* proj_ptr = nullptr;
      ASSERT_GE(H5S_select_project_intersection(
                    UnwrapSpace(src.get()), UnwrapSpace(dst.get()),
                    UnwrapSpace(sis.get()), &proj_ptr, share),
                0);
      ASSERT_NE(proj_ptr, nullptr);
      ScopedSpace proj(H5I_register(H5I_DATASPACE, proj_ptr, true));
      EXPECT_EQ(H5Sget_select_npoints(proj.get()), 49);
    }

    // ps_clean_bitmap Cases 2 & 3 (11599-11624) + trailing ss_span (11555-11585).
    constexpr std::array<hsize_t, 2> kS2Dims = {4, 6};
    constexpr std::array<hsize_t, 2> kO2 = {1, 1};
    ScopedSpace s2 = MakeSimpleSpace(kS2Dims);
    constexpr std::array<hsize_t, 2> kZ2 = {0, 0}, kS2B = {4, 4};
    ASSERT_GE(H5Sselect_hyperslab(s2.get(), H5S_SELECT_SET, kZ2.data(), nullptr,
                                  kO2.data(), kS2B.data()),
              0);
    constexpr hsize_t kD16 = 16;
    ScopedSpace d16(H5Screate_simple(1, &kD16, nullptr));
    ASSERT_GE(H5Sselect_all(d16.get()), 0);

    // Case 2: skip only (sis at [5..5], disjoint past [0..3]).
    ScopedSpace sis_c2 = MakeSimpleSpace(kS2Dims);
    constexpr std::array<hsize_t, 2> kC2S = {0, 5}, kC2B = {4, 1};
    ASSERT_GE(H5Sselect_hyperslab(sis_c2.get(), H5S_SELECT_SET, kC2S.data(),
                                  nullptr, kO2.data(), kC2B.data()),
              0);
    H5S_t* p2 = nullptr;
    ASSERT_GE(H5S_select_project_intersection(
                  UnwrapSpace(s2.get()), UnwrapSpace(d16.get()),
                  UnwrapSpace(sis_c2.get()), &p2, false),
              0);
    ScopedSpace p2_id(H5I_register(H5I_DATASPACE, p2, true));
    EXPECT_EQ(H5Sget_select_npoints(p2_id.get()), 0);

    // Case 3: skip [0..1] + match [2..3] at end of row -> skip + nelem!
    ScopedSpace sis_c3 = MakeSimpleSpace(kS2Dims);
    constexpr std::array<hsize_t, 2> kC3S = {0, 2}, kC3B = {4, 2};
    ASSERT_GE(H5Sselect_hyperslab(sis_c3.get(), H5S_SELECT_SET, kC3S.data(),
                                  nullptr, kO2.data(), kC3B.data()),
              0);
    H5S_t* p3 = nullptr;
    ASSERT_GE(H5S_select_project_intersection(
                  UnwrapSpace(s2.get()), UnwrapSpace(d16.get()),
                  UnwrapSpace(sis_c3.get()), &p3, true),
              0);
    ScopedSpace p3_id(H5I_register(H5I_DATASPACE, p3, true));
    EXPECT_EQ(H5Sget_select_npoints(p3_id.get()), 8);

    // Trailing ss_span with down != NULL and leaf ss_span + multi sis advances.
    constexpr std::array<hsize_t, 3> kT3Dims = {4, 6, 6};
    ScopedSpace t_src = MakeSimpleSpace(kT3Dims);
    constexpr std::array<hsize_t, 3> kTZ = {0, 0, 0}, kTStr = {1, 3, 3},
                                     kTCnt = {2, 2, 2}, kTBlk = {1, 1, 2};
    ASSERT_GE(H5Sselect_hyperslab(t_src.get(), H5S_SELECT_SET, kTZ.data(),
                                  kTStr.data(), kTCnt.data(), kTBlk.data()),
              0);
    ScopedSpace t_dst(H5Screate_simple(1, &kD16, nullptr));
    ASSERT_GE(H5Sselect_all(t_dst.get()), 0);
    ScopedSpace t_sis = MakeSimpleSpace(kT3Dims);
    constexpr std::array<hsize_t, 3> kTSisB = {1, 1, 1};
    constexpr std::array<hsize_t, 3> kTSis2 = {0, 1, 3};
    ASSERT_GE(H5Sselect_hyperslab(t_sis.get(), H5S_SELECT_SET, kTZ.data(),
                                  nullptr, kO3.data(), kTSisB.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(t_sis.get(), H5S_SELECT_OR, kTSis2.data(),
                                  nullptr, kO3.data(), kTSisB.data()),
              0);
    H5S_t* pt = nullptr;
    ASSERT_GE(H5S_select_project_intersection(
                  UnwrapSpace(t_src.get()), UnwrapSpace(t_dst.get()),
                  UnwrapSpace(t_sis.get()), &pt, true),
              0);
    ScopedSpace pt_id(H5I_register(H5I_DATASPACE, pt, true));
    EXPECT_EQ(H5Sget_select_npoints(pt_id.get()), 1);
  }

  // 5. H5S_hyper_clip_unlim partial last block & get_first_inc_block !partial (12379).
  {
    constexpr std::array<hsize_t, 2> kDims = {8, 4}, kMax = {H5S_UNLIMITED, 4};
    ScopedSpace cu = MakeSimpleSpace(kDims, kMax);
    constexpr std::array<hsize_t, 2> kS = {0, 0}, kSt = {4, 1},
                                     kC = {H5S_UNLIMITED, 1}, kB = {2, 4};
    ASSERT_GE(H5Sselect_hyperslab(cu.get(), H5S_SELECT_SET, kS.data(),
                                  kSt.data(), kC.data(), kB.data()),
              0);
    bool partial = true;
    EXPECT_EQ(H5S_hyper_get_first_inc_block(UnwrapSpace(cu.get()), 3, &partial),
              1u);
    EXPECT_FALSE(partial);
    ASSERT_GE(H5S_hyper_clip_unlim(UnwrapSpace(cu.get()), 5), 0);
    EXPECT_EQ(H5Sget_select_npoints(cu.get()), 12);
  }
}

TEST(H5ShyperTest, ExtraTargetedBranchClosure) {
  // 1. H5S__hyper_iter_coords u < 0 (line 822): 3D with dims 0 & 1 flattened
  //    and dim 2 strided (iter_rank == 2 < 3).
  {
    constexpr std::array<hsize_t, 3> kD3 = {4, 4, 4};
    ScopedSpace s = MakeSimpleSpace(kD3);
    constexpr std::array<hsize_t, 3> kZ = {0, 0, 0}, kSt = {1, 1, 2},
                                     kC = {1, 1, 2}, kB = {4, 4, 1};
    ASSERT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_SET, kZ.data(), kSt.data(),
                                  kC.data(), kB.data()),
              0);
    ScopedSelIter it;
    ASSERT_GE(it.init(UnwrapSpace(s.get()), 4, 0), 0);
    std::array<hsize_t, 3> coords{};
    ASSERT_GE(H5S_select_iter_coords(it.get(), coords.data()), 0);
    EXPECT_THAT(coords, ElementsAre(0, 0, 0));
  }

  // 2. Irregular get_seq_list_gen partial-span resume hitting curr_span->next
  //    (lines 1510-1518). Needs >= 3 uneven spans so rebuild leaves it irregular.
  {
    constexpr std::array<hsize_t, 1> kD1 = {12};
    ScopedSpace sg = MakeSimpleSpace(kD1);
    const hsize_t s0 = 0, b0 = 2, s1 = 4, b1 = 2, s2 = 8, b2 = 1, one = 1;
    ASSERT_GE(H5Sselect_hyperslab(sg.get(), H5S_SELECT_SET, &s0, nullptr, &one, &b0), 0);
    ASSERT_GE(H5Sselect_hyperslab(sg.get(), H5S_SELECT_OR, &s1, nullptr, &one, &b1), 0);
    ASSERT_GE(H5Sselect_hyperslab(sg.get(), H5S_SELECT_OR, &s2, nullptr, &one, &b2), 0);
    auto seqs = CollectSeqList(sg.get(), 4, 0, /*maxseq=*/8, /*maxelmts=*/1);
    EXPECT_EQ(seqs.size(), 5u);
  }

  // 3. H5S__hyper_is_contiguous lines 5251-5252 (dim1 has span->next) and
  //    5297-5298 (3D with dim0 height>1, dim1 non-full, dim2 non-regular).
  {
    constexpr std::array<hsize_t, 2> kD2 = {4, 10};
    constexpr std::array<hsize_t, 2> kO2 = {1, 1};
    ScopedSpace s2 = MakeSimpleSpace(kD2);
    constexpr std::array<hsize_t, 2> kA0 = {0, 0}, kBA0 = {2, 2};
    constexpr std::array<hsize_t, 2> kA1 = {0, 4}, kA2 = {0, 8}, kBA2 = {2, 1};
    ASSERT_GE(H5Sselect_hyperslab(s2.get(), H5S_SELECT_SET, kA0.data(), nullptr,
                                  kO2.data(), kBA0.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(s2.get(), H5S_SELECT_OR, kA1.data(), nullptr,
                                  kO2.data(), kBA0.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(s2.get(), H5S_SELECT_OR, kA2.data(), nullptr,
                                  kO2.data(), kBA2.data()),
              0);
    EXPECT_EQ(H5S_select_is_contiguous(UnwrapSpace(s2.get())), 0);

    constexpr std::array<hsize_t, 3> kD3 = {4, 4, 10};
    constexpr std::array<hsize_t, 3> kO3 = {1, 1, 1};
    ScopedSpace s3 = MakeSimpleSpace(kD3);
    constexpr std::array<hsize_t, 3> kB0 = {0, 0, 0}, kBB0 = {2, 2, 2};
    constexpr std::array<hsize_t, 3> kB1 = {0, 0, 4}, kB2 = {0, 0, 8},
                                     kBB2 = {2, 2, 1};
    ASSERT_GE(H5Sselect_hyperslab(s3.get(), H5S_SELECT_SET, kB0.data(), nullptr,
                                  kO3.data(), kBB0.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(s3.get(), H5S_SELECT_OR, kB1.data(), nullptr,
                                  kO3.data(), kBB0.data()),
              0);
    ASSERT_GE(H5Sselect_hyperslab(s3.get(), H5S_SELECT_OR, kB2.data(), nullptr,
                                  kO3.data(), kBB2.data()),
              0);
    EXPECT_EQ(H5S_select_is_contiguous(UnwrapSpace(s3.get())), 0);
  }

  // 4. Equal-npoints irregular shape_same_helper mismatches (5482, 5503, 5508,
  //    5515) & cross-rank shape_same mismatches (5709, 5713, 5717, 5736, 5740,
  //    5761, 5765).
  {
    constexpr std::array<hsize_t, 2> kD2 = {8, 8};
    auto make_irreg = [&](std::initializer_list<std::array<hsize_t, 4>> rects) {
      ScopedSpace sp = MakeSimpleSpace(kD2);
      EXPECT_GE(H5Sselect_none(sp.get()), 0);
      for (const auto& r : rects) {
        const std::array<hsize_t, 2> st = {r[0], r[2]};
        const std::array<hsize_t, 2> ones = {1, 1};
        const std::array<hsize_t, 2> blk = {r[1] - r[0] + 1, r[3] - r[2] + 1};
        EXPECT_GE(H5Sselect_hyperslab(sp.get(), H5S_SELECT_OR, st.data(),
                                      nullptr, ones.data(), blk.data()),
                  0);
      }
      return sp;
    };

    // 5482: same npoints=4, same low_bounds=(0,0), diff high_bounds[0]=(2 vs 3).
    ScopedSpace s5482_a = make_irreg({{0, 0, 0, 0}, {0, 0, 2, 2}, {2, 2, 0, 1}});
    ScopedSpace s5482_b = make_irreg({{0, 0, 0, 0}, {0, 0, 2, 2}, {3, 3, 0, 1}});
    EXPECT_EQ(H5Sselect_shape_same(s5482_a.get(), s5482_b.get()), 0);

    // 5503: same npoints=4, same bounds [0..3,0..2], diff dim0 span count (3 vs 2).
    ScopedSpace s5503_a = make_irreg({{0, 0, 0, 0}, {0, 0, 2, 2}, {1, 1, 0, 0}, {3, 3, 0, 0}});
    ScopedSpace s5503_b = make_irreg({{0, 0, 0, 0}, {0, 0, 2, 2}, {3, 3, 0, 1}});
    EXPECT_EQ(H5Sselect_shape_same(s5503_a.get(), s5503_b.get()), 0);

    // 5508: same npoints=4, same bounds [0..4,0..2], same span count=3, diff mid span.
    ScopedSpace s5508_a = make_irreg({{0, 0, 0, 0}, {0, 0, 2, 2}, {1, 1, 0, 0}, {4, 4, 0, 0}});
    ScopedSpace s5508_b = make_irreg({{0, 0, 0, 0}, {0, 0, 2, 2}, {2, 2, 0, 0}, {4, 4, 0, 0}});
    EXPECT_EQ(H5Sselect_shape_same(s5508_a.get(), s5508_b.get()), 0);

    // 5515: same npoints=6, offset=(1,0) => rest_zeros[0]=true, cmp_spans false on row1.
    ScopedSpace s5515_a = make_irreg({{0, 0, 0, 3}, {1, 1, 0, 1}});
    ScopedSpace s5515_b = make_irreg({{1, 1, 0, 1}, {1, 1, 3, 3}, {2, 2, 0, 2}});
    EXPECT_EQ(H5Sselect_shape_same(s5515_a.get(), s5515_b.get()), 0);

    // Cross-rank 2D vs 1D (equal npoints=4).
    constexpr std::array<hsize_t, 1> kD1 = {16};
    const hsize_t zero1 = 0, one1 = 1, four1 = 4, two1 = 2;
    ScopedSpace r1_c2b2_s4 = MakeSimpleSpace(kD1);
    const hsize_t st4 = 4;
    ASSERT_GE(H5Sselect_hyperslab(r1_c2b2_s4.get(), H5S_SELECT_SET, &zero1, &st4,
                                  &two1, &two1),
              0);
    ScopedSpace r1_c1b4 = MakeSimpleSpace(kD1);
    ASSERT_GE(H5Sselect_hyperslab(r1_c1b4.get(), H5S_SELECT_SET, &zero1, nullptr,
                                  &one1, &four1),
              0);
    ScopedSpace r1_c2b2 = MakeSimpleSpace(kD1);
    const hsize_t st3 = 3;
    ASSERT_GE(H5Sselect_hyperslab(r1_c2b2.get(), H5S_SELECT_SET, &zero1, &st3,
                                  &two1, &two1),
              0);

    // 5709: regular 2D stride!=1D stride.
    ScopedSpace r2_5709 = MakeSimpleSpace(kD2);
    constexpr std::array<hsize_t, 2> kZ2 = {0, 0}, kSt2 = {1, 3}, kC2 = {1, 2},
                                     kB2 = {1, 2};
    ASSERT_GE(H5Sselect_hyperslab(r2_5709.get(), H5S_SELECT_SET, kZ2.data(),
                                  kSt2.data(), kC2.data(), kB2.data()),
              0);
    EXPECT_EQ(H5Sselect_shape_same(r2_5709.get(), r1_c2b2_s4.get()), 0);
    // 5713: regular 2D count!=1D count.
    EXPECT_EQ(H5Sselect_shape_same(r2_5709.get(), r1_c1b4.get()), 0);
    // 5717: regular 2D (stride[1]==1, count[1]==1, block[1]==2) vs 1D (stride=1,count=1,block=4).
    ScopedSpace r2_5717 = MakeSimpleSpace(kD2);
    constexpr std::array<hsize_t, 2> kSt5717 = {2, 1}, kC5717 = {2, 1}, kB5717 = {1, 2};
    ASSERT_GE(H5Sselect_hyperslab(r2_5717.get(), H5S_SELECT_SET, kZ2.data(),
                                  kSt5717.data(), kC5717.data(), kB5717.data()),
              0);
    EXPECT_EQ(H5Sselect_shape_same(r2_5717.get(), r1_c1b4.get()), 0);

    // 5736, 5740, 5761, 5765: regular vs irregular across 2D & 1D (4 pts each).
    ScopedSpace ir1_4 = MakeSimpleSpace(kD1);
    const hsize_t ix1 = 5, ix2 = 9, b1_4 = 1;
    ASSERT_GE(H5Sselect_hyperslab(ir1_4.get(), H5S_SELECT_SET, &zero1, nullptr, &one1, &two1), 0);
    ASSERT_GE(H5Sselect_hyperslab(ir1_4.get(), H5S_SELECT_OR, &ix1, nullptr, &one1, &b1_4), 0);
    ASSERT_GE(H5Sselect_hyperslab(ir1_4.get(), H5S_SELECT_OR, &ix2, nullptr, &one1, &b1_4), 0);
    ScopedSpace r2_1x4 = MakeSimpleSpace(kD2);
    constexpr std::array<hsize_t, 2> kO2 = {1, 1}, kB1x4 = {1, 4};
    ASSERT_GE(H5Sselect_hyperslab(r2_1x4.get(), H5S_SELECT_SET, kZ2.data(),
                                  nullptr, kO2.data(), kB1x4.data()),
              0);
    // space1 regular, space2 irreg -> 5736.
    EXPECT_EQ(H5Sselect_shape_same(r2_1x4.get(), ir1_4.get()), 0);
    // space1 irreg multi-span in dim0 (5761) vs regular 1D -> 5740 + 5761.
    ScopedSpace ir2_5761 = make_irreg({{0, 0, 0, 1}, {2, 2, 0, 0}, {4, 4, 0, 0}});
    EXPECT_EQ(H5Sselect_shape_same(ir2_5761.get(), r1_c1b4.get()), 0);
    // space1 irreg single dim0 span [0..1] (height 2 != 1) -> 5765.
    ScopedSpace ir2_5765 = make_irreg({{0, 1, 0, 0}, {0, 1, 2, 2}});
    EXPECT_EQ(H5Sselect_shape_same(ir2_5765.get(), r1_c1b4.get()), 0);
  }

  // 5. Merge spans helper equal down spans across all 4 overlap cases (8127,
  //    8181, 8226, 8287), NOTA disjoint span2_owned (9385-9386), regular+irreg
  //    combine/modify (10607, 10704, 10842).
  {
    constexpr std::array<hsize_t, 2> kD2 = {12, 12};
    constexpr std::array<hsize_t, 2> kO2 = {1, 1};
    auto slab2 = [&](hsize_t y0, hsize_t y1, hsize_t x0, hsize_t x1) {
      ScopedSpace s = MakeSimpleSpace(kD2);
      const std::array<hsize_t, 2> st = {y0, x0}, blk = {y1 - y0 + 1, x1 - x0 + 1};
      EXPECT_GE(H5Sselect_hyperslab(s.get(), H5S_SELECT_SET, st.data(), nullptr,
                                    kO2.data(), blk.data()),
                0);
      return s;
    };
    auto or2 = [&](hid_t s, hsize_t y0, hsize_t y1, hsize_t x0, hsize_t x1) {
      const std::array<hsize_t, 2> st = {y0, x0}, blk = {y1 - y0 + 1, x1 - x0 + 1};
      EXPECT_GE(H5Sselect_hyperslab(s, H5S_SELECT_OR, st.data(), nullptr,
                                    kO2.data(), blk.data()),
                0);
    };
    // Case 8127 & 8287: [1..3] vs [2..4] with identical down [0..3], plus tail row.
    ScopedSpace m1 = slab2(1, 3, 0, 3);
    or2(m1.get(), 6, 6, 0, 0);
    ScopedSpace m2 = slab2(2, 4, 0, 3);
    or2(m2.get(), 8, 8, 0, 0);
    ScopedSpace c12(H5Scombine_select(m1.get(), H5S_SELECT_OR, m2.get()));
    ScopedSpace c21(H5Scombine_select(m2.get(), H5S_SELECT_OR, m1.get()));
    EXPECT_EQ(H5Sget_select_npoints(c12.get()), H5Sget_select_npoints(c21.get()));

    // Case 8181 & 8226: [1..5] vs [2..4] with identical down [0..3].
    ScopedSpace m3 = slab2(1, 5, 0, 3);
    or2(m3.get(), 7, 7, 0, 0);
    ScopedSpace c32(H5Scombine_select(m3.get(), H5S_SELECT_OR, m2.get()));
    ScopedSpace c23(H5Scombine_select(m2.get(), H5S_SELECT_OR, m3.get()));
    EXPECT_EQ(H5Sget_select_npoints(c32.get()), H5Sget_select_npoints(c23.get()));

    // Regular space1 + irregular space2 -> 10704 & 10842.
    ScopedSpace reg1 = slab2(0, 1, 0, 1);
    ScopedSpace reg1_copy = slab2(0, 1, 0, 1);
    ScopedSpace cmb_ri(H5Scombine_select(reg1.get(), H5S_SELECT_OR, m1.get()));
    EXPECT_GT(H5Sget_select_npoints(cmb_ri.get()), 0);
    ASSERT_GE(H5Smodify_select(reg1_copy.get(), H5S_SELECT_OR, m1.get()), 0);

    // Regular space2 with BOTH span_lst != NULL and diminfo_valid == YES -> 10607!
    ScopedSpace reg_with_spans = slab2(0, 1, 0, 1);
    ScopedSpace npts4_irreg = slab2(0, 0, 0, 1);
    or2(npts4_irreg.get(), 2, 2, 0, 0);
    or2(npts4_irreg.get(), 4, 4, 0, 0);
    EXPECT_EQ(H5Sselect_shape_same(reg_with_spans.get(), npts4_irreg.get()), 0);
    ScopedSpace cmb_10607(H5Scombine_select(m1.get(), H5S_SELECT_OR, reg_with_spans.get()));
    EXPECT_GT(H5Sget_select_npoints(cmb_10607.get()), 0);

    // Overlapping bbox on dim0 OR no overlap in dim1? Keep can_own_span2=true
    // via H5Sselect_hyperslab(irreg, H5S_SELECT_NOTA, ...) disjoint on dim0 (9385-9386)!
    constexpr std::array<hsize_t, 2> kNs = {9, 9}, kNb = {2, 2};
    ScopedSpace ir_nota = slab2(0, 0, 0, 1);
    or2(ir_nota.get(), 2, 2, 0, 0);
    ASSERT_GE(H5Sselect_hyperslab(ir_nota.get(), H5S_SELECT_NOTA, kNs.data(),
                                  nullptr, kO2.data(), kNb.data()),
              0);
    EXPECT_EQ(H5Sget_select_npoints(ir_nota.get()), 4);
  }

  // 6. H5S__hyper_project_intersection Case 2 skip-only (11605), trailing
  //    ss_span skip (11571, 11576), empty-intersect depth>0 skip (11633), and
  //    sis_span->low advance (11556).
  {
    constexpr std::array<hsize_t, 2> kD2 = {6, 8};
    constexpr std::array<hsize_t, 2> kO2 = {1, 1};
    ScopedSpace ss2 = MakeSimpleSpace(kD2);
    constexpr std::array<hsize_t, 2> kS2a = {0, 0}, kB2a = {4, 1},
                                     kS2b = {0, 2}, kB2b = {4, 1},
                                     kS2c = {0, 4}, kB2c = {4, 1};
    ASSERT_GE(H5Sselect_hyperslab(ss2.get(), H5S_SELECT_SET, kS2a.data(), nullptr, kO2.data(), kB2a.data()), 0);
    ASSERT_GE(H5Sselect_hyperslab(ss2.get(), H5S_SELECT_OR, kS2b.data(), nullptr, kO2.data(), kB2b.data()), 0);
    ASSERT_GE(H5Sselect_hyperslab(ss2.get(), H5S_SELECT_OR, kS2c.data(), nullptr, kO2.data(), kB2c.data()), 0);
    constexpr hsize_t kD12 = 12;
    ScopedSpace ds12(H5Screate_simple(1, &kD12, nullptr));
    ASSERT_GE(H5Sselect_all(ds12.get()), 0);

    ScopedSpace sis = MakeSimpleSpace(kD2);
    // rows [0..1]: cols [1..1], [3..3] -> hits 11556, 11576 (trailing [4..4]), and Case 2 11605!
    // rows [2..2]: col [6..6] -> disjoint from ss2 dim1 [0..4] -> depth=1 !check_intersect -> hits 11633!
    // rows [3..3]: col [0..0] -> keeps dim 0..1 bounds overlapping overall [0..6]!
    constexpr std::array<hsize_t, 2> kI0 = {0, 1}, kIB0 = {2, 1},
                                     kI1 = {0, 3},
                                     kI2 = {2, 6}, kIB2 = {1, 1},
                                     kI3 = {3, 0};
    ASSERT_GE(H5Sselect_hyperslab(sis.get(), H5S_SELECT_SET, kI0.data(), nullptr, kO2.data(), kIB0.data()), 0);
    ASSERT_GE(H5Sselect_hyperslab(sis.get(), H5S_SELECT_OR, kI1.data(), nullptr, kO2.data(), kIB0.data()), 0);
    ASSERT_GE(H5Sselect_hyperslab(sis.get(), H5S_SELECT_OR, kI2.data(), nullptr, kO2.data(), kIB2.data()), 0);
    ASSERT_GE(H5Sselect_hyperslab(sis.get(), H5S_SELECT_OR, kI3.data(), nullptr, kO2.data(), kIB2.data()), 0);

    H5S_t* pout = nullptr;
    ASSERT_GE(H5S_select_project_intersection(
                  UnwrapSpace(ss2.get()), UnwrapSpace(ds12.get()),
                  UnwrapSpace(sis.get()), &pout, false),
              0);
    ScopedSpace pout_id(H5I_register(H5I_DATASPACE, pout, true));
    EXPECT_EQ(H5Sget_select_npoints(pout_id.get()), 1);

    // 11571: trailing 3D ss_span with down != NULL when sis finishes early!
    constexpr std::array<hsize_t, 3> kD3 = {4, 4, 4}, kO3 = {1, 1, 1};
    ScopedSpace ss3 = MakeSimpleSpace(kD3);
    constexpr std::array<hsize_t, 3> k3A = {0, 0, 0}, k3BA = {1, 2, 2},
                                     k3B = {1, 0, 0}, k3BB = {1, 1, 2},
                                     k3C = {3, 0, 0}, k3BC = {1, 2, 2};
    ASSERT_GE(H5Sselect_hyperslab(ss3.get(), H5S_SELECT_SET, k3A.data(), nullptr, kO3.data(), k3BA.data()), 0);
    ASSERT_GE(H5Sselect_hyperslab(ss3.get(), H5S_SELECT_OR, k3B.data(), nullptr, kO3.data(), k3BB.data()), 0);
    ASSERT_GE(H5Sselect_hyperslab(ss3.get(), H5S_SELECT_OR, k3C.data(), nullptr, kO3.data(), k3BC.data()), 0);
    constexpr hsize_t kD10 = 10;
    ScopedSpace ds10(H5Screate_simple(1, &kD10, nullptr));
    ASSERT_GE(H5Sselect_all(ds10.get()), 0);
    ScopedSpace sis3 = MakeSimpleSpace(kD3);
    constexpr std::array<hsize_t, 3> kS3B = {1, 1, 1};
    ASSERT_GE(H5Sselect_hyperslab(sis3.get(), H5S_SELECT_SET, k3A.data(), nullptr, kO3.data(), kS3B.data()), 0);
    H5S_t* pout3 = nullptr;
    ASSERT_GE(H5S_select_project_intersection(
                  UnwrapSpace(ss3.get()), UnwrapSpace(ds10.get()),
                  UnwrapSpace(sis3.get()), &pout3, false),
              0);
    ScopedSpace pout3_id(H5I_register(H5I_DATASPACE, pout3, true));
    EXPECT_EQ(H5Sget_select_npoints(pout3_id.get()), 1);
  }
}

}  // namespace
}  // namespace hdf5
