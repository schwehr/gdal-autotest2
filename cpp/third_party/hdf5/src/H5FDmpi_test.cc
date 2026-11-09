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
// Tests MPI VFD constants, transfer enums, driver registration status, and
// serial fallback behavior related to third_party/hdf5/src/H5FDmpi.c.
// (LF=0 in default google3 non-MPI Linux build: all implementations in
// H5FDmpi.c are gated behind #ifdef H5_HAVE_PARALLEL.)
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDmpi.c

#include <array>
#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5FDmpiTest, DefaultBuildDisablesParallelAndExportsMpiConstants) {
#ifdef H5_HAVE_PARALLEL
  ADD_FAILURE() << "Expected H5_HAVE_PARALLEL to be disabled in default build.";
#endif

  // In non-MPI google3 builds, H5FD_MPIO evaluates to H5I_INVALID_HID.
  EXPECT_EQ(H5FD_MPIO, H5I_INVALID_HID);
  EXPECT_EQ(H5_VFD_MPIO, H5FD_class_value_t{7});
  EXPECT_EQ(H5FD_FEAT_HAS_MPI, 0x00000100UL);

  EXPECT_EQ(H5D_ONE_LINK_CHUNK_IO_THRESHOLD, 0);
  EXPECT_EQ(H5D_MULTI_CHUNK_IO_COL_THRESHOLD, 60);

  EXPECT_EQ(H5FD_MPIO_INDEPENDENT, 0);
  EXPECT_EQ(H5FD_MPIO_COLLECTIVE, 1);

  EXPECT_EQ(H5FD_MPIO_CHUNK_DEFAULT, 0);
  EXPECT_EQ(H5FD_MPIO_CHUNK_ONE_IO, 1);
  EXPECT_EQ(H5FD_MPIO_CHUNK_MULTI_IO, 2);

  EXPECT_EQ(H5FD_MPIO_COLLECTIVE_IO, 0);
  EXPECT_EQ(H5FD_MPIO_INDIVIDUAL_IO, 1);

  EXPECT_EQ(H5FD_CTL_GET_MPI_COMMUNICATOR_OPCODE, 2ULL);
  EXPECT_EQ(H5FD_CTL_GET_MPI_RANK_OPCODE, 3ULL);
  EXPECT_EQ(H5FD_CTL_GET_MPI_SIZE_OPCODE, 4ULL);
  EXPECT_EQ(H5FD_CTL_GET_MPI_FILE_SYNC_OPCODE, 8ULL);
  EXPECT_EQ(H5FD_CTL_GET_MPI_INFO_OPCODE, 9ULL);
}

TEST(H5FDmpiTest, MpioDriverIsNotRegisteredAndSerialDriversLackMpiFlag) {
  EXPECT_EQ(H5FDis_driver_registered_by_name("mpio"), 0);
  EXPECT_EQ(H5FDis_driver_registered_by_value(H5_VFD_MPIO), 0);

  unsigned long flags = 0;
  ASSERT_GE(H5FDdriver_query(H5FD_CORE, &flags), 0);
  EXPECT_EQ(flags & H5FD_FEAT_HAS_MPI, 0UL);

  flags = 0;
  ASSERT_GE(H5FDdriver_query(H5FD_SEC2, &flags), 0);
  EXPECT_EQ(flags & H5FD_FEAT_HAS_MPI, 0UL);

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());

  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_driver(fapl.get(), H5FD_MPIO, nullptr), 0);
}

TEST(H5FDmpiTest, SerialCoreFileRejectsMissingPathAndCorruptImage) {
  // 1. Create and verify a serial in-memory Core VFD file without MPI.
  ScopedPlist fapl = MakeCoreFapl();
  ASSERT_TRUE(fapl.valid());

  ScopedFile file = CreateInMemoryFile("h5fd_mpi_serial.h5");
  ASSERT_TRUE(file.valid());
  file.reset();

  // 2. Opening a missing file fails cleanly.
  {
    ScopedErrorSilence silence;
    ScopedFile missing(
        H5Fopen("nonexistent_h5fd_mpi_serial.h5", H5F_ACC_RDONLY, fapl.get()));
    EXPECT_EQ(missing.get(), H5I_INVALID_HID);
  }

  // 3. Opening an invalid file image fails cleanly.
  constexpr std::array<uint8_t, 64> kCorruptImage = {
      'N', 'O', 'T', '_', 'H', 'D', 'F', '5',
  };
  ScopedPlist img_fapl = MakeCoreFapl();
  ASSERT_TRUE(img_fapl.valid());
  ASSERT_GE(H5Pset_file_image(img_fapl.get(),
                              const_cast<uint8_t*>(kCorruptImage.data()),
                              kCorruptImage.size()),
            0);
  ScopedErrorSilence silence;
  ScopedFile corrupt(
      H5Fopen("corrupt_h5fd_mpi_serial.h5", H5F_ACC_RDONLY, img_fapl.get()));
  EXPECT_EQ(corrupt.get(), H5I_INVALID_HID);
}

}  // namespace
}  // namespace hdf5
