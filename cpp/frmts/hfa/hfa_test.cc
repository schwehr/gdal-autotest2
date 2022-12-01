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
//
// Tests HFA Erdas Imagine raster driver.
//
// See also:
//   http://www.gdal.org/frmt_hfa.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gcore/hfa_read.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gcore/hfa_rfc40.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gcore/hfa_srs.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gcore/hfa_write.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/hfa.py

#include <stddef.h>

#include <memory>
#include <string>

#include "commandlineflags_declare.h"
#include "gmock.h"
#include "googletest.h"
#include "gunit.h"
#include "third_party/absl/flags/flag.h"
#include "autotest2/cpp/util/matchers.h"
#include "gcore/gdal.h"
#include "gcore/gdal_frmts.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_spatialref.h"
#include "port/cpl_conv.h"
#include "port/cpl_error.h"

using std::unique_ptr;

namespace autotest2 {
namespace {

const char kTestData[] =
    "/google3/third_party/gdal/autotest2/cpp/frmts/hfa/testdata/";

class HfaTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_HFA(); }
};

TEST_F(HfaTest, BasicGdalOpenEx) {
  const std::string filepath = absl::GetFlag(FLAGS_test_srcdir) +
                               std::string(kTestData) + "utmsmall.img";
  unique_ptr<GDALDataset> src(static_cast<GDALDataset *>(
      GDALOpenEx(filepath.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CPLE_None, CPLGetLastErrorType());
  ASSERT_NE(nullptr, src);

  std::string projection(src->GetProjectionRef());

  OGRSpatialReference src_srs(projection.c_str());
  OGRSpatialReference expected_srs;
  int epsg = 26711;
  ASSERT_EQ(OGRERR_NONE, expected_srs.importFromEPSG(epsg))
      << "Failed to load epsg: " << epsg;
  EXPECT_THAT(src_srs, IsSameAs(expected_srs))
      << "Failed for EPSG: " << epsg << "\n"
      << "See http://spatialreference.org/ref/epsg/" << epsg;
}

}  // namespace
}  // namespace autotest2
