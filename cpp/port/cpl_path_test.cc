// Tests public functions in cpl_path.cpp.
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

// Tests for https://github.com/OSGeo/gdal/blob/master/port/cpl_path.cpp

#include <cstdlib>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/types/optional.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

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
// Almost the same as CPLGetPath, but with "." rather than ""
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
  // Reported upstream: https://trac.osgeo.org/gdal/ticket/5373
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

  EXPECT_STREQ("/a", CPLFormFilename("/a/b", "..", ""));
  EXPECT_STREQ("/a", CPLFormFilename("/a/b/", "..", ""));
  EXPECT_STREQ("/a/b", CPLFormFilename("/a/b//", "..", ""));

  EXPECT_STREQ("/a/b/.", CPLFormFilename("/a/b", ".", ""));
  EXPECT_STREQ("/a/b/.", CPLFormFilename("/a/b/", ".", ""));
  EXPECT_STREQ("/a/b//.", CPLFormFilename("/a/b//", ".", ""));

  EXPECT_STREQ("/a/b//", CPLFormFilename("/a/b", "/", ""));
  EXPECT_STREQ("/a/b//", CPLFormFilename("/a/b/", "/", ""));
  EXPECT_STREQ("/a/b///", CPLFormFilename("/a/b//", "/", ""));
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

TEST(CplExpandTilde, NoTilde) {
  EXPECT_STREQ("/foo/bar", CPLExpandTilde("/foo/bar"));
}

TEST(CplExpandTilde, TildeNotAtBeginning) {
  EXPECT_STREQ("/foo/bar/~/", CPLExpandTilde("/foo/bar/~/"));
}

TEST(CplExpandTilde, WillExpand) {
  // TODO: b/335317901 - CPLExpandTilde doesn't look up USERPROFILE on Windows.

  // Save/restore HOME config. Use CPLGetGlobalConfigOption to avoid reading
  // the HOME env var, which would pollute config state for other tests.
  // Note, CPLSetConfigOption invalidates this ptr.
  const char* curr_config = CPLGetGlobalConfigOption("HOME", nullptr);
  char* saved_config = curr_config ? CPLStrdup(curr_config) : nullptr;
  absl::Cleanup curr_config_cleanup = [saved_config] {
    CPLSetConfigOption("HOME", saved_config);
    CPLFree(saved_config);
  };

  CPLSetConfigOption("HOME", "/foo");

#ifdef WIN32
  EXPECT_STREQ("/foo\\bar", CPLExpandTilde("~/bar"));
#else
  EXPECT_STREQ("/foo/bar", CPLExpandTilde("~/bar"));
#endif
}

class CPLSaveEnv {
 public:
  CPLSaveEnv(const char* key) : key_(key) {
    const char* value = getenv(key);
    if (value != nullptr) value_ = value;
  }
  ~CPLSaveEnv() {
    if (value_.has_value())
      setenv(key_.c_str(), value_.value().c_str(), 1 /* overwrite*/);
    else
      unsetenv(key_.c_str());
  }

 private:
  const std::string key_;
  absl::optional<std::string> value_;
};

TEST(CplPathTest, CPLGetHomeDir) {
  CPLSaveEnv saved_home("HOME");
  unsetenv("HOME");
  EXPECT_EQ(nullptr, CPLGetHomeDir());
  setenv("HOME", "/home/someone", 1);
  EXPECT_STREQ("/home/someone", CPLGetHomeDir());
}

