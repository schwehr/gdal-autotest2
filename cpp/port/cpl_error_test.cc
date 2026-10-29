// Test error tracking in CPLError
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_error.cpp
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

#include "third_party/gdal/port/cpl_error.h"

#include <features.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error_internal.h"
#include "third_party/gdal/port/cpl_multiproc.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(CplErrorSetState, DoesNotOverflowErrorMsgBuffer) {
  // The test assumes that nLastErrMsgMax is set to DEFAULT_LAST_ERR_MSG_SIZE.
  constexpr size_t kDefaultLastErrMsgSize = 500;
  constexpr size_t kDefaultLastErrMsgSizeWithoutNullTerminator =
      kDefaultLastErrMsgSize - 1;
  std::string expected(kDefaultLastErrMsgSizeWithoutNullTerminator, 'x');

  constexpr size_t kErrMsgSizeExtended = 527;
  std::string message(kErrMsgSizeExtended, 'x');

  // Call once to setup
  // TODO: unfortunately, CPLErrorReset() does not reset nLastErrMsgMax, so
  // there could be a chance that if some previous error in testing pushed
  // nLastErrMsgMax to a large number, this test will fail.
  CPLErrorReset();

  CPLErrorSetState(CE_Warning, 1, message.c_str());

  ASSERT_EQ(expected, std::string(CPLGetLastErrorMsg()));

  // Call again to cleanup
  CPLErrorReset();
}

TEST(ErrorHandlerSetting, CustomHandlerShouldGetCalledAfterPushErrorHandler) {
  static const char kMessage[] = "error message";
  static bool handler_called = false;

  const auto handler = [](CPLErr err, int err_no, const char* msg) {
    EXPECT_EQ(err, CE_Debug);
    EXPECT_EQ(err_no, CPLE_AppDefined);
    EXPECT_STREQ(msg, kMessage);

    handler_called = true;
  };

  CPLPushErrorHandler(handler);
  absl::Cleanup pop_handler = [] { CPLPopErrorHandler(); };

  CPLErrorReset();

  // Use CPLError() to trigger the error handler
  CPLError(CE_Debug, CPLE_AppDefined, kMessage);

  EXPECT_TRUE(handler_called);

  // Call again to cleanup
  CPLErrorReset();
}

TEST(ErrorHandlerSetting, CustomHandlerShouldGetCalledAfterSetErrorHandler) {
  static const char kMessage[] = "error message";
  static bool handler_called = false;

  const auto handler = [](CPLErr err, int err_no, const char* msg) {
    EXPECT_EQ(err, CE_Debug);
    EXPECT_EQ(err_no, CPLE_AppDefined);
    EXPECT_STREQ(msg, kMessage);

    handler_called = true;
  };

  CPLErrorHandler existing_handler = CPLSetErrorHandler(handler);
  absl::Cleanup restore_handler = [existing_handler] {
    CPLSetErrorHandler(existing_handler);
  };

  CPLErrorReset();

  // Use CPLError() to trigger the error handler
  CPLError(CE_Debug, CPLE_AppDefined, kMessage);

  EXPECT_TRUE(handler_called);

  // Call again to cleanup
  CPLErrorReset();
}

TEST(ErrorHandlerSetting, CplSetErrorHandlerReturnsExistingHandler) {
  const auto handler1 = [](CPLErr err, int err_no, const char* msg) {};
  const auto handler2 = [](CPLErr err, int err_no, const char* msg) {};

  CPLErrorHandler existing_handler = CPLSetErrorHandler(handler1);
  absl::Cleanup restore_handler = [existing_handler] {
    CPLSetErrorHandler(existing_handler);
  };

  EXPECT_EQ(CPLSetErrorHandler(handler2), handler1);
}

