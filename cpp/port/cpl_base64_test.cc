// Tests public base64 functions:
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_string.h
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_base64.cpp
//
// Copyright 2014 Google Inc. All Rights Reserved.
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

#include "gunit.h"
#include "gcore/gdal_priv.h"
#include "port/cpl_string.h"

namespace {

TEST(CplBase64Test, Base64RoundTrip) {
  const std::string src("azAZ09~+`=");
  char *dst = CPLBase64Encode(src.length(),
                              reinterpret_cast<const GByte *>(src.c_str()));
  ASSERT_STREQ("YXpBWjA5fitgPQ==", dst);

  const int result = CPLBase64DecodeInPlace(reinterpret_cast<GByte*>(dst));
  ASSERT_EQ(10, result);
  dst[result] = '\0';
  ASSERT_STREQ(dst, src.c_str());
  CPLFree(dst);
}

TEST(CplBase64Test, Base64CornerCases) {
  char empty_dst[] = "";
  ASSERT_EQ(0, CPLBase64DecodeInPlace(reinterpret_cast<GByte*>(empty_dst)));

  char invalid_dst[] = "~";
  ASSERT_EQ(0, CPLBase64DecodeInPlace(reinterpret_cast<GByte*>(invalid_dst)));
}

}  // namespace
