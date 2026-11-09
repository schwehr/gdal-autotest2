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
// Unit tests for third_party/hdf5/c++/src/H5DataSpace.cpp.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/develop/c++/src/H5DataSpace.cpp

#include <array>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/hdf5/c++/src/H5Cpp.h"
#include "third_party/hdf5/google/util/test_util.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {

template struct ConstantGetterBypass<ConstantGetterTag<H5::DataSpace>,
                                     &H5::DataSpace::getConstant>;

namespace {

using ::testing::ElementsAre;

TEST(H5DataSpaceTest, ConstructorsCopyAssignmentAndClose) {
  EXPECT_EQ(H5S_ALL, H5::DataSpace::ALL.getId());

  H5::DataSpace scalar(H5S_SCALAR);
  EXPECT_EQ(H5S_SCALAR, scalar.getSimpleExtentType());
  EXPECT_TRUE(scalar.isSimple());
  EXPECT_EQ(0, scalar.getSimpleExtentNdims());
  EXPECT_EQ(1, scalar.getSimpleExtentNpoints());

  H5::DataSpace null_space(H5S_NULL);
  EXPECT_EQ(H5S_NULL, null_space.getSimpleExtentType());
  EXPECT_EQ(0, null_space.getSimpleExtentNpoints());

  constexpr std::array<hsize_t, 2> kDims = {3, 4};
  constexpr std::array<hsize_t, 2> kMaxDims = {6, H5S_UNLIMITED};
  H5::DataSpace simple(2, kDims.data(), kMaxDims.data());
  EXPECT_EQ(H5S_SIMPLE, simple.getSimpleExtentType());
  EXPECT_EQ(2, simple.getSimpleExtentNdims());
  EXPECT_EQ(12, simple.getSimpleExtentNpoints());

  std::array<hsize_t, 2> dims_out{};
  std::array<hsize_t, 2> maxdims_out{};
  EXPECT_EQ(2, simple.getSimpleExtentDims(dims_out.data(), maxdims_out.data()));
  EXPECT_THAT(dims_out, ElementsAre(3, 4));
  EXPECT_THAT(maxdims_out, ElementsAre(6, H5S_UNLIMITED));

  H5::DataSpace from_id(simple.getId());
  EXPECT_EQ(simple.getId(), from_id.getId());
  EXPECT_EQ(2, from_id.getCounter());

  H5::DataSpace copy_ctor(simple);
  EXPECT_EQ(simple.getId(), copy_ctor.getId());
  EXPECT_EQ(3, copy_ctor.getCounter());

  H5::DataSpace assigned(H5S_SCALAR);
  assigned = simple;
  EXPECT_NE(simple.getId(), assigned.getId());
  EXPECT_EQ(12, assigned.getSimpleExtentNpoints());
  H5::DataSpace* self_ptr = &assigned;
  assigned = *self_ptr;
  EXPECT_EQ(12, assigned.getSimpleExtentNpoints());

  H5::DataSpace extent_dest(H5S_SCALAR);
  simple.extentCopy(extent_dest);
  EXPECT_EQ(12, extent_dest.getSimpleExtentNpoints());

  constexpr std::array<hsize_t, 2> kNewDims = {2, 5};
  extent_dest.setExtentSimple(2, kNewDims.data(), nullptr);
  EXPECT_EQ(10, extent_dest.getSimpleExtentNpoints());
  extent_dest.setExtentNone();
  EXPECT_EQ(H5S_NULL, extent_dest.getSimpleExtentType());

  assigned.close();
  EXPECT_EQ(H5I_INVALID_HID, assigned.getId());
}

TEST(H5DataSpaceTest, SelectionsHyperslabsElementsAndOffsets) {
  constexpr std::array<hsize_t, 2> kDims = {8, 10};
  H5::DataSpace space(2, kDims.data());

  space.selectAll();
  EXPECT_EQ(80, space.getSelectNpoints());
  EXPECT_TRUE(space.selectValid());

  space.selectNone();
  EXPECT_EQ(0, space.getSelectNpoints());

  constexpr std::array<hsize_t, 2> kStart = {1, 2};
  constexpr std::array<hsize_t, 2> kCount = {1, 1};
  constexpr std::array<hsize_t, 2> kStride = {1, 1};
  constexpr std::array<hsize_t, 2> kBlock = {3, 4};
  space.selectHyperslab(H5S_SELECT_SET, kCount.data(), kStart.data(),
                        kStride.data(), kBlock.data());
  EXPECT_EQ(12, space.getSelectNpoints());
  EXPECT_EQ(1, space.getSelectHyperNblocks());

  std::array<hsize_t, 4> hyper_blocks{};
  space.getSelectHyperBlocklist(0, 1, hyper_blocks.data());
  EXPECT_THAT(hyper_blocks, ElementsAre(1, 2, 3, 5));

  std::array<hsize_t, 2> bound_start{};
  std::array<hsize_t, 2> bound_end{};
  space.getSelectBounds(bound_start.data(), bound_end.data());
  EXPECT_THAT(bound_start, ElementsAre(1, 2));
  EXPECT_THAT(bound_end, ElementsAre(3, 5));

  constexpr std::array<hssize_t, 2> kOffset = {1, 1};
  space.offsetSimple(kOffset.data());
  space.getSelectBounds(bound_start.data(), bound_end.data());
  EXPECT_THAT(bound_start, ElementsAre(2, 3));
  EXPECT_THAT(bound_end, ElementsAre(4, 6));

  constexpr std::array<hssize_t, 2> kOutOfBoundsOffset = {10, 0};
  space.offsetSimple(kOutOfBoundsOffset.data());
  EXPECT_FALSE(space.selectValid());

  constexpr std::array<hsize_t, 4> kCoords = {0, 0, 7, 9};
  space.selectElements(H5S_SELECT_SET, 2, kCoords.data());
  EXPECT_EQ(2, space.getSelectElemNpoints());
  std::array<hsize_t, 4> elem_points{};
  space.getSelectElemPointlist(0, 2, elem_points.data());
  EXPECT_THAT(elem_points, ElementsAre(0, 0, 7, 9));
}

TEST(H5DataSpaceTest, InvalidArgumentsAndClosedSpaceThrowDataSpaceIException) {
  ScopedQuietHdf5Errors quiet;

  ASSERT_EQ(H5S_ALL, H5::DataSpace::ALL.getId());
  ExpectDuplicateGetConstantThrows<H5::DataSpace, H5::DataSpaceIException>(
      "allocated ALL_");

  EXPECT_THROW(H5::DataSpace(static_cast<H5S_class_t>(-1)),
               H5::DataSpaceIException);
  constexpr std::array<hsize_t, 1> kBadDim = {1};
  EXPECT_THROW(H5::DataSpace(-1, kBadDim.data()), H5::DataSpaceIException);

  H5::DataSpace closed(H5S_SCALAR);
  closed.close();

  H5::DataSpace valid(H5S_SCALAR);
  EXPECT_THROW(valid.copy(closed), H5::DataSpaceIException);
  EXPECT_THROW(closed.isSimple(), H5::DataSpaceIException);
  constexpr std::array<hssize_t, 1> kZeroOffset = {0};
  EXPECT_THROW(closed.offsetSimple(kZeroOffset.data()),
               H5::DataSpaceIException);
  std::array<hsize_t, 1> dims{};
  EXPECT_THROW(closed.getSimpleExtentDims(dims.data()),
               H5::DataSpaceIException);
  EXPECT_THROW(closed.getSimpleExtentNdims(), H5::DataSpaceIException);
  EXPECT_THROW(closed.getSimpleExtentNpoints(), H5::DataSpaceIException);
  EXPECT_THROW(closed.getSimpleExtentType(), H5::DataSpaceIException);
  EXPECT_THROW(closed.extentCopy(valid), H5::DataSpaceIException);
  EXPECT_THROW(closed.setExtentSimple(1, kBadDim.data()),
               H5::DataSpaceIException);
  EXPECT_THROW(closed.setExtentNone(), H5::DataSpaceIException);
  EXPECT_THROW(closed.getSelectNpoints(), H5::DataSpaceIException);
  EXPECT_THROW(closed.getSelectHyperNblocks(), H5::DataSpaceIException);
  EXPECT_THROW(closed.getSelectHyperBlocklist(0, 1, dims.data()),
               H5::DataSpaceIException);
  EXPECT_THROW(closed.getSelectElemNpoints(), H5::DataSpaceIException);
  EXPECT_THROW(closed.getSelectElemPointlist(0, 1, dims.data()),
               H5::DataSpaceIException);
  EXPECT_THROW(closed.getSelectBounds(dims.data(), dims.data()),
               H5::DataSpaceIException);
  EXPECT_THROW(closed.selectElements(H5S_SELECT_SET, 1, kBadDim.data()),
               H5::DataSpaceIException);
  EXPECT_THROW(closed.selectAll(), H5::DataSpaceIException);
  EXPECT_THROW(closed.selectNone(), H5::DataSpaceIException);
  EXPECT_THROW(closed.selectValid(), H5::DataSpaceIException);
  EXPECT_THROW(
      closed.selectHyperslab(H5S_SELECT_SET, kBadDim.data(), kBadDim.data()),
      H5::DataSpaceIException);
}

}  // namespace
}  // namespace hdf5
