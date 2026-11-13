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
// Tests CODA ASCII product reopen, close, line-index initialization, EOL
// validation, and asciiline cursor/expression integration
// (libcoda/coda-ascii.c).
//
// google3 build caveats:
//   - Built for Linux (WIN32 handle-transfer branches in
//     coda_ascii_reopen_with_definition() are compiled out).
//   - Internal OOM branches (malloc/strdup in
//     coda_ascii_reopen_with_definition() and realloc in
//     coda_ascii_init_asciilines()), the dead error check on
//     coda_bin_product_close() in coda_ascii_close(), and
//     `default: assert(0); exit(1);` in eol_type_to_string() are unreachable
//     without fault injection.
//   - Upstream coda_ascii_cursor_set_asciilines()
//     (libcoda/coda-ascii-cursor.c:611-625) retains an extra reference to the
//     element text type, leaking 72 bytes on the first call per product; tests
//     wrap that first call in a narrow absl::LeakCheckDisabler.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-ascii.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-ascii-internal.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-ascii.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-ascii-internal.h"
#include "third_party/stcorp_coda/libcoda/coda-bin-internal.h"
#include "third_party/stcorp_coda/libcoda/coda-definition.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

constexpr int64_t kAsciiParseBlockSize = 4096;

constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="AsciiTestClass">
  <cd:ProductType name="SimpleAscii">
    <cd:ProductDefinition id="SimpleAsciiProd" format="ascii" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">ASCIIMSG</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="EmptyAscii">
    <cd:ProductDefinition id="EmptyAsciiProd" format="ascii" version="1">
      <cd:DetectionRule>
        <cd:MatchSize size="0"/>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="LineAscii">
    <cd:ProductDefinition id="LineAsciiProd" format="ascii" version="1"/>
  </cd:ProductType>
</cd:ProductClass>
)xml";

constexpr std::string_view kSimpleAsciiProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="SimpleAsciiProd" format="ascii">
  <cd:Record>
    <cd:Field name="header">
      <cd:AsciiLine/>
    </cd:Field>
    <cd:Field name="sep1" hidden="true">
      <cd:AsciiLineSeparator/>
    </cd:Field>
    <cd:Field name="count">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="sep2" hidden="true">
      <cd:AsciiLineSeparator/>
    </cd:Field>
    <cd:Field name="trailer">
      <cd:AsciiLine/>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

constexpr std::string_view kEmptyAsciiProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="EmptyAsciiProd" format="ascii">
  <cd:Record/>
</cd:ProductDefinition>
)xml";

constexpr std::string_view kLineAsciiProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="LineAsciiProd" format="ascii">
  <cd:Record>
    <cd:Field name="first_line">
      <cd:AsciiLine/>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

// Isolates CODA environment variables, definition path, coda_option_use_mmap,
// and thread-local coda_errno across every test.
class ScopedAsciiTestEnv {
 public:
  ScopedAsciiTestEnv()
      : env_def_("CODA_DEFINITION"),
        env_leap_("CODA_LEAP_SECOND_TABLE"),
        saved_use_mmap_(coda_get_option_use_mmap()) {
    env_def_.Unset();
    env_leap_.Unset();
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  ~ScopedAsciiTestEnv() {
    coda_set_option_use_mmap(saved_use_mmap_);
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  ScopedAsciiTestEnv(const ScopedAsciiTestEnv&) = delete;
  ScopedAsciiTestEnv& operator=(const ScopedAsciiTestEnv&) = delete;

 private:
  ScopedEnvVar env_def_;
  ScopedEnvVar env_leap_;
  int saved_use_mmap_;
};

std::vector<uint8_t> BuildAsciiTestCodadefBytes() {
  const std::array<std::pair<std::string_view, std::string_view>, 3> entries = {
      {
          {"products/SimpleAsciiProd.xml", kSimpleAsciiProdXml},
          {"products/EmptyAsciiProd.xml", kEmptyAsciiProdXml},
          {"products/LineAsciiProd.xml", kLineAsciiProdXml},
      }};
  return BuildCodadefBytes(kIndexXml, entries);
}

std::string ReadCursorString(const coda_cursor* cursor) {
  long len = -1;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(cursor, &len));
  if (len < 0) {
    return {};
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, '\0');
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_string(cursor, buf.data(), len + 1));
  return std::string(buf.data(), static_cast<size_t>(len));
}

