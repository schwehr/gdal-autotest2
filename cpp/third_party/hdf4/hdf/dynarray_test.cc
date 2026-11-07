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
// Tests internal storage routines for dynamic arrays (hdf/src/dynarray.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/dynarray.c

#include <cstdlib>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

// GoogleTest's gtest.h defines SUCCEED() and FAIL() as function-like macros.
// Undefine them before including HDF4 headers so hdf.h can define its constants
// SUCCEED (0) and FAIL (-1) without macro redefinition errors under no_modules.
#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/dynarray_priv.h"
#include "third_party/hdf4/hdf/src/hdf.h"

namespace hdf4 {
namespace {

TEST(DynarrayTest, DAcreateArrayValidatesArguments) {
  // Invalid start_size.
  EXPECT_EQ(nullptr, DAcreate_array(-1, 5));

  // Invalid incr_mult.
  EXPECT_EQ(nullptr, DAcreate_array(5, 0));
  EXPECT_EQ(nullptr, DAcreate_array(5, -1));
}

TEST(DynarrayTest, DAcreateArrayZeroInitialSize) {
  // Start size 0 does not allocate internal array immediately.
  dynarr_p arr = DAcreate_array(0, 4);
  ASSERT_NE(nullptr, arr);
  absl::Cleanup clean_arr = [arr] {
    EXPECT_EQ(SUCCEED, DAdestroy_array(arr, 0));
  };

  EXPECT_EQ(0, DAsize_array(arr));
  EXPECT_EQ(nullptr, DAget_elem(arr, 0));

  // Setting an element on a 0-sized array triggers the initial calloc branch.
  int val = 42;
  EXPECT_EQ(SUCCEED, DAset_elem(arr, 2, &val));
  // Growth multiple of 4 means new size is ((2/4) + 1) * 4 = 4.
  EXPECT_EQ(4, DAsize_array(arr));
  EXPECT_EQ(&val, DAget_elem(arr, 2));
  EXPECT_EQ(nullptr, DAget_elem(arr, 0));
}

TEST(DynarrayTest, DAdestroyArrayValidatesNull) {
  EXPECT_EQ(FAIL, DAdestroy_array(nullptr, 0));
}

TEST(DynarrayTest, DAdestroyArrayFreesContainedElements) {
  dynarr_p arr = DAcreate_array(2, 2);
  ASSERT_NE(nullptr, arr);

  // Allocate heap objects to be freed by DAdestroy_array(arr, 1).
  int* elem0 = static_cast<int*>(std::malloc(sizeof(int)));
  ASSERT_NE(nullptr, elem0);
  *elem0 = 100;

  int* elem1 = static_cast<int*>(std::malloc(sizeof(int)));
  ASSERT_NE(nullptr, elem1);
  *elem1 = 200;

  EXPECT_EQ(SUCCEED, DAset_elem(arr, 0, elem0));
  EXPECT_EQ(SUCCEED, DAset_elem(arr, 1, elem1));

  // free_elem != 0 frees elem0 and elem1.
  EXPECT_EQ(SUCCEED, DAdestroy_array(arr, 1));
}

TEST(DynarrayTest, DAsizeArrayValidatesNull) {
  EXPECT_EQ(FAIL, DAsize_array(nullptr));
}

TEST(DynarrayTest, DAgetElemAndDAsetElemBasicOperations) {
  dynarr_p arr = DAcreate_array(5, 5);
  ASSERT_NE(nullptr, arr);
  absl::Cleanup clean_arr = [arr] {
    EXPECT_EQ(SUCCEED, DAdestroy_array(arr, 0));
  };

  EXPECT_EQ(5, DAsize_array(arr));

  int val1 = 11;
  int val2 = 22;

  EXPECT_EQ(SUCCEED, DAset_elem(arr, 1, &val1));
  EXPECT_EQ(SUCCEED, DAset_elem(arr, 3, &val2));

  EXPECT_EQ(&val1, DAget_elem(arr, 1));
  EXPECT_EQ(&val2, DAget_elem(arr, 3));
  EXPECT_EQ(nullptr, DAget_elem(arr, 0));
  EXPECT_EQ(nullptr, DAget_elem(arr, 2));

  // Invalid arguments to get and set.
  EXPECT_EQ(nullptr, DAget_elem(nullptr, 0));
  EXPECT_EQ(nullptr, DAget_elem(arr, -1));
  EXPECT_EQ(nullptr, DAget_elem(arr, 100));  // Out of bounds

  EXPECT_EQ(FAIL, DAset_elem(nullptr, 0, &val1));
  EXPECT_EQ(FAIL, DAset_elem(arr, -1, &val1));
}

TEST(DynarrayTest, DAsetElemReallocExpandsArray) {
  // Start with size 2, increment multiple 5.
  dynarr_p arr = DAcreate_array(2, 5);
  ASSERT_NE(nullptr, arr);
  absl::Cleanup clean_arr = [arr] {
    EXPECT_EQ(SUCCEED, DAdestroy_array(arr, 0));
  };

  int val0 = 0;
  EXPECT_EQ(SUCCEED, DAset_elem(arr, 0, &val0));

  // Set element at index 7 (beyond size 2).
  // Multiples of 5: ((7 / 5) + 1) * 5 = 10 elements.
  int val7 = 77;
  EXPECT_EQ(SUCCEED, DAset_elem(arr, 7, &val7));

  EXPECT_EQ(10, DAsize_array(arr));
  EXPECT_EQ(&val0, DAget_elem(arr, 0));
  EXPECT_EQ(&val7, DAget_elem(arr, 7));
  // Newly allocated slots are zeroed.
  EXPECT_EQ(nullptr, DAget_elem(arr, 1));
  EXPECT_EQ(nullptr, DAget_elem(arr, 6));
  EXPECT_EQ(nullptr, DAget_elem(arr, 8));
}

TEST(DynarrayTest, DAdelElemRetrievesAndClearsSlot) {
  dynarr_p arr = DAcreate_array(4, 4);
  ASSERT_NE(nullptr, arr);
  absl::Cleanup clean_arr = [arr] {
    EXPECT_EQ(SUCCEED, DAdestroy_array(arr, 0));
  };

  int val = 999;
  EXPECT_EQ(SUCCEED, DAset_elem(arr, 2, &val));
  EXPECT_EQ(&val, DAget_elem(arr, 2));

  // Delete element returns the pointer and zeroes the slot.
  EXPECT_EQ(&val, DAdel_elem(arr, 2));
  EXPECT_EQ(nullptr, DAget_elem(arr, 2));
  EXPECT_EQ(nullptr, DAdel_elem(arr, 2));

  // Invalid arguments to DAdel_elem.
  EXPECT_EQ(nullptr, DAdel_elem(nullptr, 0));
  EXPECT_EQ(nullptr, DAdel_elem(arr, -1));
  EXPECT_EQ(nullptr, DAdel_elem(arr, 50));
}

}  // namespace
}  // namespace hdf4
