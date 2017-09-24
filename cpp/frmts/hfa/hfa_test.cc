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
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/hfa_read.py
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/hfa_rfc40.py
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/hfa_srs.py
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/hfa_write.py
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/gdrivers/hfa.py

#include <memory>
#include <string>

#include "gunit.h"
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
    "autotest2/cpp/frmts/hfa/testdata/";

class HfaTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_HFA(); }
};

string SrsToString(const OGRSpatialReference &src_srs) {
  char *pszPrettyWkt = NULL;
  src_srs.exportToPrettyWkt(&pszPrettyWkt, false);
  string result(pszPrettyWkt);
  CPLFree(pszPrettyWkt);
  return result;
}

TEST_F(HfaTest, BasicGdalOpenEx) {
  const string filepath =
      FLAGS_test_srcdir + string(kTestData) + "utmsmall.img";
  unique_ptr<GDALDataset> src(static_cast<GDALDataset *>(
      GDALOpenEx(filepath.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CPLE_None, CPLGetLastErrorType());
  ASSERT_NE(nullptr, src);

  string projection(src->GetProjectionRef());

  OGRSpatialReference src_srs(projection.c_str());
  OGRSpatialReference expected_srs;
  int epsg = 26711;
  ASSERT_EQ(OGRERR_NONE, expected_srs.importFromEPSG(epsg))
      << "Failed to load epsg: " << epsg;
  EXPECT_TRUE(expected_srs.IsSame(&src_srs))
      << "Failed for EPSG: " << epsg << "\n"
      << "See http://spatialreference.org/ref/epsg/" << epsg << "\n"
      << SrsToString(src_srs) << "\n\n!=\n\n"
      << SrsToString(expected_srs);
}

}  // namespace
}  // namespace autotest2