TEST(CodaAsciiTest, OpenAndRecognizeErrorsAndUnsupportedAsciiFiles) {
  const ScopedAsciiTestEnv test_env;
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));

  const TempFile plain_text("coda_ascii_unrecognized.txt");
  ASSERT_TRUE(plain_text.WriteText("ASCIIMSG header\n123\nend\n"));

  // 1. Without a .codadef, CODA classifies plain text as binary and rejects
  //    coda_open() with CODA_ERROR_UNSUPPORTED_PRODUCT.
  {
    ScopedCodaSession session;
    ASSERT_EQ(CODA_SUCCESS, session.Init());

    const std::filesystem::path missing_path =
        std::filesystem::path(::testing::TempDir()) /
        "coda_ascii_missing_file.txt";
    std::error_code ec;
    std::filesystem::remove(missing_path, ec);

    ScopedProduct product;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_open(missing_path.c_str(), product.OutParam()));
    EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);
    EXPECT_EQ(nullptr, product.get());

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_recognize_file(missing_path.c_str(), nullptr, nullptr,
                                      nullptr, nullptr, nullptr));
    EXPECT_EQ(CODA_ERROR_FILE_NOT_FOUND, coda_errno);

    const TempDir dir("coda_ascii_dir_not_file");
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_open(dir.c_str(), product.OutParam()));
    EXPECT_EQ(CODA_ERROR_FILE_OPEN, coda_errno);

    coda_format fmt = coda_format_ascii;
    const char* pclass = "sentinel";
    const char* ptype = "sentinel";
    int version = 99;
    ASSERT_EQ(CODA_SUCCESS,
              coda_recognize_file(plain_text.c_str(), nullptr, &fmt, &pclass,
                                  &ptype, &version));
    EXPECT_EQ(coda_format_binary, fmt);
    EXPECT_EQ(nullptr, pclass);
    EXPECT_EQ(nullptr, ptype);
    EXPECT_EQ(-1, version);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_open(plain_text.c_str(), product.OutParam()));
    EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
  }

  // 2. With the ASCII .codadef loaded, verify coda_open_as() error paths and
  //    coda_close(nullptr).
  const TempFile codadef_file("coda_ascii_errors.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(BuildAsciiTestCodadefBytes()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  ScopedProduct product;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open_as(plain_text.c_str(), "NoSuchClass", "SimpleAscii",
                             1, product.OutParam()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_EQ(nullptr, product.get());

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_open_as(plain_text.c_str(), nullptr, nullptr, -1,
                             product.OutParam()));
  EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);
  EXPECT_EQ(nullptr, product.get());

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_close(nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
}

