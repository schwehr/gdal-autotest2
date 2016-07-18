// Tests error handlers for specific environments.
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

#include "gunit.h"
#include "autotest2/cpp/util/error_handler.h"
#include "port/cpl_error.h"

namespace {
// Tests a log handler that uses the gUnit LOG calls.
TEST(ErrorHandlerTest, CPLGoogleLogErrorHandlerTest) {
  // These should emit log messages without crashing.
  CPLGoogleLogErrorHandler(CE_None, 0, "CE_None gives INFO");
  CPLGoogleLogErrorHandler(CE_Debug, 1, "CE_Debug gives INFO");
  CPLGoogleLogErrorHandler(CE_Warning, 2, "CE_Warning gives WARNING");
  CPLGoogleLogErrorHandler(CE_Failure, 3, "CE_Error gives ERROR");

  SUCCEED();
}

// Tests CHECK of the message pointer.
TEST(ErrorHandlerDeathTest, CPLGoogleLogErrorHandlerNullptrTest) {
  EXPECT_DEATH(
      CPLGoogleLogErrorHandler(static_cast<CPLErr>(-1), 999, nullptr), "");
}

// Tests
TEST(ErrorHandlerDeathTest, CPLGoogleLogErrorHandlerFatalTest) {
  const string fatal_msg("CE_Failure gives FATAL and die");
  EXPECT_DEATH(
      CPLGoogleLogErrorHandler(CE_Fatal, 4, fatal_msg.c_str()),
      fatal_msg);
}

// Tests
TEST(ErrorHandlerDeathTest, CPLGoogleLogErrorHandlerInvalidTest) {
  const string invalid_msg("Invalid should give FATAL and die");
  EXPECT_DEATH(
      CPLGoogleLogErrorHandler(
          static_cast<CPLErr>(-1), 5, invalid_msg.c_str()),
      invalid_msg);
}

}  // namespace
