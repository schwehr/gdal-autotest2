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
// Unit tests for libharp error and warning handling (libharp/harp-errno.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-errno.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp.h.in

#include <cstdarg>
#include <cstdio>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/libcoda/coda.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::HasSubstr;

struct ErrnoCase {
  int err;
  std::string_view expected_substring;
};

constexpr ErrnoCase kDefaultErrorCases[] = {
    {.err = HARP_SUCCESS, .expected_substring = "success (no error)"},
    {.err = HARP_ERROR_OUT_OF_MEMORY, .expected_substring = "out of memory"},
    {.err = HARP_ERROR_HDF4, .expected_substring = "HDF4 error"},
    {.err = HARP_ERROR_NO_HDF4_SUPPORT,
     .expected_substring = "HDF4 is not supported"},
    {.err = HARP_ERROR_HDF5, .expected_substring = "HDF5 error"},
    {.err = HARP_ERROR_NO_HDF5_SUPPORT,
     .expected_substring = "HDF5 is not supported"},
    {.err = HARP_ERROR_NETCDF, .expected_substring = "netCDF error"},
    {.err = HARP_ERROR_CODA, .expected_substring = "CODA error"},
    {.err = HARP_ERROR_FILE_NOT_FOUND, .expected_substring = "file not found"},
    {.err = HARP_ERROR_FILE_OPEN, .expected_substring = "error opening file"},
    {.err = HARP_ERROR_FILE_CLOSE, .expected_substring = "error closing file"},
    {.err = HARP_ERROR_FILE_READ, .expected_substring = "error reading file"},
    {.err = HARP_ERROR_FILE_WRITE, .expected_substring = "error writing file"},
    {.err = HARP_ERROR_INVALID_ARGUMENT,
     .expected_substring = "invalid argument"},
    {.err = HARP_ERROR_INVALID_INDEX, .expected_substring = "invalid index"},
    {.err = HARP_ERROR_INVALID_NAME, .expected_substring = "invalid name"},
    {.err = HARP_ERROR_INVALID_FORMAT, .expected_substring = "invalid format"},
    {.err = HARP_ERROR_INVALID_DATETIME,
     .expected_substring = "invalid date/time"},
    {.err = HARP_ERROR_INVALID_TYPE, .expected_substring = "invalid type"},
    {.err = HARP_ERROR_ARRAY_NUM_DIMS_MISMATCH,
     .expected_substring = "incorrect number of dimensions"},
    {.err = HARP_ERROR_ARRAY_OUT_OF_BOUNDS,
     .expected_substring = "array index out of bounds"},
    {.err = HARP_ERROR_VARIABLE_NOT_FOUND,
     .expected_substring = "variable not found"},
    {.err = HARP_ERROR_UNIT_CONVERSION,
     .expected_substring = "unit conversion error"},
    {.err = HARP_ERROR_OPERATION,
     .expected_substring = "product operations error"},
    {.err = HARP_ERROR_OPERATION_SYNTAX,
     .expected_substring = "syntax error in product operations string"},
    {.err = HARP_ERROR_IMPORT, .expected_substring = "import error"},
    {.err = HARP_ERROR_EXPORT, .expected_substring = "export error"},
    {.err = HARP_ERROR_INGESTION, .expected_substring = "ingestion error"},
    {.err = HARP_ERROR_INGESTION_OPTION_SYNTAX,
     .expected_substring = "syntax error in ingestion option"},
    {.err = HARP_ERROR_INVALID_INGESTION_OPTION,
     .expected_substring = "invalid ingestion option"},
    {.err = HARP_ERROR_INVALID_INGESTION_OPTION_VALUE,
     .expected_substring = "invalid ingestion option value"},
    {.err = HARP_ERROR_UNSUPPORTED_PRODUCT,
     .expected_substring = "unsupported product"},
    {.err = HARP_ERROR_NO_DATA,
     .expected_substring = "no data left after operation"},
};

std::string* g_captured_warning = nullptr;

int CaptureWarningHandler(const char* message, va_list ap) {
  char buf[512];
  int n = std::vsnprintf(buf, sizeof(buf), message, ap);
  if (g_captured_warning != nullptr && n >= 0) {
    *g_captured_warning = buf;
  }
  return n;
}

