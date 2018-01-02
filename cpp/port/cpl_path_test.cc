// Tests public functions in cpl_path.cpp.
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

#include "port/cpl_conv.h"

#include <cstdlib>
#include <string>

#include "gunit.h"
#include "third_party/absl/types/optional.h"
#include "gcore/gdal_priv.h"

namespace {

// Tests extracting the directory portion of a file path.
TEST(CplPathTest, CPLGetPath) {
  EXPECT_STREQ("", CPLGetPath(""));
  // nullptr causes a segfault.
  EXPECT_STREQ("abc", CPLGetPath("abc/def.xyz"));
  EXPECT_STREQ("/abc/def", CPLGetPath("/abc/def/"));
  EXPECT_STREQ("/", CPLGetPath("/"));
  EXPECT_STREQ("/abc", CPLGetPath("/abc/def"));
  EXPECT_STREQ("", CPLGetPath("abc"));
  EXPECT_STREQ("/tmp/../bar", CPLGetPath("/tmp/../bar/"));
  EXPECT_STREQ("/a/./b", CPLGetPath("/a/./b/"));
  EXPECT_STREQ("/foo", CPLGetPath("/foo/bar.baz"));
  EXPECT_STREQ("/e//f/g", CPLGetPath("/e//f/g/"));
  // TODO(schwehr): test really large strings.
}

// Tests getting the directory portion of a file path.
// Almost the sampe as CPLGetPath, but with "." rather than ""
// for an empty directory path.
TEST(CplPathTest, CPLGetDirname) {
  EXPECT_STREQ(".", CPLGetDirname(""));
  // nullptr causes a segfault.
  EXPECT_STREQ("abc", CPLGetDirname("abc/def.xyz"));
  EXPECT_STREQ("/abc/def", CPLGetDirname("/abc/def/"));
  EXPECT_STREQ("/", CPLGetDirname("/"));
  EXPECT_STREQ("/abc", CPLGetDirname("/abc/def"));
  EXPECT_STREQ(".", CPLGetDirname("abc"));
  EXPECT_STREQ("/tmp/../bar", CPLGetDirname("/tmp/../bar/"));
  EXPECT_STREQ("/a/./b", CPLGetDirname("/a/./b/"));
}

// Tests getting the non-directory part of a filename.
TEST(CplPathTest, CPLGetFilename) {
  EXPECT_STREQ("", CPLGetFilename(""));
  EXPECT_STREQ(".", CPLGetFilename("."));
  EXPECT_STREQ("", CPLGetFilename("/"));
  EXPECT_STREQ("a", CPLGetFilename("a"));
  EXPECT_STREQ("foo.bar", CPLGetFilename("foo.bar"));
  EXPECT_STREQ("def.xyz", CPLGetFilename("abc/def.xyz"));
  EXPECT_STREQ("def", CPLGetFilename("abc/def"));
  EXPECT_STREQ("", CPLGetFilename("/abc/def/"));
}

// Tests getting the non-extension portion of a filename.
TEST(CplPathTest, CPLGetBasename) {
  EXPECT_STREQ("", CPLGetBasename(""));
  EXPECT_STREQ("", CPLGetBasename("/"));

  // Behavior deviates from documentation:
  // EXPECT_STREQ("", CPLGetBasename("."));
  // EXPECT_STREQ("", CPLGetBasename(".a"));
  // EXPECT_STREQ("", CPLGetBasename("/a/.b"));

  EXPECT_STREQ("a", CPLGetBasename("a"));
  EXPECT_STREQ("a", CPLGetBasename("a.b"));
  EXPECT_STREQ("a.b", CPLGetBasename("a.b.c"));
  EXPECT_STREQ("c", CPLGetBasename("/a/b/c.d"));
  EXPECT_STREQ("def", CPLGetBasename("abc/def.xyz"));
  EXPECT_STREQ("def", CPLGetBasename("abc/def"));
  EXPECT_STREQ("", CPLGetBasename("abc/def/"));
}

// Tests getting the extension at the end of a file path.
TEST(CplPathTest, CPLGetExtension) {
  EXPECT_STREQ("", CPLGetExtension(""));
  EXPECT_STREQ("", CPLGetExtension("a"));
  EXPECT_STREQ("", CPLGetExtension("."));
  // Undocumented behavior.  Expected "b", but got "" for ".b"
  // Reported upstream: http://trac.osgeo.org/gdal/ticket/5373
  EXPECT_STREQ("", CPLGetExtension(".b"));
  EXPECT_STREQ("d", CPLGetExtension("/foo/c.d"));
  EXPECT_STREQ("", CPLGetExtension("/foo.bar/e"));
  EXPECT_STREQ("f", CPLGetExtension("/foo.bar/e.f"));
  EXPECT_STREQ("txt", CPLGetExtension("/foo/bar/normal.txt"));
  EXPECT_STREQ("long", CPLGetExtension("/foo/bar/extra.long"));
  EXPECT_STREQ("e", CPLGetExtension("/a/b/c.d.e"));
}

// TODO(schwehr): Write a test for CPLGetCurrentDir.

// Tests changing the current file extension to a new one.
TEST(CplPathTest, CPLResetExtension) {
  EXPECT_STREQ("a.c", CPLResetExtension("a.b", "c"));
  EXPECT_STREQ("a..c", CPLResetExtension("a.b", ".c"));
  EXPECT_STREQ("a.", CPLResetExtension("a.b", ""));

  EXPECT_STREQ(".", CPLResetExtension("", ""));
  EXPECT_STREQ(".bar", CPLResetExtension("", "bar"));

  EXPECT_STREQ("/foo.baz", CPLResetExtension("/foo.bar", "baz"));
  EXPECT_STREQ("//foo.b", CPLResetExtension("//foo.a", "b"));
  EXPECT_STREQ("/a/.b/c.e", CPLResetExtension("/a/.b/c.d", "e"));
  EXPECT_STREQ("/a.b.d", CPLResetExtension("/a.b.c", "d"));
}

// Tests concatenating a directory path, file basename and extension.
TEST(CplPathTest, CPLFormFilename) {
  EXPECT_STREQ("", CPLFormFilename("", "", ""));
  EXPECT_STREQ("abc/xyz/def.dat", CPLFormFilename("abc/xyz", "def", ".dat"));
  EXPECT_STREQ("def", CPLFormFilename(nullptr, "def", nullptr));
  EXPECT_STREQ("abc/def.dat", CPLFormFilename(nullptr, "abc/def.dat", nullptr));
  EXPECT_STREQ("/abc/xyz/def.dat",
               CPLFormFilename("/abc/xyz/", "def.dat", nullptr));

  EXPECT_STREQ("a/b.c", CPLFormFilename("a", "b", "c"));
  EXPECT_STREQ("//a/b//c.d", CPLFormFilename("//a/b//", "c", ".d"));
  EXPECT_STREQ("a/b..c", CPLFormFilename("a", "b.", "c"));

  EXPECT_STREQ("a/", CPLFormFilename("a", "", ""));
  EXPECT_STREQ("a", CPLFormFilename("", "a", ""));
  EXPECT_STREQ(".a", CPLFormFilename("", "", "a"));
  EXPECT_STREQ(".a", CPLFormFilename("", "", ".a"));
  EXPECT_STREQ("..a", CPLFormFilename("", "", "..a"));
  EXPECT_STREQ("./a", CPLFormFilename(".", "a", ""));
  EXPECT_STREQ("./a", CPLFormFilename("./", "a", ""));
  EXPECT_STREQ("./b/a", CPLFormFilename("./b", "a", ""));
  EXPECT_STREQ("./b/a", CPLFormFilename("./b/", "a", ""));
}

// TODO(schwehr): Write tests for CPLFormCIFilename, CPLProjectRelativeFilename,
// CPLIsFilenameRelative, and CPLExtractRelativePath.

// Tests removal of up to one slash at the end of a path.
TEST(CplPathTest, CPLCleanTrailingSlash) {
  EXPECT_STREQ("", CPLCleanTrailingSlash(""));
  EXPECT_STREQ(".", CPLCleanTrailingSlash("."));
  EXPECT_STREQ("", CPLCleanTrailingSlash("/"));
  EXPECT_STREQ("/a", CPLCleanTrailingSlash("/a"));
  EXPECT_STREQ("/a", CPLCleanTrailingSlash("/a/"));
  EXPECT_STREQ("/", CPLCleanTrailingSlash("//"));
  EXPECT_STREQ("a/", CPLCleanTrailingSlash("a//"));
  EXPECT_STREQ(".a", CPLCleanTrailingSlash(".a/"));
  EXPECT_STREQ(".a/", CPLCleanTrailingSlash(".a//"));

  EXPECT_STREQ("abc/def", CPLCleanTrailingSlash("abc/def/"));
  EXPECT_STREQ("abc/def", CPLCleanTrailingSlash("abc/def"));
  EXPECT_STREQ("c:\\abc\\def", CPLCleanTrailingSlash("c:\\abc\\def\\"));
  EXPECT_STREQ("c:\\abc\\def", CPLCleanTrailingSlash("c:\\abc\\def"));
  EXPECT_STREQ("abc", CPLCleanTrailingSlash("abc"));
}

// TODO(schwehr): Test CPLCorrespondingPaths
// TODO(schwehr): Test CPLGenerateTempFilename
// TODO(schwehr): Test CPLExpandTilde

class CPLSaveEnv {
 public:
  CPLSaveEnv(const char *key) : key_(key) {
    const char *value = getenv(key);
    if (value != nullptr) value_ = value;
  }
  ~CPLSaveEnv() {
    if (value_.has_value())
      setenv(key_.c_str(), value_.value().c_str(), 1 /* overwrite*/);
  }

 private:
  const string key_;
  absl::optional<string> value_;
};

TEST(CplPathTest, CPLGetHomeDir) {
  CPLSaveEnv saved_home("HOME");
  unsetenv("HOME");
  EXPECT_EQ(nullptr, CPLGetHomeDir());
  setenv("HOME", "/home/someone", 1);
  EXPECT_STREQ("/home/someone", CPLGetHomeDir());
}

}  // namespace
