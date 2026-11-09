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
// Tests Read-Only S3 (ROS3) VFD disabled-feature sentinel constants and
// FAPL/VFD rejection behavior related to
// third_party/hdf5/src/H5FDros3_s3comms.c. (LF=0 in default google3 non-MPI
// Linux build: H5_HAVE_ROS3_VFD is undefined in H5pubconf.h, so all
// implementations in H5FDros3_s3comms.c are compiled out.)
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDros3_s3comms.c

// `:hdf5` is built with -DNDEBUG; match internal struct layout in C++ tests.
#ifndef NDEBUG
#define H5FDROS3_S3COMMS_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif

#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"

extern "C" {
#include "third_party/hdf5/src/H5FDros3_s3comms.h"
}  // extern "C"

#ifdef H5FDROS3_S3COMMS_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5FDROS3_S3COMMS_TEST_RESTORE_UNDEF_NDEBUG
#endif

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

TEST(H5FDros3S3commsTest, DefaultBuildDisablesRos3AndExportsSentinels) {
#ifdef H5_HAVE_ROS3_VFD
  ADD_FAILURE() << "Expected H5_HAVE_ROS3_VFD to be disabled in default build.";
#endif

  EXPECT_EQ(H5FD_ROS3, H5I_INVALID_HID);
  EXPECT_EQ(H5FD_ROS3_VALUE, H5_VFD_INVALID);
  EXPECT_EQ(H5_VFD_ROS3, 11);

  EXPECT_EQ(H5FDis_driver_registered_by_name("ros3"), 0);
  EXPECT_EQ(H5FDis_driver_registered_by_value(H5_VFD_ROS3), 0);
}

TEST(H5FDros3S3commsTest, DisabledRos3DriverIsRejectedByFaplAndQuery) {
  ScopedErrorSilence silence;

  unsigned long flags = 0;  // NOLINT(runtime/int)
  EXPECT_LT(H5FDdriver_query(H5FD_ROS3, &flags), 0);

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());

  EXPECT_LT(H5Pset_driver(fapl.get(), H5FD_ROS3, nullptr), 0);
  EXPECT_LT(H5Pset_driver_by_name(fapl.get(), "ros3", nullptr), 0);
  EXPECT_LT(H5Pset_driver_by_value(fapl.get(), H5_VFD_ROS3, nullptr), 0);
}

}  // namespace
}  // namespace hdf5