TEST(CplErrorTest, ErrorGettersAndUserDataAndCatchDebug) {
  CPLErrorReset();
  EXPECT_EQ(CPLE_None, CPLGetLastErrorNo());
  EXPECT_EQ(CE_None, CPLGetLastErrorType());
  EXPECT_EQ(0u, CPLGetErrorCounter());
  CPLErrorHandler orig = CPLSetErrorHandler(CPLDefaultErrorHandler);
  EXPECT_TRUE(CPLIsDefaultErrorHandlerAndCatchDebug());

  int global_data = 42;
  CPLSetErrorHandlerEx(CPLQuietErrorHandler, &global_data);
  void* fetched_data = nullptr;
  EXPECT_EQ(CPLQuietErrorHandler, CPLGetErrorHandler(&fetched_data));
  EXPECT_EQ(&global_data, fetched_data);
  EXPECT_EQ(&global_data, CPLGetErrorHandlerUserData());

  int local_data1 = 100;
  int local_data2 = 200;
  static void* seen_user_data = nullptr;
  const auto parent_handler = [](CPLErr /*err*/, int /*no*/,
                                 const char* /*msg*/) {
    seen_user_data = CPLGetErrorHandlerUserData();
  };
  const auto child_handler = [](CPLErr /*err*/, int /*no*/,
                                const char* /*msg*/) {};

  CPLPushErrorHandlerEx(parent_handler, &local_data1);
  // Setting global handler while stack is non-empty logs a debug message.
  CPLSetErrorHandlerEx(CPLQuietErrorHandler, &global_data);

  CPLPushErrorHandlerEx(child_handler, &local_data2);
  EXPECT_EQ(child_handler, CPLGetErrorHandler(&fetched_data));
  EXPECT_EQ(&local_data2, fetched_data);
  EXPECT_EQ(&local_data2, CPLGetErrorHandlerUserData());

  // Make top handler ignore debug messages so parent_handler catches CE_Debug.
  CPLSetCurrentErrorHandlerCatchDebug(FALSE);
  CPLError(CE_Debug, CPLE_None, "debug routed to parent");
  EXPECT_EQ(&local_data1, seen_user_data);

  // Now make parent_handler also ignore debug messages, and global
  // gbCatchDebug=false.
  CPLPopErrorHandler();
  CPLSetCurrentErrorHandlerCatchDebug(FALSE);
  CPLPopErrorHandler();
  CPLSetCurrentErrorHandlerCatchDebug(FALSE);

  CPLPushErrorHandler(child_handler);
  CPLSetCurrentErrorHandlerCatchDebug(FALSE);
  CPLError(CE_Debug, CPLE_None, "debug falls through to default handler");
  CPLPopErrorHandler();

  // Restore global catch debug and handler.
  CPLSetCurrentErrorHandlerCatchDebug(TRUE);
  CPLSetErrorHandler(orig);
  CPLErrorReset();
}

TEST(CplErrorTest, CallPreviousHandler) {
  static int parent_calls = 0;
  const auto parent_handler = [](CPLErr /*err*/, int /*no*/,
                                 const char* /*msg*/) { parent_calls++; };
  const auto forwarding_handler = [](CPLErr err, int no, const char* msg) {
    CPLCallPreviousHandler(err, no, msg);
  };

  // When only 1 handler is on stack, CPLCallPreviousHandler calls
  // CPLDefaultErrorHandler.
  CPLPushErrorHandler(forwarding_handler);
  CPLError(CE_Debug, CPLE_None, "forwarded to default");
  CPLPopErrorHandler();

  // When 2 handlers are on stack, CPLCallPreviousHandler calls the previous
  // one.
  CPLPushErrorHandler(parent_handler);
  CPLPushErrorHandler(forwarding_handler);
  CPLError(CE_Warning, CPLE_AppDefined, "forwarded to parent");
  EXPECT_EQ(1, parent_calls);
  CPLPopErrorHandler();
  CPLPopErrorHandler();

  // When stack is empty, CPLCallPreviousHandler calls CPLDefaultErrorHandler.
  CPLCallPreviousHandler(CE_Debug, CPLE_None, "empty stack call");
  CPLErrorReset();
}

