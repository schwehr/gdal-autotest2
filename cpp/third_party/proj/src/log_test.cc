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
// Unit tests for PROJ logging level filtering, callback binding, and formatted
// message dispatch (proj/src/log.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/log.cpp

#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

using ::testing::HasSubstr;

struct RawLogState {
  int invocations = 0;
  int last_level = -1;
  std::string last_message;
};

void RawCaptureCallback(void* app_data, int level, const char* msg) {
  if (app_data == nullptr || msg == nullptr) {
    return;
  }
  auto* state = static_cast<RawLogState*>(app_data);
  ++state->invocations;
  state->last_level = level;
  state->last_message = msg;
}

void NoOpLogger(void*, int, const char*) {}

TEST(LogTest, PjStderrLoggerWritesFormattedLineToStderr) {
  ::testing::internal::CaptureStderr();
  pj_stderr_logger(nullptr, PJ_LOG_ERROR, "proj stderr probe");
  const std::string captured = ::testing::internal::GetCapturedStderr();
  EXPECT_EQ("proj stderr probe\n", captured);
}

TEST(LogTest, PjLogActiveHonorsPositiveAndNegativeDebugLevels) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Standard non-negative levels filter strictly by level <= debug_level.
  proj_log_level(ctx.get(), PJ_LOG_NONE);
  EXPECT_FALSE(pj_log_active(ctx.get(), PJ_LOG_ERROR));
  EXPECT_FALSE(pj_log_active(ctx.get(), PJ_LOG_DEBUG));

  proj_log_level(ctx.get(), PJ_LOG_ERROR);
  EXPECT_TRUE(pj_log_active(ctx.get(), PJ_LOG_ERROR));
  EXPECT_FALSE(pj_log_active(ctx.get(), PJ_LOG_DEBUG));

  proj_log_level(ctx.get(), PJ_LOG_DEBUG);
  EXPECT_TRUE(pj_log_active(ctx.get(), PJ_LOG_ERROR));
  EXPECT_TRUE(pj_log_active(ctx.get(), PJ_LOG_DEBUG));
  EXPECT_FALSE(pj_log_active(ctx.get(), PJ_LOG_TRACE));

  proj_log_level(ctx.get(), PJ_LOG_TRACE);
  EXPECT_TRUE(pj_log_active(ctx.get(), PJ_LOG_TRACE));

  // 2. Negative debug_level suppresses logging while last_errno == 0 and
  // unlocks logging up to |debug_level| once an error is recorded.
  ctx->debug_level = -PJ_LOG_DEBUG;
  proj_context_errno_set(ctx.get(), 0);
  EXPECT_FALSE(pj_log_active(ctx.get(), PJ_LOG_ERROR));
  EXPECT_FALSE(pj_log_active(ctx.get(), PJ_LOG_DEBUG));

  proj_context_errno_set(ctx.get(), PROJ_ERR_INVALID_OP);
  EXPECT_TRUE(pj_log_active(ctx.get(), PJ_LOG_ERROR));
  EXPECT_TRUE(pj_log_active(ctx.get(), PJ_LOG_DEBUG));
  EXPECT_FALSE(pj_log_active(ctx.get(), PJ_LOG_TRACE));
  proj_context_errno_set(ctx.get(), 0);
}

TEST(LogTest, ProjLogLevelSetsQueriesAndHandlesNullContext) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  // 1. Cycle through levels and verify PJ_LOG_TELL queries without modifying.
  proj_log_level(ctx.get(), PJ_LOG_NONE);
  EXPECT_EQ(PJ_LOG_NONE, proj_log_level(ctx.get(), PJ_LOG_TELL));
  EXPECT_EQ(PJ_LOG_NONE, proj_log_level(ctx.get(), PJ_LOG_ERROR));
  EXPECT_EQ(PJ_LOG_ERROR, proj_log_level(ctx.get(), PJ_LOG_DEBUG));
  EXPECT_EQ(PJ_LOG_DEBUG, proj_log_level(ctx.get(), PJ_LOG_TRACE));
  EXPECT_EQ(PJ_LOG_TRACE, proj_log_level(ctx.get(), PJ_LOG_TELL));

  // Negative debug_level reports positive magnitude via abs().
  ctx->debug_level = -PJ_LOG_DEBUG;
  EXPECT_EQ(PJ_LOG_DEBUG, proj_log_level(ctx.get(), PJ_LOG_TELL));

  // 2. Null context routes to pj_get_default_ctx(); save and restore state.
  const PJ_LOG_LEVEL prev_default = proj_log_level(nullptr, PJ_LOG_TELL);
  absl::Cleanup restore_default = [prev_default] {
    proj_log_level(nullptr, prev_default);
  };
  EXPECT_EQ(prev_default, proj_log_level(nullptr, PJ_LOG_DEBUG));
  EXPECT_EQ(PJ_LOG_DEBUG, proj_log_level(nullptr, PJ_LOG_TELL));
}

