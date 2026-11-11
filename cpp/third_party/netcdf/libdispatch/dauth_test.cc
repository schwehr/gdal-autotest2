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

// Unit tests for //third_party/netcdf/libdispatch/dauth.c (HTTP/curl
// authorization, proxy, SSL, and credentials setup).
//
// Covers NC_parseproxy, NC_combinehostport, NC_authsetup, NC_authfree, and
// NC_parsecredentials, including default flags, RC-backed overrides, URL
// embedded credentials, and error handling. Uses isolated child processes for
// RC-table mutations and upstream leak-on-error/proxy paths so tests remain
// leak-clean and order-independent under sanitizers and --gunit_repeat.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/libdispatch/dauth.c

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
#include "third_party/netcdf/include/ncauth.h"
#include "third_party/netcdf/include/ncglobal.h"
#include "third_party/netcdf/include/nclist.h"
#include "third_party/netcdf/include/ncrc.h"
#include "third_party/netcdf/include/ncs3sdk.h"
#include "third_party/netcdf/include/ncuri.h"

int NC_parseproxy(NCauth* auth, const char* surl);
}  // extern "C"

namespace netcdf {
namespace {

using ScopedNcuri = std::unique_ptr<NCURI, decltype(&ncurifree)>;
using ScopedNcauth = std::unique_ptr<NCauth, decltype(&NC_authfree)>;

ScopedNcuri ParseUriOrNull(const char* url) {
  NCURI* raw = nullptr;
  if (ncuriparse(url, &raw) != NC_NOERR) {
    return ScopedNcuri(nullptr, &ncurifree);
  }
  return ScopedNcuri(raw, &ncurifree);
}

void ConfigureEmptyHomeAndAwsEnv(const TempDir& empty_dir) {
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

TEST(DauthTest, CombineHostPortFormatsHostAndOptionalPort) {
  EXPECT_EQ(nullptr, NC_combinehostport(nullptr));

  const ScopedNcuri file_uri = ParseUriOrNull("file:///tmp_unused/data.nc");
  ASSERT_NE(nullptr, file_uri);
  EXPECT_EQ(nullptr, NC_combinehostport(file_uri.get()));

  const ScopedNcuri host_only = ParseUriOrNull("http://data.example.test/file");
  ASSERT_NE(nullptr, host_only);
  const ScopedCString hp_only =
      MakeScopedCString(NC_combinehostport(host_only.get()));
  ASSERT_NE(nullptr, hp_only);
  EXPECT_STREQ("data.example.test", hp_only.get());

  const ScopedNcuri host_port =
      ParseUriOrNull("https://data.example.test:8443/file");
  ASSERT_NE(nullptr, host_port);
  const ScopedCString hp_port =
      MakeScopedCString(NC_combinehostport(host_port.get()));
  ASSERT_NE(nullptr, hp_port);
  EXPECT_STREQ("data.example.test:8443", hp_port.get());

  // Empty non-null port is normalized to NULL.
  char host_buf[] = "emptyport.example.test";
  char empty_port[] = "";
  NCURI fake_uri{};
  fake_uri.host = host_buf;
  fake_uri.port = empty_port;
  const ScopedCString hp_empty_port =
      MakeScopedCString(NC_combinehostport(&fake_uri));
  ASSERT_NE(nullptr, hp_empty_port);
  EXPECT_STREQ("emptyport.example.test", hp_empty_port.get());
}

TEST(DauthTest, ParseCredentialsDecodesAndValidatesInput) {
  char* raw_user = nullptr;
  char* raw_pwd = nullptr;

  EXPECT_EQ(NC_EINVAL, NC_parsecredentials(nullptr, &raw_user, &raw_pwd));
  EXPECT_EQ(NC_EINVAL, NC_parsecredentials("", &raw_user, &raw_pwd));
  EXPECT_EQ(NC_EINVAL, NC_parsecredentials("onlyuser", &raw_user, &raw_pwd));

  ASSERT_EQ(NC_NOERR, NC_parsecredentials("alice%20smith:p%40ss%3A1", &raw_user,
                                          &raw_pwd));
  ScopedCString user = MakeScopedCString(raw_user);
  ScopedCString pwd = MakeScopedCString(raw_pwd);
  ASSERT_NE(nullptr, user);
  ASSERT_NE(nullptr, pwd);
  EXPECT_STREQ("alice smith", user.get());
  EXPECT_STREQ("p@ss:1", pwd.get());

  raw_user = nullptr;
  raw_pwd = nullptr;
  ASSERT_EQ(NC_NOERR, NC_parsecredentials(":secret", &raw_user, &raw_pwd));
  user = MakeScopedCString(raw_user);
  pwd = MakeScopedCString(raw_pwd);
  EXPECT_STREQ("", user.get());
  EXPECT_STREQ("secret", pwd.get());

  raw_user = nullptr;
  raw_pwd = nullptr;
  ASSERT_EQ(NC_NOERR, NC_parsecredentials("only_u:", &raw_user, &raw_pwd));
  user = MakeScopedCString(raw_user);
  pwd = MakeScopedCString(raw_pwd);
  EXPECT_STREQ("only_u", user.get());
  EXPECT_STREQ("", pwd.get());

  // Passing NULL output pointers succeeds without allocating caller-owned
  // copies.
  EXPECT_EQ(NC_NOERR, NC_parsecredentials("u:p", nullptr, nullptr));
}

TEST(DauthTest, ParseProxyHandlesNullEmptyInvalidAndValidUrls) {
  NCauth auth{};
  EXPECT_EQ(NC_NOERR, NC_parseproxy(&auth, nullptr));
  EXPECT_EQ(NC_NOERR, NC_parseproxy(&auth, ""));
  EXPECT_EQ(NC_EURL, NC_parseproxy(&auth, "not_a_valid_url"));
  EXPECT_EQ(nullptr, auth.proxy.host);

  // Upstream NC_parseproxy allocates an internal NCURI that it does not free on
  // success; run valid proxy parsing in a forked child for LSan cleanliness.
  EXPECT_TRUE(RunInForkedChild([]() -> bool {
    auto* with_port = static_cast<NCauth*>(std::calloc(1, sizeof(NCauth)));
    if (with_port == nullptr) {
      return false;
    }
    ScopedNcauth with_port_guard(with_port, &NC_authfree);
    if (NC_parseproxy(with_port,
                      "http://puser:ppass@proxy.example.test:8080/x") !=
        NC_NOERR) {
      return false;
    }
    if (with_port->proxy.host == nullptr ||
        std::strcmp(with_port->proxy.host, "proxy.example.test") != 0 ||
        with_port->proxy.port != 8080 || with_port->proxy.user == nullptr ||
        std::strcmp(with_port->proxy.user, "puser") != 0 ||
        with_port->proxy.pwd == nullptr ||
        std::strcmp(with_port->proxy.pwd, "ppass") != 0) {
      return false;
    }

    auto* default_port = static_cast<NCauth*>(std::calloc(1, sizeof(NCauth)));
    if (default_port == nullptr) {
      return false;
    }
    ScopedNcauth default_port_guard(default_port, &NC_authfree);
    if (NC_parseproxy(default_port, "http://proxy2.example.test") != NC_NOERR) {
      return false;
    }
    if (default_port->proxy.host == nullptr ||
        std::strcmp(default_port->proxy.host, "proxy2.example.test") != 0 ||
        default_port->proxy.port != 80 || default_port->proxy.user != nullptr ||
        default_port->proxy.pwd != nullptr) {
      return false;
    }
    return true;
  }));
}

TEST(DauthTest, AuthFreeHandlesNullAndDeletesCreatedCookieJar) {
  NC_authfree(nullptr);

  TempFile cookie_file("dauth_cookiejar.txt");
  {
    std::ofstream out(cookie_file.path());
    ASSERT_TRUE(out.is_open());
    out << "cookie-data\n";
  }
  ASSERT_TRUE(std::filesystem::exists(cookie_file.path()));

  auto* auth = static_cast<NCauth*>(std::calloc(1, sizeof(NCauth)));
  ASSERT_NE(nullptr, auth);
  auth->curlflags.cookiejar = AllocCString(cookie_file.string());
  ASSERT_NE(nullptr, auth->curlflags.cookiejar);
  auth->curlflags.cookiejarcreated = 1;

  NC_authfree(auth);
  EXPECT_FALSE(std::filesystem::exists(cookie_file.path()));
}

TEST(DauthTest, AuthSetupRejectsNullUriAndSetsDefaults) {
  NCauth* null_uri_auth = nullptr;
  EXPECT_EQ(NC_EDAP, NC_authsetup(&null_uri_auth, nullptr));
  EXPECT_EQ(nullptr, null_uri_auth);
  EXPECT_EQ(NC_EDAP, NC_authsetup(nullptr, nullptr));

  RunInExecveChild([]() -> bool {
    TempDir empty_dir("dauth_defaults_env");
    const std::string saved_cwd = std::filesystem::current_path().string();
    auto restore_cwd = absl::MakeCleanup(
        [&]() { static_cast<void>(::chdir(saved_cwd.c_str())); });
    if (::chdir(empty_dir.c_str()) != 0) {
      std::cerr << "chdir failed\n";
      return false;
    }
    ConfigureEmptyHomeAndAwsEnv(empty_dir);
    if (nc_initialize() != NC_NOERR) {
      std::cerr << "nc_initialize failed\n";
      return false;
    }
    auto finalize_guard =
        absl::MakeCleanup([]() { static_cast<void>(nc_finalize()); });

    const ScopedNcuri uri = ParseUriOrNull("https://defaults.dauth.test/data");
    if (!uri) {
      return false;
    }
    NCauth* raw_auth = nullptr;
    if (NC_authsetup(&raw_auth, uri.get()) != NC_NOERR || raw_auth == nullptr) {
      std::cerr << "NC_authsetup defaults failed\n";
      return false;
    }
    ScopedNcauth auth(raw_auth, &NC_authfree);

    if (auth->ssl.verifypeer != -1 || auth->ssl.verifyhost != -1 ||
        auth->curlflags.timeout != 1800 ||
        auth->curlflags.connecttimeout != 50 || auth->curlflags.encode != 1 ||
        auth->curlflags.verbose != 0 || auth->curlflags.useragent != nullptr ||
        auth->curlflags.cookiejar != nullptr ||
        auth->curlflags.netrc != nullptr || auth->creds.user != nullptr ||
        auth->creds.pwd != nullptr || auth->proxy.host != nullptr) {
      std::cerr << "Unexpected default NCauth field\n";
      return false;
    }
    if (auth->s3profile == nullptr || std::strcmp(auth->s3profile, "no") != 0) {
      std::cerr << "Expected default s3profile == no\n";
      return false;
    }
    return true;
  });
}

TEST(DauthTest, AuthSetupAppliesAllRcFieldsUrlCredsAndS3Fallback) {
  RunInExecveChild([]() -> bool {
    TempDir empty_dir("dauth_fields_env");
    const std::string saved_cwd = std::filesystem::current_path().string();
    auto restore_cwd = absl::MakeCleanup(
        [&]() { static_cast<void>(::chdir(saved_cwd.c_str())); });
    if (::chdir(empty_dir.c_str()) != 0) {
      return false;
    }
    ConfigureEmptyHomeAndAwsEnv(empty_dir);
    if (nc_initialize() != NC_NOERR) {
      return false;
    }
    auto finalize_guard =
        absl::MakeCleanup([]() { static_cast<void>(nc_finalize()); });

    // Host 1: populate every HTTP.* flag, all 4 cookie key aliases, and RC
    // user+password.
    constexpr char kHost1[] = "flags1.dauth.test:8443";
    constexpr char kPath1[] = "/path1";
    struct RcEntrySpec {
      const char* host;
      const char* path;
      const char* key;
      const char* value;
    };
    const RcEntrySpec kEntries[] = {
        {kHost1, kPath1, "HTTP.VERBOSE", "1"},
        {kHost1, kPath1, "HTTP.TIMEOUT", "120"},
        {kHost1, kPath1, "HTTP.CONNECTTIMEOUT", "15"},
        {kHost1, kPath1, "HTTP.USERAGENT", "1CustomAgent"},
        {kHost1, kPath1, "HTTP.COOKIEFILE", "cf1"},
        {kHost1, kPath1, "HTTP.COOKIE_FILE", "cf2"},
        {kHost1, kPath1, "HTTP.COOKIEJAR", "cj1"},
        {kHost1, kPath1, "HTTP.COOKIE_JAR", "cj2"},
        {kHost1, kPath1, "HTTP.PROXY.SERVER",
         "http://pxu:pxp@px1.dauth.test:3128"},
        {kHost1, kPath1, "HTTP.SSL.CERTIFICATE", "/cert.pem"},
        {kHost1, kPath1, "HTTP.SSL.KEY", "/key.pem"},
        {kHost1, kPath1, "HTTP.SSL.KEYPASSWORD", "keysecret"},
        {kHost1, kPath1, "HTTP.SSL.CAINFO", "/ca.pem"},
        {kHost1, kPath1, "HTTP.SSL.CAPATH", "/cadir"},
        {kHost1, kPath1, "HTTP.SSL.VERIFYPEER", "1"},
        {kHost1, kPath1, "HTTP.SSL.VERIFYHOST", "2"},
        {kHost1, kPath1, "HTTP.NETRC", "/netrc"},
        {kHost1, kPath1, "HTTP.CREDENTIALS.USER", "rcuser"},
        {kHost1, kPath1, "HTTP.CREDENTIALS.PASSWORD", "rcpass"},
        // Host 2: zero-valued flags, non-numeric useragent (triggers MEMCHECK),
        // HTTP.PROXY_SERVER, HTTP.SSL.VALIDATE=1, and USERPASSWORD credentials.
        {"flags2.dauth.test", "/path2", "HTTP.VERBOSE", "0"},
        {"flags2.dauth.test", "/path2", "HTTP.TIMEOUT", "0"},
        {"flags2.dauth.test", "/path2", "HTTP.CONNECTTIMEOUT", "0"},
        {"flags2.dauth.test", "/path2", "HTTP.USERAGENT", "NonNumericAgent"},
        {"flags2.dauth.test", "/path2", "HTTP.COOKIEFILE", "only_cf"},
        {"flags2.dauth.test", "/path2", "HTTP.PROXY_SERVER",
         "http://px2.dauth.test"},
        {"flags2.dauth.test", "/path2", "HTTP.SSL.VERIFYPEER", "0"},
        {"flags2.dauth.test", "/path2", "HTTP.SSL.VERIFYHOST", "0"},
        {"flags2.dauth.test", "/path2", "HTTP.SSL.VALIDATE", "1"},
        {"flags2.dauth.test", "/path2", "HTTP.CREDENTIALS.USERPASSWORD",
         "enc%20user:enc%3Apass"},
        // Host 3: VALIDATE=0, invalid proxy URL (ignored safely), URL-embedded
        // creds.
        {"flags3.dauth.test", "/path3", "HTTP.SSL.VALIDATE", "0"},
        {"flags3.dauth.test", "/path3", "HTTP.PROXY.SERVER",
         "invalid_proxy_url"},
        {"flags3.dauth.test", "/path3", "HTTP.COOKIE_FILE", "only_c_file"},
        // Host 4: malformed USERPASSWORD returns NC_EINVAL from NC_authsetup.
        {"flags4.dauth.test", "/path4", "HTTP.CREDENTIALS.USERPASSWORD",
         "missingcolon"},
        // Host 5: HTTP.COOKIEJAR alone.
        {"flags5.dauth.test", "/path5", "HTTP.COOKIEJAR", "only_cj"},
    };
    for (const auto& entry : kEntries) {
      if (NC_rcfile_insert(entry.key, entry.host, entry.path, entry.value) !=
          NC_NOERR) {
        std::cerr << "NC_rcfile_insert failed for " << entry.key << "\n";
        return false;
      }
    }

    // Verify Host 1.
    {
      const ScopedNcuri uri1 =
          ParseUriOrNull("https://flags1.dauth.test:8443/path1");
      NCauth* raw1 = nullptr;
      if (!uri1 || NC_authsetup(&raw1, uri1.get()) != NC_NOERR ||
          raw1 == nullptr) {
        std::cerr << "NC_authsetup host1 failed\n";
        return false;
      }
      ScopedNcauth auth1(raw1, &NC_authfree);
      if (auth1->curlflags.verbose != 1 || auth1->curlflags.timeout != 120 ||
          auth1->curlflags.connecttimeout != 15 ||
          !Streq(auth1->curlflags.useragent, "1CustomAgent") ||
          !Streq(auth1->curlflags.cookiejar, "cj2") ||
          !Streq(auth1->proxy.host, "px1.dauth.test") ||
          auth1->proxy.port != 3128 || !Streq(auth1->proxy.user, "pxu") ||
          !Streq(auth1->proxy.pwd, "pxp") ||
          !Streq(auth1->ssl.certificate, "/cert.pem") ||
          !Streq(auth1->ssl.key, "/key.pem") ||
          !Streq(auth1->ssl.keypasswd, "keysecret") ||
          !Streq(auth1->ssl.cainfo, "/ca.pem") ||
          !Streq(auth1->ssl.capath, "/cadir") || auth1->ssl.verifypeer != 1 ||
          auth1->ssl.verifyhost != 2 ||
          !Streq(auth1->curlflags.netrc, "/netrc") ||
          !Streq(auth1->creds.user, "rcuser") ||
          !Streq(auth1->creds.pwd, "rcpass")) {
        std::cerr << "Host 1 field mismatch\n";
        return false;
      }
    }

    // Verify Host 2.
    {
      const ScopedNcuri uri2 = ParseUriOrNull("http://flags2.dauth.test/path2");
      NCauth* raw2 = nullptr;
      if (!uri2 || NC_authsetup(&raw2, uri2.get()) != NC_NOERR ||
          raw2 == nullptr) {
        std::cerr << "NC_authsetup host2 failed\n";
        return false;
      }
      ScopedNcauth auth2(raw2, &NC_authfree);
      if (auth2->curlflags.verbose != 0 || auth2->curlflags.timeout != 1800 ||
          auth2->curlflags.connecttimeout != 50 ||
          auth2->curlflags.useragent != nullptr ||
          !Streq(auth2->curlflags.cookiejar, "only_cf") ||
          !Streq(auth2->proxy.host, "px2.dauth.test") ||
          auth2->proxy.port != 80 || auth2->ssl.verifypeer != 1 ||
          auth2->ssl.verifyhost != 2 || !Streq(auth2->creds.user, "enc user") ||
          !Streq(auth2->creds.pwd, "enc:pass")) {
        std::cerr << "Host 2 field mismatch\n";
        return false;
      }
    }

    // Verify Host 3 (URL-embedded credentials, VALIDATE=0, invalid proxy
    // ignored).
    {
      const ScopedNcuri uri3 = ParseUriOrNull(
          "https://url%20user:url%3Apass@flags3.dauth.test/path3");
      NCauth* raw3 = nullptr;
      if (!uri3 || NC_authsetup(&raw3, uri3.get()) != NC_NOERR ||
          raw3 == nullptr) {
        std::cerr << "NC_authsetup host3 failed\n";
        return false;
      }
      ScopedNcauth auth3(raw3, &NC_authfree);
      if (auth3->ssl.verifypeer != -1 || auth3->ssl.verifyhost != -1 ||
          auth3->proxy.host != nullptr ||
          !Streq(auth3->curlflags.cookiejar, "only_c_file") ||
          !Streq(auth3->creds.user, "url user") ||
          !Streq(auth3->creds.pwd, "url:pass")) {
        std::cerr << "Host 3 field mismatch\n";
        return false;
      }
    }

    // Verify Host 4 (malformed USERPASSWORD fails cleanly with NC_EINVAL).
    {
      const ScopedNcuri uri4 = ParseUriOrNull("http://flags4.dauth.test/path4");
      NCauth* raw4 = nullptr;
      if (!uri4 || NC_authsetup(&raw4, uri4.get()) != NC_EINVAL ||
          raw4 != nullptr) {
        std::cerr << "Expected NC_EINVAL on malformed USERPASSWORD\n";
        return false;
      }
    }

    // Verify Host 5 (HTTP.COOKIEJAR alone).
    {
      const ScopedNcuri uri5 = ParseUriOrNull("http://flags5.dauth.test/path5");
      NCauth* raw5 = nullptr;
      if (!uri5 || NC_authsetup(&raw5, uri5.get()) != NC_NOERR ||
          raw5 == nullptr) {
        std::cerr << "NC_authsetup host5 failed\n";
        return false;
      }
      ScopedNcauth auth5(raw5, &NC_authfree);
      if (!Streq(auth5->curlflags.cookiejar, "only_cj")) {
        std::cerr << "Host 5 cookiejar mismatch\n";
        return false;
      }
    }

    // Verify s3profile fallback when "no" profile is absent:
    // first with no profiles at all (s3profile == NULL), then with "default".
    NCglobalstate* gs = NC_getglobalstate();
    NC_s3freeprofilelist(gs->rcinfo->s3profiles);
    gs->rcinfo->s3profiles = nclistnew();
    {
      const ScopedNcuri uri_s3 =
          ParseUriOrNull("https://s3fallback.dauth.test/p");
      NCauth* raw_no_prof = nullptr;
      if (!uri_s3 || NC_authsetup(&raw_no_prof, uri_s3.get()) != NC_NOERR ||
          raw_no_prof == nullptr) {
        std::cerr << "NC_authsetup without s3 profiles failed\n";
        return false;
      }
      ScopedNcauth auth_no_prof(raw_no_prof, &NC_authfree);
      if (auth_no_prof->s3profile != nullptr) {
        std::cerr
            << "Expected NULL s3profile when neither no nor default exists\n";
        return false;
      }

      auto* def_prof = static_cast<struct AWSprofile*>(
          std::calloc(1, sizeof(struct AWSprofile)));
      def_prof->name = AllocCString("default");
      def_prof->entries = nclistnew();
      nclistpush(gs->rcinfo->s3profiles, def_prof);

      NCauth* raw_def_prof = nullptr;
      if (NC_authsetup(&raw_def_prof, uri_s3.get()) != NC_NOERR ||
          raw_def_prof == nullptr) {
        std::cerr << "NC_authsetup with default s3 profile failed\n";
        return false;
      }
      ScopedNcauth auth_def_prof(raw_def_prof, &NC_authfree);
      if (!Streq(auth_def_prof->s3profile, "default")) {
        std::cerr << "Expected s3profile == default\n";
        return false;
      }
    }

    return true;
  });
}

}  // namespace
}  // namespace netcdf
