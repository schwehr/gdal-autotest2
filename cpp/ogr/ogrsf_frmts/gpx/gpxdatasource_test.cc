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
// Tests ESRI Shapefile OGR driver.
//
// See also:
//   http://www.gdal.org/drv_gpx.html
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_gpx.py

#include "port/cpl_port.h"

#include <memory>
#include <string>

#include "file/base/path.h"
#include "googletest.h"
#include "gunit.h"
#include "third_party/absl/memory/memory.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogr_api.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_geometry.h"
#include "ogr/ogr_feature.h"
#include "ogr/ogrsf_frmts/gpx/ogr_gpx.h"
#include "ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "port/cpl_error.h"

namespace autotest2 {
namespace {

const char kTestData[] = "cpp/ogr/ogrsf_frmts/gpx/testdata/";

TEST(GPxTest, Open) {
  const string filepath =
      file::JoinPath(FLAGS_test_srcdir, kTestData,
                     "track_with_time_extension.gpx");

  auto src = gtl::MakeUnique<OGRGPXDataSource>();
  EXPECT_EQ(TRUE, src->Open(filepath.c_str(), FALSE /* bUpdateIn */));

  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CPLE_None, CPLGetLastErrorType());

  // TODO(schwehr): Test more.
}

// TODO(schwehr): Write more tests.

}  // namespace
}  // namespace autotest2
