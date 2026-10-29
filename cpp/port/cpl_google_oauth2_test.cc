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
//
// Tests for GDAL's Google OAuth2 support.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_google_oauth2.cpp

#include <cstring>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory_closer.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_http.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

struct FakeHttpState {
  std::vector<std::string> urls;
  std::vector<CPLHTTPResult*> allocated_results;

  ~FakeHttpState() {
    for (CPLHTTPResult* res : allocated_results) {
      CPLHTTPDestroyResult(res);
    }
  }
};

CPLHTTPResult* FakeHttpFetchCallback(const char* pszURL,
                                     CSLConstList /*papszOptions*/,
                                     GDALProgressFunc /*pfnProgress*/,
                                     void* /*pProgressArg*/,
                                     CPLHTTPFetchWriteFunc /*pfnWrite*/,
                                     void* /*pWriteArg*/, void* pUserData) {
  auto* state = static_cast<FakeHttpState*>(pUserData);
  state->urls.emplace_back(pszURL ? pszURL : "");
  // GOA2ProcessResponse in Google's build returns nullptr immediately without
  // destroying psResult, so track psResult here and destroy it in
  // ~FakeHttpState.
  auto* res = static_cast<CPLHTTPResult*>(CPLCalloc(1, sizeof(CPLHTTPResult)));
  state->allocated_results.push_back(res);
  return res;
}

TEST(CplGoogleOauth2Test, GetAuthorizationUrlAndTokens) {
  FakeHttpState http_state;
  CPLHTTPPushFetchCallback(FakeHttpFetchCallback, &http_state);

  {
    char* url = GOA2GetAuthorizationURL(
        "https://www.googleapis.com/auth/devstorage.read_write");
    CPLMemoryCloser<char> closer(url);
    ASSERT_NE(url, nullptr);
    EXPECT_NE(std::strstr(url, "https://accounts.google.com/o/oauth2/auth"),
              nullptr);
  }

  EXPECT_EQ(GOA2GetRefreshToken("auth_token", "scope"), nullptr);
  EXPECT_EQ(GOA2GetAccessToken("refresh_token", "scope"), nullptr);
  EXPECT_EQ(GOA2GetAccessTokenFromCloudEngineVM(nullptr), nullptr);

  CPLHTTPPopFetchCallback();
  EXPECT_EQ(http_state.urls.size(), 2u);
}

// port_lib is built without Crypto++/OpenSSL, so CPL_RSA_SHA256_Sign() always
// fails with CPLE_NotSupported and GOA2GetAccessTokenFromServiceAccount()
// returns before issuing any HTTP request.  The success path is not testable
// in this build.
TEST(CplGoogleOauth2Test, GetAccessTokenFromServiceAccount) {
  WithQuietHandler quiet;
  FakeHttpState http_state;
  CPLHTTPPushFetchCallback(FakeHttpFetchCallback, &http_state);

  CPLSetThreadLocalConfigOption("GOA2_NOW", "1700000000");
  CPLStringList claims;
  claims.AddString("sub=delegated@example.com");
  claims.AddString("entry_without_equals");

  CPLErrorReset();
  EXPECT_EQ(GOA2GetAccessTokenFromServiceAccount(
                "-----BEGIN PRIVATE KEY-----\\nMIIB...\\n"
                "-----END PRIVATE KEY-----",
                "svc@example.iam.gserviceaccount.com",
                "https://www.googleapis.com/auth/cloud-platform", claims.List(),
                nullptr),
            nullptr);
  EXPECT_EQ(CPLGetLastErrorNo(), CPLE_NotSupported);

  CPLSetThreadLocalConfigOption("GOA2_NOW", nullptr);
  CPLHTTPPopFetchCallback();
  EXPECT_TRUE(http_state.urls.empty());
}

TEST(CplGoogleOauth2Test, Goa2ManagerAllMethods) {
  WithQuietHandler quiet;
  FakeHttpState http_state;
  CPLHTTPPushFetchCallback(FakeHttpFetchCallback, &http_state);

  GOA2Manager mgr;
  EXPECT_EQ(mgr.GetAuthMethod(), GOA2Manager::AuthMethod::NONE);
  EXPECT_EQ(mgr.GetBearer(), nullptr);

  // 1) GCE auth.
  CPLStringList gce_opts;
  gce_opts.AddNameValue("TIMEOUT", "5");
  mgr.SetAuthFromGCE(gce_opts.List());
  EXPECT_EQ(mgr.GetAuthMethod(), GOA2Manager::AuthMethod::GCE);
  EXPECT_EQ(mgr.GetBearer(), nullptr);

  // 2) Refresh token auth: invalid (nullptr) and valid (with and without client
  // id/secret).
  EXPECT_FALSE(mgr.SetAuthFromRefreshToken(nullptr, nullptr, nullptr, nullptr));
  EXPECT_TRUE(mgr.SetAuthFromRefreshToken("my_refresh", "my_id", "my_secret",
                                          gce_opts.List()));
  EXPECT_EQ(mgr.GetAuthMethod(),
            GOA2Manager::AuthMethod::ACCESS_TOKEN_FROM_REFRESH);
  EXPECT_EQ(mgr.GetBearer(), nullptr);

  EXPECT_TRUE(
      mgr.SetAuthFromRefreshToken("my_refresh", nullptr, nullptr, nullptr));
  EXPECT_EQ(mgr.GetBearer(), nullptr);

  // 3) Service account auth: invalid parameters and valid parameters.
  EXPECT_FALSE(mgr.SetAuthFromServiceAccount(nullptr, "a@b.c", "scope", nullptr,
                                             nullptr));
  EXPECT_FALSE(
      mgr.SetAuthFromServiceAccount("", "a@b.c", "scope", nullptr, nullptr));
  EXPECT_FALSE(
      mgr.SetAuthFromServiceAccount("key", nullptr, "scope", nullptr, nullptr));
  EXPECT_FALSE(
      mgr.SetAuthFromServiceAccount("key", "", "scope", nullptr, nullptr));
  EXPECT_FALSE(
      mgr.SetAuthFromServiceAccount("key", "a@b.c", nullptr, nullptr, nullptr));
  EXPECT_FALSE(
      mgr.SetAuthFromServiceAccount("key", "a@b.c", "", nullptr, nullptr));

  CPLStringList claims;
  claims.AddNameValue("sub", "user@example.com");
  EXPECT_TRUE(mgr.SetAuthFromServiceAccount("valid_key", "svc@example.com",
                                            "https://scope", claims.List(),
                                            gce_opts.List()));
  EXPECT_EQ(mgr.GetAuthMethod(), GOA2Manager::AuthMethod::SERVICE_ACCOUNT);
  EXPECT_EQ(mgr.GetPrivateKey(), "valid_key");
  EXPECT_EQ(mgr.GetClientEmail(), "svc@example.com");
  EXPECT_FALSE(mgr.GetKey().empty());
  EXPECT_EQ(mgr.GetBearer(), nullptr);

  CPLHTTPPopFetchCallback();
}

}  // namespace
}  // namespace autotest2
