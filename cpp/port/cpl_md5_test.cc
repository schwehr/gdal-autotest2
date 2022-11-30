// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Tests for GDAL's C MD5 API (derived from CVS).
// Yes, there is also a cplstring.cc tested in cplstring_test.cc.
//
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_md5.cpp
//

#include "port/cpl_md5.h"

#include "gunit.h"
#include "port/cpl_port.h"

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
}

}  // namespace
}  // namespace autotest2
