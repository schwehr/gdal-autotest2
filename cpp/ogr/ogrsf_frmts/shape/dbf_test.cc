// Copyright 2020 Google Inc. All Rights Reserved.
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
// Tests Shapelib dbf driver embedded in GDAL.
//
// See also:
//   https://github.com/OSGeo/shapelib
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_shape.py
//   https://gdal.org/drivers/vector/shapefile.html
//   apt-get install shapelib

#include <array>
#include <filesystem>  // NOLINT
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/ogr/ogrsf_frmts/shape/shapelib/shapefil.h"

namespace {

const std::filesystem::path kTestDataPath(
    "google3/third_party/gdal/autotest2/cpp/ogr/ogrsf_frmts/shape/testdata/"
    "dbf");

std::filesystem::path GetTestFilePath(const std::string_view filename) {
  return std::filesystem::path(::testing::SrcDir()) / kTestDataPath / filename;
}

constexpr int kIndexError{-1};

TEST(DbfTest, OpenDoesNotExist) {
  DBFHandle handle = DBFOpen("/does/not/exist", "rb");
  ASSERT_EQ(handle, nullptr);
}

// int.dbf created like this:
//
// dbfcreate int -n int_field 10 0
// dbfadd simple.dbf 2
// dbfadd simple.dbf 1
// dbfadd simple.dbf 0
// dbfadd simple.dbf -1
TEST(DbfTest, ReadSimpleInt) {
  constexpr int kFieldCount = 1;
  constexpr char kNativeFieldType = 'N';
  constexpr int kFieldNum = 0;
  constexpr char kFieldName[] = "int_field";
  constexpr int kRecordCount = 4;

  const auto filepath = GetTestFilePath("int.dbf");

  DBFHandle handle = DBFOpen(filepath.c_str(), "rb");
  ASSERT_NE(handle, nullptr);
  auto dbf_closer = absl::MakeCleanup([handle] { DBFClose(handle); });

  const int field_count = DBFGetFieldCount(handle);
  EXPECT_EQ(kFieldCount, field_count);
  EXPECT_EQ(kNativeFieldType, DBFGetNativeFieldType(handle, kFieldNum));

  char field_name[XBASE_FLDNAME_LEN_READ + 1];
  int width;
  int decimals;
  DBFGetFieldInfo(handle, kFieldNum, field_name, &width, &decimals);
  EXPECT_STREQ(field_name, kFieldName);
  EXPECT_EQ(10, width);
  EXPECT_EQ(0, decimals);

  EXPECT_EQ(kFieldNum, DBFGetFieldIndex(handle, kFieldName));
  // Fails to get a non-existent field.
  EXPECT_EQ(kIndexError, DBFGetFieldIndex(handle, "BOGUS"));

  const int record_count = DBFGetRecordCount(handle);
  ASSERT_EQ(kRecordCount, record_count);

  std::array<int, kRecordCount> fids = {0};
  for (int recNum = 0; recNum < kRecordCount; recNum++) {
    fids[recNum] = DBFReadIntegerAttribute(handle, recNum, kFieldNum);
  }
  EXPECT_THAT(fids, testing::ElementsAre(2, 1, 0, -1));
}

}  // namespace
