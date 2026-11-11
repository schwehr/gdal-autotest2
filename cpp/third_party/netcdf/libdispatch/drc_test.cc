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

// Unit tests for //third_party/netcdf/libdispatch/drc.c (.ncrc/.daprc/.dodsrc
// configuration loading, lookup, and programmatic RC API).
//
// Covers nc_rc_get, nc_rc_set, ncrc_initialize, NC_rcclear, NC_rclookup,
// NC_rclookupx, NC_rcfile_insert, NC_rcfile_length, NC_rcfile_ith, RC file
// search precedence, S3/URL scope matching, and malformed RC/AWS error paths.
// Uses isolated child processes for startup/env/chdir tests so parent state
// remains unpolluted under --gunit_shuffle --gunit_repeat.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/drc.c

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/netcdf_internal_prelude.h"
#include "third_party/netcdf/google/util/scoped_netcdf.h"
#include "third_party/netcdf/google/util/temp_file.h"
#include "third_party/netcdf/include/netcdf.h"

extern "C" {
#include "third_party/netcdf/include/ncglobal.h"
#include "third_party/netcdf/include/nclist.h"
#include "third_party/netcdf/include/ncrc.h"
#include "third_party/netcdf/include/ncuri.h"
}  // extern "C"

namespace netcdf {
namespace {

using ScopedNcuri = std::unique_ptr<NCURI, decltype(&ncurifree)>;

ScopedNcuri ParseUriOrNull(const char* url) {
  NCURI* raw = nullptr;
  if (ncuriparse(url, &raw) != NC_NOERR) {
    return ScopedNcuri(nullptr, &ncurifree);
  }
  return ScopedNcuri(raw, &ncurifree);
}

bool WriteTextFile(const std::filesystem::path& path, std::string_view text) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    return false;
  }
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return out.good();
}

void ConfigureIsolatedDirEnv(const TempDir& empty_dir) {
  unsetenv(NCRCENVIGNORE);
  unsetenv(NCRCENVRC);
  setenv(NCRCENVHOME, empty_dir.c_str(), 1);
  setenv("HOME", empty_dir.c_str(), 1);
  setenv("NC_TEST_AWS_DIR", empty_dir.c_str(), 1);
}

char* AllocCString(std::string_view s) {
  auto* copy = static_cast<char*>(std::malloc(s.size() + 1));
  if (copy != nullptr) {
    if (!s.empty()) {
      std::memcpy(copy, s.data(), s.size());
    }
    copy[s.size()] = '\0';
  }
  return copy;
}

bool Streq(const char* actual, const char* expected) {
  return actual != nullptr && std::strcmp(actual, expected) == 0;
}

TEST(DrcTest, HandlesNullAndInvalidArgumentsAndLocalRcClear) {
  NC_rcclear(nullptr);

  // Verify NC_rcclear reclaims all fields of a populated standalone NCRCinfo.
  {
    NCRCinfo local_info{};
    bool cleared = false;
    auto clear_guard = absl::MakeCleanup([&]() {
      if (!cleared) {
        NC_rcclear(&local_info);
      }
    });
    local_info.rcfile = AllocCString("isolated_rcfile");
    local_info.rchome = AllocCString("isolated_rchome");
    local_info.entries = nclistnew();
    local_info.s3profiles = nclistnew();
    ASSERT_NE(nullptr, local_info.rcfile);
    ASSERT_NE(nullptr, local_info.rchome);
    ASSERT_NE(nullptr, local_info.entries);
    ASSERT_NE(nullptr, local_info.s3profiles);

    auto* entry = static_cast<NCRCentry*>(std::calloc(1, sizeof(NCRCentry)));
    ASSERT_NE(nullptr, entry);
    entry->host = AllocCString("clear.example.test:443");
    entry->urlpath = AllocCString("/clear");
    entry->key = AllocCString("CLEAR_KEY");
    entry->value = AllocCString("clear_val");
    nclistpush(local_info.entries, entry);
    NC_rcclear(&local_info);
    cleared = true;
  }

  // Isolate API calls that lazily initialize netCDF global RC state so the
  // parent process never reads $HOME/.ncrc or $CWD/.ncrc.
  RunInExecveChild([]() -> bool {
    TempDir empty_dir("drc_invalid_args");
    const std::string saved_cwd = std::filesystem::current_path().string();
    auto restore_cwd = absl::MakeCleanup(
        [&]() { static_cast<void>(::chdir(saved_cwd.c_str())); });
    if (::chdir(empty_dir.c_str()) != 0) {
      return false;
    }
    ConfigureIsolatedDirEnv(empty_dir);
    auto finalize_guard =
        absl::MakeCleanup([]() { static_cast<void>(nc_finalize()); });

    if (nc_rc_set(nullptr, "val") != NC_EINVAL ||
        nc_rc_set("DRCTEST_BAD", nullptr) != NC_EINVAL ||
        NC_rcfile_insert(nullptr, nullptr, nullptr, "val") != NC_EINVAL ||
        NC_rcfile_insert("DRCTEST_BAD", nullptr, nullptr, nullptr) !=
            NC_EINVAL ||
        NC_rclookup(nullptr, nullptr, nullptr) != nullptr) {
      std::cerr << "Unexpected return for invalid RC API argument\n";
      return false;
    }
    const ScopedCString null_get = MakeScopedCString(nc_rc_get(nullptr));
    return null_get == nullptr;
  });
}

