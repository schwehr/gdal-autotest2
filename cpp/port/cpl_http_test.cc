// Test libcurl based HTTP client
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_http.cpp
//
// Copyright 2024 Google Inc. All Rights Reserved.
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

#include "third_party/gdal/port/cpl_http.h"

#include <features.h>

#include <cstring>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(CplHttpFetch, CanFetchCorrectly) {
  WithQuietHandler silence_warnings;

  CPLStringList oOptions;
  CPLHTTPResult* result = CPLHTTPFetch("http://example.com", oOptions);
  absl::Cleanup cleanup = [result] { CPLHTTPDestroyResult(result); };

  EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  // curl is not enabled in google3 gdal
  EXPECT_STREQ(
      "GDAL/OGR not compiled with libcurl support, remote requests not "
      "supported.",
      CPLGetLastErrorMsg());
}

TEST(CplHttpTest, UserAgentCleanupAndEnabled) {
  CPLHTTPSetDefaultUserAgent("TestUserAgent/1.0");
  CPLHTTPSetDefaultUserAgent(nullptr);
  EXPECT_FALSE(CPLHTTPEnabled());
  CPLHTTPCleanup();
}

TEST(CplHttpTest, OptionsFromEnv) {
  CPLSetConfigOption("GDAL_HTTP_TIMEOUT", "15");
  VSISetPathSpecificOption("/vsigzip//vsimem/test.gz", "GDAL_HTTP_COOKIE",
                           "c=1");
  VSISetPathSpecificOption("/vsimem/test.gz", "GDAL_HTTP_USERPWD", "user:pass");
  absl::Cleanup cleanup = [] {
    CPLSetConfigOption("GDAL_HTTP_TIMEOUT", nullptr);
    VSIClearPathSpecificOptions("/vsigzip//vsimem/test.gz");
    VSIClearPathSpecificOptions("/vsimem/test.gz");
  };

  char** opts_null = CPLHTTPGetOptionsFromEnv(nullptr);
  EXPECT_STREQ("15", CSLFetchNameValue(opts_null, "TIMEOUT"));
  CSLDestroy(opts_null);

  char** opts_gzip = CPLHTTPGetOptionsFromEnv("/vsigzip//vsimem/test.gz");
  EXPECT_STREQ("15", CSLFetchNameValue(opts_gzip, "TIMEOUT"));
  EXPECT_STREQ("c=1", CSLFetchNameValue(opts_gzip, "COOKIE"));
  CSLDestroy(opts_gzip);

  char** opts_plain = CPLHTTPGetOptionsFromEnv("/vsimem/test.gz");
  EXPECT_STREQ("user:pass", CSLFetchNameValue(opts_plain, "USERPWD"));
  CSLDestroy(opts_plain);
}

