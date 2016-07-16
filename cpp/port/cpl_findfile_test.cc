// Tests file finding functions.
//
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_findfile.cpp
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
#include "gcore/gdal_priv.h"

namespace {

// Tests the default file finder with and without a location.
TEST(CplFindFileTest, CPLDefaultFileFind) {
  // Make sure that no search paths are registered.  The file finder
  // should then not be able to find anything.  It was likely that
  // nothing was registered using CPLPushFinderLocation, but it is
  // better to be sure.
  CPLFinderClean();

  EXPECT_EQ(nullptr, CPLDefaultFindFile(nullptr, nullptr));
  EXPECT_EQ(nullptr, CPLDefaultFindFile("", ""));
  EXPECT_EQ(nullptr, CPLDefaultFindFile("a", "b"));

  const string temp_dir("/vsimem");
  CPLPushFinderLocation(temp_dir.c_str());

  const string filename(temp_dir + "/baz.txt");
  VSILFILE *file = VSIFOpenL(filename.c_str(), "w");
  ASSERT_NE(nullptr, file);
  ASSERT_EQ(3, VSIFWriteL("012", 1, 3, file));
  ASSERT_EQ(0, VSIFCloseL(file));

  // Class is a legacy argument to the find file routines that is now
  // ignored.  It was originally one of "epsg_csv", "gdal", or "s57".
  EXPECT_STREQ(filename.c_str(), CPLDefaultFindFile("", "baz.txt"));
  EXPECT_STREQ(filename.c_str(), CPLDefaultFindFile("dummy", "baz.txt"));

  // The default finder does not respect/expect fully qualified paths.
  EXPECT_STREQ(nullptr, CPLDefaultFindFile("", filename.c_str()));
  CPLPushFinderLocation("/");
  EXPECT_STREQ(("/" + filename).c_str(),
               CPLDefaultFindFile("", filename.c_str()));

  EXPECT_STREQ(nullptr, CPLDefaultFindFile("", "does_not_exist.txt"));

  ASSERT_EQ(0, VSIUnlink(filename.c_str()));
  CPLFinderClean();
}

// TODO(schwehr): Test the rest of the functions.

}  // namespace
