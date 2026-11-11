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

// Smoke test for //third_party/netcdf/libdispatch/dreg.c.
//
// dreg.c only defines getmountpoint() when _WIN32 is defined, so it compiles
// to zero executable lines on Linux/POSIX targets. This test verifies that the
// declaring header (include/ncpathmgr.h) compiles cleanly under
// :private_headers and C++20.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dreg.c

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstddef>
#include <type_traits>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"

extern "C" {
#include "third_party/netcdf/include/ncpathmgr.h"
}

namespace netcdf {
namespace {

TEST(DregTest, PathManagerHeaderDeclaresGetmountpointAndReportsPosixKind) {
  static_assert(
      std::is_same_v<decltype(&getmountpoint), int (*)(char*, size_t)>);
#ifndef _WIN32
  EXPECT_EQ(NCPD_NIX, NCgetlocalpathkind());
#endif
}

}  // namespace
}  // namespace netcdf