TEST(CodaAsciiTest, RecognizeAndOpenSimpleAsciiInBothMmapModes) {
  const ScopedAsciiTestEnv test_env;
  const TempFile codadef_file("coda_ascii_simple.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(BuildAsciiTestCodadefBytes()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  constexpr std::string_view kPayload = "ASCIIMSG header\n42\ndone\n";
  const TempFile data_file("coda_ascii_simple.txt");
  ASSERT_TRUE(data_file.WriteText(kPayload));

  // 1. coda_recognize_file() identifies the ASCII product definition.
  int64_t recognized_size = -1;
  coda_format recognized_format = coda_format_binary;
  const char* recognized_class = nullptr;
  const char* recognized_type = nullptr;
  int recognized_version = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(data_file.c_str(), &recognized_size,
                                &recognized_format, &recognized_class,
                                &recognized_type, &recognized_version));
  EXPECT_EQ(static_cast<int64_t>(kPayload.size()), recognized_size);
  EXPECT_EQ(coda_format_ascii, recognized_format);
  EXPECT_STREQ("AsciiTestClass", recognized_class);
  EXPECT_STREQ("SimpleAscii", recognized_type);
  EXPECT_EQ(1, recognized_version);

  // 2. Open and read fields in both mmap and pread/fd modes.
  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(data_file.c_str(), product.OutParam()));
    ASSERT_NE(nullptr, product.get());

    coda_format format = coda_format_binary;
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_format(product.get(), &format));
    EXPECT_EQ(coda_format_ascii, format);

    int64_t file_size = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_get_product_file_size(product.get(), &file_size));
    EXPECT_EQ(static_cast<int64_t>(kPayload.size()), file_size);

    const char* pclass = nullptr;
    const char* ptype = nullptr;
    int version = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_class(product.get(), &pclass));
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_type(product.get(), &ptype));
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_version(product.get(), &version));
    EXPECT_STREQ("AsciiTestClass", pclass);
    EXPECT_STREQ("SimpleAscii", ptype);
    EXPECT_EQ(1, version);

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/header"));
    EXPECT_EQ("ASCIIMSG header", ReadCursorString(&cursor));

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/count"));
    int32_t count = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &count));
    EXPECT_EQ(42, count);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/trailer"));
    EXPECT_EQ("done", ReadCursorString(&cursor));

    const auto* ascii =
        reinterpret_cast<const coda_ascii_product*>(product.get());
    EXPECT_EQ(use_mmap, ascii->use_mmap);
    EXPECT_EQ(eol_lf, ascii->end_of_line);
    EXPECT_EQ(3, ascii->num_asciilines);
    EXPECT_NE(nullptr, ascii->asciiline_end_offset);
    EXPECT_EQ(eol_lf, ascii->lastline_ending);
    EXPECT_EQ(nullptr, ascii->asciilines);

    product.Reset();
  }

  // 3. Truncated ASCII file opened via coda_open_as() fails on out-of-bounds
  //    read past EOF.
  const TempFile truncated_file("coda_ascii_truncated.txt");
  ASSERT_TRUE(truncated_file.WriteText("ASCIIMSG header\n"));
  ScopedProduct trunc_product;
  ASSERT_EQ(CODA_SUCCESS,
            coda_open_as(truncated_file.c_str(), "AsciiTestClass",
                         "SimpleAscii", 1, trunc_product.OutParam()));
  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_set_product(&cursor, trunc_product.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/count"));
  int32_t count = -1;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_int32(&cursor, &count));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
}

TEST(CodaAsciiTest, DirectReopenWithDefinitionAndDirectClose) {
  const ScopedAsciiTestEnv test_env;
  const TempFile codadef_file("coda_ascii_reopen.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(BuildAsciiTestCodadefBytes()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  coda_product_definition* def = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_data_dictionary_get_definition(
                              "AsciiTestClass", "SimpleAscii", 1, &def));
  ASSERT_NE(nullptr, def);
  if (!def->initialized) {
    ASSERT_EQ(CODA_SUCCESS, coda_read_product_definition(def));
  }
  ASSERT_NE(nullptr, def->root_type);

  constexpr std::string_view kPayload = "ASCIIMSG row1\n7\nrow3\n";
  const TempFile data_file("coda_ascii_reopen.txt");
  ASSERT_TRUE(data_file.WriteText(kPayload));

  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_bin_open(data_file.c_str(),
                                          static_cast<int64_t>(kPayload.size()),
                                          product.OutParam()));
    ASSERT_NE(nullptr, product.get());

    coda_product* raw = product.release();
    const int reopen_status = coda_ascii_reopen_with_definition(&raw, def);
    product.Reset(raw);
    ASSERT_EQ(CODA_SUCCESS, reopen_status);

    const auto* ascii =
        reinterpret_cast<const coda_ascii_product*>(product.get());
    EXPECT_STREQ(data_file.c_str(), ascii->filename);
    EXPECT_EQ(static_cast<int64_t>(kPayload.size()), ascii->file_size);
    EXPECT_EQ(coda_format_ascii, ascii->format);
    EXPECT_EQ(def, ascii->product_definition);
    EXPECT_EQ(reinterpret_cast<coda_dynamic_type*>(def->root_type),
              ascii->root_type);
    EXPECT_EQ(use_mmap, ascii->use_mmap);
    if (use_mmap == 1) {
      EXPECT_EQ(-1, ascii->fd);
      EXPECT_NE(nullptr, ascii->mem_ptr);
      EXPECT_EQ(static_cast<int64_t>(kPayload.size()), ascii->mem_size);
    } else {
      EXPECT_GE(ascii->fd, 0);
      EXPECT_EQ(nullptr, ascii->mem_ptr);
      EXPECT_EQ(0, ascii->mem_size);
    }
    EXPECT_EQ(eol_unknown, ascii->end_of_line);
    EXPECT_EQ(-1, ascii->num_asciilines);
    EXPECT_EQ(nullptr, ascii->asciiline_end_offset);
    EXPECT_EQ(eol_unknown, ascii->lastline_ending);
    EXPECT_EQ(nullptr, ascii->asciilines);

    // Explicitly close via coda_ascii_close() while asciiline_end_offset and
    // asciilines are both NULL.
    EXPECT_EQ(CODA_SUCCESS, coda_ascii_close(product.release()));
  }
}

