// Copyright 2017 Google Inc.All Rights Reserved.
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
//
// Tests degrib within the GRIB raster driver.
//
// See also:
//   http://www.gdal.org/frmt_grib.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/grib.py
//   https://github.com/OSGeo/gdal/tree/master/gdal/frmts/grib/degrib/degrib
#include "port/cpl_vsi.h"
#include "frmts/grib/degrib/degrib/inventory.h"

#include <string>

#include "logging.h"
#include "file/base/path.h"
#include "gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/flags/flag.h"
#include "third_party/absl/memory/memory.h"
#include "frmts/grib/gribdataset.h"

namespace autotest2 {
namespace {

using gdal::grib::InventoryWrapper;

const char kTestData[] =
    "/google3/third_party/gdal/autotest2/cpp/frmts/grib/degrib/testdata/";

// TODO(schwehr): Try nullptr for file.
// TODO(schwehr): Try an empty file.
// TODO(schwehr): Try a file with garbage in it.
// TODO(schwehr): Make a fuzzer.
// TODO(schwehr): Request fewer messages.  Does 2 or more work?

TEST(InventoryTest, RequestOnlyOneMessage) {
  const std::string filepath = file::JoinPath(
      absl::GetFlag(FLAGS_test_srcdir), kTestData, "constant_field.grib2");
  auto grib = VSIFOpenL(filepath.c_str(), "r");
  auto done = absl::MakeCleanup([grib] { VSIFCloseL(grib); });

  inventoryType *inv = nullptr;
  uInt4 inv_len = 0;
  int num_messages = 0;

  // TODO(schwehr): Make a free wrapper for inventoryType array.
  EXPECT_EQ(1, GRIB2Inventory(grib, &inv, &inv_len, 1, &num_messages));

  // Debugging dump to stdout.
  if (inv) GRIB2InventoryPrint(inv, inv_len);

  ASSERT_NE(nullptr, inv);
  auto done2 = absl::MakeCleanup([inv] {
    GRIB2InventoryFree(inv);
    free(inv);
  });

  ASSERT_EQ(1, inv_len);
  ASSERT_EQ(1, num_messages);

  EXPECT_EQ(2, inv->GribVersion);
  EXPECT_EQ(0, inv->start);
  EXPECT_EQ(1, inv->msgNum);
  EXPECT_EQ(0, inv->subgNum);
  EXPECT_DOUBLE_EQ(1165320000.0, inv->refTime);
  EXPECT_DOUBLE_EQ(1165341600.0, inv->validTime);
  EXPECT_STREQ("SPFH", inv->element);
  EXPECT_STREQ("Specific humidity [kg/kg]", inv->comment);
  EXPECT_STREQ("[kg/kg]", inv->unitName);
  EXPECT_DOUBLE_EQ(21600.0, inv->foreSec);
  EXPECT_STREQ("1-HYBL", inv->shortFstLevel);
  EXPECT_STREQ("1[-] HYBL=\"Hybrid level\"", inv->longFstLevel);
}

TEST(InventoryTest, AllWithOnlyOne) {
  const std::string filepath = file::JoinPath(
      absl::GetFlag(FLAGS_test_srcdir), kTestData, "constant_field.grib2");
  auto file = VSIFOpenL(filepath.c_str(), "r");
  auto done = absl::MakeCleanup([file] { VSIFCloseL(file); });
  InventoryWrapper inventories(file);
  ASSERT_EQ(1, inventories.length());
  ASSERT_EQ(1, inventories.num_messages());
}

TEST(InventoryTest, AllWith2By2) {
  const std::string filepath = file::JoinPath(absl::GetFlag(FLAGS_test_srcdir),
                                              kTestData, "test_uuid.grib2");
  auto file = VSIFOpenL(filepath.c_str(), "r");
  auto done = absl::MakeCleanup([file] { VSIFCloseL(file); });
  InventoryWrapper inventories(file);
  ASSERT_EQ(2, inventories.length());
  ASSERT_EQ(2, inventories.num_messages());
}

TEST(InventoryTest, AllWith21By1) {
  const std::string filepath = file::JoinPath(absl::GetFlag(FLAGS_test_srcdir),
                                              kTestData, "multi_created.grib2");
  auto file = VSIFOpenL(filepath.c_str(), "r");
  auto done = absl::MakeCleanup([file] { VSIFCloseL(file); });
  InventoryWrapper inventories(file);
  ASSERT_EQ(21, inventories.length());
  ASSERT_EQ(1, inventories.num_messages());
}

TEST(InventoryTest, RefTime) {
  const std::string filepath = file::JoinPath(
      absl::GetFlag(FLAGS_test_srcdir), kTestData, "constant_field.grib2");

  double ref_time = 0.0;
  EXPECT_EQ(0, GRIB2RefTime(filepath.c_str(), &ref_time));
  EXPECT_DOUBLE_EQ(1165320000.0, ref_time);
}

TEST(GRIB2SectToBuffer, BadLargeSectionLength) {
  const std::string filepath =
      file::JoinPath(absl::GetFlag(FLAGS_test_srcdir), kTestData,
                     "oom-95dd3e1fee6828f1eb40a8b15c178b7fb7e769ca");
  auto file = VSIFOpenL(filepath.c_str(), "r");
  auto done = absl::MakeCleanup([file] { VSIFCloseL(file); });
  InventoryWrapper inventories(file);
  // TODO(schwehr): Add symbols upstream for the error codes.
  EXPECT_EQ(-4, inventories.result());
}

// TODO(schwehr): more.

}  // namespace
}  // namespace autotest2
