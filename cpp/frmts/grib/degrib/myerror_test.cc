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
// Tests degrib myerror.c.
//
// https://trac.osgeo.org/gdal/browser/trunk/gdal/frmts/grib/degrib18/degrib/myerror.c
//
// See also:
//   https://www.weather.gov/mdl/degrib_home
//   http://www.gdal.org/frmt_grib.html
//   https://trac.osgeo.org/gdal/browser/trunk/autotest/gdrivers/grib.py

#include "frmts/grib/degrib18/degrib/myerror.h"

#include <memory>

#include "gunit.h"

namespace {

TEST(MyErrorTest, MallocSprintf) {
  // Anything pointed to by ptr will be leaked inside mallocSprintf.
  char *ptr = nullptr;
  mallocSprintf(&ptr, "a");
  ASSERT_NE(nullptr, ptr);
  EXPECT_STREQ("a", ptr);
  free(ptr);

  mallocSprintf(&ptr, "%d", 1);
  EXPECT_STREQ("1", ptr);
  free(ptr);

  // Calling with a nullptr for the format will set ptr to nullptr.
  mallocSprintf(&ptr, nullptr);
  EXPECT_EQ(nullptr, ptr);
}

TEST(MyErrorTest, ReallocSprintf) {
  // Anything pointed to by ptr will be leaked inside mallocSprintf.
  char *ptr = nullptr;
  reallocSprintf(&ptr, "a");
  ASSERT_NE(nullptr, ptr);
  EXPECT_STREQ("a", ptr);

  reallocSprintf(&ptr, "bc");
  EXPECT_STREQ("abc", ptr);

  // Calling with a nullptr for the format is a no-op.
  reallocSprintf(&ptr, nullptr);
  EXPECT_STREQ("abc", ptr);

  // Try shrinking.
  reallocSprintf(&ptr, "");
  EXPECT_STREQ("abc", ptr);

  free(ptr);
}

TEST(MyErrorTest, ErrSprintf) {
  char *msg = errSprintf(nullptr);
  ASSERT_EQ(nullptr, msg);

  EXPECT_EQ(nullptr, errSprintf("a"));
  EXPECT_EQ(nullptr, errSprintf("b"));
  EXPECT_EQ(nullptr, errSprintf("%d", 1));
  msg = errSprintf(nullptr);
  ASSERT_NE(nullptr, msg);
  EXPECT_STREQ("ab1", msg);
  free(msg);

  EXPECT_EQ(nullptr, errSprintf("c"));
  msg = errSprintf(nullptr);
  EXPECT_STREQ("c", msg);
  free(msg);
}

TEST(MyErrorTest, PreErrSprintf) {
  preErrSprintf("a");
  preErrSprintf(nullptr);  // noop.
  errSprintf("b");
  preErrSprintf("%0.1f", 1.2);
  char *msg = errSprintf(nullptr);
  ASSERT_NE(nullptr, msg);
  EXPECT_STREQ("1.2ab", msg);
  free(msg);
}

constexpr sChar kNoWarnings = -1;
constexpr sChar kNotation = 0;
constexpr sChar kWarnings = 1;
constexpr sChar kErrors = 2;

TEST(MyErrorTest, MyWarnRet) {
  EXPECT_FALSE(myWarnNotEmpty());
  EXPECT_EQ(kNoWarnings, myWarnLevel());

  char *msg = nullptr;
  EXPECT_EQ(kNoWarnings, myWarnClear(&msg, 0));
  ASSERT_EQ(nullptr, msg);

  uChar out_type = 1;     // Memory and stdout.
  uChar detail = 0;       // All.
  uChar file_detail = 0;  // All.
  myWarnSet(out_type, detail, file_detail, nullptr);

  for (const sChar warn_level : {kNotation, kWarnings, kErrors}) {
    constexpr int app_err = 3;
    constexpr int line_num = 4;
    EXPECT_EQ(app_err, myWarnRet(warn_level, app_err, "file", line_num, "msg"));
    EXPECT_TRUE(myWarnNotEmpty());
    EXPECT_EQ(warn_level, myWarnLevel());

    EXPECT_EQ(warn_level, myWarnClear(&msg, 0));
    EXPECT_STREQ("(file, line 4) msg", msg);
    free(msg);
    EXPECT_FALSE(myWarnNotEmpty());
    EXPECT_EQ(kNoWarnings, myWarnLevel());
  }

  for (const sChar warn_level : {kNotation, kWarnings, kErrors}) {
    constexpr int app_err = 5;
    constexpr int line_num = 6;
    EXPECT_EQ(app_err, myWarnRet(warn_level, app_err, "file", line_num, "msg"));
    EXPECT_TRUE(myWarnNotEmpty());
    EXPECT_EQ(warn_level, myWarnLevel());
  }
  EXPECT_EQ(kErrors, myWarnClear(&msg, 0));
  free(msg);
  EXPECT_FALSE(myWarnNotEmpty());
  EXPECT_EQ(kNoWarnings, myWarnLevel());
}

}  // namespace
