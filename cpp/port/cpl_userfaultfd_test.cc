// Tests for cpl_userfaultfd:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_userfaultfd.h
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_userfaultfd.cpp
//
// Copyright 2026 Google Inc. All Rights Reserved.
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
// The userfaultfd implementation in cpl_userfaultfd.cpp is only compiled when
// ENABLE_UFFD is defined, which is not the case in the google3 GDAL build. In
// that configuration CPLIsUserFaultMappingSupported() and friends are not
// linked into port_lib, so only the header is exercised here. The real test
// below becomes active if ENABLE_UFFD is ever enabled.

// cpl_userfaultfd.h is not self-contained: it uses CPL_DLL without including
// cpl_port.h, so cpl_port.h must come first.
// clang-format off
#include "third_party/gdal/port/cpl_port.h"  // IWYU pragma: keep
#include "third_party/gdal/port/cpl_userfaultfd.h"
// clang-format on

#include "testing/base/public/gunit.h"

#ifdef ENABLE_UFFD
#include <cstdint>
#include <string>

#include "third_party/gdal/port/cpl_vsi.h"
#endif  // ENABLE_UFFD

namespace autotest2 {
namespace {

// The config option name used to cap the number of concurrent mappings.
TEST(CplUserfaultfdTest, HeaderConstants) {
  EXPECT_STREQ("GDAL_UFFD_LIMIT", GDAL_UFFD_LIMIT);
}

#ifdef ENABLE_UFFD
TEST(CplUserfaultfdTest, CreateAndDeleteMapping) {
  if (!CPLIsUserFaultMappingSupported()) {
    GTEST_SKIP() << "userfaultfd is not available in this kernel/sandbox.";
  }

  // userfaultfd mappings only work on real files, not /vsimem/.
  const std::string path = std::string(::testing::TempDir()) + "/uffd.bin";
  const std::string data(8192, 'u');
  {
    VSILFILE* fp = VSIFOpenL(path.c_str(), "wb");
    ASSERT_NE(nullptr, fp);
    ASSERT_EQ(data.size(), VSIFWriteL(data.data(), 1, data.size(), fp));
    ASSERT_EQ(0, VSIFCloseL(fp));
  }

  void* vma = nullptr;
  uint64_t vma_size = 0;
  cpl_uffd_context* ctx =
      CPLCreateUserFaultMapping(path.c_str(), &vma, &vma_size);
  ASSERT_NE(nullptr, ctx);
  ASSERT_NE(nullptr, vma);
  EXPECT_GE(vma_size, data.size());
  // Touching the mapping services a page fault from the file contents.
  EXPECT_EQ('u', static_cast<const char*>(vma)[0]);
  EXPECT_EQ('u', static_cast<const char*>(vma)[data.size() - 1]);
  CPLDeleteUserFaultMapping(ctx);
  VSIUnlink(path.c_str());
}
#endif  // ENABLE_UFFD

}  // namespace
}  // namespace autotest2
