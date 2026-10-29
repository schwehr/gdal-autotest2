// Copyright 2026 Google Inc. All Rights Reserved.
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
// Tests gcore/gdal_adbc.cpp.

#include "third_party/gdal/gcore/gdal_adbc.h"

#include <cstdint>

#include "testing/base/public/gunit.h"

namespace {

uint8_t DummyAdbcLoadDriver(const char* driver_name, const char* entrypoint,
                            int version, void* driver, void* error) {
  return 0;
}

TEST(GdalAdbcTest, SetAndGetAdbcLoadDriverOverride) {
  EXPECT_EQ(nullptr, GDALGetAdbcLoadDriverOverride());
  GDALSetAdbcLoadDriverOverride(&DummyAdbcLoadDriver);
  EXPECT_EQ(&DummyAdbcLoadDriver, GDALGetAdbcLoadDriverOverride());
  GDALSetAdbcLoadDriverOverride(nullptr);
  EXPECT_EQ(nullptr, GDALGetAdbcLoadDriverOverride());
}

}  // namespace
