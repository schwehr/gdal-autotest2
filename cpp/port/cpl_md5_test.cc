// Copyright 2017 Google Inc. All Rights Reserved.
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

// Tests for GDAL's C MD5 API (derived from CVS).
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_md5.cpp
//

#include "third_party/gdal/port/cpl_md5.h"

#include "testing/base/public/gunit.h"

namespace autotest2 {
namespace {

TEST(CplMd5Test, CPLMD5String) {
  EXPECT_STREQ("d41d8cd98f00b204e9800998ecf8427e", CPLMD5String(""));
  EXPECT_STREQ("cfcd208495d565ef66e7dff9f98764da", CPLMD5String("0"));
  EXPECT_STREQ("64f01cf3958c3c87bd983b43ba3d603b",
               CPLMD5String("\x01\x23\xee\xff"));
  EXPECT_STREQ("0d9af13ecf5bcb599e2304a8d2616f3b",
               CPLMD5String("--------------------------------------------------"
                            "-----------------------------------------------"));
  // A 56-byte input triggers the count < 8 two-block padding branch in
  // CPLMD5Final.
  EXPECT_STREQ("a68f061e81239660f6305195739ba7f0",
               CPLMD5String("0123456789abcdef0123456789abcdef"
                            "0123456789abcdef01234567"));
}

TEST(CplMd5Test, IncrementalUpdate) {
  struct CPLMD5Context context;
  CPLMD5Init(&context);
  // First update with 10 bytes (sets t = 10).
  CPLMD5Update(&context, "0123456789", 10);
  // Second update with 10 bytes (< 64 - 10 = 54, hits len < t branch).
  CPLMD5Update(&context, "abcdef0123", 10);
  // Third update with 60 bytes (>= 64 - 20 = 44, hits the transform &
  // remaining branch).
  CPLMD5Update(&context,
               "456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
               60);
  unsigned char digest[16];
  CPLMD5Final(digest, &context);

  struct CPLMD5Context single_context;
  CPLMD5Init(&single_context);
  CPLMD5Update(
      &single_context,
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef01234567"
      "89abcdef",
      80);
  unsigned char expected_digest[16];
  CPLMD5Final(expected_digest, &single_context);

  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(expected_digest[i], digest[i]);
  }
}

}  // namespace
}  // namespace autotest2