TEST(CplErrorTest, TurnFailureIntoWarningAndAccumAndPasswordAndLogErrors) {
  CPLPushErrorHandler(CPLQuietErrorHandler);
  absl::Cleanup cleanup = [] {
    CPLPopErrorHandler();
    CPLSetConfigOption("CPL_ACCUM_ERROR_MSG", nullptr);
    CPLSetConfigOption("CPL_LOG_ERRORS", nullptr);
    CPLSetConfigOption("CPL_DEBUG", nullptr);
    CPLSetConfigOption("CPL_TIMESTAMP", nullptr);
    CPLErrorReset();
    CPLCleanupTLS();
  };

  {
    CPLTurnFailureIntoWarningBackuper backuper;
    CPLError(CE_Failure, CPLE_AppDefined, "failure converted");
    EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    EXPECT_GT(CPLGetErrorCounter(), 0u);
  }
  // Unbalanced FALSE triggers debug message
  CPLTurnFailureIntoWarning(FALSE);
  CPLTurnFailureIntoWarning(TRUE);

  // Test CPL_ACCUM_ERROR_MSG=ON and buffer expansion + password masking
  CPLSetConfigOption("CPL_ACCUM_ERROR_MSG", "ON");
  CPLSetConfigOption("CPL_LOG_ERRORS", "ON");
  CPLSetConfigOption("CPL_DEBUG", "ON");
  CPLSetConfigOption("CPL_TIMESTAMP", "ON");

  CPLErrorReset();
  CPLError(CE_Failure, CPLE_AppDefined, "first url?password=secret1 next");
  CPLError(CE_Failure, CPLE_AppDefined, "second url?password=secret2");
  std::string accum_msg = CPLGetLastErrorMsg();
  EXPECT_NE(std::string::npos, accum_msg.find("password=XXXXXXX next"));
  EXPECT_NE(std::string::npos, accum_msg.find("\nsecond url"));

  // Trigger dynamic buffer growth in CPLErrorV and CPLErrorSetState:
  // 1) Hit the (nPreviousSize + 1 + 1 >= psCtx->nLastErrMsgMax) branch in
  //    CPLErrorV while keeping nPreviousSize < 500 after increment (498 -> 499)
  //    to avoid UBSan array-bounds trap on psCtx->szLastErrMsg + nPreviousSize.
  CPLErrorReset();
  std::string str498(498, 'X');
  CPLError(CE_Failure, CPLE_AppDefined, "%s", str498.c_str());
  CPLError(CE_Failure, CPLE_AppDefined, "Y");
  EXPECT_EQ(500u, std::strlen(CPLGetLastErrorMsg()));

  // 2) Hit the while-loop buffer growth in CPLErrorV with nPreviousSize == 0.
  CPLSetConfigOption("CPL_ACCUM_ERROR_MSG", nullptr);
  CPLErrorReset();
  std::string long_str(1600, 'A');
  CPLError(CE_Failure, CPLE_AppDefined, "%s", long_str.c_str());
  EXPECT_EQ(1600u, std::strlen(CPLGetLastErrorMsg()));

  std::string even_longer(5000, 'B');
  CPLErrorSetState(CE_Failure, CPLE_AppDefined, even_longer.c_str());
  EXPECT_GT(std::strlen(CPLGetLastErrorMsg()), 4000u);
}

