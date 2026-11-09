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
// Tests parallel file constants, block structs, disabled MPI VFD sentinel, and
// serial file access fallback behavior related to third_party/hdf5/src/H5Fmpi.c.
// (LF=0 in default google3 non-MPI Linux build: all implementations in H5Fmpi.c
// are gated behind #ifdef H5_HAVE_PARALLEL.)
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Fmpi.c

// `:hdf5` is built with -DNDEBUG; match internal struct layout in C++ tests.
#ifndef NDEBUG
#define H5FMPI_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#define H5F_FRIEND
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#undef try
#define try hdf5_try_param
#include "third_party/hdf5/src/H5Fpkg.h"
#pragma pop_macro("try")
#pragma clang diagnostic pop
}  // extern "C"

#ifdef H5FMPI_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FMPI_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include <cstddef>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5FmpiTest, DefaultBuildDisablesParallelAndExportsMpiEnums) {
#ifdef H5_HAVE_PARALLEL
  ADD_FAILURE() << "Expected H5_HAVE_PARALLEL to be disabled in default build.";
#endif

  // In non-MPI google3 builds, H5FD_MPIO evaluates to H5I_INVALID_HID.
  EXPECT_EQ(H5FD_MPIO, H5I_INVALID_HID);

  EXPECT_EQ(H5D_ONE_LINK_CHUNK_IO_THRESHOLD, 0);
  EXPECT_EQ(H5D_MULTI_CHUNK_IO_COL_THRESHOLD, 60);

  EXPECT_EQ(H5FD_MPIO_INDEPENDENT, 0);
  EXPECT_EQ(H5FD_MPIO_COLLECTIVE, 1);

  EXPECT_EQ(H5FD_MPIO_CHUNK_DEFAULT, 0);
  EXPECT_EQ(H5FD_MPIO_CHUNK_ONE_IO, 1);
  EXPECT_EQ(H5FD_MPIO_CHUNK_MULTI_IO, 2);

  EXPECT_EQ(H5FD_MPIO_COLLECTIVE_IO, 0);
  EXPECT_EQ(H5FD_MPIO_INDIVIDUAL_IO, 1);
}

TEST(H5FmpiTest, BlockStructAndPublicFileEnumsHaveExpectedLayout) {
  static_assert(offsetof(H5F_block_t, offset) == 0);
  static_assert(offsetof(H5F_block_t, length) >= sizeof(haddr_t));
  static_assert(sizeof(H5F_block_t) >= sizeof(haddr_t) + sizeof(hsize_t));

  H5F_block_t blk{/*.offset=*/128, /*.length=*/256};
  EXPECT_EQ(blk.offset, haddr_t{128});
  EXPECT_EQ(blk.length, hsize_t{256});

  EXPECT_EQ(H5F_SCOPE_LOCAL, 0);
  EXPECT_EQ(H5F_SCOPE_GLOBAL, 1);
  EXPECT_EQ(H5F_CLOSE_DEFAULT, 0);
  EXPECT_EQ(H5F_CLOSE_WEAK, 1);
  EXPECT_EQ(H5F_CLOSE_SEMI, 2);
  EXPECT_EQ(H5F_CLOSE_STRONG, 3);
}

TEST(H5FmpiTest, SerialCoreFileCreatesAndRejectsNonExistentPath) {
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  EXPECT_NE(H5Pget_driver(fapl.get()), H5FD_MPIO);

  ScopedFile file = CreateInMemoryFile("h5f_mpi_serial.h5");
  ASSERT_TRUE(file.valid());

  ScopedPlist actual_fapl(H5Fget_access_plist(file.get()));
  ASSERT_TRUE(actual_fapl.valid());
  EXPECT_EQ(H5Pget_driver(actual_fapl.get()), H5FD_CORE);

  ScopedErrorSilence silence;
  ScopedFile missing(
      H5Fopen("nonexistent_h5f_mpi_serial.h5", H5F_ACC_RDONLY, fapl.get()));
  EXPECT_EQ(missing.get(), H5I_INVALID_HID);
}

}  // namespace
}  // namespace hdf5
