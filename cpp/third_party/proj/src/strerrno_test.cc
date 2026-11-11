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
// Unit tests for PROJ error code string formatting (proj/src/strerrno.cpp).
//
// See also:
//   https://github.com/OSGeo/PROJ/blob/9.9.0/src/strerrno.cpp

#include <limits>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/proj4/google/util/scoped_proj.h"
#include "third_party/proj4/proj/src/proj.h"
#include "third_party/proj4/proj/src/proj_internal.h"

namespace proj {
namespace {

TEST(StrerrnoTest, ZeroReturnsNullptr) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  EXPECT_EQ(nullptr, proj_context_errno_string(ctx.get(), 0));
  EXPECT_EQ(nullptr, proj_context_errno_string(nullptr, 0));
  EXPECT_EQ(nullptr, proj_errno_string(0));
}

TEST(StrerrnoTest, ContextErrnoStringMapsAllKnownCodes) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct ErrorCase {
    int code;
    std::string_view expected;
  };
  constexpr ErrorCase kCases[] = {
      {.code = PROJ_ERR_INVALID_OP_WRONG_SYNTAX,
       .expected = "Invalid PROJ string syntax"},
      {.code = PROJ_ERR_INVALID_OP_MISSING_ARG, .expected = "Missing argument"},
      {.code = PROJ_ERR_INVALID_OP_ILLEGAL_ARG_VALUE,
       .expected = "Invalid value for an argument"},
      {.code = PROJ_ERR_INVALID_OP_MUTUALLY_EXCLUSIVE_ARGS,
       .expected = "Mutually exclusive arguments"},
      {.code = PROJ_ERR_INVALID_OP_FILE_NOT_FOUND_OR_INVALID,
       .expected = "File not found or invalid"},
      {.code = PROJ_ERR_COORD_TRANSFM_INVALID_COORD,
       .expected = "Invalid coordinate"},
      {.code = PROJ_ERR_COORD_TRANSFM_OUTSIDE_PROJECTION_DOMAIN,
       .expected = "Point outside of projection domain"},
      {.code = PROJ_ERR_COORD_TRANSFM_NO_OPERATION,
       .expected = "No operation matching criteria found for coordinate"},
      {.code = PROJ_ERR_COORD_TRANSFM_OUTSIDE_GRID,
       .expected = "Coordinate to transform falls outside grid"},
      {.code = PROJ_ERR_COORD_TRANSFM_GRID_AT_NODATA,
       .expected =
           "Coordinate to transform falls into a grid cell that evaluates to "
           "nodata"},
      {.code = PROJ_ERR_COORD_TRANSFM_NO_CONVERGENCE,
       .expected =
           "Iterative method fails to converge on coordinate to transform"},
      {.code = PROJ_ERR_COORD_TRANSFM_MISSING_TIME,
       .expected = "Coordinate to transform lacks time"},
      {.code = PROJ_ERR_OTHER_API_MISUSE, .expected = "API misuse"},
      {.code = PROJ_ERR_OTHER_NO_INVERSE_OP,
       .expected = "No inverse operation"},
      {.code = PROJ_ERR_OTHER_NETWORK_ERROR,
       .expected = "Network error when accessing a remote resource"},
  };

  for (const auto& tc : kCases) {
    const char* msg = proj_context_errno_string(ctx.get(), tc.code);
    ASSERT_NE(nullptr, msg) << "code=" << tc.code;
    EXPECT_EQ(tc.expected, std::string_view(msg)) << "code=" << tc.code;
    EXPECT_EQ(ctx->lastFullErrorMessage.c_str(), msg);
  }
}

