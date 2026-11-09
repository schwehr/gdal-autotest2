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
// Tests Windows VFD public definitions and disabled-feature sentinels
// (third_party/hdf5/src/H5FDwindows.c).
//
// Build caveat: H5FDwindows.c has LF=0 in the default google3 non-MPI Linux
// build because H5_HAVE_WINDOWS is undefined in H5pubconf.h.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5FDwindows.c

#include "third_party/hdf5/src/H5FDwindows.h"

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/src/hdf5.h"

namespace hdf5 {
namespace {

TEST(H5FdWindowsTest, WindowsMacroAliasesSec2WhenDisabledOnLinux) {
#ifdef H5_HAVE_WINDOWS
  FAIL() << "Expected H5_HAVE_WINDOWS to be undefined on Linux";
#endif
  ASSERT_GE(H5open(), 0);
  EXPECT_NE(H5FD_WINDOWS, H5I_INVALID_HID);
  EXPECT_EQ(H5FD_WINDOWS, H5FD_SEC2);
  EXPECT_EQ(H5Iget_type(H5FD_WINDOWS), H5I_VFL);
}

TEST(H5FdWindowsTest, WindowsDriverNameIsNotRegisteredAndSetByNameFails) {
  EXPECT_EQ(H5FDis_driver_registered_by_name("windows"), 0);

  ScopedPlist fapl(H5Pcreate(H5P_FILE_ACCESS));
  ASSERT_TRUE(fapl.valid());
  ASSERT_GE(H5Pset_fapl_sec2(fapl.get()), 0);
  EXPECT_EQ(H5Pget_driver(fapl.get()), H5FD_WINDOWS);

  ScopedErrorSilence silence;
  EXPECT_LT(H5Pset_driver_by_name(fapl.get(), "windows", nullptr), 0);
  EXPECT_EQ(H5Pget_driver(fapl.get()), H5FD_WINDOWS);
}

}  // namespace
}  // namespace hdf5