TEST(DrcTest, PublicRcGetTriggersLazyInitAndSupportsOverwrite) {
  RunInExecveChild([]() -> bool {
    TempDir empty_dir("drc_public_get_first");
    const std::string saved_cwd = std::filesystem::current_path().string();
    auto restore_cwd = absl::MakeCleanup(
        [&]() { static_cast<void>(::chdir(saved_cwd.c_str())); });
    if (::chdir(empty_dir.c_str()) != 0) {
      return false;
    }
    ConfigureIsolatedDirEnv(empty_dir);
    // Also exercise ncrc_setrchome() fallback when NCRCENV_HOME is unset.
    unsetenv(NCRCENVHOME);

    // First call exercises lazy !NC_initialized inside nc_rc_get().
    const ScopedCString missing = MakeScopedCString(nc_rc_get("DRCTEST_KEY1"));
    if (missing != nullptr) {
      std::cerr << "Expected missing key before set\n";
      return false;
    }
    if (nc_rc_set("DRCTEST_KEY1", "first_value") != NC_NOERR) {
      return false;
    }
    const ScopedCString first = MakeScopedCString(nc_rc_get("DRCTEST_KEY1"));
    if (!Streq(first.get(), "first_value")) {
      return false;
    }
    if (nc_rc_set("DRCTEST_KEY1", "second_value") != NC_NOERR) {
      return false;
    }
    const ScopedCString second = MakeScopedCString(nc_rc_get("DRCTEST_KEY1"));
    if (!Streq(second.get(), "second_value")) {
      return false;
    }
    // Second ncrc_initialize() hits already-initialized fast path.
    ncrc_initialize();
    return nc_finalize() == NC_NOERR;
  });
}

TEST(DrcTest, PublicRcSetTriggersLazyInitWhenCalledFirst) {
  RunInExecveChild([]() -> bool {
    TempDir empty_dir("drc_public_set_first");
    const std::string saved_cwd = std::filesystem::current_path().string();
    auto restore_cwd = absl::MakeCleanup(
        [&]() { static_cast<void>(::chdir(saved_cwd.c_str())); });
    if (::chdir(empty_dir.c_str()) != 0) {
      return false;
    }
    ConfigureIsolatedDirEnv(empty_dir);

    // First call exercises lazy !NC_initialized inside nc_rc_set().
    if (nc_rc_set("DRCTEST_LAZY", "lazy_val") != NC_NOERR) {
      return false;
    }
    const ScopedCString got = MakeScopedCString(nc_rc_get("DRCTEST_LAZY"));
    if (!got || std::strcmp(got.get(), "lazy_val") != 0) {
      return false;
    }
    return nc_finalize() == NC_NOERR;
  });
}

