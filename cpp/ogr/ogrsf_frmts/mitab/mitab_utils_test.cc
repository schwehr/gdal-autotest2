// Copyright 2017 Google Inc. All Rights Reserved.
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

#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab_utils.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogrsf_frmts/mitab/mitab.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

TEST(MitabUtilsTest, TABGenerateArc) {
  OGRLineString line;
  // dEndAngle > dStartAngle
  EXPECT_EQ(0, TABGenerateArc(&line, 5, 0.0, 0.0, 10.0, 10.0, 0.0, M_PI));
  EXPECT_EQ(6, line.getNumPoints());
  EXPECT_NEAR(10.0, line.getX(0), 1e-6);
  EXPECT_NEAR(0.0, line.getY(0), 1e-6);

  // dEndAngle < dStartAngle (triggers dEndAngle += 2.0 * M_PI)
  OGRLineString line2;
  EXPECT_EQ(0, TABGenerateArc(&line2, 5, 0.0, 0.0, 10.0, 10.0, M_PI, 0.0));
  EXPECT_EQ(6, line2.getNumPoints());
}

TEST(MitabUtilsTest, TABCloseRing) {
  OGRLinearRing ring;
  EXPECT_EQ(0, TABCloseRing(&ring));
  EXPECT_EQ(0, ring.getNumPoints());

  ring.addPoint(0, 0);
  ring.addPoint(1, 0);
  ring.addPoint(1, 1);
  EXPECT_FALSE(ring.get_IsClosed());
  EXPECT_EQ(0, TABCloseRing(&ring));
  EXPECT_TRUE(ring.get_IsClosed());
  EXPECT_EQ(4, ring.getNumPoints());

  // Already closed
  EXPECT_EQ(0, TABCloseRing(&ring));
  EXPECT_EQ(4, ring.getNumPoints());
}

TEST(MitabUtilsTest, TABAdjustFilenameExtension) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mitab_utils_case"); };
  VSIMkdir("/vsimem/mitab_utils_case", 0777);
  VSIMkdir("/vsimem/mitab_utils_case/SubDir", 0777);
  VsiMemTempWrapper file("/vsimem/mitab_utils_case/SubDir/MyFile.Tab",
                         "content");

  // Exact file exists
  char path1[128] = "/vsimem/mitab_utils_case/SubDir/MyFile.Tab";
  EXPECT_EQ(TRUE, TABAdjustFilenameExtension(path1));
  EXPECT_STREQ("/vsimem/mitab_utils_case/SubDir/MyFile.Tab", path1);

  // Uppercase extension
  char path2[128] = "/vsimem/mitab_utils_case/SubDir/MyFile.TAB";
  // The file is MyFile.Tab on Unix case-sensitive VSI
  EXPECT_EQ(TRUE, TABAdjustFilenameExtension(path2));
  EXPECT_STREQ("/vsimem/mitab_utils_case/SubDir/MyFile.Tab", path2);

  // Lowercase / mixed case directory and filename
  char path3[128] = "/vsimem/mitab_utils_case/subdir/myfile.tab";
  EXPECT_EQ(TRUE, TABAdjustFilenameExtension(path3));
  EXPECT_STREQ("/vsimem/mitab_utils_case/SubDir/MyFile.Tab", path3);

  // Non-existent file
  char path4[128] = "/vsimem/mitab_utils_case/nonexistent.tab";
  EXPECT_EQ(FALSE, TABAdjustFilenameExtension(path4));

  // No extension
  char path5[128] = "/vsimem/mitab_utils_case/noext";
  EXPECT_EQ(FALSE, TABAdjustFilenameExtension(path5));
}

TEST(MitabUtilsTest, TABGetBasename) {
  {
    std::unique_ptr<char, CplFreeDeleter> base(
        TABGetBasename("/path/to/test.tab"));
    EXPECT_STREQ("test", base.get());
  }
  {
    std::unique_ptr<char, CplFreeDeleter> base(
        TABGetBasename("C:\\windows\\test.TAB"));
    EXPECT_STREQ("test", base.get());
  }
  {
    std::unique_ptr<char, CplFreeDeleter> base(TABGetBasename("simple.tab"));
    EXPECT_STREQ("simple", base.get());
  }
  {
    std::unique_ptr<char, CplFreeDeleter> base(TABGetBasename("no_ext"));
    EXPECT_STREQ("no_ext", base.get());
  }
  {
    std::unique_ptr<char, CplFreeDeleter> base(
        TABGetBasename("/dir.with.dot/file"));
    EXPECT_STREQ("file", base.get());
  }
}

TEST(MitabUtilsTest, TAB_CSLLoad) {
  EXPECT_EQ(nullptr, TAB_CSLLoad("/vsimem/nonexistent.txt"));

  constexpr char kPath[] = "/vsimem/mitab_utils_csl.txt";
  VsiMemTempWrapper file(kPath, "line 1\nline 2\nline 3\n");
  char** list = TAB_CSLLoad(kPath);
  ASSERT_NE(nullptr, list);
  EXPECT_EQ(3, CSLCount(list));
  EXPECT_STREQ("line 1", list[0]);
  EXPECT_STREQ("line 2", list[1]);
  EXPECT_STREQ("line 3", list[2]);
  CSLDestroy(list);
}

