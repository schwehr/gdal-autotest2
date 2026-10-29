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
// Tests for GDAL's VSI error reporting.
//
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsi_error.cpp

#include "third_party/gdal/port/cpl_vsi_error.h"

#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

TEST(CplVsiErrorTest, BasicErrorSetAndReset) {
  VSIErrorReset();
  EXPECT_EQ(VSIGetLastErrorNo(), VSIE_None);
  EXPECT_STREQ(VSIGetLastErrorMsg(), "");

  VSIError(VSIE_FileError, "File error %d: %s", 42, "bad file");
  EXPECT_EQ(VSIGetLastErrorNo(), VSIE_FileError);
  EXPECT_STREQ(VSIGetLastErrorMsg(), "File error 42: bad file");

  VSIErrorReset();
  EXPECT_EQ(VSIGetLastErrorNo(), VSIE_None);
  EXPECT_STREQ(VSIGetLastErrorMsg(), "");
}

TEST(CplVsiErrorTest, LargeErrorMessageReallocation) {
  VSIErrorReset();
  // Default buffer size is 500 bytes; format a ~3000-byte message to trigger
  // multiple reallocations in VSIErrorV.
  std::string long_payload(3000, 'X');
  VSIError(VSIE_HttpError, "HTTP failure: %s", long_payload.c_str());
  EXPECT_EQ(VSIGetLastErrorNo(), VSIE_HttpError);
  EXPECT_EQ(std::string(VSIGetLastErrorMsg()), "HTTP failure: " + long_payload);
  VSIErrorReset();
}

TEST(CplVsiErrorTest, ErrorNumToString) {
  EXPECT_STREQ(VSIErrorNumToString(VSIE_None), "None");
  EXPECT_STREQ(VSIErrorNumToString(VSIE_FileError), "FileError");
  EXPECT_STREQ(VSIErrorNumToString(VSIE_HttpError), "HttpError");
  EXPECT_STREQ(VSIErrorNumToString(VSIE_ObjectStorageGenericError),
               "ObjectStorageGenericError");
  EXPECT_STREQ(VSIErrorNumToString(VSIE_AccessDenied), "AccessDenied");
  EXPECT_STREQ(VSIErrorNumToString(VSIE_BucketNotFound), "BucketNotFound");
  EXPECT_STREQ(VSIErrorNumToString(VSIE_ObjectNotFound), "ObjectNotFound");
  EXPECT_STREQ(VSIErrorNumToString(VSIE_InvalidCredentials),
               "InvalidCredentials");
  EXPECT_STREQ(VSIErrorNumToString(VSIE_SignatureDoesNotMatch),
               "SignatureDoesNotMatch");
}

TEST(CplVsiErrorTest, VsiToCplErrorAllMappings) {
  WithQuietHandler quiet_handler;

  struct Case {
    VSIErrorNum vsi_err;
    CPLErrorNum expected_cpl_err;
    bool sets_cpl_error;
  };
  const Case kCases[] = {
      {VSIE_None, CPLE_AppDefined, false},
      {VSIE_ObjectStorageGenericError, CPLE_ObjectStorageGenericError, true},
      {VSIE_AccessDenied, CPLE_AccessDenied, true},
      {VSIE_BucketNotFound, CPLE_BucketNotFound, true},
      {VSIE_ObjectNotFound, CPLE_ObjectNotFound, true},
      {VSIE_InvalidCredentials, CPLE_InvalidCredentials, true},
      {VSIE_SignatureDoesNotMatch, CPLE_SignatureDoesNotMatch, true},
      {VSIE_HttpError, CPLE_HttpResponse, true},
      {VSIE_FileError, CPLE_AppDefined, true},
      {static_cast<VSIErrorNum>(999), CPLE_HttpResponse, true},
  };

  for (const auto& tc : kCases) {
    CPLErrorReset();
    VSIErrorReset();
    if (tc.vsi_err != VSIE_None) {
      VSIError(tc.vsi_err, "detail_%d", static_cast<int>(tc.vsi_err));
    }
    EXPECT_EQ(VSIToCPLError(CE_Failure, CPLE_AppDefined), tc.sets_cpl_error);
    if (tc.sets_cpl_error) {
      EXPECT_EQ(CPLGetLastErrorNo(), tc.expected_cpl_err);
    } else {
      EXPECT_EQ(CPLGetLastErrorNo(), CPLE_None);
    }
  }
}

TEST(CplVsiErrorTest, VsiToCplErrorWithMsgAllMappings) {
  WithQuietHandler quiet_handler;

  struct Case {
    VSIErrorNum vsi_err;
    CPLErrorNum expected_cpl_err;
  };
  const Case kCases[] = {
      {VSIE_None, CPLE_AppDefined},
      {VSIE_ObjectStorageGenericError, CPLE_ObjectStorageGenericError},
      {VSIE_AccessDenied, CPLE_AccessDenied},
      {VSIE_BucketNotFound, CPLE_BucketNotFound},
      {VSIE_ObjectNotFound, CPLE_ObjectNotFound},
      {VSIE_InvalidCredentials, CPLE_InvalidCredentials},
      {VSIE_SignatureDoesNotMatch, CPLE_SignatureDoesNotMatch},
      {VSIE_HttpError, CPLE_HttpResponse},
      {VSIE_FileError, CPLE_AppDefined},
      {static_cast<VSIErrorNum>(999), CPLE_HttpResponse},
  };

  // pszMsg is formatted with "%s", so a nullptr message is not a valid input.
  for (const auto& tc : kCases) {
    for (const char* custom_msg : {"", "ctx"}) {
      CPLErrorReset();
      VSIErrorReset();
      if (tc.vsi_err != VSIE_None) {
        VSIError(tc.vsi_err, "vsi_msg");
      }
      VSIToCPLErrorWithMsg(CE_Failure, CPLE_AppDefined, custom_msg);
      EXPECT_EQ(CPLGetLastErrorNo(), tc.expected_cpl_err)
          << "vsi_err=" << tc.vsi_err << " msg=\"" << custom_msg << "\"";
      const std::string cpl_msg = CPLGetLastErrorMsg();
      if (tc.vsi_err == VSIE_None) {
        EXPECT_EQ(cpl_msg, custom_msg);
      } else if (tc.vsi_err == 999) {
        EXPECT_EQ(cpl_msg, std::string(custom_msg) +
                               ": A filesystem error with code 999 occurred");
      } else {
        EXPECT_EQ(cpl_msg, std::string(custom_msg) + ": vsi_msg");
      }
    }
  }
  CPLErrorReset();
  VSIErrorReset();
}

}  // namespace
}  // namespace autotest2
