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
// Tests UUID generation for OpenFileGDB (ogropenfilegdb_generate_uuid.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/ogropenfilegdb_generate_uuid.cpp

#include <cctype>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/ogr/ogrsf_frmts/openfilegdb/ogr_openfilegdb.h"
#include "third_party/gdal/port/cpl_conv.h"

namespace autotest2 {
namespace {

bool IsHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool IsValidUuidFormat(std::string_view uuid) {
  if (uuid.size() != 38) {
    return false;
  }
  if (uuid[0] != '{' || uuid[37] != '}') {
    return false;
  }
  if (uuid[9] != '-' || uuid[14] != '-' || uuid[19] != '-' || uuid[24] != '-') {
    return false;
  }
  if (uuid[15] != '4') {
    return false;
  }
  const char variant = uuid[20];
  if (variant != '8' && variant != '9' && variant != 'a' && variant != 'b') {
    return false;
  }
  for (std::size_t i = 1; i < 37; ++i) {
    if (i == 9 || i == 14 || i == 19 || i == 24) {
      continue;
    }
    if (!IsHexDigit(uuid[i])) {
      return false;
    }
  }
  return true;
}

TEST(OgrOpenFileGdbGenerateUuidTest, NonReproducibleUuid) {
  // bInit = true returns an empty string.
  EXPECT_TRUE(::OFGDBGenerateUUID(/*bInit=*/true).empty());

  const std::string uuid1 = ::OFGDBGenerateUUID(/*bInit=*/false);
  const std::string uuid2 = ::OFGDBGenerateUUID(/*bInit=*/false);

  EXPECT_TRUE(IsValidUuidFormat(uuid1)) << "Actual UUID: " << uuid1;
  EXPECT_TRUE(IsValidUuidFormat(uuid2)) << "Actual UUID: " << uuid2;
  EXPECT_NE(uuid1, uuid2);
}

TEST(OgrOpenFileGdbGenerateUuidTest, ReproducibleUuid) {
  CPLConfigOptionSetter setter("OPENFILEGDB_REPRODUCIBLE_UUID", "YES", false);

  // Initializing resets the counter and returns an empty string.
  EXPECT_TRUE(::OFGDBGenerateUUID(/*bInit=*/true).empty());

  const std::string uuid1 = ::OFGDBGenerateUUID(/*bInit=*/false);
  const std::string uuid2 = ::OFGDBGenerateUUID(/*bInit=*/false);

  EXPECT_TRUE(IsValidUuidFormat(uuid1)) << "Actual UUID: " << uuid1;
  EXPECT_TRUE(IsValidUuidFormat(uuid2)) << "Actual UUID: " << uuid2;
  EXPECT_NE(uuid1, uuid2);
  EXPECT_EQ('8', uuid1[20]);
  EXPECT_EQ('8', uuid2[20]);

  // Re-initialization resets the deterministic sequence.
  EXPECT_TRUE(::OFGDBGenerateUUID(/*bInit=*/true).empty());
  const std::string uuid1_again = ::OFGDBGenerateUUID(/*bInit=*/false);
  const std::string uuid2_again = ::OFGDBGenerateUUID(/*bInit=*/false);

  EXPECT_EQ(uuid1, uuid1_again);
  EXPECT_EQ(uuid2, uuid2_again);
}

}  // namespace
}  // namespace autotest2