TEST(CodaAsciiTest, EmptyZeroByteAsciiFile) {
  const ScopedAsciiTestEnv test_env;
  const TempFile codadef_file("coda_ascii_empty.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(BuildAsciiTestCodadefBytes()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const TempFile empty_file("coda_ascii_empty.txt");
  ASSERT_TRUE(empty_file.WriteBytes({}));

  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    // 1. Open via MatchSize(size="0") and initialize asciiline metadata.
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(empty_file.c_str(), product.OutParam()));

    const auto* ascii =
        reinterpret_cast<const coda_ascii_product*>(product.get());
    EXPECT_EQ(0, ascii->file_size);
    EXPECT_EQ(0, ascii->use_mmap);  // CODA skips mmap for 0-byte files.
    ASSERT_EQ(CODA_SUCCESS, coda_ascii_init_asciilines(product.get()));
    EXPECT_EQ(0, ascii->num_asciilines);
    EXPECT_EQ(nullptr, ascii->asciiline_end_offset);
    EXPECT_EQ(eol_unknown, ascii->end_of_line);
    EXPECT_EQ(eol_unknown, ascii->lastline_ending);
    product.Reset();

    // 2. On a fresh 0-byte product, coda_ascii_cursor_set_asciilines() yields
    //    a 0-element array cursor.
    ScopedProduct cursor_product;
    ASSERT_EQ(CODA_SUCCESS,
              coda_open(empty_file.c_str(), cursor_product.OutParam()));
    coda_cursor cursor;
    {
      // Upstream coda-ascii-cursor.c:611-625 leaks the 72B element type once
      // per product on the initial set_asciilines() call.
      const absl::LeakCheckDisabler leak_check_disabler;
      ASSERT_EQ(CODA_SUCCESS, coda_ascii_cursor_set_asciilines(
                                  &cursor, cursor_product.get()));
    }
    long num_elements = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_num_elements(&cursor, &num_elements));
    EXPECT_EQ(0, num_elements);
    cursor_product.Reset();

    // 3. Reading an AsciiLine field from an empty file fails out-of-bounds.
    ScopedProduct line_product;
    ASSERT_EQ(CODA_SUCCESS,
              coda_open_as(empty_file.c_str(), "AsciiTestClass", "LineAscii", 1,
                           line_product.OutParam()));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_set_product(&cursor, line_product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/first_line"));
    long line_len = -1;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_string_length(&cursor, &line_len));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }
}

