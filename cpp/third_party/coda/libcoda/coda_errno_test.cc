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
// Tests CODA error codes, thread-local error state, and error message
// formatting (libcoda/coda-errno.c).
//
// google3 build caveats:
//   - HDF4 is disabled in config.h (HAVE_HDF4 undefined), so the HAVE_HDF4
//     branch in coda_set_error() is compiled out.
//   - HDF5 is enabled in config.h (HAVE_HDF5=1), so coda_set_error() invokes
//     coda_hdf5_add_error_message() when err == CODA_ERROR_HDF5 and message is
//     NULL.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-errno.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-internal.h

#include <array>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

// coda.h hides coda_get_errno() when CODA_INTERNAL is defined (via
// coda_internal_prelude.h -> coda-internal.h). Declare the exported C accessor
// at global scope so we can verify both the public accessor and internal TLS.
extern "C" int* coda_get_errno(void);

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

void CallSetErrorMessageVargs(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  coda_set_error_message_vargs(fmt, ap);
  va_end(ap);
}

void CallAddErrorMessageVargs(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  coda_add_error_message_vargs(fmt, ap);
  va_end(ap);
}

TEST(CodaErrnoTest, GetErrnoReturnsThreadLocalAddressAndMatchesErrno) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  EXPECT_EQ(&coda_errno, coda_get_errno());
  EXPECT_EQ(CODA_SUCCESS, *coda_get_errno());

  coda_set_error(CODA_ERROR_INVALID_ARGUMENT, nullptr);
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, *coda_get_errno());
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  *coda_get_errno() = CODA_ERROR_FILE_READ;
  EXPECT_EQ(CODA_ERROR_FILE_READ, coda_errno);
}

TEST(CodaErrnoTest, ErrnoToStringDefaultMessagesForAllKnownCodes) {
  struct ErrorCase {
    int code;
    std::string_view expected;
  };
  constexpr ErrorCase kErrorCodeCases[] = {
      {.code = CODA_SUCCESS, .expected = "success (no error)"},
      {.code = CODA_ERROR_OUT_OF_MEMORY, .expected = "out of memory"},
      {.code = CODA_ERROR_HDF4, .expected = "HDF4 error"},
      {.code = CODA_ERROR_NO_HDF4_SUPPORT,
       .expected =
           "HDF4 is not supported (this version of CODA was not built with "
           "HDF4 support)"},
      {.code = CODA_ERROR_HDF5, .expected = "HDF5 error"},
      {.code = CODA_ERROR_NO_HDF5_SUPPORT,
       .expected =
           "HDF5 is not supported (this version of CODA was not built with "
           "HDF5 support)"},
      {.code = CODA_ERROR_XML,
       .expected = "unknown error while parsing XML data"},
      {.code = CODA_ERROR_FILE_NOT_FOUND, .expected = "file not found"},
      {.code = CODA_ERROR_FILE_OPEN, .expected = "could not open file"},
      {.code = CODA_ERROR_FILE_READ,
       .expected = "could not read data from file"},
      {.code = CODA_ERROR_FILE_WRITE,
       .expected = "could not write data to file"},
      {.code = CODA_ERROR_INVALID_ARGUMENT, .expected = "invalid argument"},
      {.code = CODA_ERROR_INVALID_INDEX, .expected = "invalid index argument"},
      {.code = CODA_ERROR_INVALID_NAME, .expected = "invalid name argument"},
      {.code = CODA_ERROR_INVALID_FORMAT,
       .expected = "invalid format in argument"},
      {.code = CODA_ERROR_INVALID_DATETIME,
       .expected = "invalid date/time argument"},
      {.code = CODA_ERROR_INVALID_TYPE, .expected = "invalid type"},
      {.code = CODA_ERROR_ARRAY_NUM_DIMS_MISMATCH,
       .expected = "incorrect number of dimensions argument"},
      {.code = CODA_ERROR_ARRAY_OUT_OF_BOUNDS,
       .expected = "array index out of bounds"},
      {.code = CODA_ERROR_NO_PARENT, .expected = "cursor has no parent"},
      {.code = CODA_ERROR_UNSUPPORTED_PRODUCT,
       .expected = "unsupported product file"},
      {.code = CODA_ERROR_PRODUCT, .expected = "product error detected"},
      {.code = CODA_ERROR_OUT_OF_BOUNDS_READ,
       .expected = "trying to read outside the element boundary"},
      {.code = CODA_ERROR_DATA_DEFINITION,
       .expected = "error in data definitions detected"},
      {.code = CODA_ERROR_EXPRESSION,
       .expected = "error detected while parsing/evaluating expression"},
  };

  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // Querying with coda_errno == CODA_SUCCESS checks the switch-case defaults.
  for (const ErrorCase& tc : kErrorCodeCases) {
    const char* msg = coda_errno_to_string(tc.code);
    ASSERT_NE(nullptr, msg);
    EXPECT_EQ(tc.expected, std::string_view(msg)) << "code=" << tc.code;
  }

  // Setting an error with a NULL message still yields the default string when
  // queried for the active error code.
  coda_set_error(CODA_ERROR_INVALID_ARGUMENT, nullptr);
  EXPECT_STREQ("invalid argument",
               coda_errno_to_string(CODA_ERROR_INVALID_ARGUMENT));
}

