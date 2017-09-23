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
// Tests hfatype.cpp.
//
// See:
//   https://trac.osgeo.org/gdal/browser/trunk/gdal/frmts/hfa/hfatype.cpp

#include "port/cpl_port.h"
#include "frmts/hfa/hfa_p.h"

#include <unistd.h>
#include <cstdio>
#include <string>

#include "file/base/helpers.h"
#include "file/base/options.h"
#include "file/base/path.h"
#include "gmock.h"
#include "googletest.h"
#include "gunit.h"

namespace {

// The contents written by Dump for the input line in this test should look like
// this:
//
// HFAType Eimg_StatisticsParameters830/0 bytes
//     Emif_String         p LayerNames[0];
//     BASEDATA            * ExcludedValues[1];
//     Emif_String           AOIname[1];
//     ULONG                 SkipFactorX[1];
//     ULONG                 SkipFactorY[1];
//     Edsc_BinFunction    * BinFunction[1];
TEST(HfatypeTest, Dump) {
  constexpr char kInput[] =
      "{0:poEmif_String,LayerNames,1:*bExcludedValues,1:oEmif_String,AOIname,1:"
      "lSkipFactorX,1:lSkipFactorY,1:*oEdsc_BinFunction,BinFunction,}Eimg_"
      "StatisticsParameters830,.";

  const string filename = file::JoinPath(FLAGS_test_tmpdir, "dump");
  FILE *dump = fopen(filename.c_str(), "w+");
  ASSERT_NE(nullptr, dump);

  HFAType hfa_type;
  hfa_type.Initialize(kInput);
  hfa_type.Dump(dump);

  EXPECT_EQ(0, fclose(dump));

  string contents;
  ASSERT_OK(file::GetContents(filename, &contents, file::Defaults()));
  EXPECT_THAT(contents, testing::ContainsRegex(R"RE(Emif_String\s+AOIname)RE"));

  // Remove the file.
  EXPECT_EQ(0, unlink(filename.c_str()));
}

}  // namespace