TEST(CplPathTest, VsicurlAndDriveAndOverlongPaths) {
  WithQuietHandler quiet;
  EXPECT_STREQ("/vsicurl/http://example.com/dir?foo=bar",
               CPLGetPath("/vsicurl/http://example.com/dir/file.tif?foo=bar"));
  EXPECT_STREQ(
      "/vsicurl?foo=bar&url=http%3A%2F%2Fexample.com%2Fdir",
      CPLGetPath(
          "/vsicurl?foo=bar&url=http%3A%2F%2Fexample.com%2Fdir%2Ffile.tif"));
  EXPECT_STREQ("", CPLGetPath("C:"));

  EXPECT_STREQ(
      "/vsicurl/http://example.com/dir?foo=bar",
      CPLGetDirname("/vsicurl/http://example.com/dir/file.tif?foo=bar"));
  EXPECT_STREQ(
      "/vsicurl?foo=bar&url=http%3A%2F%2Fexample.com%2Fdir",
      CPLGetDirname(
          "/vsicurl?foo=bar&url=http%3A%2F%2Fexample.com%2Fdir%2Ffile.tif"));
  EXPECT_STREQ(".", CPLGetDirname("C:"));

  // Extension >= 10 chars is ignored
  EXPECT_STREQ("", CPLGetExtension("/foo/bar.0123456789abcdef"));

  // Path with slash after dot in CPLResetExtension
  EXPECT_STREQ("/dir.with.dot/file.tif",
               CPLResetExtension("/dir.with.dot/file", "tif"));

  // Overlong string triggers CPLPathReturnTLSString buffer limit error
  std::string long_path(2100, 'a');
  long_path += "/file.txt";
  EXPECT_STREQ("", CPLGetPath(long_path.c_str()));
}

TEST(CplPathTest, CPLGetCurrentDirAndFormFilenameAdvanced) {
  char* cwd = CPLGetCurrentDir();
  EXPECT_NE(nullptr, cwd);
  CPLFree(cwd);

  EXPECT_STREQ("/a/b/c.txt", CPLFormFilename("/a/b", "./c", "txt"));
  EXPECT_STREQ("/vsicurl/http://example.com/dir/file.tif?foo=bar",
               CPLFormFilename("/vsicurl/http://example.com/dir?foo=bar",
                               "file", "tif"));
  EXPECT_STREQ("/", CPLFormFilename("/a", "..", nullptr));
  EXPECT_STREQ("/b", CPLFormFilename("/a", "../b", nullptr));
  EXPECT_STREQ("/a/../..", CPLFormFilename("/a", "../..", nullptr));
  EXPECT_STREQ("/a/d", CPLFormFilename("/a/b/c", "../../d", nullptr));
  EXPECT_STREQ("http://example.com/..",
               CPLFormFilename("http://example.com", "..", nullptr));
}

TEST(CplPathTest, CPLFormCIFilename) {
  const char* kDir = "/vsimem/cpl_path_ci_test";
  ASSERT_EQ(0, VSIMkdir(kDir, 0755));
  absl::Cleanup cleanup = [kDir] { VSIRmdirRecursive(kDir); };
  VSILFILE* f1 = VSIFOpenL("/vsimem/cpl_path_ci_test/FOO.TXT", "wb");
  ASSERT_NE(nullptr, f1);
  ASSERT_EQ(0, VSIFCloseL(f1));
  VSILFILE* f2 = VSIFOpenL("/vsimem/cpl_path_ci_test/bar.txt", "wb");
  ASSERT_NE(nullptr, f2);
  ASSERT_EQ(0, VSIFCloseL(f2));

  // Lowercase request matches uppercase file
  EXPECT_STREQ("/vsimem/cpl_path_ci_test/FOO.TXT",
               CPLFormCIFilename(kDir, "foo", "txt"));
  // Uppercase request matches lowercase file
  EXPECT_STREQ("/vsimem/cpl_path_ci_test/bar.txt",
               CPLFormCIFilename(kDir, "BAR", "TXT"));
  // Mixed case request falls back to VSIReadDir scan
  EXPECT_STREQ("/vsimem/cpl_path_ci_test/FOO.TXT",
               CPLFormCIFilename(kDir, "Foo", "Txt"));
  // Exact match
  EXPECT_STREQ("/vsimem/cpl_path_ci_test/FOO.TXT",
               CPLFormCIFilename(kDir, "FOO", "TXT"));
  // Non-existent file falls back to CPLFormFilename
  EXPECT_STREQ("/vsimem/cpl_path_ci_test/missing.txt",
               CPLFormCIFilename(kDir, "missing", "txt"));
}