TEST(CodaErrnoTest, ErrnoToStringUnknownErrorCodes) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // Unknown code that does not match coda_errno returns an empty string.
  EXPECT_STREQ("", coda_errno_to_string(-999));
  EXPECT_STREQ("", coda_errno_to_string(999));

  // Reserved range -900..-999 matching coda_errno with an empty buffer hits the
  // default switch branch (err == coda_errno) and returns the empty buffer.
  coda_set_error(-950, nullptr);
  EXPECT_STREQ("", coda_errno_to_string(-950));
  EXPECT_STREQ("", coda_errno_to_string(-951));

  // Reserved range with a custom message returns that custom message when
  // queried with the matching code.
  coda_set_error(-950, "layer error %d", 7);
  EXPECT_STREQ("layer error 7", coda_errno_to_string(-950));
  EXPECT_STREQ("", coda_errno_to_string(-951));
  EXPECT_STREQ("invalid argument",
               coda_errno_to_string(CODA_ERROR_INVALID_ARGUMENT));
}

TEST(CodaErrnoTest, SetErrorCustomMessageAndMismatchBehavior) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  coda_set_error(CODA_ERROR_PRODUCT, "bad field %s (%d)", "foo", 42);
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
  EXPECT_STREQ("bad field foo (42)", coda_errno_to_string(CODA_ERROR_PRODUCT));

  // Querying a different error code returns its default message without
  // overwriting the stored custom message for coda_errno.
  EXPECT_STREQ("invalid argument",
               coda_errno_to_string(CODA_ERROR_INVALID_ARGUMENT));
  EXPECT_STREQ("bad field foo (42)", coda_errno_to_string(CODA_ERROR_PRODUCT));

  // coda_set_error_message_vargs formats into a local buffer first so
  // coda_errno_to_string(coda_errno) can safely be passed inside va_list.
  coda_set_error(CODA_ERROR_INVALID_INDEX, "%s: idx=%d",
                 coda_errno_to_string(CODA_ERROR_INVALID_INDEX), 5);
  EXPECT_STREQ("invalid index argument: idx=5",
               coda_errno_to_string(CODA_ERROR_INVALID_INDEX));

  // Clearing only the message buffer restores the default error string.
  coda_set_error_message(nullptr);
  EXPECT_STREQ("invalid index argument",
               coda_errno_to_string(CODA_ERROR_INVALID_INDEX));

  coda_set_error_message("override %s", "msg");
  EXPECT_STREQ("override msg", coda_errno_to_string(CODA_ERROR_INVALID_INDEX));
}

