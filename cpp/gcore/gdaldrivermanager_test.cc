// Test gcore/gdaldrivermanager.cpp.
//
// Copyright 2016 Google Inc. All Rights Reserved.
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

#include <unordered_set>

#include "gunit.h"
#include "autotest2/cpp/util/error_handler.h"
#include "gcore/gdal_priv.h"

namespace {

// Remove all drivers from the current GDAL environment.
void DeregisterAllDrivers() {
  GDALDriverManager *manager = GetGDALDriverManager();

  std::unordered_set<GDALDriver *> drivers;
  const int driver_count = manager->GetDriverCount();
  for (int driver_num = 0; driver_num < driver_count; ++driver_num) {
    drivers.insert(manager->GetDriver(driver_num));
  }
  for (GDALDriver *driver : drivers) {
    manager->DeregisterDriver(driver);
    delete(driver);
  }
}


// A test fixture is used because {TODO(schwehr): put reason here}.
class GDALDriverManagerTest : public ::testing::Test {
 protected:
  GDALDriverManagerTest() : manager_(GetGDALDriverManager()) {
    DeregisterAllDrivers();
  }

  GDALDriverManager *manager_;
};

// This test checks loading all gives a reasonable number of drivers.
// TODO(schwehr): Make this test more general.
TEST_F(GDALDriverManagerTest, DISABLED_DriverCount) {
  // Never expect this to happen, but good to check at least once.
  ASSERT_NE(nullptr, manager_);

  // No drivers should be registered at this point.
  ASSERT_EQ(0, manager_->GetDriverCount());

  GDALAllRegister();

  // Must have at least 1 vector and 1 raster driver.
  EXPECT_LT(2, manager_->GetDriverCount());
  // Cannot have more than some rediculously high number of drivers.
  EXPECT_GT(300, manager_->GetDriverCount());

  // If we know how many drivers should be available.
  // If you enable more drivers, set this number to the new total number of
  // drivers and make sure that there are C++ and Python tests for
  // every active driver.
  EXPECT_EQ(31, manager_->GetDriverCount());
}

// This test verifies that a single driver config works.
TEST_F(GDALDriverManagerTest, AddRemoveSingleDriver) {
  ASSERT_EQ(0, manager_->GetDriverCount());
  GDALRegister_VRT();
  ASSERT_EQ(1, manager_->GetDriverCount());

  GDALDriver *driver = manager_->GetDriver(0);
  string driver_name = driver->GetDescription();
  EXPECT_STREQ("VRT", driver_name.c_str());
  EXPECT_EQ(driver, manager_->GetDriverByName(driver_name.c_str()));

  manager_->DeregisterDriver(driver);
  delete driver;
  driver = nullptr;

  ASSERT_EQ(0, manager_->GetDriverCount());
  EXPECT_EQ(nullptr, manager_->GetDriver(0));
  EXPECT_EQ(nullptr, manager_->GetDriverByName(driver_name.c_str()));
}

// This test make sure this can remove one of many drivers.
TEST_F(GDALDriverManagerTest, RemoveSingleDriver) {
  GDALAllRegister();
  const int driver_count = manager_->GetDriverCount();
  ASSERT_LT(2, driver_count);

  const string name = "VRT";
  GDALDriver *driver = manager_->GetDriverByName(name.c_str());
  string driver_name = driver->GetDescription();
  EXPECT_STREQ(name.c_str(), driver->GetDescription());

  manager_->DeregisterDriver(driver);
  delete driver;
  driver = nullptr;

  EXPECT_EQ(driver_count - 1, manager_->GetDriverCount());
  EXPECT_EQ(nullptr, manager_->GetDriverByName(name.c_str()));
}

}  // namespace
