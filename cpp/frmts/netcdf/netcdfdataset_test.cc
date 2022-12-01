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
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/netcdf.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/netcdf_cf.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/netcdf_cfchecks.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/netcdf_multidim.py

#include "frmts/netcdf/netcdfdataset.h"

#include <map>
#include <vector>

#include "file/base/path.h"
#include "gmock.h"
#include "gunit.h"
#include "third_party/absl/flags/flag.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/absl/strings/str_split.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_priv.h"
#include "port/cpl_port.h"

namespace autotest2 {
namespace {

using ::testing::IsSupersetOf;
using ::testing::Pair;

const char kTestData[] =
    "/google3/third_party/gdal/autotest2/cpp/frmts/netcdf/testdata/";

TEST(IdentifyTest, IdentifyDoesNotExist) {
  auto open_info = absl::make_unique<GDALOpenInfo>("/does_not_exist",
                                                   GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, netCDFDataset::Identify(open_info.get()));
}

// Convert a C string list of "key=value" to a std::map<k, v>.
std::map<std::string, std::string> ParseMetadata(const char *const *csl) {
  std::map<std::string, std::string> result;
  if (csl == nullptr) return result;
  for (; *csl != nullptr; csl++) {
    std::vector<std::string> v = absl::StrSplit(*csl, '=');
    CHECK_EQ(v.size(), 2);
    result[v[0]] = v[1];
  }
  return result;
}

TEST(OpenTest, Int16) {
  const std::string filepath = file::JoinPath(absl::GetFlag(FLAGS_test_srcdir),
                                              kTestData, "int16-nogeo.nc");
  auto open_info = absl::make_unique<GDALOpenInfo>(filepath.c_str(),
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
                            Pair("NC_GLOBAL#Conventions", "CF-1.5")));

  // TODO(schwehr): Future versions of GDAL can read this band.
  GDALRasterBand *band = src->GetRasterBand(1);
  ASSERT_EQ(nullptr, band);
}

TEST(OpenTest, Goes_RadC_Dqf) {
  // 8-bit data that caused:
  //   Warning 1:
  //   NetCDF driver detected file type=5, but libnetcdf detected type=3
  const char kFilename[] =
      "OR_ABI-L1b-RadC-M3C06_G16_s20180402242198_e20180402244576_"
      "c20180402245020.nc";
  const std::string filepath =
      "NETCDF:" +
      file::JoinPath(absl::GetFlag(FLAGS_test_srcdir), kTestData, kFilename) +
      ":DQF";
  auto open_info = absl::make_unique<GDALOpenInfo>(filepath.c_str(),
                                                   GDAL_OF_READONLY, nullptr);
  auto src = absl::WrapUnique(netCDFDataset::Open(open_info.get()));
  ASSERT_NE(nullptr, src);

  double geo_transform[6] = {};
  src->GetGeoTransform(geo_transform);
  EXPECT_THAT(geo_transform,
              testing::ElementsAre(-3627271.3409673548, 2004.0173154875411, 0,
                                   1583173.7916531805, 0, 2004.0173154875411));

  EXPECT_EQ(0, src->GetGCPCount());
  EXPECT_EQ(2500, src->GetRasterXSize());
  EXPECT_EQ(1500, src->GetRasterYSize());
  EXPECT_EQ(1, src->GetRasterCount());

  // Dataset owns metadata.
  char **metadata_list = src->GetMetadata();
  EXPECT_NE(nullptr, metadata_list);

  auto metadata = ParseMetadata(metadata_list);
  EXPECT_EQ(66, metadata.size());
  EXPECT_THAT(
      metadata,
      IsSupersetOf({Pair("DQF#valid_range", "{0,3}"),
                    Pair("DQF#_Unsigned", "true"),
                    Pair("goes_imager_projection#sweep_angle_axis", "x"),
                    Pair("NC_GLOBAL#Conventions", "CF-1.7")}));

  GDALRasterBand *band = src->GetRasterBand(1);
  ASSERT_NE(nullptr, band);

  int block_xsize = 0;
  int block_ysize = 0;
  band->GetBlockSize(&block_xsize, &block_ysize);
  EXPECT_EQ(2500, block_xsize);
  EXPECT_EQ(1, block_ysize);
  EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
  EXPECT_EQ(GCI_Undefined, band->GetColorInterpretation());

  double minmax[2] = {-1.0, -1.0};
  ASSERT_EQ(CE_None, band->ComputeRasterMinMax(false, minmax));
  EXPECT_THAT(minmax, testing::ElementsAre(0.0, 0.0));
}

// TODO(schwehr): Write more tests.

}  // namespace
}  // namespace autotest2