TEST(DrcTest, EnvIgnoreSuppressesLoadingAndGetSetLookup) {
  RunInExecveChild([]() -> bool {
    TempDir root("drc_env_ignore");
    const std::filesystem::path rc_path = root.child("ignored.ncrc");
    if (!WriteTextFile(rc_path, "IGNORED_KEY=should_not_load\n")) {
      return false;
    }
    const std::string saved_cwd = std::filesystem::current_path().string();
    auto restore_cwd = absl::MakeCleanup(
        [&]() { static_cast<void>(::chdir(saved_cwd.c_str())); });
    if (::chdir(root.c_str()) != 0) {
      return false;
    }

    setenv(NCRCENVIGNORE, "1", 1);
    setenv(NCRCENVRC, rc_path.c_str(), 1);
    setenv(NCRCENVHOME, root.c_str(), 1);
    setenv("HOME", root.c_str(), 1);
    setenv("NC_TEST_AWS_DIR", root.c_str(), 1);

    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    auto finalize_guard =
        absl::MakeCleanup([]() { static_cast<void>(nc_finalize()); });

    if (nc_rc_get("IGNORED_KEY") != nullptr) {
      std::cerr << "Expected nc_rc_get to return NULL under ignore\n";
      return false;
    }
    if (nc_rc_set("IGNORED_KEY", "456") != NC_NOERR) {
      std::cerr << "Expected nc_rc_set to return NC_NOERR under ignore\n";
      return false;
    }
    if (nc_rc_get("IGNORED_KEY") != nullptr) {
      return false;
    }
    if (NC_rclookup("IGNORED_KEY", nullptr, nullptr) != nullptr) {
      return false;
    }
    if (NC_rcfile_length(NC_getglobalstate()->rcinfo) != 0) {
      return false;
    }
    return true;
  });
}

