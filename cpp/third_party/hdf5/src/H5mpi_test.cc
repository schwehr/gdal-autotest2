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
// Tests disabled-MPI sentinels, public MPI driver constants/enums, and serial
// VFD non-MPI feature flags related to third_party/hdf5/src/H5mpi.c.
// (LF=0 in default google3 non-MPI Linux build: all implementations in H5mpi.c
// are gated inside #ifdef H5_HAVE_PARALLEL.)
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5mpi.c

#include <array>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5MpiTest, ParallelSupportIsDisabledAndPublicConstantsMatch) {
#ifdef H5_HAVE_PARALLEL
  FAIL()
      << "Expected H5_HAVE_PARALLEL to be disabled in default google3 build.";
#endif
#ifdef H5_HAVE_PARALLEL_FILTERED_WRITES
  FAIL() << "Expected H5_HAVE_PARALLEL_FILTERED_WRITES to be disabled.";
#endif
#ifdef H5_CHECK_MPI_VERSION
  FAIL() << "Expected H5_CHECK_MPI_VERSION to be absent outside H5private.h.";
#endif

  ASSERT_GE(H5open(), 0);
  EXPECT_EQ(H5FD_MPIO, H5I_INVALID_HID);
  EXPECT_EQ(H5FD_SUBFILING, H5I_INVALID_HID);
  EXPECT_EQ(H5FD_IOC, H5I_INVALID_HID);

  EXPECT_EQ(H5D_ONE_LINK_CHUNK_IO_THRESHOLD, 0);
  EXPECT_EQ(H5D_MULTI_CHUNK_IO_COL_THRESHOLD, 60);
  EXPECT_EQ(H5FD_MPIO_INDEPENDENT, 0);
  EXPECT_EQ(H5FD_MPIO_COLLECTIVE, 1);
  EXPECT_EQ(H5FD_MPIO_CHUNK_DEFAULT, 0);
  EXPECT_LT(H5FD_MPIO_CHUNK_DEFAULT, H5FD_MPIO_CHUNK_ONE_IO);
  EXPECT_LT(H5FD_MPIO_CHUNK_ONE_IO, H5FD_MPIO_CHUNK_MULTI_IO);
  EXPECT_EQ(H5FD_MPIO_COLLECTIVE_IO, 0);
  EXPECT_LT(H5FD_MPIO_COLLECTIVE_IO, H5FD_MPIO_INDIVIDUAL_IO);
}

TEST(H5MpiTest, MpioDriverQueryFailsAndSerialDriversReportNoMpi) {
  unsigned long flags = 0;
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5FDdriver_query(H5FD_MPIO, &flags), 0);
  }

  const auto serial_drivers = std::to_array<hid_t>({
      H5FD_CORE,
      H5FD_SEC2,
      H5FD_STDIO,
      H5FD_LOG,
      H5FD_FAMILY,
      H5FD_MULTI,
      H5FD_SPLITTER,
      H5FD_ONION,
  });
  for (const hid_t driver : serial_drivers) {
    ASSERT_NE(driver, H5I_INVALID_HID);
    unsigned long driver_flags = H5FD_FEAT_HAS_MPI;
    ASSERT_GE(H5FDdriver_query(driver, &driver_flags), 0);
    EXPECT_EQ(driver_flags & H5FD_FEAT_HAS_MPI, 0UL);
  }

  ScopedPlist default_fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(default_fapl.valid());
  EXPECT_EQ(H5Pget_driver(default_fapl.get()), H5FD_SEC2);
}

}  // namespace
}  // namespace hdf5
