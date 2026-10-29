// Copyright 2026 Google Inc. All Rights Reserved.
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
// Tests gcore/tiff_common.cpp.

#include "third_party/gdal/gcore/tiff_common.h"

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal_mdreader.h"
#include "third_party/gdal/port/cpl_conv.h"

namespace {

TEST(TiffCommonTest, PrepareTIFFErrorFormat) {
  char* fmt = gdal::tiff_common::PrepareTIFFErrorFormat("mod%name%", "err %s");
  ASSERT_NE(nullptr, fmt);
  EXPECT_STREQ("mod%%name%%:err %s", fmt);
  CPLFree(fmt);
}

TEST(TiffCommonTest, TIFFColorMapTagToColorTable) {
  // Case 1: values in [0, 255] range -> multiplier fixed up to 1.
  {
    unsigned short r[2] = {0, 100};
    unsigned short g[2] = {0, 150};
    unsigned short b[2] = {0, 200};
    int mult = 0;
    auto ct = gdal::tiff_common::TIFFColorMapTagToColorTable(
        r, g, b, 2, mult, 257, /*bNoDataSet=*/true, /*dfNoDataValue=*/0.0);
    ASSERT_NE(nullptr, ct);
    EXPECT_EQ(1, mult);
    EXPECT_EQ(0, ct->GetColorEntry(0)->c4);
    EXPECT_EQ(100, ct->GetColorEntry(1)->c1);
    EXPECT_EQ(150, ct->GetColorEntry(1)->c2);
    EXPECT_EQ(200, ct->GetColorEntry(1)->c3);
    EXPECT_EQ(255, ct->GetColorEntry(1)->c4);
  }

  // Case 2: multiples of 256 -> multiplier = 256.
  {
    unsigned short r[2] = {0, 2560};
    unsigned short g[2] = {0, 5120};
    unsigned short b[2] = {0, 7680};
    int mult = 0;
    auto ct = gdal::tiff_common::TIFFColorMapTagToColorTable(
        r, g, b, 2, mult, 257, /*bNoDataSet=*/false, 0.0);
    ASSERT_NE(nullptr, ct);
    EXPECT_EQ(256, mult);
    EXPECT_EQ(10, ct->GetColorEntry(1)->c1);
    EXPECT_EQ(20, ct->GetColorEntry(1)->c2);
    EXPECT_EQ(30, ct->GetColorEntry(1)->c3);
  }

  // Case 3: not multiples of 256 or 257 -> defaults to
  // nDefaultColorTableMultiplier.
  {
    unsigned short r[2] = {0, 1000};
    unsigned short g[2] = {0, 2000};
    unsigned short b[2] = {0, 3000};
    int mult = 0;
    auto ct = gdal::tiff_common::TIFFColorMapTagToColorTable(
        r, g, b, 2, mult, 257, /*bNoDataSet=*/false, 0.0);
    ASSERT_NE(nullptr, ct);
    EXPECT_EQ(257, mult);
  }

  // Case 4: explicit non-zero multiplier passed in.
  {
    unsigned short r[1] = {65535};
    unsigned short g[1] = {65535};
    unsigned short b[1] = {65535};
    int mult = 257;
    auto ct = gdal::tiff_common::TIFFColorMapTagToColorTable(
        r, g, b, 1, mult, 257, /*bNoDataSet=*/false, 0.0);
    ASSERT_NE(nullptr, ct);
    EXPECT_EQ(255, ct->GetColorEntry(0)->c1);
  }
}

TEST(TiffCommonTest, TIFFRPCTagToRPCMetadata) {
  double rpc[92];
  for (int i = 0; i < 92; ++i) {
    rpc[i] = static_cast<double>(i + 1);
  }
  CPLStringList md = gdal::tiff_common::TIFFRPCTagToRPCMetadata(rpc);
  EXPECT_STREQ("1", md.FetchNameValue(RPC_ERR_BIAS));
  EXPECT_STREQ("2", md.FetchNameValue(RPC_ERR_RAND));
  EXPECT_STREQ("3", md.FetchNameValue(RPC_LINE_OFF));
  EXPECT_STREQ("4", md.FetchNameValue(RPC_SAMP_OFF));
  EXPECT_STREQ("5", md.FetchNameValue(RPC_LAT_OFF));
  EXPECT_STREQ("6", md.FetchNameValue(RPC_LONG_OFF));
  EXPECT_STREQ("7", md.FetchNameValue(RPC_HEIGHT_OFF));
  EXPECT_STREQ("8", md.FetchNameValue(RPC_LINE_SCALE));
  EXPECT_STREQ("9", md.FetchNameValue(RPC_SAMP_SCALE));
  EXPECT_STREQ("10", md.FetchNameValue(RPC_LAT_SCALE));
  EXPECT_STREQ("11", md.FetchNameValue(RPC_LONG_SCALE));
  EXPECT_STREQ("12", md.FetchNameValue(RPC_HEIGHT_SCALE));
  EXPECT_NE(nullptr, md.FetchNameValue(RPC_LINE_NUM_COEFF));
  EXPECT_NE(nullptr, md.FetchNameValue(RPC_LINE_DEN_COEFF));
  EXPECT_NE(nullptr, md.FetchNameValue(RPC_SAMP_NUM_COEFF));
  EXPECT_NE(nullptr, md.FetchNameValue(RPC_SAMP_DEN_COEFF));
}

}  // namespace
