// Copyright 2014 Google Inc. All Rights Reserved.
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

#include <ctype.h>
#include <stdio.h>
#include <set>
#include <string>

#include "logging.h"
#include "gunit.h"
#include "autotest2/cpp/util/error_handler.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_spatialref.h"
#include "port/cpl_csv.h"
#include "port/cpl_error.h"
#include "port/cpl_string.h"
#include "port/cpl_vsi.h"

namespace {

TEST(OgrSrsValidateTest, ValidateEpsgSimpleSuccess) {
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));  // WGS84
  EXPECT_EQ(OGRERR_NONE, srs.Validate());
}

TEST(OgrSrsValidateTest, ValidateEpsgFailure) {
  // Tests rejecting an EPSG that does not exist.
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_UNSUPPORTED_SRS, srs.importFromEPSG(999999999));
  EXPECT_EQ(OGRERR_CORRUPT_DATA, srs.Validate());
}

TEST(OgrSrsValidateTest, ValidatePcsWkt) {
  // Tests that EPSG codes in pcs.csv codes validate or are known bad.
  set<int> known_bad_import_epsgs = {
    // None from gcs.csv.
    // From pcs.csv:
    2218, 2221, 2296, 2297, 2298, 2299, 2300, 2301, 2302, 2303, 2304, 2305,
    2306, 2307, 2963, 2985, 2986, 3052, 3053, 3139, 3144, 3145, 3173, 3295,
    3993, 5017, 5224, 5225, 5515, 5516, 5819, 5820, 5821,
    6200, 6201, 6202, 6244, 6245, 6246, 6247, 6248, 6249, 6250, 6251, 6252,
    6253, 6254, 6255, 6256, 6257, 6258, 6259, 6260, 6261, 6262, 6263, 6264,
    6265, 6266, 6267, 6268, 6269, 6270, 6271, 6272, 6273, 6274, 6275,
    32600, 32700,
    // None from cubewerx_extra.wkt.
    // From esri_StatePlane_extra.wkt:
    1010, 1011, 1012, 1014, 1020, 1021, 1022, 1024, 4040, 4064, 4074, 5010,
    5012, 5020, 5021, 5022, 5023, 5024, 5030, 5031, 5032, 5033, 5034, 6000,
    6001, 6002, 6003, 6004, 7000, 7001, 7002, 7003, 7004, 9010, 9011, 9012,
    9013, 9014, 9020, 9021, 9022, 9023, 9024, 9030, 9031, 9032, 9033, 9034,
    10010, 10011, 10012, 10013, 10014, 10020, 10021, 10022, 10023, 10024,
    11010, 11011, 11012, 11013, 11014, 11020, 11021, 11022, 11023, 11024,
    11030, 11031, 11032, 11033, 11034, 12010, 12011, 12012, 12013, 12014,
    12020, 12021, 12022, 12023, 12024, 13010, 13011, 13012, 13013, 13014,
    13020, 13021, 13022, 13023, 13024, 14010, 14011, 14012, 14013, 14014,
    14020, 14021, 14022, 14023, 14024, 15010, 15011, 15012, 15013, 15014,
    15020, 15021, 15022, 15023, 15024, 16000, 16001, 16002, 16003, 16010,
    16011, 16012, 16013, 16014, 16020, 16021, 16022, 16023, 16024, 17010,
    17011, 17012, 17013, 17014, 17020, 17021, 17022, 17023, 17024, 17031,
    17032, 17034, 18010, 18011, 18012, 18014, 18020, 18021, 18022, 18024,
    19000, 19001, 19002, 19003, 19004, 21110, 21111, 21112, 21113, 21114,
    21115, 21116, 21120, 21121, 21122, 21123, 21124, 21125, 21126, 21130,
    21131, 21132, 21133, 21134, 21135, 21136, 22010, 22011, 22012, 22013,
    22014, 22020, 22021, 22022, 22023, 22024, 22030, 22031, 22034, 23010,
    23011, 23012, 23013, 23014, 23020, 23021, 23022, 23023, 23024, 24010,
    24011, 24012, 24014, 24020, 24021, 24022, 24024, 24030, 24031, 24032,
    24034, 25001, 25002, 25003, 25005, 25006, 25014, 25024, 25034, 26000,
    26001, 26002, 26014, 26024, 27010, 27011, 27012, 27013, 27014, 27020,
    27021, 27022, 27023, 27024, 27030, 27031, 27032, 27033, 27034, 28000,
    28001, 28002, 28003, 28004, 29000, 29001, 29002, 29003, 29004, 30010,
    30011, 30012, 30013, 30014, 30020, 30021, 30022, 30023, 30024, 30030,
    30031, 30032, 30033, 30034, 31010, 31011, 31012, 31013, 31014, 31020,
    31021, 31022, 31023, 31024, 31030, 31031, 31032, 31033, 31034, 31040,
    31041, 31042, 31043, 31044, 32004, 33010, 33011, 33012, 33013, 33014,
    33015, 33016, 33020, 33021, 33022, 33023, 33024, 33025, 33026, 34004,
    34010, 34011, 34012, 34013, 34014, 34020, 34021, 34022, 34023, 34024,
    35010, 35011, 35012, 35013, 35014, 35020, 35021, 35022, 35023, 35024,
    36010, 36011, 36012, 36013, 36014, 36015, 36016, 36020, 36021, 36022,
    36023, 36024, 36025, 36026, 37010, 37011, 37012, 37013, 37014, 37020,
    37021, 37022, 37023, 37024, 38000, 38001, 38002, 38003, 38004, 39000,
    39001, 39002, 39003, 39005, 39006, 39014, 39024, 40010, 40011, 40012,
    40013, 40014, 40020, 40021, 40022, 40023, 40024, 41000, 41002, 41003,
    41004, 42010, 42011, 42012, 42013, 42014, 42020, 42021, 42022, 42023,
    42024, 42030, 42031, 42032, 42033, 42034, 42040, 42041, 42042, 42043,
    42044, 42050, 42051, 42052, 42053, 42054, 43010, 43011, 43012, 43013,
    43014, 43015, 43016, 43020, 43021, 43022, 43023, 43024, 43025, 43026,
    43030, 43031, 43032, 43033, 43034, 43035, 43036, 44000, 44001, 44002,
    45010, 45011, 45012, 45013, 45014, 45020, 45021, 45022, 45023, 45024,
    46010, 46011, 46012, 46013, 46014, 46020, 46021, 46022, 46023, 46024,
    47010, 47011, 47012, 47014, 47020, 47021, 47022, 47024, 48010, 48011,
    48012, 48013, 48014, 48020, 48021, 48022, 48023, 48024, 48030, 48031,
    48032, 48033, 48034, 49010, 49011, 49012, 49013, 49014, 49020, 49021,
    49022, 49023, 49024, 49030, 49031, 49032, 49033, 49034, 49040, 49041,
    49042, 49043, 49044, 50011, 50012, 50014, 50021, 50022, 50024, 50031,
    50032, 50034, 50041, 50042, 50044, 50051, 50052, 50054, 50061, 50062,
    50064, 50071, 50072, 50074, 50081, 50082, 50084, 50091, 50092, 50094,
    50101, 50102, 50104, 51010, 51011, 51012, 51013, 51014, 51020, 51021,
    51022, 51023, 51024, 51030, 51031, 51032, 51033, 51034, 51040, 51041,
    51042, 51043, 51044, 51050, 51051, 51052, 51053, 51054, 52000, 52001,
    52002, 52014, 52020, 52024, 54000, 102964, 102991, 102993, 102994,
    102996,
    // From esri_Wisconsin_extra.wkt:
    103301, 103302, 103303, 103304, 103305, 103306, 103307, 103308, 103309,
    103310, 103311, 103312, 103313, 103314, 103315, 103316, 103317, 103318,
    103319, 103320, 103321, 103322, 103323, 103324, 103325, 103326, 103327,
    103328, 103329, 103330, 103331, 103332, 103333, 103334, 103335, 103336,
    103337, 103338, 103339, 103340, 103341, 103342, 103343, 103344, 103345,
    103346, 103347, 103348, 103349, 103350, 103351, 103352, 103353, 103354,
    103355, 103356, 103357, 103358, 103359, 103360, 103361, 103362, 103363,
    103364, 103365, 103366, 103367, 103368, 103369, 103370, 103371, 103400,
    103401, 103402, 103403, 103404, 103405, 103406, 103407, 103408, 103409,
    103410, 103411, 103412, 103413, 103414, 103415, 103416, 103417, 103418,
    103419, 103420, 103421, 103422, 103423, 103424, 103425, 103426, 103427,
    103428, 103429, 103430, 103431, 103432, 103433, 103434, 103435, 103436,
    103437, 103438, 103439, 103440, 103441, 103442, 103443, 103444, 103445,
    103446, 103447, 103448, 103449, 103450, 103451, 103452, 103453, 103454,
    103455, 103456, 103457, 103458, 103459, 103460, 103461, 103462, 103463,
    103464, 103465, 103466, 103467, 103468, 103469, 103470, 103471,
    // None esri_extra.wkt.
  };

  set<int> crash_on_validate = {
    // From esri_extra.wkt:
    104305,
  };

  set<int> known_invalid_epsgs = {
    // None from gcs.csv.
    // From pcs.csv:
    22300, 29701,
    // From cubewerx_extra.wkt:
    42106, 42301, 42303, 42308,
    // From esri_StatePlane_extra.wkt:
    54001, 54004,
    // From esri_Wisconsin_extra.wkt:
    103300,
    // From esri_extra.wkt:
    53001, 53003, 53004, 53008, 53017, 53018, 53019, 53022, 53023, 53025,
    53028, 53030, 53032, 54003, 54008, 54017, 54018, 54019, 54022, 54023,
    54025, 54028, 54030, 54032, 102011, 102016, 102017, 102019, 102020,
    102065, 102066, 102067, 102191, 102192, 102193, 102491, 102492, 102581,
    102582, 102583, 102584, 102591, 102592,
  };

  vector<string> filenames = {
    "gcs.csv",
    "pcs.csv",
    "cubewerx_extra.wkt",
    "esri_StatePlane_extra.wkt",
    "esri_Wisconsin_extra.wkt",
    "esri_extra.wkt",
  };

  for (const string &filename : filenames) {
    LOG(INFO) << "Checking " << filename;
    ASSERT_NE(nullptr, CSVFilename(filename.c_str()));
    string csv_path(CSVFilename(filename.c_str()));
    ASSERT_FALSE(csv_path.empty());

    VSILFILE *csv = VSIFOpenL(csv_path.c_str(), "rb");
    ASSERT_NE(nullptr, csv);

    char **fields = nullptr;

    while (nullptr != (fields = CSVReadParseLineL(csv))) {
      if (fields[0] == nullptr) {
        CSLDestroy(fields);
        continue;
      }
      const string epsg_str(fields[0]);
      CSLDestroy(fields);
      fields = nullptr;

      if (epsg_str.empty() || !isdigit(epsg_str[0])) {
        // Skip the header and any commented lines.
        continue;
      }
      int epsg = std::stoi(epsg_str);
      OGRSpatialReference srs;
      const OGRErr import_error = srs.importFromEPSG(epsg);

      if (known_bad_import_epsgs.count(epsg) != 0) {
        EXPECT_NE(OGRERR_NONE, import_error)
            << "EPSG unexpectedly succeeded with import."
            << "Need to remove from bad_imports: " << epsg;
        continue;
      }
      EXPECT_EQ(OGRERR_NONE, import_error)
          << "EPSG failed to import " << epsg;
      if (import_error != OGRERR_NONE) {
        continue;
      }

      if (crash_on_validate.count(epsg) != 0) {
        continue;
      }

      if (known_invalid_epsgs.count(epsg) == 0) {
        EXPECT_EQ(OGRERR_NONE, srs.Validate())
            << "EPSG did not validate: " << epsg;
      } else {
        EXPECT_NE(OGRERR_NONE, srs.Validate())
            << "EPSG known to not validate started validating: " << epsg;
      }
    }
    LOG(INFO) << "Closing file";
    ASSERT_EQ(0, VSIFCloseL(csv));
  }
}

}  // namespace