TEST(DrcTest, NcrcEnvRcParsesUrlsCommentsCrlfAndMatchesScopes) {
  RunInExecveChild([]() -> bool {
    TempDir root("drc_env_rc");
    const std::string saved_cwd = std::filesystem::current_path().string();
    auto restore_cwd = absl::MakeCleanup(
        [&]() { static_cast<void>(::chdir(saved_cwd.c_str())); });
    if (::chdir(root.c_str()) != 0) {
      return false;
    }
    ConfigureIsolatedDirEnv(root);

    // Keep malformed [url] lines before any valid [url] line so rccompile's
    // loop-carried uri pointer remains NULL on ncuriparse failures.
    // Put URL-scoped SCOPED_KEY before global SCOPED_KEY because rccompile's
    // rclocate() treats an existing global entry as a wildcard match and would
    // overwrite the global entry (and leak host) instead of adding a scoped
    // one. Always end the file with '\n' so rcreadline stays within allocation
    // bounds.
    constexpr std::string_view kRcContents =
        "\n"
        "  # comment with spaces\tand\rcarriage return\n"
        "[unclosed_bracket=ignored\n"
        "[not_a_valid_url]BAD_ENTRY=1\n"
        "CRLF_KEY=crlf_val\r\n"
        "FLAG_NO_EQ\n"
        "DUP_KEY=first\n"
        "DUP_KEY=second\n"
        "[https://host1.drc.test:8443/sub/path]SCOPED_KEY=host_path_val\n"
        "SCOPED_KEY=global_val\n"
        "[http://host2.drc.test]HOST2_KEY=host2_val\n"
        "[file:///local/data]FILE_KEY=file_val\n"
        "[https://mybucket.s3.us-west-2.amazonaws.com/prefix]S3_KEY=s3_val\n";

    const std::filesystem::path rc_path = root.child("custom.ncrc");
    if (!WriteTextFile(rc_path, kRcContents)) {
      return false;
    }
    setenv(NCRCENVRC, rc_path.c_str(), 1);

    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    auto finalize_guard =
        absl::MakeCleanup([]() { static_cast<void>(nc_finalize()); });

    const ScopedCString crlf = MakeScopedCString(nc_rc_get("CRLF_KEY"));
    const ScopedCString flag_no_eq = MakeScopedCString(nc_rc_get("FLAG_NO_EQ"));
    const ScopedCString dup = MakeScopedCString(nc_rc_get("DUP_KEY"));
    const ScopedCString scoped_global =
        MakeScopedCString(nc_rc_get("SCOPED_KEY"));
    if (!crlf || std::strcmp(crlf.get(), "crlf_val") != 0 || !flag_no_eq ||
        std::strcmp(flag_no_eq.get(), "") != 0 || !dup ||
        std::strcmp(dup.get(), "second") != 0 || !scoped_global ||
        std::strcmp(scoped_global.get(), "global_val") != 0) {
      std::cerr << "Global rc key mismatch\n";
      return false;
    }

    // Exercise rcorder (host-specific entries moved ahead of global entries)
    // and every branch of rcequal() on host and urlpath.
    const char* exact =
        NC_rclookup("SCOPED_KEY", "host1.drc.test:8443", "/sub/path");
    const char* diff_host =
        NC_rclookup("SCOPED_KEY", "other.drc.test:8443", "/sub/path");
    const char* diff_path =
        NC_rclookup("SCOPED_KEY", "host1.drc.test:8443", "/other");
    const char* null_path =
        NC_rclookup("SCOPED_KEY", "host1.drc.test:8443", nullptr);
    if (exact == nullptr || std::strcmp(exact, "host_path_val") != 0 ||
        diff_host == nullptr || std::strcmp(diff_host, "global_val") != 0 ||
        diff_path == nullptr || std::strcmp(diff_path, "global_val") != 0 ||
        null_path == nullptr || std::strcmp(null_path, "global_val") != 0) {
      std::cerr << "SCOPED_KEY lookup mismatch\n";
      return false;
    }

    const char* host2_null =
        NC_rclookup("HOST2_KEY", "host2.drc.test", nullptr);
    const char* host2_any =
        NC_rclookup("HOST2_KEY", "host2.drc.test", "/any/path");
    const char* host2_miss = NC_rclookup("HOST2_KEY", nullptr, nullptr);
    if (host2_null == nullptr || std::strcmp(host2_null, "host2_val") != 0 ||
        host2_any == nullptr || std::strcmp(host2_any, "host2_val") != 0 ||
        host2_miss != nullptr) {
      std::cerr << "HOST2_KEY lookup mismatch\n";
      return false;
    }

    const char* file_any_host =
        NC_rclookup("FILE_KEY", "any.drc.test", "/local/data");
    const char* file_miss = NC_rclookup("FILE_KEY", nullptr, "/other");
    if (!Streq(file_any_host, "file_val") || file_miss != nullptr) {
      std::cerr << "FILE_KEY lookup mismatch\n";
      return false;
    }

    // NC_rclookupx with host+port+path, host-only (null path), and hostless
    // file:/// URI.
    const ScopedNcuri host1_uri =
        ParseUriOrNull("https://host1.drc.test:8443/sub/path");
    const ScopedNcuri host2_uri = ParseUriOrNull("http://host2.drc.test");
    const ScopedNcuri file_uri = ParseUriOrNull("file:///local/data");
    if (!host1_uri || !host2_uri || !file_uri) {
      return false;
    }
    const char* x_host1 = NC_rclookupx(host1_uri.get(), "SCOPED_KEY");
    const char* x_host2 = NC_rclookupx(host2_uri.get(), "HOST2_KEY");
    const char* x_file = NC_rclookupx(file_uri.get(), "FILE_KEY");
    if (!Streq(x_host1, "host_path_val") || !Streq(x_host2, "host2_val") ||
        !Streq(x_file, "file_val")) {
      std::cerr << "NC_rclookupx mismatch\n";
      return false;
    }

    // S3 URL inside [url] was canonicalized to
    // s3.<region>.amazonaws.com/<bucket>/<key>.
    const char* s3_val =
        NC_rclookup("S3_KEY", "s3.us-west-2.amazonaws.com", "/mybucket/prefix");
    if (s3_val == nullptr || std::strcmp(s3_val, "s3_val") != 0) {
      std::cerr << "S3_KEY lookup mismatch\n";
      return false;
    }

    // Programmatic NC_rcfile_insert insert + update + length/ith queries.
    if (NC_rcfile_insert("PROG_KEY", "prog.drc.test:9000", "/v1", "val1") !=
            NC_NOERR ||
        NC_rcfile_insert("PROG_KEY", "prog.drc.test:9000", "/v1", "val2") !=
            NC_NOERR) {
      return false;
    }
    const char* prog_val = NC_rclookup("PROG_KEY", "prog.drc.test:9000", "/v1");
    if (prog_val == nullptr || std::strcmp(prog_val, "val2") != 0) {
      return false;
    }

    NCRCinfo* info = NC_getglobalstate()->rcinfo;
    const size_t nentries = NC_rcfile_length(info);
    if (nentries == 0 || NC_rcfile_ith(info, 0) == nullptr ||
        NC_rcfile_ith(info, nentries) != nullptr ||
        NC_rcfile_ith(info, nentries + 5) != nullptr) {
      std::cerr << "NC_rcfile_length/ith mismatch\n";
      return false;
    }
    return true;
  });
}