TEST(CodaErrnoTest, AddErrorMessageAppendsAndHandlesNullAndEmptyBuffer) {
  coda_set_error(CODA_ERROR_INVALID_ARGUMENT, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // Passing NULL is a no-op and leaves the message buffer empty.
  coda_add_error_message(nullptr);
  EXPECT_STREQ("invalid argument",
               coda_errno_to_string(CODA_ERROR_INVALID_ARGUMENT));

  // When coda_error_message_buffer is empty, coda_add_error_message_vargs
  // computes the default message length before formatting into the local
  // buffer.
  coda_add_error_message("detail %d", 1);
  EXPECT_STREQ("detail 1", coda_errno_to_string(CODA_ERROR_INVALID_ARGUMENT));

  // Subsequent appends concatenate onto the existing message buffer, and
  // coda_errno_to_string() can safely be referenced inside the format args.
  coda_add_error_message(": %s", "more");
  EXPECT_STREQ("detail 1: more",
               coda_errno_to_string(CODA_ERROR_INVALID_ARGUMENT));

  coda_add_error_message(" (%s)",
                         coda_errno_to_string(CODA_ERROR_INVALID_ARGUMENT));
  EXPECT_STREQ("detail 1: more (detail 1: more)",
               coda_errno_to_string(CODA_ERROR_INVALID_ARGUMENT));

  coda_add_error_message(nullptr);
  EXPECT_STREQ("detail 1: more (detail 1: more)",
               coda_errno_to_string(CODA_ERROR_INVALID_ARGUMENT));
}

TEST(CodaErrnoTest, SetAndAddErrorMessageVargsFormatArguments) {
  coda_set_error(CODA_ERROR_EXPRESSION, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  CallSetErrorMessageVargs("syntax error at col %d", 12);
  EXPECT_STREQ("syntax error at col 12",
               coda_errno_to_string(CODA_ERROR_EXPRESSION));

  CallAddErrorMessageVargs(" near '%s'", "foo");
  EXPECT_STREQ("syntax error at col 12 near 'foo'",
               coda_errno_to_string(CODA_ERROR_EXPRESSION));
}

TEST(CodaErrnoTest, MessageFormattingTruncatesAtMaxErrorInfoLength) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // MAX_ERROR_INFO_LENGTH is 4096 in coda-errno.c, so vsnprintf(..., 4096, ...)
  // caps coda_error_message_buffer at 4095 bytes plus NUL.
  const std::string long_msg(5000, 'a');
  coda_set_error(CODA_ERROR_PRODUCT, "%s", long_msg.c_str());
  const std::string_view set_actual(coda_errno_to_string(CODA_ERROR_PRODUCT));
  EXPECT_EQ(4095U, set_actual.size());
  EXPECT_EQ(std::string(4095, 'a'), set_actual);

  // Appending to a near-full buffer truncates the appended slice to the
  // remaining capacity (4096 - 4090 - 1 = 5 bytes).
  coda_set_error(CODA_ERROR_PRODUCT, "%s", std::string(4090, 'x').c_str());
  coda_add_error_message("%s", "0123456789");
  const std::string_view add_actual(coda_errno_to_string(CODA_ERROR_PRODUCT));
  EXPECT_EQ(4095U, add_actual.size());
  EXPECT_TRUE(add_actual.ends_with("01234"));

  // Appending when already at 4095 bytes leaves 1 byte for vsnprintf (NUL
  // only), keeping the buffer unchanged.
  coda_add_error_message("z");
  EXPECT_EQ(4095U,
            std::string_view(coda_errno_to_string(CODA_ERROR_PRODUCT)).size());
}

TEST(CodaErrnoTest, CursorAddToErrorMessageAppendsRootAndNestedPaths) {
  TempFile xml_file("coda_errno_cursor.xml");
  ASSERT_TRUE(xml_file.WriteText(
      "<?xml version=\"1.0\"?><root><item>123</item></root>"));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(xml_file.c_str(), product.OutParam()));

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  coda_set_error(CODA_ERROR_INVALID_TYPE, "cannot read value");
  coda_cursor_add_to_error_message(&cursor);
  EXPECT_STREQ("cannot read value at /",
               coda_errno_to_string(CODA_ERROR_INVALID_TYPE));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/root/item"));
  coda_set_error(CODA_ERROR_INVALID_TYPE, "bad leaf");
  coda_cursor_add_to_error_message(&cursor);
  EXPECT_STREQ("bad leaf at /root/item",
               coda_errno_to_string(CODA_ERROR_INVALID_TYPE));
}

TEST(CodaErrnoTest, SetErrorHdf5PopulatesMessageFromHdf5Stack) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // Non-NULL message skips coda_hdf5_add_error_message().
  coda_set_error(CODA_ERROR_HDF5, "explicit hdf5 message");
  EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
  EXPECT_STREQ("explicit hdf5 message", coda_errno_to_string(CODA_ERROR_HDF5));

  // NULL message triggers the HAVE_HDF5 branch (coda_hdf5_add_error_message()).
  // Note: upstream coda-hdf5.c leaks strings from H5Eget_major/H5Eget_minor if
  // the HDF5 error stack is non-empty, so we exercise the dispatch branch with
  // an empty HDF5 error stack.
  coda_set_error(CODA_ERROR_HDF5, nullptr);
  EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
  EXPECT_STREQ("HDF5 error", coda_errno_to_string(CODA_ERROR_HDF5));
}

TEST(CodaErrnoTest, RealApiFailuresSetExpectedErrnoAndMessages) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Opening a non-existent file sets CODA_ERROR_FILE_NOT_FOUND with the
  // path.
  TempFile missing("coda_errno_missing.dat");
  ScopedProduct product;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(missing.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
  EXPECT_THAT(coda_errno_to_string(CODA_ERROR_FILE_NOT_FOUND),
              HasSubstr(missing.string()));

  // 2. Opening malformed XML sets CODA_ERROR_XML and appends parse location.
  TempFile bad_xml("coda_errno_malformed.xml");
  ASSERT_TRUE(
      bad_xml.WriteText("<?xml version=\"1.0\"?><root><unclosed></root>"));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open(bad_xml.c_str(), product.OutParam()));
  EXPECT_EQ(CODA_ERROR_XML, coda_errno);
  EXPECT_THAT(coda_errno_to_string(CODA_ERROR_XML),
              HasSubstr("xml parse error"));
}

// TODO(schwehr): 98.9% line coverage in libcoda/coda-errno.c (93/94); only line
// 180 (defensive va_list/snprintf error guard) is unreached.

}  // namespace
}  // namespace stcorp_coda
