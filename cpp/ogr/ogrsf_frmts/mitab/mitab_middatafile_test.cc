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

#include <memory>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

TEST(MitabMidDataFileTest, OpenModesAndErrors) {
  MIDDATAFile file("UTF-8");
  // Invalid mode
  EXPECT_EQ(-1, file.Open("/vsimem/mid_test/none.mid", "invalid"));

  // Non-existent read
  EXPECT_EQ(-1, file.Open("/vsimem/mid_test/nonexistent.mid", "r"));
}

TEST(MitabMidDataFileTest, WriteAndRead) {
  constexpr char kPath[] = "/vsimem/mid_test/test.mid";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mid_test"); };
  VSIMkdir("/vsimem/mid_test", 0777);

  // Write mode
  {
    MIDDATAFile file("UTF-8");
    EXPECT_EQ(0, file.Open(kPath, "w"));
    // Attempting to open again while open returns -1
    EXPECT_EQ(-1, file.Open(kPath, "w"));
    // Rewind in write mode returns -1
    EXPECT_EQ(-1, file.Rewind());

    file.WriteLine("%s\t%d\n", "FirstLine", 100);
    file.WriteLine("  \tSecondLine,Value\n");
    file.WriteLine("\"Quoted string\",42\n");
    EXPECT_EQ(0, file.Close());
    // Closing when already closed returns 0
    EXPECT_EQ(0, file.Close());
  }

  // Read mode
  {
    MIDDATAFile file("UTF-8");
    EXPECT_EQ(0, file.Open(kPath, "r"));
    file.SetDelimiter(",");
    EXPECT_FALSE(file.GetEof());

    // First line
    const char* l1 = file.GetLine();
    ASSERT_NE(nullptr, l1);
    EXPECT_STREQ("FirstLine\t100", l1);
    EXPECT_STREQ("FirstLine\t100", file.GetLastLine());

    // Second line (leading spaces/tabs skipped when delimiter is not tab)
    const char* l2 = file.GetLine();
    ASSERT_NE(nullptr, l2);
    EXPECT_STREQ("SecondLine,Value", l2);

    // Rewind
    EXPECT_EQ(0, file.Rewind());
    EXPECT_FALSE(file.GetEof());
    const char* l1_again = file.GetLine();
    ASSERT_NE(nullptr, l1_again);
    EXPECT_STREQ("FirstLine\t100", l1_again);

    // Read remaining
    EXPECT_NE(nullptr, file.GetLine());  // second line
    EXPECT_NE(nullptr, file.GetLine());  // third line

    // EOF
    EXPECT_EQ(nullptr, file.GetLine());
    EXPECT_TRUE(file.GetEof());
    EXPECT_EQ(nullptr, file.GetLastLine());

    EXPECT_EQ(0, file.Close());
  }
}

TEST(MitabMidDataFileTest, TokenizedReading) {
  constexpr char kPath[] = "/vsimem/mid_test/tokens.mid";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mid_test"); };
  VSIMkdir("/vsimem/mid_test", 0777);

  VsiMemTempWrapper file(kPath, "Alpha,123,\"Quoted,Comma\"\nBeta,456,Plain\n");

  MIDDATAFile mid("UTF-8");
  EXPECT_EQ(0, mid.Open(kPath, "r"));
  mid.SetDelimiter(",");
  EXPECT_STREQ(",", mid.GetDelimiter());

  char** tokens1 = mid.GetTokenizedNextLine();
  ASSERT_NE(nullptr, tokens1);
  EXPECT_EQ(3, CSLCount(tokens1));
  EXPECT_STREQ("Alpha", tokens1[0]);
  EXPECT_STREQ("123", tokens1[1]);
  EXPECT_STREQ("Quoted,Comma", tokens1[2]);
  CSLDestroy(tokens1);

  char** tokens2 = mid.GetTokenizedNextLine();
  ASSERT_NE(nullptr, tokens2);
  EXPECT_EQ(3, CSLCount(tokens2));
  EXPECT_STREQ("Beta", tokens2[0]);
  EXPECT_STREQ("456", tokens2[1]);
  EXPECT_STREQ("Plain", tokens2[2]);
  CSLDestroy(tokens2);

  // EOF
  EXPECT_EQ(nullptr, mid.GetTokenizedNextLine());
  EXPECT_TRUE(mid.GetEof());
  EXPECT_EQ(0, mid.Close());
}

