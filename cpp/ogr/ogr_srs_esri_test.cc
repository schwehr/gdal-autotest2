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

#include <string>
#include <vector>

#include "gmock.h"
#include "gunit.h"
#include "autotest2/cpp/util/cpl_cstringlist.h"
#include "autotest2/cpp/util/error_handler.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_spatialref.h"
#include "port/cpl_conv.h"
#include "port/cpl_csv.h"
#include "port/cpl_error.h"
#include "port/cpl_string.h"
#include "port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(EsriTest, MorphToESRI) {
  OGRSpatialReference srs;
  srs.importFromEPSG(4202);
  EXPECT_STREQ("Australian_Geodetic_Datum_1966", srs.GetAttrValue("DATUM"));

  srs.morphToESRI();
  EXPECT_STREQ("D_Australian_1966", srs.GetAttrValue("DATUM"));
}

TEST(EsriTest, EmptyString) {
  OGRSpatialReference srs;
  // Totally bogus string.
  auto csl = VectorToCsl(std::vector<string>({""}));
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromESRI(csl));
  CSLDestroy(csl);
}

TEST(EsriTest, ImportFromEsriStatePlaneFlorida) {
  OGRSpatialReference srs;

  {
    auto csl = VectorToCsl(
        {"Projection    STATEPLANE", "Fipszone      903", "Datum         NAD83",
         "Spheroid      GRS80", "Units         FEET", "Zunits        NO",
         "Xshift        0.0", "Yshift        0.0", "Parameters    ", ""});
    EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
    CSLDestroy(csl);
  }
  char *wkt = nullptr;
  srs.exportToWkt(&wkt);

  EXPECT_THAT(string(wkt), testing::HasSubstr("Florida"));
  CPLFree(wkt);
}

}  // namespace
}  // namespace autotest2