TEST(CplHttpTest, RetryContextAndParameters) {
  CPLStringList opts;
  opts.SetNameValue("MAX_RETRY", "2");
  opts.SetNameValue("RETRY_DELAY", "1.0");
  CPLHTTPRetryParameters params(opts);
  EXPECT_EQ(2, params.nMaxRetry);
  EXPECT_DOUBLE_EQ(1.0, params.dfInitialDelay);

  CPLHTTPRetryContext ctx(params);
  // Calling GetCurrentDelay before CanRetry logs a debug message and returns 0.
  EXPECT_DOUBLE_EQ(0.0, ctx.GetCurrentDelay());

  // No-arg CanRetry
  EXPECT_TRUE(ctx.CanRetry());
  EXPECT_DOUBLE_EQ(0.0, ctx.GetCurrentDelay());
  EXPECT_TRUE(ctx.CanRetry());
  EXPECT_FALSE(ctx.CanRetry());

  ctx.ResetCounter();
  EXPECT_TRUE(ctx.CanRetry(429, nullptr, nullptr));
  EXPECT_GT(ctx.GetCurrentDelay(), 0.0);
  EXPECT_TRUE(ctx.CanRetry(500, nullptr, nullptr));
  // Exceeded max retry
  EXPECT_FALSE(ctx.CanRetry(500, nullptr, nullptr));

  // Test various status codes and curl error strings with higher max_retry
  CPLStringList opts_many;
  opts_many.SetNameValue("MAX_RETRY", "20");
  opts_many.SetNameValue("RETRY_DELAY", "0.5");
  CPLHTTPRetryContext ctx2((CPLHTTPRetryParameters(opts_many)));
  EXPECT_TRUE(ctx2.CanRetry(502, "Bad Gateway", "err"));
  EXPECT_TRUE(ctx2.CanRetry(503, nullptr, nullptr));
  EXPECT_TRUE(ctx2.CanRetry(504, nullptr, nullptr));
  EXPECT_TRUE(ctx2.CanRetry(400, "RequestTimeout", nullptr));
  EXPECT_TRUE(ctx2.CanRetry(0, nullptr, "Connection timed out"));
  EXPECT_TRUE(ctx2.CanRetry(0, nullptr, "Operation timed out"));
  EXPECT_TRUE(ctx2.CanRetry(0, nullptr, "Connection reset by peer"));
  EXPECT_TRUE(ctx2.CanRetry(0, nullptr, "Connection was reset"));
  EXPECT_TRUE(ctx2.CanRetry(0, nullptr, "SSL connection timeout"));
  EXPECT_FALSE(ctx2.CanRetry(400, "OtherError", nullptr));

  CPLHTTPRetryContext ctx3((CPLHTTPRetryParameters(opts_many)));
  EXPECT_FALSE(ctx3.CanRetry(404, nullptr, nullptr));

  // Test RETRY_CODES=ALL and specific list
  CPLStringList opts_all;
  opts_all.SetNameValue("MAX_RETRY", "5");
  opts_all.SetNameValue("RETRY_CODES", "ALL");
  CPLHTTPRetryContext ctx_all((CPLHTTPRetryParameters(opts_all)));
  EXPECT_TRUE(ctx_all.CanRetry(404, nullptr, nullptr));

  CPLStringList opts_list;
  opts_list.SetNameValue("MAX_RETRY", "5");
  opts_list.SetNameValue("RETRY_CODES", "404,418");
  CPLHTTPRetryContext ctx_list((CPLHTTPRetryParameters(opts_list)));
  EXPECT_TRUE(ctx_list.CanRetry(418, nullptr, nullptr));
  EXPECT_FALSE(ctx_list.CanRetry(403, nullptr, nullptr));
}

CPLHTTPResult* DummyFetchCallback(const char* pszURL,
                                  CSLConstList /*papszOptions*/,
                                  GDALProgressFunc /*pfnProgress*/,
                                  void* /*pProgressArg*/,
                                  CPLHTTPFetchWriteFunc /*pfnWrite*/,
                                  void* /*pWriteArg*/, void* pUserData) {
  const char* match_prefix = static_cast<const char*>(pUserData);
  if (match_prefix && STARTS_WITH(pszURL, match_prefix)) {
    auto* res =
        static_cast<CPLHTTPResult*>(CPLCalloc(1, sizeof(CPLHTTPResult)));
    res->nStatus = 0;
    res->pabyData = reinterpret_cast<GByte*>(CPLStrdup("ok"));
    res->nDataLen = 2;
    return res;
  }
  return nullptr;
}

TEST(CplHttpTest, FetchCallbacks) {
  WithQuietHandler silence_warnings;

  // Unbalanced pop fails
  EXPECT_FALSE(CPLHTTPPopFetchCallback());

  // Push and pop multiple callbacks
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(
        CPLHTTPPushFetchCallback(DummyFetchCallback, const_cast<char*>("a")));
  }
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(CPLHTTPPopFetchCallback());
  }

  // Push a callback that handles "http://pushed"
  EXPECT_TRUE(CPLHTTPPushFetchCallback(DummyFetchCallback,
                                       const_cast<char*>("http://pushed")));
  // Also set global callback that handles "http://global"
  CPLHTTPSetFetchCallback(DummyFetchCallback,
                          const_cast<char*>("http://global"));
  absl::Cleanup cleanup = [] {
    CPLHTTPPopFetchCallback();
    CPLHTTPSetFetchCallback(nullptr, nullptr);
  };

  CPLHTTPResult* res1 = CPLHTTPFetch("http://pushed/test", nullptr);
  ASSERT_NE(nullptr, res1);
  EXPECT_EQ(0, res1->nStatus);
  CPLHTTPDestroyResult(res1);

  CPLStringList close_opts;
  close_opts.SetNameValue("CLOSE_PERSISTENT", "YES");
  EXPECT_EQ(nullptr, CPLHTTPFetch("http://pushed/test", close_opts.List()));

  // Falls through pushed callback to global callback
  CPLHTTPResult* res2 = CPLHTTPFetch("http://global/test", nullptr);
  ASSERT_NE(nullptr, res2);
  EXPECT_EQ(0, res2->nStatus);
  CPLHTTPDestroyResult(res2);

  EXPECT_EQ(nullptr, CPLHTTPFetch("http://global/test", close_opts.List()));

  // Falls through both callbacks to the no-curl error path (returns nullptr
  // when !HAVE_CURL).  curl is not enabled in google3 gdal, so no network
  // request is ever made; the .invalid TLD is reserved and unroutable anyway.
  EXPECT_EQ(nullptr, CPLHTTPFetch("http://unhandled.invalid/test", nullptr));
}