TEST(MitabMidDataFileTest, MaxLineLengthLimit) {
  constexpr char kPath[] = "/vsimem/mid_test/longline.mid";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mid_test"); };
  VSIMkdir("/vsimem/mid_test", 0777);

  // Line exceeding default 1,000,000 characters limit
  std::string long_line(1000005, 'X');
  long_line += "\n";
  VsiMemTempWrapper file(kPath, long_line);

  MIDDATAFile mid("UTF-8");
  EXPECT_EQ(0, mid.Open(kPath, "r"));

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, mid.GetLine());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Maximum number of characters allowed reached"));
  }

  EXPECT_EQ(0, mid.Rewind());

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, mid.GetTokenizedNextLine());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Maximum number of characters allowed reached"));
  }

  EXPECT_EQ(0, mid.Close());
}

TEST(MitabMidDataFileTest, SavedLineAndTranslation) {
  MIDDATAFile file("CP1252");
  EXPECT_STREQ("CP1252", file.GetEncoding().c_str());
  file.SetEncoding("UTF-8");
  EXPECT_STREQ("UTF-8", file.GetEncoding().c_str());

  // Saved line
  EXPECT_STREQ("", file.GetSavedLine());
  file.SaveLine("SavedContent");
  EXPECT_STREQ("SavedContent", file.GetSavedLine());
  file.SaveLine(nullptr);
  EXPECT_STREQ("", file.GetSavedLine());

  // Translation
  file.SetTranslation(2.0, 3.0, 10.0, 20.0);
  EXPECT_DOUBLE_EQ(2.0, file.GetXMultiplier());
  EXPECT_DOUBLE_EQ(20.0, file.GetXTrans(5.0));  // 5 * 2 + 10
  EXPECT_DOUBLE_EQ(35.0, file.GetYTrans(5.0));  // 5 * 3 + 20

  // EOF
  file.SetEof(TRUE);
  EXPECT_TRUE(file.GetEof());
  file.SetEof(FALSE);
  EXPECT_FALSE(file.GetEof());
}

TEST(MitabMidDataFileTest, IsValidFeature) {
  EXPECT_FALSE(MIDDATAFile::IsValidFeature(""));
  EXPECT_FALSE(MIDDATAFile::IsValidFeature("   "));
  EXPECT_FALSE(MIDDATAFile::IsValidFeature("INVALID_KEYWORD"));

  EXPECT_TRUE(MIDDATAFile::IsValidFeature("NONE"));
  EXPECT_TRUE(MIDDATAFile::IsValidFeature("POINT 1.0 2.0"));
  EXPECT_TRUE(MIDDATAFile::IsValidFeature("LINE 0 0 1 1"));
  EXPECT_TRUE(MIDDATAFile::IsValidFeature("PLINE 2"));
  EXPECT_TRUE(MIDDATAFile::IsValidFeature("REGION 1"));
  EXPECT_TRUE(MIDDATAFile::IsValidFeature("ARC 0 0 1 1 0 90"));
  EXPECT_TRUE(MIDDATAFile::IsValidFeature("TEXT \"hello\""));
  EXPECT_TRUE(MIDDATAFile::IsValidFeature("RECT 0 0 1 1"));
  EXPECT_TRUE(MIDDATAFile::IsValidFeature("ROUNDRECT 0 0 1 1 0.1"));
  EXPECT_TRUE(MIDDATAFile::IsValidFeature("ELLIPSE 0 0 1 1"));
  EXPECT_TRUE(MIDDATAFile::IsValidFeature("MULTIPOINT 3"));
  EXPECT_TRUE(MIDDATAFile::IsValidFeature("COLLECTION 2"));
}

}  // namespace
}  // namespace autotest2
