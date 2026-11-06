// Copyright 2014 Google LLC
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
// Tests for https://github.com/OSGeo/gdal/blob/master/port/cpl_path.cpp

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

struct UnaryPathCase {
  const char* input;
  const char* expected;
};

// Tests extracting the directory portion of a file path.
TEST(CplPathTest, CPLGetPath) {
  // Passing nullptr causes a segfault.
  constexpr UnaryPathCase kCases[] = {
      {.input = "", .expected = ""},
      {.input = "abc/def.xyz", .expected = "abc"},
      {.input = "/abc/def/", .expected = "/abc/def"},
      {.input = "/", .expected = "/"},
      {.input = "/abc/def", .expected = "/abc"},
      {.input = "abc", .expected = ""},
      {.input = "/tmp/../bar/", .expected = "/tmp/../bar"},
      {.input = "/a/./b/", .expected = "/a/./b"},
      {.input = "/foo/bar.baz", .expected = "/foo"},
      {.input = "/e//f/g/", .expected = "/e//f/g"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_STREQ(tc.expected, CPLGetPath(tc.input));
  }
}

// Tests getting the directory portion of a file path.
// Almost the same as CPLGetPath, but with "." rather than ""
// for an empty directory path.
TEST(CplPathTest, CPLGetDirname) {
  // Passing nullptr causes a segfault.
  constexpr UnaryPathCase kCases[] = {
      {.input = "", .expected = "."},
      {.input = "abc/def.xyz", .expected = "abc"},
      {.input = "/abc/def/", .expected = "/abc/def"},
      {.input = "/", .expected = "/"},
      {.input = "/abc/def", .expected = "/abc"},
      {.input = "abc", .expected = "."},
      {.input = "/tmp/../bar/", .expected = "/tmp/../bar"},
      {.input = "/a/./b/", .expected = "/a/./b"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_STREQ(tc.expected, CPLGetDirname(tc.input));
  }
}

// Tests getting the non-directory part of a filename.
TEST(CplPathTest, CPLGetFilename) {
  constexpr UnaryPathCase kCases[] = {
      {.input = "", .expected = ""},
      {.input = ".", .expected = "."},
      {.input = "/", .expected = ""},
      {.input = "a", .expected = "a"},
      {.input = "foo.bar", .expected = "foo.bar"},
      {.input = "abc/def.xyz", .expected = "def.xyz"},
      {.input = "abc/def", .expected = "def"},
      {.input = "/abc/def/", .expected = ""},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_STREQ(tc.expected, CPLGetFilename(tc.input));
  }
}

// Tests getting the non-extension portion of a filename.
TEST(CplPathTest, CPLGetBasename) {
  // Behavior deviates from documentation for dot-files (".", ".a", "/a/.b").
  constexpr UnaryPathCase kCases[] = {
      {.input = "", .expected = ""},
      {.input = "/", .expected = ""},
      {.input = "a", .expected = "a"},
      {.input = "a.b", .expected = "a"},
      {.input = "a.b.c", .expected = "a.b"},
      {.input = "/a/b/c.d", .expected = "c"},
      {.input = "abc/def.xyz", .expected = "def"},
      {.input = "abc/def", .expected = "def"},
      {.input = "abc/def/", .expected = ""},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_STREQ(tc.expected, CPLGetBasename(tc.input));
  }
}

// Tests getting the extension at the end of a file path.
TEST(CplPathTest, CPLGetExtension) {
  constexpr UnaryPathCase kCases[] = {
      {.input = "", .expected = ""},
      {.input = "a", .expected = ""},
      {.input = ".", .expected = ""},
      // Undocumented behavior. Expected "b", but got "" for ".b" (upstream
      // issue #5373).
      {.input = ".b", .expected = ""},
      {.input = "/foo/c.d", .expected = "d"},
      {.input = "/foo.bar/e", .expected = ""},
      {.input = "/foo.bar/e.f", .expected = "f"},
      {.input = "/foo/bar/normal.txt", .expected = "txt"},
      {.input = "/foo/bar/extra.long", .expected = "long"},
      {.input = "/a/b/c.d.e", .expected = "e"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_STREQ(tc.expected, CPLGetExtension(tc.input));
  }
}

// Tests changing the current file extension to a new one.
TEST(CplPathTest, CPLResetExtension) {
  struct ResetCase {
    const char* path;
    const char* ext;
    const char* expected;
  };
  constexpr ResetCase kCases[] = {
      {.path = "a.b", .ext = "c", .expected = "a.c"},
      {.path = "a.b", .ext = ".c", .expected = "a..c"},
      {.path = "a.b", .ext = "", .expected = "a."},
      {.path = "", .ext = "", .expected = "."},
      {.path = "", .ext = "bar", .expected = ".bar"},
      {.path = "/foo.bar", .ext = "baz", .expected = "/foo.baz"},
      {.path = "//foo.a", .ext = "b", .expected = "//foo.b"},
      {.path = "/a/.b/c.d", .ext = "e", .expected = "/a/.b/c.e"},
      {.path = "/a.b.c", .ext = "d", .expected = "/a.b.d"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.path);
    EXPECT_STREQ(tc.expected, CPLResetExtension(tc.path, tc.ext));
  }
}

// Tests concatenating a directory path, file basename and extension.
TEST(CplPathTest, CPLFormFilename) {
  struct FormCase {
    const char* path;
    const char* basename;
    const char* ext;
    const char* expected;
  };
  constexpr FormCase kCases[] = {
      {.path = "", .basename = "", .ext = "", .expected = ""},
      {.path = "abc/xyz",
       .basename = "def",
       .ext = ".dat",
       .expected = "abc/xyz/def.dat"},
      {.path = nullptr, .basename = "def", .ext = nullptr, .expected = "def"},
      {.path = nullptr,
       .basename = "abc/def.dat",
       .ext = nullptr,
       .expected = "abc/def.dat"},
      {.path = "/abc/xyz/",
       .basename = "def.dat",
       .ext = nullptr,
       .expected = "/abc/xyz/def.dat"},
      {.path = "a", .basename = "b", .ext = "c", .expected = "a/b.c"},
      {.path = "//a/b//",
       .basename = "c",
       .ext = ".d",
       .expected = "//a/b//c.d"},
      {.path = "a", .basename = "b.", .ext = "c", .expected = "a/b..c"},
      {.path = "a", .basename = "", .ext = "", .expected = "a/"},
      {.path = "", .basename = "a", .ext = "", .expected = "a"},
      {.path = "", .basename = "", .ext = "a", .expected = ".a"},
      {.path = "", .basename = "", .ext = ".a", .expected = ".a"},
      {.path = "", .basename = "", .ext = "..a", .expected = "..a"},
      {.path = ".", .basename = "a", .ext = "", .expected = "./a"},
      {.path = "./", .basename = "a", .ext = "", .expected = "./a"},
      {.path = "./b", .basename = "a", .ext = "", .expected = "./b/a"},
      {.path = "./b/", .basename = "a", .ext = "", .expected = "./b/a"},
      {.path = "/a/b", .basename = "..", .ext = "", .expected = "/a"},
      {.path = "/a/b/", .basename = "..", .ext = "", .expected = "/a"},
      {.path = "/a/b//", .basename = "..", .ext = "", .expected = "/a/b"},
      {.path = "/a/b", .basename = ".", .ext = "", .expected = "/a/b/."},
      {.path = "/a/b/", .basename = ".", .ext = "", .expected = "/a/b/."},
      {.path = "/a/b//", .basename = ".", .ext = "", .expected = "/a/b//."},
      {.path = "/a/b", .basename = "/", .ext = "", .expected = "/a/b//"},
      {.path = "/a/b/", .basename = "/", .ext = "", .expected = "/a/b//"},
      {.path = "/a/b//", .basename = "/", .ext = "", .expected = "/a/b///"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.expected);
    EXPECT_STREQ(tc.expected, CPLFormFilename(tc.path, tc.basename, tc.ext));
  }
}

// Tests removal of up to one slash at the end of a path.
TEST(CplPathTest, CPLCleanTrailingSlash) {
  constexpr UnaryPathCase kCases[] = {
      {.input = "", .expected = ""},
      {.input = ".", .expected = "."},
      {.input = "/", .expected = ""},
      {.input = "/a", .expected = "/a"},
      {.input = "/a/", .expected = "/a"},
      {.input = "//", .expected = "/"},
      {.input = "a//", .expected = "a/"},
      {.input = ".a/", .expected = ".a"},
      {.input = ".a//", .expected = ".a/"},
      {.input = "abc/def/", .expected = "abc/def"},
      {.input = "abc/def", .expected = "abc/def"},
      {.input = "c:\\abc\\def\\", .expected = "c:\\abc\\def"},
      {.input = "c:\\abc\\def", .expected = "c:\\abc\\def"},
      {.input = "abc", .expected = "abc"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.input);
    EXPECT_STREQ(tc.expected, CPLCleanTrailingSlash(tc.input));
  }
}

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
  std::optional<std::string> saved_config;
  if (curr_config != nullptr) {
    saved_config = curr_config;
  }
  absl::Cleanup curr_config_cleanup = [&saved_config] {
    CPLSetConfigOption(
        "HOME", saved_config.has_value() ? saved_config->c_str() : nullptr);
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
  explicit CPLSaveEnv(const char* key) : key_(key) {
    const char* value = getenv(key);
    if (value != nullptr) value_ = value;
  }
  ~CPLSaveEnv() {
    if (value_.has_value())
      setenv(key_.c_str(), value_->c_str(), 1 /* overwrite*/);
    else
      unsetenv(key_.c_str());
  }

 private:
  const std::string key_;
  std::optional<std::string> value_;
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
  std::unique_ptr<char, CplFreeDeleter> cwd(CPLGetCurrentDir());
  EXPECT_NE(nullptr, cwd.get());

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
  static constexpr char kDir[] = "/vsimem/cpl_path_ci_test";
  ASSERT_EQ(0, VSIMkdir(kDir, 0755));
  absl::Cleanup cleanup = [] { VSIRmdirRecursive(kDir); };
  VSILFILE* f1 = VSIFOpenL("/vsimem/cpl_path_ci_test/FOO.TXT", "wb");
  ASSERT_NE(nullptr, f1);
  ASSERT_EQ(0, VSIFCloseL(f1));
  VSILFILE* f2 = VSIFOpenL("/vsimem/cpl_path_ci_test/bar.txt", "wb");
  ASSERT_NE(nullptr, f2);
  ASSERT_EQ(0, VSIFCloseL(f2));

  struct CICase {
    const char* basename;
    const char* ext;
    const char* expected;
  };
  constexpr CICase kCases[] = {
      // Lowercase request matches uppercase file
      {.basename = "foo",
       .ext = "txt",
       .expected = "/vsimem/cpl_path_ci_test/FOO.TXT"},
      // Uppercase request matches lowercase file
      {.basename = "BAR",
       .ext = "TXT",
       .expected = "/vsimem/cpl_path_ci_test/bar.txt"},
      // Mixed case request falls back to VSIReadDir scan
      {.basename = "Foo",
       .ext = "Txt",
       .expected = "/vsimem/cpl_path_ci_test/FOO.TXT"},
      // Exact match
      {.basename = "FOO",
       .ext = "TXT",
       .expected = "/vsimem/cpl_path_ci_test/FOO.TXT"},
      // Non-existent file falls back to CPLFormFilename
      {.basename = "missing",
       .ext = "txt",
       .expected = "/vsimem/cpl_path_ci_test/missing.txt"},
  };
  for (const auto& tc : kCases) {
    SCOPED_TRACE(tc.basename);
    EXPECT_STREQ(tc.expected, CPLFormCIFilename(kDir, tc.basename, tc.ext));
  }
}

TEST(CplPathTest, RelativeAndProjectPaths) {
  EXPECT_TRUE(CPLIsFilenameRelative(""));
  EXPECT_TRUE(CPLIsFilenameRelative("rel/file.tif"));
  EXPECT_FALSE(CPLIsFilenameRelative("/abs/file.tif"));
  EXPECT_FALSE(CPLIsFilenameRelative("\\abs\\file.tif"));
  EXPECT_FALSE(CPLIsFilenameRelative("C:\\abs\\file.tif"));

  struct ProjCase {
    const char* proj_dir;
    const char* sub_path;
    const char* expected;
  };
  constexpr ProjCase kProjCases[] = {
      {.proj_dir = nullptr, .sub_path = "b.tif", .expected = "b.tif"},
      {.proj_dir = "", .sub_path = "b.tif", .expected = "b.tif"},
      {.proj_dir = "/proj", .sub_path = "/abs/b.tif", .expected = "/abs/b.tif"},
      {.proj_dir = "/proj",
       .sub_path = "sub/b.tif",
       .expected = "/proj/sub/b.tif"},
      {.proj_dir = "/proj/",
       .sub_path = "sub/b.tif",
       .expected = "/proj/sub/b.tif"},
  };
  for (const auto& tc : kProjCases) {
    SCOPED_TRACE(tc.expected);
    EXPECT_STREQ(tc.expected,
                 CPLProjectRelativeFilename(tc.proj_dir, tc.sub_path));
  }

  EXPECT_STREQ("/a/b.tif",
               CPLExtractRelativePath(nullptr, "/a/b.tif", nullptr));

  struct ExtractCase {
    const char* base_dir;
    const char* target;
    const char* expected;
    bool expected_got_rel;
  };
  constexpr ExtractCase kExtractCases[] = {
      {.base_dir = nullptr,
       .target = "/a/b.tif",
       .expected = "/a/b.tif",
       .expected_got_rel = false},
      {.base_dir = "",
       .target = "rel/b.tif",
       .expected = "rel/b.tif",
       .expected_got_rel = true},
      {.base_dir = ".",
       .target = "rel/b.tif",
       .expected = "rel/b.tif",
       .expected_got_rel = true},
      {.base_dir = "",
       .target = "/abs/b.tif",
       .expected = "/abs/b.tif",
       .expected_got_rel = false},
      {.base_dir = "/a/b",
       .target = "/c/d/e.tif",
       .expected = "/c/d/e.tif",
       .expected_got_rel = false},
      {.base_dir = "/a/b",
       .target = "/a/b/c/d.tif",
       .expected = "c/d.tif",
       .expected_got_rel = true},
      {.base_dir = "/a/b/",
       .target = "/a/b/c.tif",
       .expected = "/a/b/c.tif",
       .expected_got_rel = false},
  };
  for (const auto& tc : kExtractCases) {
    SCOPED_TRACE(tc.target);
    int got_rel = -1;
    EXPECT_STREQ(tc.expected,
                 CPLExtractRelativePath(tc.base_dir, tc.target, &got_rel));
    EXPECT_EQ(tc.expected_got_rel, static_cast<bool>(got_rel));
  }
}

TEST(CplPathTest, CPLCorrespondingPaths) {
  WithQuietHandler quiet;

  // Empty list -> nullptr
  EXPECT_EQ(nullptr,
            CPLCorrespondingPaths("/a/foo.shp", "/b/bar.shp", nullptr));

  // Single matching element
  const char* single_list[] = {"/a/foo.shp", nullptr};
  StringListPtr res1(CPLCorrespondingPaths("/a/foo.shp", "/b/bar.shp",
                                           const_cast<char**>(single_list)),
                     CSLDestroy);
  ASSERT_NE(nullptr, res1.get());
  EXPECT_EQ(1, CSLCount(res1.get()));
  EXPECT_STREQ("/b/bar.shp", res1.get()[0]);

  // Multi-item list with same basename, different dir
  const char* multi_list[] = {"/old/foo.shp", "/old/foo.shx", "/old/foo.dbf",
                              nullptr};
  StringListPtr res2(CPLCorrespondingPaths("/old/foo.shp", "/new/foo.shp",
                                           const_cast<char**>(multi_list)),
                     CSLDestroy);
  ASSERT_NE(nullptr, res2.get());
  EXPECT_EQ(3, CSLCount(res2.get()));
  EXPECT_STREQ("/new/foo.shx", res2.get()[1]);

  // Multi-item list with different basename
  StringListPtr res3(CPLCorrespondingPaths("/old/foo.shp", "/new/bar.shp",
                                           const_cast<char**>(multi_list)),
                     CSLDestroy);
  ASSERT_NE(nullptr, res3.get());
  EXPECT_EQ(3, CSLCount(res3.get()));
  EXPECT_STREQ("/new/bar.shx", res3.get()[1]);

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
}  // namespace autotest2