TEST(CplHttpTest, VsimemFetchAndMultiFetch) {
  WithQuietHandler silence_warnings;

  CPLSetConfigOption("CPL_CURL_ENABLE_VSIMEM", "TRUE");
  CPLSetConfigOption("CPL_CURL_VSIMEM_PRINT_HEADERS", "TRUE");
  const char* kVsimemBase = "/vsimem/cpl_http_test_file.txt";
  const char* kVsimemFull =
      "/vsimem/cpl_http_test_file.txt&CUSTOMREQUEST=POST&USERPWD=u:p&"
      "POSTFIELDS=body=1&HEADERS=X-One: 1\r\nX-Two: 2";
  // The /vsimem/ file below is created with bTakeOwnership=FALSE pointing at
  // this buffer, so it must outlive the VSIUnlink in the cleanup lambda.
  const std::string content =
      "Content-Type: application/json\r\nX-Server: test\r\n\r\n{\"ok\":true}";
  absl::Cleanup cleanup = [kVsimemFull] {
    CPLSetConfigOption("CPL_CURL_ENABLE_VSIMEM", nullptr);
    CPLSetConfigOption("CPL_CURL_VSIMEM_PRINT_HEADERS", nullptr);
    VSIUnlink(kVsimemFull);
  };

  CPLStringList opts;
  opts.SetNameValue("CUSTOMREQUEST", "POST");
  opts.SetNameValue("USERPWD", "u:p");
  opts.SetNameValue("POSTFIELDS", "body=1");
  opts.SetNameValue("HEADERS", "X-One: 1\r\nX-Two: 2");

  // Non-existent /vsimem/ file -> 404
  CPLHTTPResult* res_missing = CPLHTTPFetch(kVsimemBase, opts.List());
  ASSERT_NE(nullptr, res_missing);
  EXPECT_EQ(1, res_missing->nStatus);
  EXPECT_STREQ("HTTP error code : 404", res_missing->pszErrBuf);
  CPLHTTPDestroyResult(res_missing);

  // Existing /vsimem/ file with Content-Type and headers
  VSILFILE* fp = VSIFileFromMemBuffer(
      kVsimemFull, reinterpret_cast<GByte*>(const_cast<char*>(content.data())),
      content.size(), FALSE);
  ASSERT_NE(nullptr, fp);
  VSIFCloseL(fp);

  CPLHTTPResult* res_ok = CPLHTTPFetch(kVsimemBase, opts.List());
  ASSERT_NE(nullptr, res_ok);
  EXPECT_EQ(0, res_ok->nStatus);
  ASSERT_NE(nullptr, res_ok->pszContentType);
  EXPECT_NE(nullptr, strstr(res_ok->pszContentType, "application/json"));
  CPLHTTPDestroyResult(res_ok);

  // MultiFetch without curl returns nullptr.  curl is not enabled in google3
  // gdal, so no network request is made; the .invalid TLD is unroutable anyway.
  const char* urls[] = {"http://example.invalid/1", "http://example.invalid/2"};
  EXPECT_EQ(nullptr, CPLHTTPMultiFetch(urls, 2, 2, nullptr));

  // DestroyMultiResult
  CPLHTTPDestroyMultiResult(nullptr, 0);
  auto** multi =
      static_cast<CPLHTTPResult**>(CPLCalloc(1, sizeof(CPLHTTPResult*)));
  multi[0] = static_cast<CPLHTTPResult*>(CPLCalloc(1, sizeof(CPLHTTPResult)));
  CPLHTTPDestroyMultiResult(multi, 1);
}

