// Copyright 2026 Google LLC
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
// Tests for SQLite REGEXP function (ogrsqliteregexp.cpp).
//
// Note: In google3 (//third_party/gdal/BUILD), HAVE_PCRE and HAVE_PCRE2 are
// not enabled, so OGRSQLiteRegisterRegExpFunction returns nullptr and
// OGRSQLiteFreeRegExpCache is a no-op. SQLite REGEXP queries fail cleanly with
// CE_Failure.
//
// See also:
//   https://gdal.org/drivers/vector/sqlite.html
//   https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/sqlite/ogrsqliteregexp.cpp

#include <memory>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

TEST(OgrSqliteRegexpTest, RegexpQueryFailsCleanlyWithoutPcre) {
  RegisterOGRSQLite();
  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("SQLite");
  ASSERT_NE(nullptr, poDriver);
  GDALDatasetUniquePtr poDS(
      poDriver->Create(":memory:", 0, 0, 0, GDT_Unknown, nullptr));
  ASSERT_NE(nullptr, poDS);

  WithQuietHandler quiet;
  CPLErrorReset();
  OGRLayer* poLayer =
      poDS->ExecuteSQL("SELECT 'abc' REGEXP 'a.c'", nullptr, nullptr);
  EXPECT_EQ(nullptr, poLayer);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("REGEXP"));
  CPLErrorReset();
}

// TODO: b/335317901 - Cover PCRE2/PCRE REGEXP compilation and cache paths if
// HAVE_PCRE2 is enabled.

}  // namespace
}  // namespace autotest2
