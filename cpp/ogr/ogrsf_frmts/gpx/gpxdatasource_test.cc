// Copyright 2017 Google Inc. All Rights Reserved.
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
// Tests GPX (GPS Exchange Format) OGR driver.
//
// See also:
//   https://gdal.org/drivers/vector/gpx.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_gpx.py
//   https://en.wikipedia.org/wiki/GPS_Exchange_Format

#include <filesystem>  // NOLINT
#include <memory>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/ogr/ogrsf_frmts/gpx/ogr_gpx.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

const std::filesystem::path kTestDataPath(
    "google3/third_party/gdal/autotest2/cpp/ogr/ogrsf_frmts/gpx/testdata");

std::filesystem::path GetTestFilePath(const std::string_view filename) {
  return std::filesystem::path(::testing::SrcDir()) / kTestDataPath / filename;
}

TEST(GpxTest, Open) {
  const auto filepath = GetTestFilePath("track_with_time_extension.gpx");

  auto src = std::make_unique<OGRGPXDataSource>();

  auto open_info = std::make_unique<GDALOpenInfo>(filepath.c_str(),
                                                  GDAL_OF_READONLY, nullptr);
  ASSERT_NE(nullptr, open_info);

  EXPECT_EQ(TRUE, src->Open(open_info.get()));

  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CPLE_None, CPLGetLastErrorType());
}

// TODO(schwehr): Write more tests.

}  // namespace
}  // namespace autotest2