TEST(HarpErrnoTest, DefaultMessagesForAllErrorCodes) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  for (const auto& tc : kDefaultErrorCases) {
    harp_set_error(HARP_SUCCESS, nullptr);
    EXPECT_THAT(harp_errno_to_string(tc.err), HasSubstr(tc.expected_substring));
  }

  // Unknown error code returns empty string when not matching harp_errno,
  // and returns custom buffer when matching harp_errno.
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_STREQ("", harp_errno_to_string(-9999));
  harp_set_error(-9999, nullptr);
  EXPECT_STREQ("", harp_errno_to_string(-9999));
  harp_set_error(-9999, "custom unknown %d", 7);
  EXPECT_STREQ("custom unknown 7", harp_errno_to_string(-9999));
}

TEST(HarpErrnoTest, CustomMessageExtendAndTruncation) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_set_error(HARP_ERROR_INVALID_ARGUMENT, "bad arg %s", "foo");
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_STREQ("bad arg foo",
               harp_errno_to_string(HARP_ERROR_INVALID_ARGUMENT));
  // Non-matching error code still gets default string.
  EXPECT_STREQ("invalid name", harp_errno_to_string(HARP_ERROR_INVALID_NAME));

  harp_add_error_message(nullptr);
  harp_add_error_message(" (code=%d)", 42);
  EXPECT_STREQ("bad arg foo (code=42)",
               harp_errno_to_string(HARP_ERROR_INVALID_ARGUMENT));

  // Exceed MAX_ERROR_INFO_LENGTH (4096) to cover the early-return guard.
  const std::string long_msg(4150, 'x');
  harp_set_error(HARP_ERROR_IMPORT, "%s", long_msg.c_str());
  const std::string recorded = harp_errno_to_string(HARP_ERROR_IMPORT);
  EXPECT_EQ(4095u, recorded.size());
  harp_add_error_message("overflow");
  // MAX_ERROR_INFO_LENGTH - 4095 == 1, so vsnprintf writes only NUL and leaves
  // the buffer capped at 4095 chars.
  EXPECT_EQ(4095u, std::string(harp_errno_to_string(HARP_ERROR_IMPORT)).size());
}

TEST(HarpErrnoTest, CodaAndHdf5ErrorMessagesAndCursorPath) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  coda_set_error(CODA_ERROR_INVALID_ARGUMENT, "bad coda value");
  harp_set_error(HARP_ERROR_CODA, nullptr);
  EXPECT_THAT(harp_errno_to_string(HARP_ERROR_CODA),
              HasSubstr("bad coda value"));

  harp_set_error(HARP_ERROR_HDF5, nullptr);
  EXPECT_EQ(HARP_ERROR_HDF5, harp_errno);
  EXPECT_NE(nullptr, harp_errno_to_string(HARP_ERROR_HDF5));

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  ASSERT_TRUE(prod.AddField(prod.root(), "alpha", prod.MakeInt32(1)));
  coda_cursor cursor;
  ASSERT_EQ(0, coda_cursor_set_product(&cursor, prod.product()));
  ASSERT_EQ(0, coda_cursor_goto(&cursor, "/alpha"));

  harp_set_error(HARP_ERROR_INGESTION, "bad field");
  harp_add_coda_cursor_path_to_error_message(&cursor);
  EXPECT_STREQ("bad field at '/alpha'",
               harp_errno_to_string(HARP_ERROR_INGESTION));
}

TEST(HarpErrnoTest, WarningHandlerGetSetAndReport) {
  int (*prev_handler)(const char*, va_list) = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_get_warning_handler(&prev_handler));
  auto restore_handler = absl::MakeCleanup([&] {
    harp_set_warning_handler(prev_handler);
    g_captured_warning = nullptr;
  });

  ASSERT_EQ(HARP_SUCCESS, harp_set_warning_handler(nullptr));
  EXPECT_EQ(0, harp_report_warning("ignored %d", 1));

  std::string captured;
  g_captured_warning = &captured;
  ASSERT_EQ(HARP_SUCCESS, harp_set_warning_handler(CaptureWarningHandler));
  int (*current_handler)(const char*, va_list) = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_get_warning_handler(&current_handler));
  EXPECT_EQ(CaptureWarningHandler, current_handler);

  EXPECT_GT(harp_report_warning("warning %s %d", "test", 9), 0);
  EXPECT_EQ("warning test 9", captured);
}

}  // namespace
}  // namespace stcorp_harp