TEST(CplErrorTest, DebugAndBuiltInHandlersAndLogFile) {
  const std::string log_with_ext = ::testing::TempDir() + "/cpl_err_test.log";
  const std::string log_with_ext_0 =
      ::testing::TempDir() + "/cpl_err_test_0.log";
  const std::string log_no_ext = ::testing::TempDir() + "/cpl_err_noext";
  const std::string log_no_ext_0 =
      ::testing::TempDir() + "/cpl_err_noext_0.log";

  absl::Cleanup cleanup = [&] {
    CPLSetConfigOption("CPL_LOG", nullptr);
    CPLSetConfigOption("CPL_LOG_APPEND", nullptr);
    CPLSetConfigOption("CPL_MAX_ERROR_REPORTS", nullptr);
    CPLCleanupErrorMutex();
    VSIUnlink(log_with_ext.c_str());
    VSIUnlink(log_with_ext_0.c_str());
    VSIUnlink(log_no_ext.c_str());
    VSIUnlink(log_no_ext_0.c_str());
    CPLErrorReset();
  };

  // 1. Test CPLDefaultErrorHandler writing to CPL_LOG with
  // CPL_MAX_ERROR_REPORTS
  CPLCleanupErrorMutex();
  CPLSetConfigOption("CPL_LOG", log_with_ext.c_str());
  CPLSetConfigOption("CPL_LOG_APPEND", "ON");
  CPLSetConfigOption("CPL_MAX_ERROR_REPORTS", "2");

  CPLDefaultErrorHandler(CE_Debug, CPLE_None, "debug line");
  CPLDefaultErrorHandler(CE_Warning, CPLE_AppDefined, "warn line");
  CPLDefaultErrorHandler(CE_Failure, CPLE_AppDefined, "error line");
  // Exceeds max_errors (2)
  CPLDefaultErrorHandler(CE_Failure, CPLE_AppDefined, "suppressed error line");

  CPLQuietErrorHandler(CE_Debug, CPLE_None, "quiet debug");
  CPLQuietErrorHandler(CE_Warning, CPLE_AppDefined, "quiet warning");
  CPLQuietWarningsErrorHandler(CE_Warning, CPLE_AppDefined, "ignored warning");
  CPLQuietWarningsErrorHandler(CE_Failure, CPLE_AppDefined, "non-warning");

  // Reset log file via CPLCleanupErrorMutex (closes fpLog and sets
  // bLogInit=false)
  CPLCleanupErrorMutex();

  // 2. Test CPLLoggingErrorHandler when log_with_ext already exists ->
  // creates _0.log
  CPLLoggingErrorHandler(CE_Debug, CPLE_None, "log debug");
  CPLLoggingErrorHandler(CE_Warning, CPLE_AppDefined, "log warning");
  CPLLoggingErrorHandler(CE_Failure, CPLE_AppDefined, "log failure");
  CPLCleanupErrorMutex();

  // 3. Test CPLLoggingErrorHandler with filename without extension that
  // already exists
  FILE* f = fopen(log_no_ext.c_str(), "wt");
  if (f) fclose(f);
  CPLSetConfigOption("CPL_LOG", log_no_ext.c_str());
  CPLLoggingErrorHandler(CE_Debug, CPLE_None, "noext debug");
  CPLCleanupErrorMutex();

  // CPLDebug / CPLDebugProgress / CPLDebugOnce are not exercised here: this
  // target (like port_lib) is built with -DWITHOUT_CPLDEBUG, which turns those
  // macros into no-ops.
}

TEST(CplErrorTest, StateBackuperAndAccumulatorAndOnceMacros) {
  CPLErrorHandlerPusher quiet(CPLQuietErrorHandler);
  CPLErrorReset();
  CPLError(CE_Warning, CPLE_AppDefined, "initial warning");
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());

  {
    CPLErrorStateBackuper backuper(CPLQuietErrorHandler);
    CPLError(CE_Failure, CPLE_FileIO, "inner failure");
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_EQ(CPLE_FileIO, CPLGetLastErrorNo());
  }
  EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
  EXPECT_STREQ("initial warning", CPLGetLastErrorMsg());

  CPLErrorAccumulator acc;
  {
    auto ctx = acc.InstallForCurrentScope();
    CPLError(CE_Debug, CPLE_None, "debug ignored by accumulator");
    CPLError(CE_Warning, CPLE_AppDefined, "accumulated warning");
    CPLError(CE_Failure, CPLE_FileIO, "accumulated failure");
  }
  ASSERT_EQ(2u, acc.GetErrors().size());
  EXPECT_EQ(CE_Warning, acc.GetErrors()[0].type);
  EXPECT_EQ(CE_Failure, acc.GetErrors()[1].type);

  CPLErrorReset();
  acc.ReplayErrors();
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(CPLE_FileIO, CPLGetLastErrorNo());

  CPLErrorOnce(CE_Warning, CPLE_AppDefined, "once without period");
  CPLErrorOnce(CE_Warning, CPLE_AppDefined, "once with period.");
  // CPLDebugOnce is a no-op under -DWITHOUT_CPLDEBUG, so it is not tested.

  CPLErrorReset();
}

TEST(CplErrorDeathTest, FatalAndEmergencyAndAssert) {
  EXPECT_DEATH(_CPLAssert("1 == 2", "test.cc", 123), "");
  EXPECT_DEATH(CPLEmergencyError("fatal emergency"), "fatal emergency");
}

}  // namespace
}  // namespace autotest2
