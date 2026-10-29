// Tests public base64 functions:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_string.h
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_base64.cpp
//
// Copyright 2014 Google Inc. All Rights Reserved.
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

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace {

TEST(CplBase64Test, Base64RoundTrip) {
  const std::string src("azAZ09~+`=");
  char* dst = CPLBase64Encode(src.length(),
                              reinterpret_cast<const GByte*>(src.c_str()));
  ASSERT_STREQ("YXpBWjA5fitgPQ==", dst);

  const int result = CPLBase64DecodeInPlace(reinterpret_cast<GByte*>(dst));
  ASSERT_EQ(10, result);
  dst[result] = '\0';
  ASSERT_STREQ(dst, src.c_str());
  CPLFree(dst);
}

TEST(CplBase64Test, Base64CornerCases) {
  ASSERT_EQ(0, CPLBase64DecodeInPlace(nullptr));

  char empty_dst[] = "";
  ASSERT_EQ(0, CPLBase64DecodeInPlace(reinterpret_cast<GByte*>(empty_dst)));

  char invalid_dst[] = "~";
  ASSERT_EQ(0, CPLBase64DecodeInPlace(reinterpret_cast<GByte*>(invalid_dst)));

  // A complete, padded 4-char group decodes to exactly 3 bytes.
  char padded_group[] = "YWJj";
  ASSERT_EQ(3, CPLBase64DecodeInPlace(reinterpret_cast<GByte*>(padded_group)));
  ASSERT_EQ('a', padded_group[0]);
  ASSERT_EQ('b', padded_group[1]);
  ASSERT_EQ('c', padded_group[2]);

  // Unpadded inputs (length mod 4 == 1, 2, 3).  The canonical decoded lengths
  // are 0, 1 and 2 bytes, but CPLBase64DecodeInPlace() over-counts: it treats
  // missing trailing characters as 'A' (not as '=' padding) and only stops
  // writing once the output length catches up with the input length.  The
  // leading decoded bytes are still correct; the trailing bytes are zero.
  // These expectations pin that quirk, so a future GDAL upgrade that returns
  // the canonical length is an improvement, not a regression.
  char unpadded2[] = "YQ";
  ASSERT_EQ(2, CPLBase64DecodeInPlace(reinterpret_cast<GByte*>(unpadded2)));
  ASSERT_EQ('a', unpadded2[0]);
  ASSERT_EQ('\0', unpadded2[1]);

  char unpadded3[] = "YWI";
  ASSERT_EQ(3, CPLBase64DecodeInPlace(reinterpret_cast<GByte*>(unpadded3)));
  ASSERT_EQ('a', unpadded3[0]);
  ASSERT_EQ('b', unpadded3[1]);
  ASSERT_EQ('\0', unpadded3[2]);

  char unpadded1[] = "Y";
  ASSERT_EQ(1, CPLBase64DecodeInPlace(reinterpret_cast<GByte*>(unpadded1)));
}

}  // namespace
