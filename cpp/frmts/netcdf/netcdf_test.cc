// Copyright 2019 Google Inc. All Rights Reserved.
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
// Tests netcdf/hdf5 raster drivers.
//
// See also:
//   https://gdal.org/drivers/raster/netcdf.html
//   https://gdal.org/drivers/raster/hdf5.html
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/netcdf.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/netcdf_cf.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/netcdf_cfchecks.py
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/netcdf_multidim.py

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_split.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

using ::testing::IsSupersetOf;
using ::testing::Pair;

const char kTestData[] =
    "/google3/third_party/gdal/autotest2/cpp/frmts/netcdf/testdata/";

// Convert a C string list of "key=value" to a std::map<k, v>.
std::map<std::string, std::string> ParseMetadata(CSLConstList csl) {
  std::map<std::string, std::string> result;
  if (csl == nullptr) return result;
  for (; *csl != nullptr; csl++) {
    std::vector<std::string> v = absl::StrSplit(*csl, '=');
    CHECK_EQ(v.size(), 2);
    result[v[0]] = v[1];
  }
  return result;
}

class NetcdfTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // NETCDF files have the potential to be opened by any of these drivers.
    GDALRegister_netCDF();
    GDALRegister_HDF5();
    GDALRegister_HDF5Image();
  }
};

TEST_F(NetcdfTest, ChirpsGdalOpenEx) {
  const std::string filepath = ::testing::SrcDir() + std::string(kTestData) +
                               "chirps-v2.0.1985.06.days_p05.nc";
  std::unique_ptr<GDALDataset> src(static_cast<GDALDataset *>(
      GDALOpenEx(filepath.c_str(), GDAL_OF_READONLY | GDAL_OF_RASTER, nullptr,
                 nullptr, nullptr)));
  EXPECT_STREQ("", CPLGetLastErrorMsg());
  EXPECT_EQ(CPLE_None, CPLGetLastErrorType());
  ASSERT_NE(nullptr, src);

  EXPECT_STREQ("netCDF", src->GetDriver()->GetDescription());

  EXPECT_STREQ("", src->GetProjectionRef());
  double geo_transform[6] = {};
  src->GetGeoTransform(geo_transform);
  // No geotransform, so GDAL will return the default.
  EXPECT_THAT(geo_transform, testing::ElementsAre(
                                 -180.00000610436345, 0.050000001695656514, 0,
                                 49.99999847335777, 0, -0.049999998473357773));

  EXPECT_EQ(0, src->GetGCPCount());
  EXPECT_EQ(7200, src->GetRasterXSize());
  EXPECT_EQ(2000, src->GetRasterYSize());
  EXPECT_EQ(30, src->GetRasterCount());

  {
    // Dataset owns metadata.
    CSLConstList metadata_list = src->GetMetadata();
    EXPECT_NE(nullptr, metadata_list);

    const auto metadata = ParseMetadata(metadata_list);
    EXPECT_EQ(40, metadata.size());
    EXPECT_THAT(metadata,
                IsSupersetOf({Pair("NC_GLOBAL#Conventions", "CF-1.6"),
                              Pair("NC_GLOBAL#date_created", "2015-11-20"),
                              Pair("NC_GLOBAL#version", "Version 2.0")}));
  }

  GDALRasterBand *band = src->GetRasterBand(30);
  ASSERT_NE(nullptr, band);

  int block_xsize = 0;
  int block_ysize = 0;
  band->GetBlockSize(&block_xsize, &block_ysize);
  EXPECT_EQ(900, block_xsize);
  EXPECT_EQ(250, block_ysize);
  EXPECT_EQ(GDT_Float32, band->GetRasterDataType());
  EXPECT_EQ(GCI_Undefined, band->GetColorInterpretation());

  double minmax[2] = {0.0, 0.0};
  ASSERT_EQ(CE_None, band->ComputeRasterMinMax(false, minmax));
  EXPECT_DOUBLE_EQ(0.0, minmax[0]);
  EXPECT_NEAR(336.712036132812, minmax[1], 0.000001);

  {
    // Band owns metadata.
    CSLConstList metadata_list = band->GetMetadata();
    EXPECT_NE(nullptr, metadata_list);

    const auto metadata = ParseMetadata(metadata_list);
    EXPECT_EQ(12, metadata.size());
    EXPECT_THAT(metadata, IsSupersetOf({Pair("geostatial_lat_max", "50"),
                                        Pair("missing_value", "-9999"),
                                        Pair("_FillValue", "-9999")}));
  }
}

// TODO: b/430052067 - Flush out tests.

}  // namespace
}  // namespace autotest2
