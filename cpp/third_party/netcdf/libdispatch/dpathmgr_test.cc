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

// Unit tests for //third_party/netcdf/libdispatch/dpathmgr.c.
//
// Covers platform/path-kind inspection, path parsing and cross-platform
// conversion, canonicalization, relative-to-absolute resolution, UTF-8/hex
// utilities, and process-init environment variable flags in an isolated child.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dpathmgr.c

#include <fcntl.h>      // IWYU pragma: keep
#include <sys/stat.h>   // IWYU pragma: keep
#include <sys/types.h>  // IWYU pragma: keep
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"

extern "C" {
#include "third_party/netcdf/include/ncpathmgr.h"
}  // extern "C"

namespace netcdf {
namespace {

// Restores dpathmgr.c's global `platform` override to default on scope exit.
class ScopedPathPlatformReset {
 public:
  ScopedPathPlatformReset() = default;
  ~ScopedPathPlatformReset() { NCpathsetplatform(NCPD_UNKNOWN); }

  ScopedPathPlatformReset(const ScopedPathPlatformReset&) = delete;
  ScopedPathPlatformReset& operator=(const ScopedPathPlatformReset&) = delete;
};

void ExpectCStringEq(std::string_view expected, const ScopedCString& actual) {
  ASSERT_NE(nullptr, actual.get());
  EXPECT_EQ(expected, std::string_view(actual.get()));
}

TEST(DpathmgrTest, KindNamesAndPlatformSelection) {
  struct KindNameCase {
    int kind;
    const char* expected;
  };
  constexpr KindNameCase kKindCases[] = {
      {NCPD_UNKNOWN, "NCPD_UNKNOWN"},
      {NCPD_NIX, "NCPD_NIX"},
      {NCPD_MSYS, "NCPD_MSYS"},
      {NCPD_CYGWIN, "NCPD_CYGWIN"},
      {NCPD_WIN, "NCPD_WIN"},
      {NCPD_REL, "NCPD_REL"},
      {5, "NCPD_UNKNOWN"},
      {-1, "NCPD_UNKNOWN"},
      {99, "NCPD_UNKNOWN"},
  };
  for (const KindNameCase& tc : kKindCases) {
    SCOPED_TRACE(tc.kind);
    EXPECT_STREQ(tc.expected, NCgetkindname(tc.kind));
  }

  ScopedPathPlatformReset platform_guard;
  EXPECT_EQ(NCPD_NIX, NCgetlocalpathkind());

  constexpr int kPlatforms[] = {NCPD_NIX, NCPD_MSYS, NCPD_CYGWIN, NCPD_WIN};
  for (int target : kPlatforms) {
    SCOPED_TRACE(target);
    NCpathsetplatform(target);
    EXPECT_EQ(target, NCgetlocalpathkind());
  }

  NCpathsetplatform(NCPD_UNKNOWN);
  EXPECT_EQ(NCPD_NIX, NCgetlocalpathkind());

  NCpathsetplatform(NCPD_WIN);
  EXPECT_EQ(NCPD_WIN, NCgetlocalpathkind());
  NCpathsetplatform(99);
  EXPECT_EQ(NCPD_NIX, NCgetlocalpathkind());

  NCpathsetplatform(NCPD_CYGWIN);
  EXPECT_EQ(NCPD_CYGWIN, NCgetlocalpathkind());
  NCpathforceplatform();
  EXPECT_EQ(NCPD_NIX, NCgetlocalpathkind());
}

TEST(DpathmgrTest, InputPathKindDetection) {
  struct InputCase {
    const char* name;
    const char* path;
    int expected_kind;
  };
  constexpr InputCase kCases[] = {
      {"null_path", nullptr, NCPD_UNKNOWN},
      {"https_url", "https://example.com/data/test.nc", NCPD_UNKNOWN},
      {"http_url", "http://host:8080/path?x=1#y", NCPD_UNKNOWN},
      {"file_url_triple", "file:///tmp/test.nc", NCPD_UNKNOWN},
      {"file_url_single", "file:/tmp/test.nc", NCPD_UNKNOWN},
      {"file_url_win", "file:C:/tmp/test.nc", NCPD_UNKNOWN},
      {"file_url_rel", "file://rel/test.nc", NCPD_UNKNOWN},
      {"dap_bracket_url", "[mode=1]https://example.com/a.nc", NCPD_UNKNOWN},
      {"nix_root", "/", NCPD_NIX},
      {"nix_abs", "/usr/local/file.nc", NCPD_NIX},
      {"nix_cygdrive_prefix_only", "/cygdrive/", NCPD_NIX},
      {"nix_cygdrive_digit", "/cygdrive/1/foo.nc", NCPD_NIX},
      {"nix_cygdrive_multichar", "/cygdrive/ab/foo.nc", NCPD_NIX},
      {"nix_single_letter_under_nix", "/d/foo/bar.nc", NCPD_NIX},
      {"cygwin_drive_only", "/cygdrive/c", NCPD_CYGWIN},
      {"cygwin_path", "/cygdrive/Z/dir/file.nc", NCPD_CYGWIN},
      {"win_drive_only", "Z:", NCPD_WIN},
      {"win_forward", "C:/foo/bar.nc", NCPD_WIN},
      {"win_backslash", "c:\\foo\\bar.nc", NCPD_WIN},
      {"win_unc_bare", "//", NCPD_WIN},
      {"win_unc_forward", "//server/share/file.nc", NCPD_WIN},
      {"win_unc_backslash", "\\\\server\\share\\file.nc", NCPD_WIN},
      // Do not pass "" to NCgetinputpathkind: testurl("") calls ncuriparse("")
      // which jumps to done: before memset(&tmp, 0, sizeof(tmp)) in ncuri.c.
      {"rel_single_char", "a", NCPD_REL},
      {"rel_dot", ".", NCPD_REL},
      {"rel_simple", "file.nc", NCPD_REL},
      {"rel_forward", "dir/sub/file.nc", NCPD_REL},
      {"rel_backslash", "dir\\sub\\file.nc", NCPD_REL},
      {"rel_win_drive_nodir", "C:rel.nc", NCPD_REL},
      {"rel_digit_drive", "1:/foo.nc", NCPD_REL},
      {"rel_colon_prefix", ":/foo.nc", NCPD_REL},
  };
  for (const InputCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    EXPECT_EQ(tc.expected_kind, NCgetinputpathkind(tc.path));
  }

  ScopedPathPlatformReset platform_guard;
  NCpathsetplatform(NCPD_MSYS);
  EXPECT_EQ(NCPD_MSYS, NCgetinputpathkind("/d"));
  EXPECT_EQ(NCPD_MSYS, NCgetinputpathkind("/D/foo/bar.nc"));
  EXPECT_EQ(NCPD_NIX, NCgetinputpathkind("/1/foo.nc"));
  EXPECT_EQ(NCPD_NIX, NCgetinputpathkind("/ab/foo.nc"));
  EXPECT_EQ(NCPD_NIX, NCgetinputpathkind("/"));
}

TEST(DpathmgrTest, HasDriveLetterAndIsNetworkPath) {
  struct FlagCase {
    const char* path;
    int expected_drive;
    int expected_network;
  };
  constexpr FlagCase kCases[] = {
      {nullptr, 0, 0},
      {"", 0, 0},
      {"rel/path.nc", 0, 0},
      {"/nix/path.nc", 0, 0},
      {"C:rel.nc", 0, 0},
      {"D:", 1, 0},
      {"c:\\dir\\file.nc", 1, 0},
      {"C:/dir/file.nc", 1, 0},
      {"/cygdrive/e", 1, 0},
      {"/cygdrive/E/file.nc", 1, 0},
      {"//", 1, 1},
      {"\\\\", 1, 1},
      {"//server/share/file.nc", 1, 1},
      {"\\\\server\\share\\file.nc", 1, 1},
  };
  for (const FlagCase& tc : kCases) {
    SCOPED_TRACE(tc.path ? tc.path : "<null>");
    EXPECT_EQ(tc.expected_drive, NChasdriveletter(tc.path));
    EXPECT_EQ(tc.expected_network, NCisnetworkpath(tc.path));
  }

  ScopedPathPlatformReset platform_guard;
  EXPECT_EQ(0, NChasdriveletter("/d/data.nc"));
  NCpathsetplatform(NCPD_MSYS);
  EXPECT_EQ(1, NChasdriveletter("/d/data.nc"));
  EXPECT_EQ(0, NCisnetworkpath("/d/data.nc"));
}

TEST(DpathmgrTest, PathCvtLocalNixAndPlatformOverride) {
  EXPECT_EQ(nullptr, MakeScopedCString(NCpathcvt(nullptr)).get());

  struct CvtCase {
    const char* input;
    const char* expected;
  };
  constexpr CvtCase kNixCases[] = {
      {"https://example.com/a/b.nc", "https://example.com/a/b.nc"},
      {"file:///tmp/a.nc", "file:///tmp/a.nc"},
      {"file:C:/data/a.nc", "file:C:/data/a.nc"},
      {"file://rel/a.nc", "file://rel/a.nc"},
      // Do not pass "" to NCpathcvt: testurl("") calls ncuriparse("") which
      // jumps to done: before memset(&tmp, 0, sizeof(tmp)) in ncuri.c.
      {"a", "a"},
      {".", "."},
      {"rel/sub.nc", "rel/sub.nc"},
      {"rel\\sub.nc", "rel/sub.nc"},
      {"/", "/"},
      {"/a/b/c.nc", "/a/b/c.nc"},
      {"\\a\\b\\c.nc", "/a/b/c.nc"},
      {"/cygdrive/c", "/c"},
      {"/cygdrive/D/x/y.nc", "/D/x/y.nc"},
      {"C:", "/C"},
      {"c:\\x\\y.nc", "/c/x/y.nc"},
      {"D:/x/y.nc", "/D/x/y.nc"},
      {"//", "//"},
      {"//srv/sh/f.nc", "///srv/sh/f.nc"},
      {"\\\\srv\\sh\\f.nc", "///srv/sh/f.nc"},
  };
  for (const CvtCase& tc : kNixCases) {
    SCOPED_TRACE(tc.input);
    ExpectCStringEq(tc.expected, MakeScopedCString(NCpathcvt(tc.input)));
  }

  ScopedPathPlatformReset platform_guard;
  NCpathsetplatform(NCPD_WIN);
  ExpectCStringEq("C:\\x\\y.nc", MakeScopedCString(NCpathcvt("C:/x/y.nc")));
  ExpectCStringEq("rel\\x.nc", MakeScopedCString(NCpathcvt("rel/x.nc")));
  // Unparsing NCPD_NIX to NCPD_WIN without a defined mountpoint returns NULL.
  EXPECT_EQ(nullptr, MakeScopedCString(NCpathcvt("/nix/x.nc")).get());
}

TEST(DpathmgrTest, PathCvtTestCrossPlatformMatrix) {
  ScopedPathPlatformReset platform_guard;

  struct MatrixCase {
    const char* name;
    const char* input;
    int ukind;
    int udrive;
    const char* expected;  // nullptr means NCpathcvt_test must return nullptr.
  };
  constexpr MatrixCase kCases[] = {
      // Defensive null and URL pass-through.
      {"null_input", nullptr, NCPD_WIN, 'c', nullptr},
      {"url_passthrough", "https://example.com/f.nc", NCPD_WIN, 'c',
       "https://example.com/f.nc"},

      // Target: NCPD_NIX.
      {"rel_to_nix", "a\\b\\c", NCPD_NIX, 0, "a/b/c"},
      {"nix_to_nix", "/a/b/c", NCPD_NIX, 0, "/a/b/c"},
      {"cyg_drive_to_nix", "/cygdrive/d", NCPD_NIX, 0, "/d"},
      {"cyg_to_nix", "/cygdrive/d/a/b", NCPD_NIX, 0, "/d/a/b"},
      {"win_drive_to_nix", "E:", NCPD_NIX, 0, "/E"},
      {"win_to_nix", "E:\\a\\b", NCPD_NIX, 0, "/E/a/b"},
      {"unc_bare_to_nix", "//", NCPD_NIX, 0, "//"},
      {"unc_to_nix", "\\\\srv\\sh", NCPD_NIX, 0, "///srv/sh"},

      // Target: NCPD_CYGWIN.
      {"rel_to_cyg", "a\\b\\c", NCPD_CYGWIN, 0, "a/b/c"},
      {"nix_to_cyg_nodrive", "/opt/a/b", NCPD_CYGWIN, 0, "/opt/a/b"},
      {"nix_to_cyg_withdrive", "/opt/a/b", NCPD_CYGWIN, 'c',
       "/cygdrive/c/opt/a/b"},
      {"nix_special_bin_to_cyg", "/bin/sh", NCPD_CYGWIN, 'c', "/bin/sh"},
      {"nix_special_dev_to_cyg", "/dev/null", NCPD_CYGWIN, 'c', "/dev/null"},
      {"nix_special_etc_to_cyg", "/etc/hosts", NCPD_CYGWIN, 'c', "/etc/hosts"},
      {"nix_special_home_to_cyg", "/home/u", NCPD_CYGWIN, 'c', "/home/u"},
      {"nix_special_lib_to_cyg", "/lib/x", NCPD_CYGWIN, 'c', "/lib/x"},
      {"nix_special_proc_to_cyg", "/proc/1", NCPD_CYGWIN, 'c', "/proc/1"},
      {"nix_special_sbin_to_cyg", "/sbin/init", NCPD_CYGWIN, 'c', "/sbin/init"},
      {"nix_special_tmp_to_cyg", "/tmp/a.nc", NCPD_CYGWIN, 'c', "/tmp/a.nc"},
      {"nix_special_usr_to_cyg", "/usr/bin", NCPD_CYGWIN, 'c', "/usr/bin"},
      {"nix_special_var_to_cyg", "/var/log", NCPD_CYGWIN, 'c', "/var/log"},
      {"cyg_drive_to_cyg", "/cygdrive/d", NCPD_CYGWIN, 0, "/cygdrive/d"},
      {"cyg_to_cyg", "/cygdrive/d/a/b", NCPD_CYGWIN, 0, "/cygdrive/d/a/b"},
      {"win_drive_to_cyg", "D:", NCPD_CYGWIN, 0, "/cygdrive/D"},
      {"win_to_cyg", "D:\\a\\b", NCPD_CYGWIN, 0, "/cygdrive/D/a/b"},
      {"unc_to_cyg", "\\\\srv\\sh", NCPD_CYGWIN, 0, "/cygdrive///srv/sh"},

      // Target: NCPD_WIN.
      {"rel_to_win", "a/b/c", NCPD_WIN, 0, "a\\b\\c"},
      {"nix_to_win_nodrive_err", "/a/b/c", NCPD_WIN, 0, nullptr},
      {"nix_root_to_win", "/", NCPD_WIN, 'c', "c:\\"},
      {"nix_to_win", "/a/b/c", NCPD_WIN, 'c', "c:\\a\\b\\c"},
      {"cyg_drive_to_win", "/cygdrive/d", NCPD_WIN, 0, "d:"},
      {"cyg_root_to_win", "/cygdrive/c/", NCPD_WIN, 0, "c:\\"},
      {"cyg_to_win", "/cygdrive/d/a/b", NCPD_WIN, 0, "d:\\a\\b"},
      {"win_drive_to_win", "D:", NCPD_WIN, 0, "D:"},
      {"win_to_win", "D:/a/b", NCPD_WIN, 0, "D:\\a\\b"},
      {"unc_bare_to_win", "//", NCPD_WIN, 0, "\\"},
      {"unc_to_win", "//srv/share/f", NCPD_WIN, 0, "\\\\srv\\share\\f"},

      // Target: NCPD_MSYS.
      {"rel_to_msys", "a/b/c", NCPD_MSYS, 0, "a\\b\\c"},
      {"nix_to_msys_nodrive_err", "/opt/a/b", NCPD_MSYS, 0, nullptr},
      {"nix_root_to_msys_err", "/", NCPD_MSYS, 0, nullptr},
      {"nix_to_msys", "/opt/a/b", NCPD_MSYS, 'e', "e:\\opt\\a\\b"},
      {"msys_drive_to_msys", "/d", NCPD_MSYS, 0, "d:"},
      {"msys_to_msys", "/d/a/b", NCPD_MSYS, 0, "d:\\a\\b"},
      {"cyg_to_msys", "/cygdrive/d/a/b", NCPD_MSYS, 0, "d:\\a\\b"},
      {"win_to_msys", "D:/a/b", NCPD_MSYS, 0, "D:\\a\\b"},

      // Invalid target platform triggers NC_EINTERNAL -> nullptr.
      {"invalid_target_platform", "/a/b", 99, 'c', nullptr},
  };

  for (const MatrixCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    ScopedCString actual =
        MakeScopedCString(NCpathcvt_test(tc.input, tc.ukind, tc.udrive));
    if (tc.expected == nullptr) {
      EXPECT_EQ(nullptr, actual.get());
    } else {
      ExpectCStringEq(tc.expected, actual);
    }
  }
}

TEST(DpathmgrTest, PathCanonicalAllFormats) {
  char* untouched = nullptr;
  EXPECT_EQ(NC_NOERR, NCpathcanonical(nullptr, &untouched));
  EXPECT_EQ(nullptr, untouched);

  // Passing canonp == nullptr succeeds without leaking the converted path.
  EXPECT_EQ(NC_NOERR, NCpathcanonical("/a/b/c", nullptr));

  struct CanonCase {
    const char* input;
    const char* expected;
  };
  constexpr CanonCase kCases[] = {
      {"", ""},
      {"/a/b/c", "/a/b/c"},
      {"/tmp/a.nc", "/tmp/a.nc"},
      {"C:\\Users\\test\\file.nc", "/cygdrive/C/Users/test/file.nc"},
      {"C:", "/cygdrive/C"},
      {"/cygdrive/d/a/b", "/cygdrive/d/a/b"},
      {"rel\\sub\\file.nc", "rel/sub/file.nc"},
      {"\\\\srv\\share\\f.nc", "/cygdrive///srv/share/f.nc"},
  };
  for (const CanonCase& tc : kCases) {
    SCOPED_TRACE(tc.input);
    char* raw = nullptr;
    ASSERT_EQ(NC_NOERR, NCpathcanonical(tc.input, &raw));
    ExpectCStringEq(tc.expected, MakeScopedCString(raw));
  }

  ScopedPathPlatformReset platform_guard;
  NCpathsetplatform(NCPD_MSYS);
  {
    char* raw = nullptr;
    ASSERT_EQ(NC_NOERR, NCpathcanonical("/d/foo/bar", &raw));
    ExpectCStringEq("/cygdrive/d/foo/bar", MakeScopedCString(raw));
  }
  {
    char* raw = nullptr;
    ASSERT_EQ(NC_NOERR, NCpathcanonical("/d", &raw));
    ExpectCStringEq("/cygdrive/d", MakeScopedCString(raw));
  }
}

TEST(DpathmgrTest, PathAbsoluteResolvesRelativeAndHandlesErrors) {
  EXPECT_EQ(nullptr, MakeScopedCString(NCpathabsolute(nullptr)).get());

  std::array<char, 8192> cwd_buf{};
  ASSERT_NE(nullptr, ::getcwd(cwd_buf.data(), cwd_buf.size()));
  const std::string cwd(cwd_buf.data());
  ASSERT_FALSE(cwd.empty());

  if (cwd != "/") {
    ExpectCStringEq(cwd + "/sub/file.nc",
                    MakeScopedCString(NCpathabsolute("sub/file.nc")));
    ExpectCStringEq(cwd + "/sub/file.nc",
                    MakeScopedCString(NCpathabsolute("sub\\file.nc")));
    ExpectCStringEq(cwd + "/", MakeScopedCString(NCpathabsolute("")));
  }

  ExpectCStringEq("/abs/path/file.nc",
                  MakeScopedCString(NCpathabsolute("/abs/path/file.nc")));
  ExpectCStringEq("/C/abs/file.nc",
                  MakeScopedCString(NCpathabsolute("C:\\abs\\file.nc")));
  ExpectCStringEq("/d/abs.nc",
                  MakeScopedCString(NCpathabsolute("/cygdrive/d/abs.nc")));

  // Converting a Unix absolute path under NCPD_WIN without a mountpoint fails.
  ScopedPathPlatformReset platform_guard;
  NCpathsetplatform(NCPD_WIN);
  EXPECT_EQ(nullptr,
            MakeScopedCString(NCpathabsolute("/abs/nix/path.nc")).get());
}

TEST(DpathmgrTest, Path2Utf8StdBinaryAndPrintUtf8Hex) {
  {
    char* out = nullptr;
    ASSERT_EQ(NC_NOERR, NCpath2utf8("/a/b/c.nc", &out));
    ExpectCStringEq("/a/b/c.nc", MakeScopedCString(out));
  }
  {
    char* out = nullptr;
    ASSERT_EQ(NC_NOERR, NCpath2utf8(nullptr, &out));
    EXPECT_EQ(nullptr, out);
  }
  EXPECT_EQ(NC_NOERR, NCpath2utf8(nullptr, nullptr));
  EXPECT_EQ(NC_NOERR, NCstdbinary());

  struct HexCase {
    std::string_view input;
    std::string_view expected;
  };
  constexpr HexCase kHexCases[] = {
      {"", ""},
      {" /AZaz09~", " /AZaz09~"},
      {"\x01\t\n\x1f\x7f", "\\x01\\x09\\x0a\\x1f\x7f"},
      {"\x80\xc3\xa9\xff", "\\x80\\xc3\\xa9\\xff"},
  };
  for (const HexCase& tc : kHexCases) {
    SCOPED_TRACE(std::string(tc.expected));
    std::vector<char> out(4 * tc.input.size() + 1, '\0');
    const std::string input_str(tc.input);
    printutf8hex(input_str.c_str(), out.data());
    EXPECT_EQ(tc.expected, std::string_view(out.data()));
  }
}

TEST(DpathmgrTest, EnvInitDebugPrefixAndMsysNoPathconvInExecveChild) {
  RunInExecveChild([]() -> bool {
    if (::setenv("NCPATHDEBUG", "1", 1) != 0) {
      return false;
    }
    if (::setenv("MSYS2_PREFIX", "/msys64", 1) != 0) {
      return false;
    }
    if (::setenv("MSYS_NO_PATHCONV", "1", 1) != 0) {
      return false;
    }

    auto check_eq = [](char* raw, std::string_view expected) -> bool {
      ScopedCString guard = MakeScopedCString(raw);
      return guard.get() != nullptr &&
             std::string_view(guard.get()) == expected;
    };

    // First call triggers pathinit() with NCPATHDEBUG, MSYS2_PREFIX, and
    // MSYS_NO_PATHCONV active; exercises printPATH with drive == 0 and != 0.
    if (!check_eq(NCpathcvt("/tmp/debug_probe.nc"), "/tmp/debug_probe.nc")) {
      return false;
    }
    if (!check_eq(NCpathcvt("C:\\win\\probe.nc"), "/C/win/probe.nc")) {
      return false;
    }
    if (NCpathcvt(nullptr) != nullptr) {
      return false;
    }

    ScopedCString abs_rel = MakeScopedCString(NCpathabsolute("rel/probe.nc"));
    if (abs_rel.get() == nullptr) {
      return false;
    }
    if (NCpathabsolute(nullptr) != nullptr) {
      return false;
    }

    // With MSYS_NO_PATHCONV=1, /d/... is treated as NCPD_NIX even under MSYS.
    const absl::Cleanup reset_platform = []() {
      NCpathsetplatform(NCPD_UNKNOWN);
    };
    NCpathsetplatform(NCPD_MSYS);
    return NCgetinputpathkind("/d/foo/bar.nc") == NCPD_NIX;
  });
}

}  // namespace
}  // namespace netcdf
