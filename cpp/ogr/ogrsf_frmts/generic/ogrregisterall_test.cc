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

// Tests for OGRRegisterAll and OGRRegisterAllInternal.
//
// See also:
// https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/generic/ogrregisterall.cpp
// http://google3/third_party/gdal/ogr/ogrsf_frmts/generic/ogrregisterall.cpp

#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"

namespace autotest2 {
namespace {

TEST(OGRRegisterAllTest, RegisterAllDrivers) {
  OGRRegisterAll();

  auto* driver_manager = GetGDALDriverManager();
  ASSERT_NE(nullptr, driver_manager);
  EXPECT_GT(driver_manager->GetDriverCount(), 0);

  // Check that core vector drivers are available.
  const std::vector<const char*> expected_drivers = {
      "MEM", "GeoJSON", "CSV", "ESRI Shapefile", "VRT",
  };

  for (const char* driver_name : expected_drivers) {
    EXPECT_NE(nullptr, driver_manager->GetDriverByName(driver_name))
        << "Expected driver: " << driver_name;
  }
}

TEST(OGRRegisterAllTest, RegisterAllInternalDirect) {
  // Test direct invocation of OGRRegisterAllInternal().
  OGRRegisterAllInternal();

  auto* driver_manager = GetGDALDriverManager();
  ASSERT_NE(nullptr, driver_manager);
  EXPECT_GT(driver_manager->GetDriverCount(), 0);
}

// TODO: b/335317901 - Cover remaining error branches in ogrregisterall.cpp.

}  // namespace
}  // namespace autotest2
