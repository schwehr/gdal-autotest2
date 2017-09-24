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
// Tests Kakadu JPEG 2000 raster driver.
//
// See also:
//   http://www.gdal.org/frmt_jp2kak.html
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/gdrivers/jp2kak.py

// TODO(schwehr): Try these:
//   CPLSetConfigOption("JP2KAK_THREADS", "0");
//   CPLSetConfigOption("JP2KAK_THREADS", "1");
//   CPLSetConfigOption("JP2KAK_THREADS", "2");
//   CPLSetConfigOption("JP2KAK_THREADS", "-1");

#include "frmts/jp2kak/jp2kakdataset.h"

#include <initializer_list>
#include <memory>
#include <string>

#include "gunit.h"
#include "autotest2/cpp/util/vsimem.h"
#include "gcore/gdal.h"
#include "gcore/gdal_frmts.h"
#include "gcore/gdal_priv.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_spatialref.h"
#include "port/cpl_conv.h"
#include "port/cpl_error.h"

namespace autotest2 {
namespace {

TEST(Jp2kakdatasetTest, IdentifyDoesNotExist) {
  std::unique_ptr<GDALOpenInfo> open_info(
      new GDALOpenInfo("/does_not_exist", GDAL_OF_READONLY, nullptr));
  ASSERT_NE(nullptr, open_info);
  EXPECT_EQ(FALSE, JP2KAKDataset::Identify(open_info.get()));
}

TEST(Jp2kakdatasetTest, IdentifyNoJpip) {
  // JPIP is a windows only thing.
  for (const auto &url : {"http://localhost/a.jp2", "https://localhost/a.jp2",
                          "jpip://localhost/a.jp2"}) {
    std::unique_ptr<GDALOpenInfo> open_info(
        new GDALOpenInfo(url, GDAL_OF_READONLY, nullptr));
    ASSERT_NE(nullptr, open_info);
    EXPECT_EQ(FALSE, JP2KAKDataset::Identify(open_info.get()));
  }
}

// TODO(schwehr): Identify with J2K_SUBFILE.

TEST(Jp2kakdatasetTest, IdentifyJp2) {
  // Only goes off the contents.  Ignores the extension.
  const char kFilename[] = "/vsimem/a";
  const unsigned char kJp2[] =
      "\x00\x00\x00\x0c\x6a\x50\x20\x20\x0d\x0a\x87\x0a yada yada ";
  const string data(reinterpret_cast<const char *>(kJp2), CPL_ARRAYSIZE(kJp2));
  VsiMemTempWrapper wrapper(kFilename, data);
  std::unique_ptr<GDALOpenInfo> open_info(
      new GDALOpenInfo(kFilename, GDAL_OF_READONLY, nullptr));
  ASSERT_NE(nullptr, open_info);
  EXPECT_EQ(TRUE, JP2KAKDataset::Identify(open_info.get()));
}

TEST(Jp2kakdatasetTest, IdentifyJpc) {
  std::initializer_list<string> exts = {"jpc", "j2k", "jp2", "jpx", "jpc"};

  // Do not match with just the extension.
  for (const auto &ext : exts) {
    const string filepath = "/vsimem/b." + ext;
    VsiMemTempWrapper wrapper(filepath, "junk");
    std::unique_ptr<GDALOpenInfo> open_info(
        new GDALOpenInfo(filepath.c_str(), GDAL_OF_READONLY, nullptr));
    ASSERT_NE(nullptr, open_info);
    EXPECT_EQ(FALSE, JP2KAKDataset::Identify(open_info.get()));
  }

  // Must have more than jp2_header bytes of data (12 bytes) or it will be
  // treated as JPIP.
  const unsigned char kJpc[] = "\xff\x4f yada yada yada yada";
  const string data(reinterpret_cast<const char *>(kJpc), CPL_ARRAYSIZE(kJpc));

  // Success with extension and header.
  for (const auto &ext : exts) {
    const string filepath = "/vsimem/c." + ext;
    VsiMemTempWrapper wrapper(filepath, data);
    std::unique_ptr<GDALOpenInfo> open_info(
        new GDALOpenInfo(filepath.c_str(), GDAL_OF_READONLY, nullptr));
    ASSERT_NE(nullptr, open_info);
    ASSERT_EQ(TRUE, JP2KAKDataset::Identify(open_info.get()));
  }

  // Fail with correct header and wrong extension.
  const string filepath = "/vsimem/d.bad";
  VsiMemTempWrapper wrapper(filepath, data);
  std::unique_ptr<GDALOpenInfo> open_info(
      new GDALOpenInfo(filepath.c_str(), GDAL_OF_READONLY, nullptr));
  ASSERT_NE(nullptr, open_info);
  ASSERT_EQ(FALSE, JP2KAKDataset::Identify(open_info.get()));
}

TEST(Jp2kakdatasetTest, OpenJpip) {
  // The Identify() at the beginning of Open blocks setting bIsJPIP.
  for (const auto &url : {"http://localhost/e.jp2", "https://localhost/e.jp2",
                          "jpip://localhost/e.jp2"}) {
    std::unique_ptr<GDALOpenInfo> open_info(
        new GDALOpenInfo(url, GDAL_OF_READONLY, nullptr));
    ASSERT_NE(nullptr, open_info);
    ASSERT_LT(open_info->nHeaderBytes, 16);
    EXPECT_EQ(nullptr, JP2KAKDataset::Open(open_info.get()));
  }
}

// TODO(schwehr): more.

}  // namespace
}  // namespace autotest2