CPLHTTPResult* MakeMimeResult(const char* content_type,
                              const std::string& body) {
  auto* res = static_cast<CPLHTTPResult*>(CPLCalloc(1, sizeof(CPLHTTPResult)));
  if (content_type) {
    res->pszContentType = CPLStrdup(content_type);
  }
  if (!body.empty()) {
    res->nDataLen = static_cast<int>(body.size());
    res->pabyData = static_cast<GByte*>(CPLMalloc(body.size() + 1));
    memcpy(res->pabyData, body.data(), body.size() + 1);
  }
  return res;
}

TEST(CplHttpTest, ParseMultipartMime) {
  WithQuietHandler silence_warnings;

  // 1. Null content type
  CPLHTTPResult* r1 = MakeMimeResult(nullptr, "");
  EXPECT_FALSE(CPLHTTPParseMultipartMime(r1));
  CPLHTTPDestroyResult(r1);

  // 2. Empty boundary value
  CPLHTTPResult* r2 = MakeMimeResult("multipart/mixed; boundary=", "data");
  EXPECT_FALSE(CPLHTTPParseMultipartMime(r2));
  CPLHTTPDestroyResult(r2);

  // 3. Boundary string not found in the data ("No parts found.")
  CPLHTTPResult* r3 =
      MakeMimeResult("multipart/mixed; boundary=mybound", "no boundary here");
  EXPECT_FALSE(CPLHTTPParseMultipartMime(r3));
  CPLHTTPDestroyResult(r3);

  // 4. Trailing garbage after the initial boundary: the parser skips to the
  // end of the line, which is also the end of the data, so no bytes remain
  // for the first part (nBytesAvail == 0).
  CPLHTTPResult* r4 =
      MakeMimeResult("multipart/mixed; boundary=mybound", "--myboundBAD");
  EXPECT_FALSE(CPLHTTPParseMultipartMime(r4));
  CPLHTTPDestroyResult(r4);

  // 5. Header line without newline
  CPLHTTPResult* r5 = MakeMimeResult("multipart/mixed; boundary=mybound",
                                     "--mybound\r\nContent-Type: text/plain");
  EXPECT_FALSE(CPLHTTPParseMultipartMime(r5));
  CPLHTTPDestroyResult(r5);

  // 6. Part without closing boundary
  CPLHTTPResult* r6 = MakeMimeResult(
      "multipart/mixed; boundary=mybound",
      "--mybound\r\nContent-Type: text/plain\r\n\r\nBodyWithoutClosing");
  EXPECT_FALSE(CPLHTTPParseMultipartMime(r6));
  CPLHTTPDestroyResult(r6);

  // 7. Closing boundary not followed by -- or newline
  CPLHTTPResult* r7 = MakeMimeResult("multipart/mixed; boundary=mybound",
                                     "--mybound\r\n\r\nBody\r\n--myboundBAD");
  EXPECT_FALSE(CPLHTTPParseMultipartMime(r7));
  CPLHTTPDestroyResult(r7);

  // 8. Valid multipart with 2 parts (testing both \r\n and \n line endings and
  // non-Content header)
  const std::string valid_mime =
      "--mybound\r\n"
      "Content-Type: text/plain\r\n"
      "X-Ignore: stop_headers\r\n"
      "\r\n"
      "FirstPart\r\n"
      "--mybound\n"
      "Content-Disposition: inline\n"
      "\n"
      "SecondPart\n"
      "--mybound--\r\n";
  CPLHTTPResult* r8 =
      MakeMimeResult("multipart/mixed; boundary=mybound", valid_mime);
  r8->pszErrBuf = CPLStrdup("dummy_err");
  r8->papszHeaders = CSLAddString(nullptr, "Top-Header: 1");
  EXPECT_TRUE(CPLHTTPParseMultipartMime(r8));
  EXPECT_EQ(2, r8->nMimePartCount);
  // Second call returns TRUE immediately when already parsed
  EXPECT_TRUE(CPLHTTPParseMultipartMime(r8));
  CPLHTTPDestroyResult(r8);
}

}  // namespace
}  // namespace autotest2
