// Tests file finding functions.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_findfile.cpp
//
// Copyright 2014 Google Inc. All Rights Reserved.
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

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

// Tests the default file finder with and without a location.
TEST(CplFindFileTest, CPLDefaultFileFind) {
  // Make sure that no search paths are registered. The file finder
  // should then not be able to find anything. It was likely that
  // nothing was registered using CPLPushFinderLocation, but it is
  // better to be sure.
  CPLFinderClean();

  EXPECT_EQ(nullptr, CPLDefaultFindFile(nullptr, nullptr));
  EXPECT_EQ(nullptr, CPLDefaultFindFile("", ""));
  EXPECT_EQ(nullptr, CPLDefaultFindFile("a", "b"));

  // TODO(schwehr): Allow this to work outside of google3.
  const std::string temp_dir(::testing::TempDir());
  CPLPushFinderLocation(temp_dir.c_str());

  const std::string filename(temp_dir + "/baz.txt");
  VSILFILE* file = VSIFOpenL(filename.c_str(), "w");
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

const char* CustomFinder(const char* /*pszClass*/, const char* pszBasename) {
  if (pszBasename != nullptr &&
      std::string(pszBasename) == "custom_found.txt") {
    return "/custom/path/custom_found.txt";
  }
  return nullptr;
}

TEST(CplFindFileTest, FinderStackAndLocations) {
  CPLFinderClean();

  const std::string temp_dir(::testing::TempDir());
  const std::string filename(temp_dir + "/findme.txt");
  VSILFILE* file = VSIFOpenL(filename.c_str(), "w");
  ASSERT_NE(nullptr, file);
  ASSERT_EQ(3, VSIFWriteL("abc", 1, 3, file));
  ASSERT_EQ(0, VSIFCloseL(file));

  CPLPushFinderLocation(temp_dir.c_str());
  // Pushing duplicate location is a no-op.
  CPLPushFinderLocation(temp_dir.c_str());

  EXPECT_STREQ(filename.c_str(), CPLFindFile("gdal", "findme.txt"));
  EXPECT_EQ(nullptr, CPLFindFile("gdal", "nonexistent_file_xyz.txt"));

  CPLPushFileFinder(CustomFinder);
  EXPECT_STREQ("/custom/path/custom_found.txt",
               CPLFindFile("gdal", "custom_found.txt"));
  EXPECT_EQ(CustomFinder, CPLPopFileFinder());

  // Pop all file finders including default, then pop empty stack.
  while (CPLPopFileFinder() != nullptr) {
  }
  EXPECT_EQ(nullptr, CPLPopFileFinder());

  // Pop all finder locations including default, then pop on empty stack.
  CPLPopFinderLocation();
  CPLPopFinderLocation();
  CPLPopFinderLocation();

  ASSERT_EQ(0, VSIUnlink(filename.c_str()));
  CPLFinderClean();
}

}  // namespace
