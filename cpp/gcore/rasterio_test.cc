// Tests gcore/rasterio.cpp.
//
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

#include "gunit.h"
#include "autotest2/cpp/util/error_handler.h"
#include "gcore/gdal.h"

namespace {

// TODO(schwehr): Test GDALRasterBand::IRasterIO.

TEST(RasterIoTest, GdalSwapWords) {
  // GdalSwapWords takes a block size of number of bytes (2, 4 or 8), a number
  // of blocks to swap and the number of bytes to advance.  If the number of
  // bytes to advance is 0, then the number of blocks must be 1.  This
  // function allows for gaps between swaps.
  // It only checks the input values for validity if DEBUG is defined.

  // Swapping single 2 byte value.
  {
    char buf[] = "123";
    GDALSwapWords(buf, 2, 1, 0);
    EXPECT_STREQ("213", buf);
  }

  {
    char buf[] = "12345";
    GDALSwapWords(buf, 4, 1, 0);
    EXPECT_STREQ("43215", buf);
  }

  {
    char buf[] = "1234567890";
    GDALSwapWords(buf, 8, 1, 0);
    EXPECT_STREQ("8765432190", buf);
  }

  // Swap 2 bytes, advance 2 bytes and swap 2 bytes.
  {
    char buf[] = "abcde";
    GDALSwapWords(buf, 2, 2, 2);
    EXPECT_STREQ("badce", buf);
  }

  // Swap 4 bytes, advance 4 bytes, swap 4 bytes.
  {
    char buf[] = "abcdefghi";
    GDALSwapWords(buf, 4, 2, 4);
    EXPECT_STREQ("dcbahgfei", buf);
  }

  {
    char buf[] = "abcdefghijklmnopq";
    GDALSwapWords(buf, 8, 2, 8);
    EXPECT_STREQ("hgfedcbaponmlkjiq", buf);
  }

  // Gap between words.
  {
    char buf[] = "123456";
    GDALSwapWords(buf, 2, 2, 3);
    EXPECT_STREQ("213546", buf);
  }

  {
    char buf[] = "1234567890";
    GDALSwapWords(buf, 4, 2, 5);
    EXPECT_STREQ("4321598760", buf);
  }

  {
    char buf[] = "1234567890123456789";
    GDALSwapWords(buf, 8, 2, 9);
    EXPECT_STREQ("8765432197654321089", buf);
  }
}

// TODO(schwehr): Test GDALCopyWords.
// TODO(schwehr): Test GDALBandGetBestOverviewLevel.
// TODO(schwehr): Test GDALRasterBand::OverviewRasterIO.
// TODO(schwehr): Test GDALDataset::BlockBasedRasterIO.
// TODO(schwehr): Test GDALDatasetCopyWholeRaster.
// TODO(schwehr): Test GDALRasterBandCopyWholeRaster.

}  // namespace