TEST(CodaAsciiTest, InitAsciilinesHandlesAllLineEndingsAndNoTrailingEol) {
  const ScopedAsciiTestEnv test_env;
  const TempFile codadef_file("coda_ascii_eols.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(BuildAsciiTestCodadefBytes()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  struct EolCase {
    std::string_view name;
    std::string_view content;
    eol_type expected_eol;
    eol_type expected_lastline;
    std::vector<long> expected_offsets;
    std::string_view expected_first_line;
  };

  const EolCase kCases[] = {
      {
          .name = "lf",
          .content = "alpha\nbeta\ngamma\n",
          .expected_eol = eol_lf,
          .expected_lastline = eol_lf,
          .expected_offsets = {6, 11, 17},
          .expected_first_line = "alpha",
      },
      {
          .name = "cr",
          .content = "alpha\rbeta\rgamma\r",
          .expected_eol = eol_cr,
          .expected_lastline = eol_cr,
          .expected_offsets = {6, 11, 17},
          .expected_first_line = "alpha",
      },
      {
          .name = "single_line_cr_at_eof",
          .content = "only\r",
          .expected_eol = eol_cr,
          .expected_lastline = eol_cr,
          .expected_offsets = {5},
          .expected_first_line = "only",
      },
      {
          .name = "crlf",
          .content = "alpha\r\nbeta\r\ngamma\r\n",
          .expected_eol = eol_crlf,
          .expected_lastline = eol_crlf,
          .expected_offsets = {7, 13, 20},
          .expected_first_line = "alpha",
      },
      {
          .name = "lf_without_trailing_eol",
          .content = "alpha\nbeta",
          .expected_eol = eol_lf,
          .expected_lastline = eol_unknown,
          .expected_offsets = {6, 10},
          .expected_first_line = "alpha",
      },
      {
          .name = "single_line_without_eol",
          .content = "single_line",
          .expected_eol = eol_unknown,
          .expected_lastline = eol_unknown,
          .expected_offsets = {11},
          .expected_first_line = "single_line",
      },
  };

  for (const EolCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const TempFile file(std::string("coda_ascii_eol_") + std::string(tc.name) +
                        ".txt");
    ASSERT_TRUE(file.WriteText(tc.content));

    for (const int use_mmap : {1, 0}) {
      SCOPED_TRACE(use_mmap);
      ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

      ScopedProduct product;
      ASSERT_EQ(CODA_SUCCESS, coda_open_as(file.c_str(), "AsciiTestClass",
                                           "LineAscii", 1, product.OutParam()));

      coda_cursor cursor;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/first_line"));
      EXPECT_EQ(tc.expected_first_line, ReadCursorString(&cursor));

      const auto* ascii =
          reinterpret_cast<const coda_ascii_product*>(product.get());
      EXPECT_EQ(tc.expected_eol, ascii->end_of_line);
      EXPECT_EQ(tc.expected_lastline, ascii->lastline_ending);
      ASSERT_EQ(static_cast<long>(tc.expected_offsets.size()),
                ascii->num_asciilines);
      ASSERT_NE(nullptr, ascii->asciiline_end_offset);
      for (size_t i = 0; i < tc.expected_offsets.size(); ++i) {
        EXPECT_EQ(tc.expected_offsets[i], ascii->asciiline_end_offset[i])
            << "line " << i;
      }
    }
  }
}