TEST(DrcTest, HandlesEmptyRcFileMissingFileAndMalformedS3OrAwsConfig) {
  // Empty RC file covers rcorder len == 0 early return.
  RunInExecveChild([]() -> bool {
    TempDir root("drc_empty_rc");
    const std::filesystem::path rc_path = root.child("empty.ncrc");
    if (!WriteTextFile(rc_path, "")) {
      return false;
    }
    const std::string saved_cwd = std::filesystem::current_path().string();
    auto restore_cwd = absl::MakeCleanup(
        [&]() { static_cast<void>(::chdir(saved_cwd.c_str())); });
    if (::chdir(root.c_str()) != 0) {
      return false;
    }
    ConfigureIsolatedDirEnv(root);
    setenv(NCRCENVRC, rc_path.c_str(), 1);
    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    return nc_finalize() == NC_NOERR;
  });

  // Missing RC file + invalid AWS INI covers error warnings in NC_rcload &
  // ncrc_initialize without failing library initialization.
  RunInExecveChild([]() -> bool {
    TempDir root("drc_missing_rc_bad_aws");
    const std::filesystem::path missing_rc = root.child("does_not_exist.ncrc");
    const std::filesystem::path bad_aws = root.child(".aws/credentials");
    if (!WriteTextFile(bad_aws, "not_an_ini_section\n")) {
      return false;
    }
    const std::string saved_cwd = std::filesystem::current_path().string();
    auto restore_cwd = absl::MakeCleanup(
        [&]() { static_cast<void>(::chdir(saved_cwd.c_str())); });
    if (::chdir(root.c_str()) != 0) {
      return false;
    }
    ConfigureIsolatedDirEnv(root);
    setenv(NCRCENVRC, missing_rc.c_str(), 1);
    setenv("NC_TEST_AWS_DIR", root.c_str(), 1);

    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    return nc_finalize() == NC_NOERR;
  });

  // Malformed S3 host in [url] covers NC_s3urlrebuild error exit in rccompile.
  RunInExecveChild([]() -> bool {
    TempDir root("drc_bad_s3_rc");
    const std::filesystem::path bad_s3_rc = root.child("bad_s3.ncrc");
    if (!WriteTextFile(bad_s3_rc,
                       "[https://a.b.s3.us-east-1.amazonaws.com/p]BADS3=1\n")) {
      return false;
    }
    const std::string saved_cwd = std::filesystem::current_path().string();
    auto restore_cwd = absl::MakeCleanup(
        [&]() { static_cast<void>(::chdir(saved_cwd.c_str())); });
    if (::chdir(root.c_str()) != 0) {
      return false;
    }
    ConfigureIsolatedDirEnv(root);
    setenv(NCRCENVRC, bad_s3_rc.c_str(), 1);

    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    return nc_finalize() == NC_NOERR;
  });
}