TEST(LogTest, ProjLogFuncIgnoresNullFunctionAndSupportsNullContext) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  proj_log_level(ctx.get(), PJ_LOG_TRACE);

  // Detach logger before local state structs leave scope.
  absl::Cleanup detach_local = [&ctx] {
    proj_log_func(ctx.get(), nullptr, &NoOpLogger);
  };

  RawLogState state_a;
  RawLogState state_b;
  proj_log_func(ctx.get(), &state_a, &RawCaptureCallback);
  pj_log(ctx.get(), PJ_LOG_ERROR, "first %s", "entry");
  EXPECT_EQ(1, state_a.invocations);
  EXPECT_EQ("first entry", state_a.last_message);

  // Passing nullptr for logf updates app_data while preserving existing logger.
  proj_log_func(ctx.get(), &state_b, nullptr);
  pj_log(ctx.get(), PJ_LOG_DEBUG, "second %d", 2);
  EXPECT_EQ(1, state_a.invocations);
  EXPECT_EQ(1, state_b.invocations);
  EXPECT_EQ(PJ_LOG_DEBUG, state_b.last_level);
  EXPECT_EQ("second 2", state_b.last_message);

  // Null context in proj_log_func/proj_log_level targets default ctx; preserve
  // and restore exact logger, app_data, and level state.
  PJ_CONTEXT* default_ctx = pj_get_default_ctx();
  ASSERT_NE(nullptr, default_ctx);
  auto prev_fn = default_ctx->logger;
  void* prev_data = default_ctx->logger_app_data;
  const PJ_LOG_LEVEL prev_level = proj_log_level(nullptr, PJ_LOG_TELL);
  absl::Cleanup restore_default = [&] {
    default_ctx->logger = prev_fn;
    default_ctx->logger_app_data = prev_data;
    proj_log_level(nullptr, prev_level);
  };

  RawLogState default_state;
  proj_log_level(nullptr, PJ_LOG_DEBUG);
  proj_log_func(nullptr, &default_state, &RawCaptureCallback);
  proj_context_log_debug(default_ctx, "default ctx %s", "msg");
  EXPECT_EQ(1, default_state.invocations);
  EXPECT_EQ("default ctx msg", default_state.last_message);
  proj_log_error(nullptr, "null pj %s", "err");
  EXPECT_EQ(2, default_state.invocations);
  EXPECT_EQ("null pj err", default_state.last_message);
}

TEST(LogTest, PjLogAndHelperFunctionsFormatShortNameAndFilter) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());
  ScopedLogCapture capture(ctx.get(), PJ_LOG_ERROR);

  // 1. Filter paths in pj_log() and pj_vlog() when level exceeds debug_level.
  pj_log(ctx.get(), PJ_LOG_DEBUG, "filtered in pj_log");
  proj_context_log_debug(ctx.get(), "filtered in pj_vlog");
  EXPECT_TRUE(capture.entries().empty());

  pj_log(ctx.get(), PJ_LOG_ERROR, "direct error %d", 7);
  ASSERT_EQ(1u, capture.entries().size());
  EXPECT_EQ(PJ_LOG_ERROR, capture.entries()[0].level);
  EXPECT_EQ("direct error 7", capture.entries()[0].message);

  // 2. Full PJ_LOG_TRACE coverage with P->short_name populated.
  proj_log_level(ctx.get(), PJ_LOG_TRACE);
  ScopedPj pj(proj_create(ctx.get(), "+proj=merc +ellps=WGS84"));
  ASSERT_NE(nullptr, pj.get());
  ASSERT_NE(nullptr, pj->short_name);

  capture.Clear();
  proj_log_error(pj.get(), "err=%d", 1);
  proj_log_debug(pj.get(), "dbg=%d", 2);
  proj_log_trace(pj.get(), "trc=%d", 3);
  proj_context_log_debug(ctx.get(), "ctx=%s", "ok");

  ASSERT_EQ(4u, capture.entries().size());
  EXPECT_EQ(PJ_LOG_ERROR, capture.entries()[0].level);
  EXPECT_EQ("merc: err=1", capture.entries()[0].message);
  EXPECT_EQ(PJ_LOG_DEBUG, capture.entries()[1].level);
  EXPECT_EQ("merc: dbg=2", capture.entries()[1].message);
  EXPECT_EQ(PJ_LOG_TRACE, capture.entries()[2].level);
  EXPECT_EQ("merc: trc=3", capture.entries()[2].message);
  EXPECT_EQ(PJ_LOG_DEBUG, capture.entries()[3].level);
  EXPECT_EQ("ctx=ok", capture.entries()[3].message);

  // 3. Non-null PJ with null short_name skips the "<short_name>: " prefix.
  ScopedPj pj_no_name(pj_new());
  ASSERT_NE(nullptr, pj_no_name.get());
  pj_no_name->ctx = ctx.get();
  EXPECT_EQ(nullptr, pj_no_name->short_name);
  capture.Clear();
  proj_log_error(pj_no_name.get(), "plain %s", "message");
  ASSERT_EQ(1u, capture.entries().size());
  EXPECT_EQ("plain message", capture.entries()[0].message);

  // 4. Oversized formatted message truncates safely at BUF_SIZE - 1 (99999).
  capture.Clear();
  const std::string oversized(100050, 'x');
  pj_log(ctx.get(), PJ_LOG_ERROR, "%s", oversized.c_str());
  ASSERT_EQ(1u, capture.entries().size());
  EXPECT_EQ(99999u, capture.entries()[0].message.size());
  EXPECT_THAT(capture.entries()[0].message, HasSubstr("xxxx"));
}

}  // namespace
}  // namespace proj