TEST(CodaAsciiTest, InitAsciilinesReallocatesPastBlockSizeAndCachesAsciilines) {
  const ScopedAsciiTestEnv test_env;
  const TempFile codadef_file("coda_ascii_many_lines.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(BuildAsciiTestCodadefBytes()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 40 lines exceeds 2 * BLOCK_SIZE (16) to trigger multiple realloc() calls.
  constexpr int kNumLines = 40;
  std::string payload;
  std::vector<long> expected_offsets;
  expected_offsets.reserve(kNumLines);
  for (int i = 0; i < kNumLines; ++i) {
    payload += "line_";
    payload.push_back(static_cast<char>('0' + (i / 10)));
    payload.push_back(static_cast<char>('0' + (i % 10)));
    payload.push_back('\n');
    expected_offsets.push_back(static_cast<long>(payload.size()));
  }

  const TempFile file("coda_ascii_many_lines.txt");
  ASSERT_TRUE(file.WriteText(payload));

  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    // 1. Direct coda_ascii_cursor_set_asciilines() initializes both
    //    asciiline_end_offset and asciilines, then reuses cached asciilines on
    //    a second call without leaking.
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(file.c_str(), "AsciiTestClass",
                                         "LineAscii", 1, product.OutParam()));

    coda_cursor cursor;
    {
      const absl::LeakCheckDisabler leak_check_disabler;
      ASSERT_EQ(CODA_SUCCESS,
                coda_ascii_cursor_set_asciilines(&cursor, product.get()));
    }
    // Second call uses the cached product->asciilines array directly.
    ASSERT_EQ(CODA_SUCCESS,
              coda_ascii_cursor_set_asciilines(&cursor, product.get()));

    const auto* ascii =
        reinterpret_cast<const coda_ascii_product*>(product.get());
    EXPECT_EQ(kNumLines, ascii->num_asciilines);
    EXPECT_NE(nullptr, ascii->asciilines);
    ASSERT_NE(nullptr, ascii->asciiline_end_offset);
    for (int i = 0; i < kNumLines; ++i) {
      EXPECT_EQ(expected_offsets[static_cast<size_t>(i)],
                ascii->asciiline_end_offset[i]);
    }

    long count = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &count));
    EXPECT_EQ(kNumLines, count);

    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_array_element_by_index(&cursor, 0));
    EXPECT_EQ("line_00\n", ReadCursorString(&cursor));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_array_element_by_index(&cursor, kNumLines - 1));
    EXPECT_EQ("line_39\n", ReadCursorString(&cursor));

    product.Reset();

    // 2. Public CODA expressions `numelements(asciiline)` and
    //    `str(asciiline[1])` invoke coda_ascii_cursor_set_asciilines().
    ScopedProduct expr_product;
    ASSERT_EQ(CODA_SUCCESS,
              coda_open_as(file.c_str(), "AsciiTestClass", "LineAscii", 1,
                           expr_product.OutParam()));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_set_product(&cursor, expr_product.get()));

    ScopedExpr count_expr;
    constexpr char kCountExpr[] = "numelements(asciiline)";
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(kCountExpr, count_expr.OutParam()));
    int64_t expr_count = -1;
    {
      const absl::LeakCheckDisabler leak_check_disabler;
      ASSERT_EQ(CODA_SUCCESS, coda_expression_eval_integer(
                                  count_expr.get(), &cursor, &expr_count));
    }
    EXPECT_EQ(kNumLines, expr_count);

    ScopedExpr line_expr;
    constexpr char kLineExpr[] = "str(asciiline[1])";
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string(kLineExpr, line_expr.OutParam()));
    ScopedCodaString line_val;
    long line_len = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_eval_string(line_expr.get(), &cursor,
                                          line_val.OutParam(), &line_len));
    EXPECT_EQ(8, line_len);
    EXPECT_STREQ("line_01\n", line_val.get());
  }
}