TEST(StrerrnoTest, CategoryFallbacksAndUnknownErrorFormatting) {
  ScopedContext ctx = MakeContext();
  ASSERT_NE(nullptr, ctx.get());

  struct FallbackCase {
    int code;
    std::string_view expected;
  };
  constexpr FallbackCase kCases[] = {
      // Base PROJ_ERR_INVALID_OP and unlisted codes with bit
      // PROJ_ERR_INVALID_OP.
      {.code = PROJ_ERR_INVALID_OP,
       .expected =
           "Unspecified error related to coordinate operation initialization"},
      {.code = PROJ_ERR_INVALID_OP + 99,
       .expected =
           "Unspecified error related to coordinate operation initialization"},
      // Base PROJ_ERR_COORD_TRANSFM and unlisted codes with that bit.
      {.code = PROJ_ERR_COORD_TRANSFM,
       .expected = "Unspecified error related to coordinate transformation"},
      {.code = PROJ_ERR_COORD_TRANSFM + 99,
       .expected = "Unspecified error related to coordinate transformation"},
      // When both category bits are set, PROJ_ERR_INVALID_OP takes precedence.
      {.code = PROJ_ERR_INVALID_OP | PROJ_ERR_COORD_TRANSFM,
       .expected =
           "Unspecified error related to coordinate operation initialization"},
      // PROJ_ERR_OTHER and unrecognized positive/negative codes fall through.
      {.code = PROJ_ERR_OTHER, .expected = "Unknown error (code 4096)"},
      {.code = 1, .expected = "Unknown error (code 1)"},
      {.code = -1, .expected = "Unknown error (code -1)"},
      {.code = -PROJ_ERR_INVALID_OP, .expected = "Unknown error (code -1024)"},
      {.code = std::numeric_limits<int>::min(),
       .expected = "Unknown error (code -2147483648)"},
  };

  for (const auto& tc : kCases) {
    const char* msg = proj_context_errno_string(ctx.get(), tc.code);
    ASSERT_NE(nullptr, msg) << "code=" << tc.code;
    EXPECT_EQ(tc.expected, std::string_view(msg)) << "code=" << tc.code;
  }
}

TEST(StrerrnoTest, NullContextAndProjErrnoStringUseDefaultContext) {
  PJ_CONTEXT* default_ctx = pj_get_default_ctx();
  ASSERT_NE(nullptr, default_ctx);
  const std::string saved_msg = default_ctx->lastFullErrorMessage;
  absl::Cleanup restore_default = [default_ctx, &saved_msg] {
    default_ctx->lastFullErrorMessage = saved_msg;
  };

  EXPECT_STREQ("Missing argument",
               proj_errno_string(PROJ_ERR_INVALID_OP_MISSING_ARG));
  EXPECT_STREQ("Missing argument", default_ctx->lastFullErrorMessage.c_str());

  EXPECT_STREQ("Unknown error (code -7)", proj_errno_string(-7));
  EXPECT_STREQ(
      "Invalid coordinate",
      proj_context_errno_string(nullptr, PROJ_ERR_COORD_TRANSFM_INVALID_COORD));
  EXPECT_STREQ("Invalid coordinate", default_ctx->lastFullErrorMessage.c_str());
}

TEST(StrerrnoTest, ContextBuffersAreIsolatedAndMatchRealErrorCodes) {
  ScopedContext ctx_a = MakeContext();
  ScopedContext ctx_b = MakeContext();
  ASSERT_NE(nullptr, ctx_a.get());
  ASSERT_NE(nullptr, ctx_b.get());
  ScopedLogSuppressor suppress_a(ctx_a.get());

  const std::string msg_a(
      proj_context_errno_string(ctx_a.get(), PROJ_ERR_INVALID_OP_MISSING_ARG));
  const std::string msg_b(proj_context_errno_string(ctx_b.get(), -42));
  EXPECT_EQ("Missing argument", msg_a);
  EXPECT_EQ("Unknown error (code -42)", msg_b);
  EXPECT_EQ("Missing argument", ctx_a->lastFullErrorMessage);
  EXPECT_EQ("Unknown error (code -42)", ctx_b->lastFullErrorMessage);

  ScopedPj bad(proj_create(ctx_a.get(), "+proj=not_a_valid_projection"));
  EXPECT_EQ(nullptr, bad.get());
  const int err = proj_context_errno(ctx_a.get());
  EXPECT_NE(0, err);
  const char* err_str = proj_context_errno_string(ctx_a.get(), err);
  ASSERT_NE(nullptr, err_str);
  EXPECT_STRNE("", err_str);
}

}  // namespace
}  // namespace proj