TEST(CplPathTest, RelativeAndProjectPaths) {
  EXPECT_TRUE(CPLIsFilenameRelative(""));
  EXPECT_TRUE(CPLIsFilenameRelative("rel/file.tif"));
  EXPECT_FALSE(CPLIsFilenameRelative("/abs/file.tif"));
  EXPECT_FALSE(CPLIsFilenameRelative("\\abs\\file.tif"));
  EXPECT_FALSE(CPLIsFilenameRelative("C:\\abs\\file.tif"));

  EXPECT_STREQ("b.tif", CPLProjectRelativeFilename(nullptr, "b.tif"));
  EXPECT_STREQ("b.tif", CPLProjectRelativeFilename("", "b.tif"));
  EXPECT_STREQ("/abs/b.tif", CPLProjectRelativeFilename("/proj", "/abs/b.tif"));
  EXPECT_STREQ("/proj/sub/b.tif",
               CPLProjectRelativeFilename("/proj", "sub/b.tif"));
  EXPECT_STREQ("/proj/sub/b.tif",
               CPLProjectRelativeFilename("/proj/", "sub/b.tif"));

  int got_rel = -1;
  EXPECT_STREQ("/a/b.tif",
               CPLExtractRelativePath(nullptr, "/a/b.tif", &got_rel));
  EXPECT_FALSE(got_rel);
  EXPECT_STREQ("/a/b.tif",
               CPLExtractRelativePath(nullptr, "/a/b.tif", nullptr));

  EXPECT_STREQ("rel/b.tif", CPLExtractRelativePath("", "rel/b.tif", &got_rel));
  EXPECT_TRUE(got_rel);
  EXPECT_STREQ("rel/b.tif", CPLExtractRelativePath(".", "rel/b.tif", &got_rel));
  EXPECT_TRUE(got_rel);

  EXPECT_STREQ("/abs/b.tif",
               CPLExtractRelativePath("", "/abs/b.tif", &got_rel));
  EXPECT_FALSE(got_rel);

  EXPECT_STREQ("/c/d/e.tif",
               CPLExtractRelativePath("/a/b", "/c/d/e.tif", &got_rel));
  EXPECT_FALSE(got_rel);

  EXPECT_STREQ("c/d.tif",
               CPLExtractRelativePath("/a/b", "/a/b/c/d.tif", &got_rel));
  EXPECT_TRUE(got_rel);

  EXPECT_STREQ("/a/b/c.tif",
               CPLExtractRelativePath("/a/b/", "/a/b/c.tif", &got_rel));
  EXPECT_FALSE(got_rel);
}

TEST(CplPathTest, CPLCorrespondingPaths) {
  WithQuietHandler quiet;

  // Empty list -> nullptr
  EXPECT_EQ(nullptr,
            CPLCorrespondingPaths("/a/foo.shp", "/b/bar.shp", nullptr));

  // Single matching element
  const char* single_list[] = {"/a/foo.shp", nullptr};
  char** res1 = CPLCorrespondingPaths("/a/foo.shp", "/b/bar.shp",
                                      const_cast<char**>(single_list));
  ASSERT_NE(nullptr, res1);
  EXPECT_EQ(1, CSLCount(res1));
  EXPECT_STREQ("/b/bar.shp", res1[0]);
  CSLDestroy(res1);

  // Multi-item list with same basename, different dir
  const char* multi_list[] = {"/old/foo.shp", "/old/foo.shx", "/old/foo.dbf",
                              nullptr};
  char** res2 = CPLCorrespondingPaths("/old/foo.shp", "/new/foo.shp",
                                      const_cast<char**>(multi_list));
  ASSERT_NE(nullptr, res2);
  EXPECT_EQ(3, CSLCount(res2));
  EXPECT_STREQ("/new/foo.shx", res2[1]);
  CSLDestroy(res2);

  // Multi-item list with different basename
  char** res3 = CPLCorrespondingPaths("/old/foo.shp", "/new/bar.shp",
                                      const_cast<char**>(multi_list));
  ASSERT_NE(nullptr, res3);
  EXPECT_EQ(3, CSLCount(res3));
  EXPECT_STREQ("/new/bar.shx", res3[1]);
  CSLDestroy(res3);

  // Multi-item list with mismatched basename -> nullptr
  const char* bad_list[] = {"/old/foo.shp", "/old/other.shx", nullptr};
  EXPECT_EQ(nullptr, CPLCorrespondingPaths("/old/foo.shp", "/new/bar.shp",
                                           const_cast<char**>(bad_list)));

  // Multi-item list with extra extension on prototype -> nullptr
  const char* ext_list[] = {"/old/foo.tar.gz", "/old/foo.aux", nullptr};
  EXPECT_EQ(nullptr,
            CPLCorrespondingPaths("/old/foo.tar.gz", "/new/bar.tar.bz2",
                                  const_cast<char**>(ext_list)));
}

