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
// Tests HFA raster driver.
//
// See also:
//   http://www.gdal.org/frmt_hfa.html
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/hfa_read.py
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/hfa_rfc40.py
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/hfa_srs.py
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/hfa_write.py
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/gdrivers/hfa.py

#include "port/cpl_port.h"
#include "frmts/hfa/hfadataset.h"

#include <map>
#include <string>
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

using ::testing::Pair;

constexpr char kTestData[] = "cpp/frmts/hfa/testdata/";

TEST(IdentifyTest, DoesNotExist) {
  auto open_info = gtl::MakeUnique<GDALOpenInfo>("/does_not_exist",
                                                 GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(FALSE, HFADataset::Identify(open_info.get()));
}

TEST(IdentifyTest, MinimumSuccess) {
  constexpr char kFilename[] = "/vsimem/identify-minimum.img";
  constexpr char kData[] = "EHFA_HEADER_TAG";
  autotest2::VsiMemTempWrapper wrapper(kFilename, kData);
  auto open_info =
      gtl::MakeUnique<GDALOpenInfo>(kFilename, GDAL_OF_READONLY, nullptr);
  EXPECT_EQ(TRUE, HFADataset::Identify(open_info.get()));
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

TEST(OpenTest, Byte) {
  const string filepath =
      file::JoinPath(FLAGS_test_srcdir, kTestData, "byte.img");
  auto open_info = gtl::MakeUnique<GDALOpenInfo>(filepath.c_str(),
                                                 GDAL_OF_READONLY, nullptr);
  auto src = absl::WrapUnique(HFADataset::Open(open_info.get()));
  ASSERT_NE(nullptr, src);
  double geo_transform[6] = {};
  src->GetGeoTransform(geo_transform);
  EXPECT_THAT(geo_transform,
              testing::ElementsAre(440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0));

  EXPECT_EQ(0, src->GetGCPCount());
  EXPECT_EQ(20, src->GetRasterXSize());
  EXPECT_EQ(20, src->GetRasterYSize());
  EXPECT_EQ(1, src->GetRasterCount());

  // No file level metadata.
  EXPECT_EQ(nullptr, src->GetMetadata());

  GDALRasterBand *band = src->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  int block_xsize = 0;
  int block_ysize = 0;
  band->GetBlockSize(&block_xsize, &block_ysize);
  EXPECT_EQ(20, block_xsize);
  EXPECT_EQ(20, block_ysize);
  EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
  EXPECT_EQ(GCI_Undefined, band->GetColorInterpretation());

  double minmax[2] = {0.0, 0.0};
  ASSERT_EQ(CE_None, band->ComputeRasterMinMax(false, minmax));
  EXPECT_THAT(minmax, testing::ElementsAre(74.0, 255.0));

  // band owns metadata.
  char **metadata_list = band->GetMetadata();
  EXPECT_NE(nullptr, metadata_list);

  auto metadata = ParseMetadata(metadata_list);
  EXPECT_EQ(14, metadata.size());

  // UnorderedElementsAre only allows 10 elements.  Check 4 and remove them from
  // the map to allow using UnorderedElementsAre on the rest.
  EXPECT_EQ(metadata["STATISTICS_MODE"], "132");
  EXPECT_EQ(metadata["STATISTICS_SKIPFACTORX"], "1");
  EXPECT_EQ(metadata["STATISTICS_SKIPFACTORY"], "1");
  EXPECT_EQ(metadata["STATISTICS_STDDEV"], "22.957185278028");

  metadata.erase("STATISTICS_MODE");
  metadata.erase("STATISTICS_SKIPFACTORX");
  metadata.erase("STATISTICS_SKIPFACTORY");
  metadata.erase("STATISTICS_STDDEV");

  EXPECT_THAT(
      metadata,
      testing::UnorderedElementsAre(
          Pair("LAYER_TYPE", "athematic"),
          Pair("STATISTICS_EXCLUDEDVALUES", "0"),
          Pair("STATISTICS_HISTOBINVALUES",
               "0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|"
               "0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|"
               "0|0|0|0|0|0|0|0|0|0|0|0|1|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|6|0|0|"
               "0|0|0|0|0|0|37|0|0|0|0|0|0|0|57|0|0|0|0|0|0|0|62|0|0|0|0|0|0|0|"
               "66|0|0|0|0|0|0|0|0|72|0|0|0|0|0|0|0|31|0|0|0|0|0|0|0|24|0|0|0|"
               "0|0|0|0|12|0|0|0|0|0|0|0|0|7|0|0|0|0|0|0|0|12|0|0|0|0|0|0|0|5|"
               "0|0|0|0|0|0|0|3|0|0|0|0|0|0|0|1|0|0|0|0|0|0|0|0|2|0|0|0|0|0|0|"
               "0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|0|"
               "0|0|0|1|0|0|0|0|0|0|0|1|"),
          Pair("STATISTICS_HISTOMAX", "255"),
          Pair("STATISTICS_HISTOMIN", "0"),
          Pair("STATISTICS_HISTONUMBINS", "256"),
          Pair("STATISTICS_MAXIMUM", "255"),
          Pair("STATISTICS_MEAN", "126.765"),
          Pair("STATISTICS_MEDIAN", "123"),
          Pair("STATISTICS_MINIMUM", "74")
      ));
}

TEST(OpenTest, Compresses1x1) {
  const string filepath =
      file::JoinPath(FLAGS_test_srcdir, kTestData, "compressed-1x1.img");
  auto open_info = gtl::MakeUnique<GDALOpenInfo>(filepath.c_str(),
                                                 GDAL_OF_READONLY, nullptr);
  auto src = absl::WrapUnique(HFADataset::Open(open_info.get()));
  ASSERT_NE(nullptr, src);
  double geo_transform[6] = {};
  src->GetGeoTransform(geo_transform);
  EXPECT_THAT(geo_transform,
              testing::ElementsAre( 0.0, 1.0, 0.0, 0.0, 0.0, 1.0));

  EXPECT_EQ(0, src->GetGCPCount());
  EXPECT_EQ(1, src->GetRasterXSize());
  EXPECT_EQ(1, src->GetRasterYSize());
  EXPECT_EQ(1, src->GetRasterCount());

  // No file level metadata.
  EXPECT_EQ(nullptr, src->GetMetadata());

  GDALRasterBand *band = src->GetRasterBand(1);
  ASSERT_NE(nullptr, band);
  int block_xsize = 0;
  int block_ysize = 0;
  band->GetBlockSize(&block_xsize, &block_ysize);
  EXPECT_EQ(64, block_xsize);
  EXPECT_EQ(64, block_ysize);
  EXPECT_EQ(GDT_Byte, band->GetRasterDataType());
  EXPECT_EQ(GCI_Undefined, band->GetColorInterpretation());

  double minmax[2] = {0.0, 0.0};
  ASSERT_EQ(CE_None, band->ComputeRasterMinMax(false, minmax));
  EXPECT_THAT(minmax, testing::ElementsAre(0.0, 0.0));

  // band owns metadata.
  char **metadata_list = band->GetMetadata();
  EXPECT_NE(nullptr, metadata_list);

  auto metadata = ParseMetadata(metadata_list);
  EXPECT_EQ(1, metadata.size());

  EXPECT_EQ(metadata["LAYER_TYPE"], "athematic");
}

// TODO(schwehr): Write more tests.

}  // namespace
}  // namespace autotest2
