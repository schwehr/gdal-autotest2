// Copyright 2017 Google Inc.All Rights Reserved.
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
// Tests degrib within the GRIB raster driver.
//
// See also:
//   https://gdal.org/drivers/raster/grib.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/grib.py
//   https://github.com/OSGeo/gdal/tree/master/gdal/frmts/grib/degrib/degrib

#include <cstdlib>
#include <filesystem>  // NOLINT
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/frmts/grib/degrib/degrib/type.h"
#include "third_party/gdal/frmts/grib/gribdataset.h"
#include "third_party/gdal/port/cpl_vsi.h"
#include "third_party/gdal/frmts/grib/degrib/degrib/inventory.h"

namespace autotest2 {
namespace {

const std::filesystem::path kTestDataPath(
    "google3/third_party/gdal/autotest2/cpp/frmts/grib/degrib/testdata");

std::filesystem::path GetTestFilePath(const std::string_view filename) {
  return std::filesystem::path(::testing::SrcDir()) / kTestDataPath / filename;
}

// Code taken from gribdataset.cpp that made this private.
class InventoryWrapperGrib : public gdal::grib::InventoryWrapper {
 public:
  explicit InventoryWrapperGrib(VSILFILE *fp) : gdal::grib::InventoryWrapper() {
    result_ = GRIB2Inventory(fp, &inv_, &inv_len_, 0 /* all messages */,
                             &num_messages_);
  }

  ~InventoryWrapperGrib() override {
    if (inv_ == nullptr) return;
    for (uInt4 i = 0; i < inv_len_; i++) {
      GRIB2InventoryFree(inv_ + i);
    }
    free(inv_);
  }
};

// TODO(schwehr): Try nullptr for file.
// TODO(schwehr): Try an empty file.
// TODO(schwehr): Try a file with garbage in it.
// TODO(schwehr): Make a fuzzer.
// TODO(schwehr): Request fewer messages.  Does 2 or more work?

TEST(InventoryTest, RequestOnlyOneMessage) {
  const auto filepath = GetTestFilePath("constant_field.grib2");
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
  const auto filepath = GetTestFilePath("constant_field.grib2");
  auto file = VSIFOpenL(filepath.c_str(), "r");
  auto done = absl::MakeCleanup([file] { VSIFCloseL(file); });
  InventoryWrapperGrib inventories(file);
  ASSERT_EQ(1, inventories.length());
  ASSERT_EQ(1, inventories.num_messages());
}

TEST(InventoryTest, AllWith2By2) {
  const auto filepath = GetTestFilePath("test_uuid.grib2");
  auto file = VSIFOpenL(filepath.c_str(), "r");
  auto done = absl::MakeCleanup([file] { VSIFCloseL(file); });
  InventoryWrapperGrib inventories(file);
  ASSERT_EQ(2, inventories.length());
  ASSERT_EQ(2, inventories.num_messages());
}

TEST(InventoryTest, AllWith21By1) {
  const auto filepath = GetTestFilePath("multi_created.grib2");
  auto file = VSIFOpenL(filepath.c_str(), "r");
  auto done = absl::MakeCleanup([file] { VSIFCloseL(file); });
  InventoryWrapperGrib inventories(file);
  ASSERT_EQ(21, inventories.length());
  ASSERT_EQ(1, inventories.num_messages());
}

TEST(InventoryTest, RefTime) {
  const auto filepath = GetTestFilePath("constant_field.grib2");

  double ref_time = 0.0;
  EXPECT_EQ(0, GRIB2RefTime(filepath.c_str(), &ref_time));
  EXPECT_DOUBLE_EQ(1165320000.0, ref_time);
}

TEST(GRIB2SectToBuffer, BadLargeSectionLength) {
  const auto filepath =
      GetTestFilePath("oom-95dd3e1fee6828f1eb40a8b15c178b7fb7e769ca");
  auto file = VSIFOpenL(filepath.c_str(), "r");
  auto done = absl::MakeCleanup([file] { VSIFCloseL(file); });
  InventoryWrapperGrib inventories(file);
  // TODO(schwehr): Add symbols upstream for the error codes.
  EXPECT_EQ(-4, inventories.result());
}

// TODO(schwehr): more.

}  // namespace
}  // namespace autotest2