TEST(CplPathTest, TempFilenameLaunderAndTraversal) {
  CPLSaveEnv save_cpl_tmpdir("CPL_TMPDIR");
  CPLSaveEnv save_tmpdir("TMPDIR");
  CPLSaveEnv save_temp("TEMP");

  CPLSetConfigOption("CPL_TMPDIR", "/tmp/cpl_tmpdir");
  EXPECT_TRUE(
      STARTS_WITH(CPLGenerateTempFilename("stem"), "/tmp/cpl_tmpdir/stem"));
  EXPECT_TRUE(
      STARTS_WITH(CPLGenerateTempFilename(nullptr), "/tmp/cpl_tmpdir/"));
  CPLSetConfigOption("CPL_TMPDIR", nullptr);
  unsetenv("CPL_TMPDIR");

  setenv("TMPDIR", "/tmp/env_tmpdir", 1);
  EXPECT_TRUE(STARTS_WITH(CPLGenerateTempFilename("s"), "/tmp/env_tmpdir/s"));
  unsetenv("TMPDIR");

  setenv("TEMP", "/tmp/env_temp", 1);
  EXPECT_TRUE(STARTS_WITH(CPLGenerateTempFilename(""), "/tmp/env_temp/"));
  unsetenv("TEMP");

  // Falls back to "."
  EXPECT_NE(nullptr, CPLGenerateTempFilename("local"));

  // LaunderForFilename
  EXPECT_STREQ("a_b_c_d_e_f_g|h_i_j",
               CPLLaunderForFilename("a<b>c:d\"e/f\\g|h?i*j", nullptr));
  EXPECT_EQ("a_b", CPLLaunderForFilenameSafe("a:b", nullptr));

  // Path traversal
  EXPECT_TRUE(CPLHasPathTraversal(".."));
  EXPECT_TRUE(CPLHasPathTraversal("../a"));
  EXPECT_TRUE(CPLHasPathTraversal("a/../b"));
  EXPECT_TRUE(CPLHasPathTraversal("a/.."));
  EXPECT_FALSE(CPLHasPathTraversal("a..b"));

  CPLSetConfigOption("CPL_ENABLE_PATH_TRAVERSAL_DETECTION", "NO");
  EXPECT_FALSE(CPLHasPathTraversal(".."));
  CPLSetConfigOption("CPL_ENABLE_PATH_TRAVERSAL_DETECTION", nullptr);

  // Unbalanced path traversal
  EXPECT_FALSE(CPLHasUnbalancedPathTraversal("./a//b/../c"));
  EXPECT_FALSE(CPLHasUnbalancedPathTraversal("a/.."));
  EXPECT_TRUE(CPLHasUnbalancedPathTraversal("a/../.."));
  EXPECT_TRUE(CPLHasUnbalancedPathTraversal("../a"));
}

}  // namespace
