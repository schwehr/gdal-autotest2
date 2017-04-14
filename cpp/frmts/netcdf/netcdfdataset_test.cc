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
// Tests netCDF raster driver.
//
// See also:
//   http://www.gdal.org/frmt_netcdf.html
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/gdrivers/netcdf.py

#include "gdal/port/cpl_port.h"
#include "gdal/frmts/netcdf/netcdfdataset.h"

#include <map>
#include <vector>

#include "file/base/path.h"
#include "gmock.h"
#include "gunit.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/absl/strings/str_split.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"

namespace autotest2 {
namespace {

const char kTestData[] =
    "autotest2/cpp/frmts/netcdf/testdata/";

TEST(IdentifyTest, IdentifyDoesNotExist) {
  auto open_info = gtl::MakeUnique<GDALOpenInfo>("/does_not_exist",
                                                 GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, netCDFDataset::Identify(open_info.get()));
}

// Convert a C string list of "key=value" to a std::map<k, v>.
std::map<string, string> ParseMetadata(const char *const *csl) {
  std::map<string, string> result;
  if (csl == nullptr) return result;
  for (; *csl != nullptr; csl++) {
    std::vector<string> v = strings::Split(*csl, '=');
    CHECK_EQ(v.size(), 2);
    result[v[0]] = v[1];
  }
  return result;
}

TEST(OpenTest, Int16) {
  const string filepath =
      file::JoinPath(FLAGS_test_srcdir, kTestData, "int16-nogeo.nc");
  auto open_info = gtl::MakeUnique<GDALOpenInfo>(filepath.c_str(),
                                                 GDAL_OF_READONLY, nullptr);
  auto src = absl::WrapUnique(netCDFDataset::Open(open_info.get()));
  ASSERT_NE(nullptr, src);

  double geo_transform[6] = {};
  src->GetGeoTransform(geo_transform);
  // No geotransform, so it will be return the default.
  EXPECT_THAT(geo_transform,
              testing::ElementsAre(0.0, 1.0, 0.0, 0.0, 0.0, 1.0));

  EXPECT_EQ(0, src->GetGCPCount());
  EXPECT_EQ(512, src->GetRasterXSize());
  EXPECT_EQ(512, src->GetRasterYSize());
  // TODO(schwehr): This should become 1 in future version of GDAL.
  EXPECT_EQ(0, src->GetRasterCount());

  // Dataset owns metadata.
  char **metadata_list = src->GetMetadata();
  EXPECT_NE(nullptr, metadata_list);

  auto metadata = ParseMetadata(metadata_list);
  EXPECT_EQ(1, metadata.size());
  EXPECT_THAT(metadata, testing::UnorderedElementsAre(
                            testing::Pair("NC_GLOBAL#Conventions", "CF-1.5")));

  // TODO(schwehr): Future versions of GDAL can read this band.
  GDALRasterBand *band = src->GetRasterBand(1);
  ASSERT_EQ(nullptr, band);
}

// TODO(schwehr): Write more tests.

}  // namespace
}  // namespace autotest2
