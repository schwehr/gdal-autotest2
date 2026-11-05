// Copyright 2014 Google Inc. All Rights Reserved.
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
// See also:
//   https://github.com/OSGeo/gdal/blob/master/autotest/osr/osr_validate.py


#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/ogr/ogr_srs_validate_test_constants.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/google/gdal_init.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"

namespace autotest2 {
namespace {

using ::testing::AnyOf;

TEST(OgrSrsValidateTest, ValidateEpsgSimpleSuccess) {
  OGRSpatialReference srs;
  EXPECT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));  // WGS84
  EXPECT_EQ(OGRERR_NONE, srs.Validate());
}

TEST(OgrSrsValidateTest, ValidateEpsgFailure) {
  // Tests rejecting an EPSG that does not exist.
  OGRSpatialReference srs;
  EXPECT_THAT(srs.importFromEPSG(999999999),
              AnyOf(OGRERR_FAILURE, OGRERR_UNSUPPORTED_SRS));
  EXPECT_EQ(srs.Validate(), OGRERR_CORRUPT_DATA);
}

TEST(OgrSrsValidateTest, ValidateAllSupportedEpsgCodes) {
  gdal::GoogleGdalInitSupportFiles();

  WithQuietHandler handler;

  for (int epsg : kSupportedEpsgCodes) {
    OGRSpatialReference srs;
    EXPECT_EQ(OGRERR_NONE, srs.importFromEPSG(epsg))
        << "EPSG failed to import " << epsg;
    EXPECT_EQ(OGRERR_NONE, srs.Validate()) << "EPSG did not validate: " << epsg;
  }
}
}  // namespace
}  // namespace autotest2
