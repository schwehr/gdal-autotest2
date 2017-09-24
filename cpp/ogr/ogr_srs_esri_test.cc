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
#include "util/gtl/cleanup.h"

namespace autotest2 {
namespace {

string Wkt(const OGRSpatialReference &srs) {
  char *wkt_tmp = nullptr;
  EXPECT_EQ(OGRERR_NONE, srs.exportToWkt(&wkt_tmp));
  CHECK_NOTNULL(wkt_tmp);
  string wkt(wkt_tmp);
  free(wkt_tmp);

  return wkt;
}

TEST(EsriTest, MorphToESRI) {
  OGRSpatialReference srs;
  srs.importFromEPSG(4202);
  EXPECT_STREQ("Australian_Geodetic_Datum_1966", srs.GetAttrValue("DATUM"));

  srs.morphToESRI();
  EXPECT_STREQ("D_Australian_1966", srs.GetAttrValue("DATUM"));
}

TEST(EsriTest, EmptyString) {
  // Totally bogus string.
  auto csl = VectorToCsl(std::vector<string>({""}));
  auto csl_cleaner = gtl::MakeCleanup([csl] { CSLDestroy(csl); });
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromESRI(csl));
}

TEST(EsriTest, OverRangeStatePlane) {
  WithQuietHandler error_handler;
  auto csl = VectorToCsl(
      std::vector<string>({"Projection STATEPLANE", "zone7777777777"}));
  auto csl_cleaner = gtl::MakeCleanup([csl] { CSLDestroy(csl); });
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromESRI(csl));
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("zone"));
}

TEST(EsriTest, ImportFromEsriStatePlaneFlorida) {
  auto csl = VectorToCsl({
      "Projection    STATEPLANE",
      "Fipszone      903",
      "Datum         NAD83",
      "Spheroid      GRS80",
      "Units         FEET",
      "Zunits        NO",
      "Xshift        0.0",
      "Yshift        0.0",
      "Parameters    ",
      "",
  });
  auto csl_cleaner = gtl::MakeCleanup([csl] { CSLDestroy(csl); });
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.importFromESRI(csl));
  string wkt = Wkt(srs);
  EXPECT_THAT(wkt, testing::HasSubstr("Florida"));
}

TEST(EsriTest, EquidistantConic) {
  // Rewrite of osr_esri_9.

  constexpr char original[] =
      R"wkt(PROJCS["edc",GEOGCS["GCS_North_American_1983",
      DATUM["D_North_American_1983",
      SPHEROID["GRS_1980",6378137.0,298.257222101]],
      PRIMEM["Greenwich",0.0],
      UNIT["Degree",0.0174532925199433]],
      PROJECTION["Equidistant_Conic"],
      PARAMETER["False_Easting",0.0],
      PARAMETER["False_Northing",0.0],
      PARAMETER["Central_Meridian",-96.0],
      PARAMETER["Standard_Parallel_1",29.5],
      PARAMETER["Standard_Parallel_2",45.5],
      PARAMETER["Latitude_Of_Origin",37.5],
      UNIT["Meter",1.0]])wkt";

  constexpr char expected[] =
      "PROJCS[\"edc\",GEOGCS[\"GCS_North_American_1983\","
      "DATUM[\"North_American_Datum_1983\","
      "SPHEROID[\"GRS_1980\",6378137.0,298.257222101]],"
      "PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433]],"
      "PROJECTION[\"Equidistant_Conic\"],PARAMETER[\"False_Easting\",0.0],"
      "PARAMETER[\"False_Northing\",0.0],"
      "PARAMETER[\"longitude_of_center\",-96.0],"
      "PARAMETER[\"Standard_Parallel_1\",29.5],"
      "PARAMETER[\"Standard_Parallel_2\",45.5],"
      "PARAMETER[\"latitude_of_center\",37.5],""UNIT[\"Meter\",1.0]]";

  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.SetFromUserInput(original));
  EXPECT_EQ(OGRERR_NONE, srs.morphFromESRI());
  auto wkt = Wkt(srs);
  EXPECT_EQ(expected, wkt);
}

TEST(EsriTest, EquidistantConicBad) {
  // Autofuzz POC from b/65416453
  auto csl = VectorToCsl({
      "PROJECTIONLOCA?L_CSw?(  EQUIDISTANT_CONIC",
      "Paramet",
      "55555555555555",
  });
  auto csl_cleaner = gtl::MakeCleanup([csl] { CSLDestroy(csl); });
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.importFromESRI(csl));
  string wkt = Wkt(srs);
  EXPECT_TRUE(wkt.empty());
  EXPECT_THAT(CPLGetLastErrorMsg(), testing::HasSubstr("StdPCount"));
}

}  // namespace
}  // namespace autotest2