TEST(MitabUtilsTest, TABUnEscapeString) {
  EXPECT_EQ(nullptr, TABUnEscapeString(nullptr, FALSE));

  char no_escape[] = "hello world";
  EXPECT_EQ(no_escape, TABUnEscapeString(no_escape, FALSE));

  // bSrcIsConst = TRUE
  char const_src[] = "line1\\nline2\\\\rest";
  char* unesc1 = TABUnEscapeString(const_src, TRUE);
  EXPECT_NE(const_src, unesc1);
  EXPECT_STREQ("line1\nline2\\rest", unesc1);
  CPLFree(unesc1);

  // bSrcIsConst = FALSE (in-place)
  char mut_src[] = "line1\\nline2\\\\rest";
  char* unesc2 = TABUnEscapeString(mut_src, FALSE);
  EXPECT_EQ(mut_src, unesc2);
  EXPECT_STREQ("line1\nline2\\rest", unesc2);
}

TEST(MitabUtilsTest, TABEscapeString) {
  EXPECT_EQ(nullptr, TABEscapeString(nullptr));

  char no_nl[] = "hello\\world";
  EXPECT_EQ(no_nl, TABEscapeString(no_nl));

  char with_nl[] = "line1\nline2\\rest";
  char* esc = TABEscapeString(with_nl);
  EXPECT_NE(with_nl, esc);
  EXPECT_STREQ("line1\\nline2\\\\rest", esc);
  CPLFree(esc);
}

TEST(MitabUtilsTest, TABCleanFieldName) {
  // Strict laundering:
  // 1. Name > 31 chars
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<char, CplFreeDeleter> name(TABCleanFieldName(
        "this_is_a_very_long_field_name_exceeding_thirty_one_chars", "UTF-8",
        /*bStrictLaundering=*/true));
    EXPECT_EQ(31, strlen(name.get()));
    EXPECT_STREQ("this_is_a_very_long_field_name_", name.get());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }
  // 2. Starts with '#'
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<char, CplFreeDeleter> name(
        TABCleanFieldName("#field#2", "UTF-8", /*bStrictLaundering=*/true));
    EXPECT_STREQ("_field#2", name.get());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }
  // 3. Starts with digit in neutral charset
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<char, CplFreeDeleter> name(
        TABCleanFieldName("1field", "", /*bStrictLaundering=*/true));
    EXPECT_STREQ("_field", name.get());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }
  // 4. Invalid chars (spaces, punctuation) with neutral charset
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<char, CplFreeDeleter> name(
        TABCleanFieldName("my field!val", "", /*bStrictLaundering=*/true));
    EXPECT_STREQ("my_field_val", name.get());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }
  // 5. Valid field with non-neutral charset
  {
    CPLErrorReset();
    std::unique_ptr<char, CplFreeDeleter> name(
        TABCleanFieldName("valid_name_2", "UTF-8", /*bStrictLaundering=*/true));
    EXPECT_STREQ("valid_name_2", name.get());
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
  }
  // 6. Non-strict laundering: only spaces replaced
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<char, CplFreeDeleter> name(TABCleanFieldName(
        "field with spaces!@#", "UTF-8", /*bStrictLaundering=*/false));
    EXPECT_STREQ("field_with_spaces!@#", name.get());
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  }
  // 7. Non-strict laundering clean name: no warnings
  {
    CPLErrorReset();
    std::unique_ptr<char, CplFreeDeleter> name(
        TABCleanFieldName("field_clean", "UTF-8", /*bStrictLaundering=*/false));
    EXPECT_STREQ("field_clean", name.get());
    EXPECT_EQ(CE_None, CPLGetLastErrorType());
  }
}

TEST(MitabUtilsTest, TABSaturatedAdd) {
  const auto max_val = std::numeric_limits<GInt32>::max();
  const auto min_val = std::numeric_limits<GInt32>::min();
  std::initializer_list<std::vector<GInt32>> tests = {
      {0, 0, 0},
      {0, -1, -1},
      {0, 1, 1},
      {-1, max_val, max_val - 1},
      {0, max_val, max_val},
      {1, max_val, max_val},
      {-1, min_val, min_val},
      {0, min_val, min_val},
      {1, min_val, min_val + 1},
      {max_val, max_val, max_val},
      {min_val, min_val, min_val},
  };
  for (const auto& v : tests) {
    GInt32 a = v[0];
    GInt32 b = v[1];
    GInt32 expected = v[2];
    TABSaturatedAdd(a, b);
    EXPECT_EQ(expected, a);
    a = v[1];
    b = v[0];
    TABSaturatedAdd(a, b);
    EXPECT_EQ(expected, a);
  }

  // Edge cases:
  // nAdd == int_min && nVal < 0
  GInt32 val = -5;
  TABSaturatedAdd(val, min_val);
  EXPECT_EQ(min_val, val);

  // nAdd != int_min && nAdd < 0 && nVal < int_min - nAdd
  val = min_val + 5;
  TABSaturatedAdd(val, -10);
  EXPECT_EQ(min_val, val);
}

TEST(MitabUtilsTest, TABInt16Diff) {
  EXPECT_EQ(5, TABInt16Diff(10, 5));
  EXPECT_EQ(-5, TABInt16Diff(5, 10));
  EXPECT_EQ(32767, TABInt16Diff(40000, 0));
  EXPECT_EQ(-32768, TABInt16Diff(-40000, 0));
}

}  // namespace
}  // namespace autotest2