TEST(CodaAsciiTest, LargeMultiBlockFilesAndBlockBoundaryCrAndCrlf) {
  const ScopedAsciiTestEnv test_env;
  const TempFile codadef_file("coda_ascii_large.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(BuildAsciiTestCodadefBytes()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Multi-block LF file (~10 KiB > 2 * ASCII_PARSE_BLOCK_SIZE).
  std::string large_lf;
  std::vector<long> large_offsets;
  for (int i = 0; i < 100; ++i) {
    large_lf.append(100, static_cast<char>('a' + (i % 26)));
    large_lf.push_back('\n');
    large_offsets.push_back(static_cast<long>(large_lf.size()));
  }
  ASSERT_GT(static_cast<int64_t>(large_lf.size()), 2 * kAsciiParseBlockSize);

  // 2. CRLF split across the 4096-byte block boundary: '\r' at index 4095,
  //    '\n' at index 4096.
  std::string split_crlf(static_cast<size_t>(kAsciiParseBlockSize - 1), 'A');
  split_crlf += "\r\ntail\r\n";

  // 3. Bare CR at the 4096-byte block boundary followed by non-LF at index
  //    4096: '\r' at index 4095, 'n' at index 4096.
  std::string split_cr(static_cast<size_t>(kAsciiParseBlockSize - 1), 'B');
  split_cr += "\rnext\r";

  const TempFile large_lf_file("coda_ascii_large_lf.txt");
  const TempFile split_crlf_file("coda_ascii_split_crlf.txt");
  const TempFile split_cr_file("coda_ascii_split_cr.txt");
  ASSERT_TRUE(large_lf_file.WriteText(large_lf));
  ASSERT_TRUE(split_crlf_file.WriteText(split_crlf));
  ASSERT_TRUE(split_cr_file.WriteText(split_cr));

  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    {
      ScopedProduct product;
      ASSERT_EQ(CODA_SUCCESS,
                coda_open_as(large_lf_file.c_str(), "AsciiTestClass",
                             "LineAscii", 1, product.OutParam()));
      ASSERT_EQ(CODA_SUCCESS, coda_ascii_init_asciilines(product.get()));
      const auto* ascii =
          reinterpret_cast<const coda_ascii_product*>(product.get());
      EXPECT_EQ(eol_lf, ascii->end_of_line);
      EXPECT_EQ(eol_lf, ascii->lastline_ending);
      ASSERT_EQ(100, ascii->num_asciilines);
      EXPECT_EQ(large_offsets.front(), ascii->asciiline_end_offset[0]);
      EXPECT_EQ(large_offsets.back(), ascii->asciiline_end_offset[99]);
    }

    {
      ScopedProduct product;
      ASSERT_EQ(CODA_SUCCESS,
                coda_open_as(split_crlf_file.c_str(), "AsciiTestClass",
                             "LineAscii", 1, product.OutParam()));
      ASSERT_EQ(CODA_SUCCESS, coda_ascii_init_asciilines(product.get()));
      const auto* ascii =
          reinterpret_cast<const coda_ascii_product*>(product.get());
      EXPECT_EQ(eol_crlf, ascii->end_of_line);
      EXPECT_EQ(eol_crlf, ascii->lastline_ending);
      ASSERT_EQ(2, ascii->num_asciilines);
      EXPECT_EQ(4097, ascii->asciiline_end_offset[0]);
      EXPECT_EQ(4103, ascii->asciiline_end_offset[1]);
    }

    {
      ScopedProduct product;
      ASSERT_EQ(CODA_SUCCESS,
                coda_open_as(split_cr_file.c_str(), "AsciiTestClass",
                             "LineAscii", 1, product.OutParam()));
      ASSERT_EQ(CODA_SUCCESS, coda_ascii_init_asciilines(product.get()));
      const auto* ascii =
          reinterpret_cast<const coda_ascii_product*>(product.get());
      EXPECT_EQ(eol_cr, ascii->end_of_line);
      EXPECT_EQ(eol_cr, ascii->lastline_ending);
      ASSERT_EQ(2, ascii->num_asciilines);
      EXPECT_EQ(4096, ascii->asciiline_end_offset[0]);
      EXPECT_EQ(4101, ascii->asciiline_end_offset[1]);
    }
  }
}