TEST(DrcTest, SearchesHomeAndCwdFilesInOrderAndRespectsNcrcEnvHome) {
  // NCRCENV_HOME empty -> falls back to $HOME, then searches $CWD.
  RunInExecveChild([]() -> bool {
    TempDir root("drc_home_cwd");
    const std::filesystem::path home_dir = root.child("home");
    const std::filesystem::path cwd_dir = root.child("cwd");
    if (!WriteTextFile(home_dir / ".ncrc",
                       "K_HOME=from_ncrc\nK_ORDER=home_ncrc\n") ||
        !WriteTextFile(home_dir / ".daprc",
                       "K_DAPRC=from_daprc\nK_ORDER=home_daprc\n") ||
        !WriteTextFile(home_dir / ".dodsrc", "K_DODSRC=from_dodsrc\n") ||
        !WriteTextFile(cwd_dir / ".ncrc",
                       "K_CWD=from_cwd\nK_ORDER=cwd_ncrc\n")) {
      return false;
    }

    const std::string saved_cwd = std::filesystem::current_path().string();
    auto restore_cwd = absl::MakeCleanup(
        [&]() { static_cast<void>(::chdir(saved_cwd.c_str())); });
    if (::chdir(cwd_dir.c_str()) != 0) {
      return false;
    }
    unsetenv(NCRCENVIGNORE);
    unsetenv(NCRCENVRC);
    setenv(NCRCENVHOME, "", 1);
    setenv("HOME", home_dir.c_str(), 1);
    setenv("NC_TEST_AWS_DIR", home_dir.c_str(), 1);

    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    auto finalize_guard =
        absl::MakeCleanup([]() { static_cast<void>(nc_finalize()); });

    const ScopedCString k_home = MakeScopedCString(nc_rc_get("K_HOME"));
    const ScopedCString k_daprc = MakeScopedCString(nc_rc_get("K_DAPRC"));
    const ScopedCString k_dodsrc = MakeScopedCString(nc_rc_get("K_DODSRC"));
    const ScopedCString k_cwd = MakeScopedCString(nc_rc_get("K_CWD"));
    const ScopedCString k_order = MakeScopedCString(nc_rc_get("K_ORDER"));
    if (!k_home || std::strcmp(k_home.get(), "from_ncrc") != 0 || !k_daprc ||
        std::strcmp(k_daprc.get(), "from_daprc") != 0 || !k_dodsrc ||
        std::strcmp(k_dodsrc.get(), "from_dodsrc") != 0 || !k_cwd ||
        std::strcmp(k_cwd.get(), "from_cwd") != 0 || !k_order ||
        std::strcmp(k_order.get(), "cwd_ncrc") != 0) {
      std::cerr << "HOME/CWD precedence mismatch\n";
      return false;
    }
    return true;
  });

  // Non-empty NCRCENV_HOME overrides $HOME.
  RunInExecveChild([]() -> bool {
    TempDir root("drc_env_home");
    const std::filesystem::path real_home = root.child("real_home");
    const std::filesystem::path rc_home = root.child("rc_home");
    const std::filesystem::path empty_cwd = root.child("empty_cwd");
    std::error_code ec;
    std::filesystem::create_directories(empty_cwd, ec);
    if (!WriteTextFile(real_home / ".ncrc", "WHICH_HOME=real_home\n") ||
        !WriteTextFile(rc_home / ".ncrc", "WHICH_HOME=ncrc_env_home\n")) {
      return false;
    }

    const std::string saved_cwd = std::filesystem::current_path().string();
    auto restore_cwd = absl::MakeCleanup(
        [&]() { static_cast<void>(::chdir(saved_cwd.c_str())); });
    if (::chdir(empty_cwd.c_str()) != 0) {
      return false;
    }
    unsetenv(NCRCENVIGNORE);
    unsetenv(NCRCENVRC);
    setenv(NCRCENVHOME, rc_home.c_str(), 1);
    setenv("HOME", real_home.c_str(), 1);
    setenv("NC_TEST_AWS_DIR", real_home.c_str(), 1);

    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    auto finalize_guard =
        absl::MakeCleanup([]() { static_cast<void>(nc_finalize()); });

    const ScopedCString which = MakeScopedCString(nc_rc_get("WHICH_HOME"));
    if (!which || std::strcmp(which.get(), "ncrc_env_home") != 0) {
      std::cerr << "NCRCENV_HOME override failed\n";
      return false;
    }
    return true;
  });
}

}  // namespace
}  // namespace netcdf
