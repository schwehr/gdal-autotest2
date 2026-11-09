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
// Tests disabled Direct I/O VFD sentinels, configuration default constants,
// and runtime registration/driver-selection behavior related to
// third_party/hdf5/src/H5FDdirect.c.
// (LF=0 in default google3 non-MPI Linux build: all implementations in
// H5FDdirect.c are gated inside #ifdef H5_HAVE_DIRECT.)
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDdirect.c

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

static_assert(H5FD_DIRECT_VALUE == H5_VFD_INVALID);
static_assert(H5_VFD_DIRECT == 8);
static_assert(MBOUNDARY_DEF == 4096);
static_assert(FBSIZE_DEF == 4096);
static_assert(CBSIZE_DEF == 16 * 1024 * 1024);

TEST(H5FDdirectTest, DisabledInDefaultBuildAndExportsSentinelConstants) {
#ifdef H5_HAVE_DIRECT
  ADD_FAILURE() << "Expected H5_HAVE_DIRECT to be disabled in default build.";
#endif

  EXPECT_EQ(H5I_INVALID_HID, H5FD_DIRECT);
  EXPECT_EQ(H5_VFD_INVALID, H5FD_DIRECT_VALUE);
  EXPECT_EQ(8, H5_VFD_DIRECT);
  EXPECT_EQ(4096, MBOUNDARY_DEF);
  EXPECT_EQ(4096, FBSIZE_DEF);
  EXPECT_EQ(16 * 1024 * 1024, CBSIZE_DEF);

  EXPECT_EQ(0, H5FDis_driver_registered_by_name("direct"));
  EXPECT_EQ(0, H5FDis_driver_registered_by_value(H5_VFD_DIRECT));
}

TEST(H5FDdirectTest, SelectingDisabledDirectDriverOnFaplFails) {
  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());

  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_driver(fapl.get(), H5FD_DIRECT, nullptr), 0);
  EXPECT_LT(H5Pset_driver_by_name(fapl.get(), "direct", nullptr), 0);
  EXPECT_LT(H5Pset_driver_by_value(fapl.get(), H5_VFD_DIRECT, nullptr), 0);
}

}  // namespace
}  // namespace hdf5