TEST(CodaAsciiTest, InitAsciilinesRejectsMixedLineEndings) {
  const ScopedAsciiTestEnv test_env;
  const TempFile codadef_file("coda_ascii_mixed_eols.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(BuildAsciiTestCodadefBytes()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  struct MixedEolCase {
    std::string_view name;
    std::string content;
    std::string_view expected_substr;
  };

  // Pad LF + 4093 'b' bytes so the following '\r' lands at index 4095 (end of
  // the first 4096-byte read block) and the next byte lands at index 4096.
  const std::string prefix_to_4095 =
      std::string("a\n") + std::string(kAsciiParseBlockSize - 3, 'b');

  const MixedEolCase kCases[] = {
      {.name = "lf_then_cr",
       .content = "a\nb\rc\n",
       .expected_substr = "got CR but expected LF"},
      {.name = "lf_then_crlf",
       .content = "a\nb\r\nc\n",
       .expected_substr = "got CRLF but expected LF"},
      {.name = "cr_then_lf",
       .content = "a\rb\nc\r",
       .expected_substr = "got LF but expected CR"},
      {.name = "cr_then_crlf",
       .content = "a\rb\r\nc\r",
       .expected_substr = "got CRLF but expected CR"},
      {.name = "crlf_then_lf",
       .content = "a\r\nb\nc\r\n",
       .expected_substr = "got LF but expected CRLF"},
      {.name = "crlf_then_cr",
       .content = "a\r\nb\rc\r\n",
       .expected_substr = "got CR but expected CRLF"},
      {.name = "lf_then_cr_at_eof",
       .content = "a\nb\r",
       .expected_substr = "got CR but expected LF"},
      {.name = "crlf_then_cr_at_eof",
       .content = "a\r\nb\r",
       .expected_substr = "got CR but expected CRLF"},
      {.name = "lf_then_split_crlf_at_block_boundary",
       .content = prefix_to_4095 + "\r\nc\n",
       .expected_substr = "got CRLF but expected LF"},
      {.name = "lf_then_split_cr_at_block_boundary",
       .content = prefix_to_4095 + "\rx\n",
       .expected_substr = "got CR but expected LF"},
  };

  for (const MixedEolCase& tc : kCases) {
    SCOPED_TRACE(tc.name);
    const TempFile file(std::string("coda_ascii_mixed_") +
                        std::string(tc.name) + ".txt");
    ASSERT_TRUE(file.WriteText(tc.content));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(file.c_str(), "AsciiTestClass",
                                         "LineAscii", 1, product.OutParam()));

    coda_cursor cursor;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_ascii_cursor_set_asciilines(&cursor, product.get()));
    EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr("inconsistent end-of-line sequence"));
    EXPECT_THAT(coda_errno_to_string(coda_errno),
                HasSubstr(tc.expected_substr));
  }
}

TEST(CodaAsciiTest, InitAsciilinesPropagatesLseekAndReadBytesErrors) {
  const ScopedAsciiTestEnv test_env;
  const TempFile codadef_file("coda_ascii_io_err.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(BuildAsciiTestCodadefBytes()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  const TempFile file("coda_ascii_io_err.txt");
  ASSERT_TRUE(file.WriteText("hello\n"));

  // 1. In fd mode (use_mmap == 0), an invalid file descriptor triggers lseek()
  //    failure -> CODA_ERROR_FILE_READ.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(0));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(file.c_str(), "AsciiTestClass",
                                         "LineAscii", 1, product.OutParam()));
    auto* ascii = reinterpret_cast<coda_ascii_product*>(product.get());
    const int saved_fd = ascii->fd;
    ascii->fd = -1;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_ascii_init_asciilines(product.get()));
    EXPECT_EQ(CODA_ERROR_FILE_READ, coda_errno);
    ascii->fd = saved_fd;
  }

  // 2. In mmap mode (use_mmap == 1), a file_size exceeding mem_size triggers
  //    read_bytes() failure -> CODA_ERROR_OUT_OF_BOUNDS_READ.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(1));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(file.c_str(), "AsciiTestClass",
                                         "LineAscii", 1, product.OutParam()));
    auto* ascii = reinterpret_cast<coda_ascii_product*>(product.get());
    const int64_t saved_mem_size = ascii->mem_size;
    ascii->mem_size = 1;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_ascii_init_asciilines(product.get()));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
    ascii->mem_size = saved_mem_size;
  }
}

// TODO(schwehr): Add fault-injection coverage for malloc/strdup OOM paths in
// coda_ascii_reopen_with_definition() and realloc() OOM in
// coda_ascii_init_asciilines() if an allocator test seam is introduced.

}  // namespace
}  // namespace stcorp_coda
