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
//
// Tests ASCII Arc/Info (AAI) and GRASS ASCII raster driver.
//
// See also:
//   http://www.gdal.org/frmt_various.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/aaigrid.py

#include "frmts/aaigrid/aaigriddataset.h"

#include <memory>

#include "gunit.h"
#include "third_party/absl/memory/memory.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"

namespace autotest2 {
namespace {

TEST(AaiGridDatasetTest, IdentifyDoesNotExist) {
  auto open_info = std::make_unique<GDALOpenInfo>("/does_not_exist",
                                                  GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, AAIGDataset::Identify(open_info.get()));
}

TEST(GrassAsciiDatasetTest, IdentifyDoesNotExist) {
  auto open_info = std::make_unique<GDALOpenInfo>("/does_not_exist",
                                                  GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, GRASSASCIIDataset::Identify(open_info.get()));
}

// TODO(schwehr): more.

}  // namespace
}  // namespace autotest2
