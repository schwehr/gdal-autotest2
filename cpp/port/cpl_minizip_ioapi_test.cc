// Tests for cpl_minizip_ioapi:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_minizip_ioapi.h
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_minizip_ioapi.cpp
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

#include "third_party/gdal/port/cpl_minizip_ioapi.h"

#include <cstring>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(CplMinizipIoapiTest, FileFuncOperations) {
  zlib_filefunc_def filefunc = {};
  cpl_fill_fopen_filefunc(&filefunc);
  ASSERT_NE(nullptr, filefunc.zopen_file);
  ASSERT_NE(nullptr, filefunc.zread_file);
  ASSERT_NE(nullptr, filefunc.zwrite_file);
  ASSERT_NE(nullptr, filefunc.ztell_file);
  ASSERT_NE(nullptr, filefunc.zseek_file);
  ASSERT_NE(nullptr, filefunc.zclose_file);
  ASSERT_NE(nullptr, filefunc.zerror_file);
  EXPECT_EQ(nullptr, filefunc.opaque);

  const char* filename = "/vsimem/cpl_minizip_ioapi_test.bin";

  // Invalid mode or null filename.
  EXPECT_EQ(nullptr,
            filefunc.zopen_file(nullptr, nullptr, ZLIB_FILEFUNC_MODE_CREATE));
  EXPECT_EQ(nullptr, filefunc.zopen_file(nullptr, filename, 0));

  // Create file and write.
  voidpf stream =
      filefunc.zopen_file(nullptr, filename, ZLIB_FILEFUNC_MODE_CREATE);
  ASSERT_NE(nullptr, stream);
  EXPECT_EQ(5u, filefunc.zwrite_file(nullptr, stream, "hello", 5));
  EXPECT_EQ(5u, filefunc.ztell_file(nullptr, stream));
  EXPECT_EQ(0, filefunc.zerror_file(nullptr, stream));
  EXPECT_EQ(0, filefunc.zclose_file(nullptr, stream));

  // Open existing (r+b) and seek/write.
  stream = filefunc.zopen_file(nullptr, filename, ZLIB_FILEFUNC_MODE_EXISTING);
  ASSERT_NE(nullptr, stream);
  EXPECT_EQ(0, filefunc.zseek_file(nullptr, stream, 0, ZLIB_FILEFUNC_SEEK_END));
  EXPECT_EQ(5u, filefunc.ztell_file(nullptr, stream));
  EXPECT_EQ(6u, filefunc.zwrite_file(nullptr, stream, " world", 6));
  EXPECT_EQ(0, filefunc.zclose_file(nullptr, stream));

  // Open read-only (rb) and test seek modes + read.
  stream = filefunc.zopen_file(nullptr, filename, ZLIB_FILEFUNC_MODE_READ);
  ASSERT_NE(nullptr, stream);
  EXPECT_EQ(0, filefunc.zseek_file(nullptr, stream, 6, ZLIB_FILEFUNC_SEEK_SET));
  char buf[6] = {};
  EXPECT_EQ(5u, filefunc.zread_file(nullptr, stream, buf, 5));
  EXPECT_STREQ("world", buf);

  EXPECT_EQ(0, filefunc.zseek_file(nullptr, stream, 0, ZLIB_FILEFUNC_SEEK_SET));
  EXPECT_EQ(0, filefunc.zseek_file(nullptr, stream, 2, ZLIB_FILEFUNC_SEEK_CUR));
  EXPECT_EQ(2u, filefunc.ztell_file(nullptr, stream));

  // Invalid seek origin returns -1.
  EXPECT_EQ(-1, filefunc.zseek_file(nullptr, stream, 0, 9999));

  EXPECT_EQ(0, filefunc.zclose_file(nullptr, stream));
  EXPECT_EQ(0, VSIUnlink(filename));
}

}  // namespace
}  // namespace autotest2
