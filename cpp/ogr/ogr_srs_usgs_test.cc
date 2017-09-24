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
// This is a complete rewrite of a file licensed as follows:
//
// Copyright (c) 2004, Andrey Kiselev <dron@remotesensing.org>
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

// https://trac.osgeo.org/gdal/browser/trunk/autotest/osr/osr_usgs.py

#include <string>

#include "gmock.h"
#include "gunit.h"
#include "autotest2/cpp/util/cpl_cstringlist.h"
#include "autotest2/cpp/util/error_handler.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_spatialref.h"
#include "ogr/ogr_srs_api.h"
#include "port/cpl_conv.h"
#include "port/cpl_csv.h"
#include "port/cpl_error.h"
#include "port/cpl_port.h"
#include "port/cpl_string.h"
#include "port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(UsgsTest, NoPrjParams) {
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromUSGS(8, 0, nullptr, 15, TRUE));

  EXPECT_EQ(OGRERR_CORRUPT_DATA, OSRImportFromUSGS(&srs, 8, 0, nullptr, 15));
}

TEST(UsgsTest, Wgs84) {
  OGRSpatialReference wgs84;
  wgs84.importFromEPSG(4326);

  long proj_sys = 0;
  long zone = 0;
  double *prj_param_out = nullptr;
  long datum = 0;
  EXPECT_EQ(OGRERR_NONE,
            wgs84.exportToUSGS(&proj_sys, &zone, &prj_param_out, &datum));

  EXPECT_DOUBLE_EQ(0, proj_sys);
  EXPECT_DOUBLE_EQ(0, zone);
  EXPECT_DOUBLE_EQ(0, prj_param_out[0]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[1]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[2]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[3]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[4]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[5]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[6]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[7]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[8]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[9]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[10]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[11]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[12]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[13]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[14]);

  EXPECT_EQ(12, datum);

  CPLFree(prj_param_out);
}

TEST(UsgsTest, Krassowsky) {
  OGRSpatialReference srs;

  double prj_params[] = {0.0,
                         0.0,
                         CPLDecToPackedDMS(47.0),
                         CPLDecToPackedDMS(62.0),
                         CPLDecToPackedDMS(45.0),
                         CPLDecToPackedDMS(54.5),
                         0.0,
                         0.0,
                         1.0,
                         0.0,
                         0.0,
                         0.0,
                         0.0,
                         0.0,
                         0.0};
  srs.importFromUSGS(8, 0, prj_params, 15, TRUE);
  char *wkt = nullptr;
  srs.exportToWkt(&wkt);

  EXPECT_THAT(string(wkt), testing::HasSubstr("Krassowsky"));
  CPLFree(wkt);

  EXPECT_FALSE(srs.Validate());

  EXPECT_NEAR(47.0, srs.GetProjParm(SRS_PP_STANDARD_PARALLEL_1), 0.0000005);
  EXPECT_NEAR(62.0, srs.GetProjParm(SRS_PP_STANDARD_PARALLEL_2), 0.0000005);
  EXPECT_NEAR(54.5, srs.GetProjParm(SRS_PP_LATITUDE_OF_CENTER), 0.0000005);
  EXPECT_NEAR(45.0, srs.GetProjParm(SRS_PP_LONGITUDE_OF_CENTER), 0.0000005);
  EXPECT_NEAR(0.0, srs.GetProjParm(SRS_PP_FALSE_EASTING), 0.0000005);
  EXPECT_NEAR(0.0, srs.GetProjParm(SRS_PP_FALSE_NORTHING), 0.0000005);

  long proj_sys = 0;
  long zone = 0;
  double *prj_param_out = nullptr;
  long datum = 0;
  EXPECT_EQ(OGRERR_NONE,
            srs.exportToUSGS(&proj_sys, &zone, &prj_param_out, &datum));

  EXPECT_DOUBLE_EQ(8, proj_sys);
  EXPECT_DOUBLE_EQ(0, zone);
  EXPECT_DOUBLE_EQ(0, prj_param_out[0]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[1]);
  EXPECT_DOUBLE_EQ(4.7e+07, prj_param_out[2]);
  EXPECT_DOUBLE_EQ(6.2e+07, prj_param_out[3]);
  EXPECT_DOUBLE_EQ(4.5e+07, prj_param_out[4]);
  EXPECT_DOUBLE_EQ(5.403e+07, prj_param_out[5]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[6]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[7]);
  EXPECT_DOUBLE_EQ(1, prj_param_out[8]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[9]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[10]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[11]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[12]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[13]);
  EXPECT_DOUBLE_EQ(0, prj_param_out[14]);

  EXPECT_EQ(15, datum);

  CPLFree(prj_param_out);
}

}  // namespace
}  // namespace autotest2
