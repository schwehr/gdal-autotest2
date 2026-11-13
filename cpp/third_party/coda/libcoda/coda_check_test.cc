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
// Tests coda_product_check() across binary, ASCII, XML, NetCDF, HDF5, and
// synthetic memory-backend products, including definition checks, size
// expressions, fixed values, EOL checks, attributes, and error handling
// (libcoda/coda-check.c).
//
// google3 build caveats:
//   - HDF4 is compiled out (HAVE_HDF4 undefined); HDF5 is enabled (HAVE_HDF5=1)
//     with H5_USE_16_API in config.h, so HDF5 creation uses versioned *2 APIs.
//   - c_data_check's trailing-XML-whitespace branch under size_check is dead
//     because coda_product_check() only sets size_check=1 for ascii/binary
//     (21 lines in 878-909).
//   - Remaining missed lines in coda-check.c (507/574 = 88.3%, 67 missed):
//     9 OOM checks (409-410, 412, 522-523, 525, 675, 677-678); 1
//     assert(0)/exit(1) (462); 21 dead XML-under-size_check lines (878-909); 2
//     unreachable bit_size==NULL lines (873, 875; asserted non-NULL at 306); 2
//     unreachable attributes_definition==NULL lines (600-601;
//     coda_type_get_attributes returns coda_type_empty_record); and 32
//     defensive error returns on already-validated cursors/types/elements (56,
//     60, 140, 154, 203, 220, 229, 251, 274, 278, 314, 401, 506, 548, 575, 586,
//     621, 640, 644, 648, 708, 729, 744, 755, 766, 836, 842, 844, 846, 849,
//     949, 967).
//   - HDF5 bulk array read error (coda-check.c:685) leaks its temporary
//     `values` buffer in upstream CODA, so that sub-test wraps the call in
//     absl::LeakCheckDisabler.
//   - ASCII tests avoid coda_ascii_cursor_set_asciilines (coda-ascii-cursor.c)
//     which intentionally retains an unfreed asciiline type on the product.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-check.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-internal.h

#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/debugging/leak_check.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/hdf5_test_util.h"
#include "third_party/stcorp_coda/google/util/netcdf_builder.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-bin-internal.h"
#include "third_party/stcorp_coda/libcoda/coda-definition.h"
#include "third_party/stcorp_coda/libcoda/coda-internal.h"
#include "third_party/stcorp_coda/libcoda/coda-mem-internal.h"
#include "third_party/stcorp_coda/libcoda/coda-type.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::AllOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

constexpr coda_endianness kHostEndianness =
    (std::endian::native == std::endian::big) ? coda_big_endian
                                              : coda_little_endian;

struct CheckFinding {
  std::string path;
  std::string message;
};

// Reconstructs a readable cursor path without relying on stdout/thread-locals.
std::string FormatCursorPath(const coda_cursor* cursor) {
  if (cursor == nullptr) {
    return "<null>";
  }
  int depth = 0;
  if (coda_cursor_get_depth(cursor, &depth) != CODA_SUCCESS || depth <= 0) {
    return "/";
  }
  coda_cursor cur = *cursor;
  std::vector<std::string> segments;
  segments.reserve(static_cast<size_t>(depth));
  for (int d = depth; d >= 1; --d) {
    long index = -1;
    if (coda_cursor_get_index(&cur, &index) != CODA_SUCCESS) {
      break;
    }
    if (index == -1) {
      segments.emplace_back("@");
      if (coda_cursor_goto_parent(&cur) != CODA_SUCCESS) {
        break;
      }
      continue;
    }
    if (coda_cursor_goto_parent(&cur) != CODA_SUCCESS) {
      break;
    }
    coda_type_class parent_class = coda_record_class;
    if (coda_cursor_get_type_class(&cur, &parent_class) != CODA_SUCCESS) {
      break;
    }
    if (parent_class == coda_array_class) {
      segments.push_back(absl::StrCat("[", index, "]"));
    } else if (parent_class == coda_record_class) {
      coda_type* parent_type = nullptr;
      const char* field_name = nullptr;
      if (coda_cursor_get_type(&cur, &parent_type) == CODA_SUCCESS &&
          coda_type_get_record_field_name(parent_type, index, &field_name) ==
              CODA_SUCCESS &&
          field_name != nullptr) {
        segments.push_back(absl::StrCat("/", field_name));
      } else {
        segments.push_back(absl::StrCat("/#", index));
      }
    }
  }
  std::string path;
  for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
    path += *it;
  }
  return path.empty() ? "/" : path;
}

void CollectFinding(coda_cursor* cursor, const char* message, void* userdata) {
  auto* findings = static_cast<std::vector<CheckFinding>*>(userdata);
  findings->push_back({
      .path = FormatCursorPath(cursor),
      .message = (message != nullptr) ? message : "",
  });
}

struct TypeDeleter {
  void operator()(coda_type* type) const { coda_type_release(type); }
};
using ScopedType = std::unique_ptr<coda_type, TypeDeleter>;

template <typename T>
coda_type* TrackType(T* raw_type, std::vector<ScopedType>& owned_types) {
  auto* base = reinterpret_cast<coda_type*>(raw_type);
  owned_types.emplace_back(base);
  return base;
}

void AppendBeInt32(std::vector<uint8_t>& out, int32_t value) {
  const uint32_t u = static_cast<uint32_t>(value);
  out.push_back(static_cast<uint8_t>((u >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(u & 0xFF));
}

// Writes a single .codadef archive into `def_dir` and initializes CODA against
// it. Ensures CODA_DEFINITION/coda_definition_path/coda_errno never leak across
// tests.
class ScopedCodadefSession {
 public:
  explicit ScopedCodadefSession(std::string_view dir_name)
      : def_dir_(dir_name),
        restore_def_("CODA_DEFINITION"),
        restore_leap_("CODA_LEAP_SECOND_TABLE") {
    restore_def_.Unset();
    restore_leap_.Unset();
  }

  ~ScopedCodadefSession() {
    session_.Reset();
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  ScopedCodadefSession(const ScopedCodadefSession&) = delete;
  ScopedCodadefSession& operator=(const ScopedCodadefSession&) = delete;

  void InitWithCodadef(
      std::string_view index_xml,
      std::span<const std::pair<std::string_view, std::string_view>> entries) {
    const std::filesystem::path def_path = def_dir_.child("test.codadef");
    ASSERT_TRUE(
        WriteFileBytes(def_path, BuildCodadefBytes(index_xml, entries)));
    ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(def_dir_.c_str()));
    ASSERT_EQ(CODA_SUCCESS, session_.Init());
  }

  void InitEmpty() {
    ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(nullptr));
    ASSERT_EQ(CODA_SUCCESS, session_.Init());
  }

 private:
  const ScopedCodaOptions restore_options_;
  TempDir def_dir_;
  ScopedEnvVar restore_def_;
  ScopedEnvVar restore_leap_;
  ScopedCodaSession session_;
};

TEST(CodaCheckTest, NullProductReturnsInvalidArgument) {
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };
  std::vector<CheckFinding> findings;

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_check(nullptr, 0, CollectFinding, &findings));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_THAT(findings, IsEmpty());

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_check(nullptr, 1, CollectFinding, &findings));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_THAT(findings, IsEmpty());
}

TEST(CodaCheckTest, BinaryProductRichTypesQuickAndFullChecks) {
  constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="CheckClass">
  <cd:ProductType name="BinRich">
    <cd:ProductDefinition id="BinRich" format="binary" version="1"/>
  </cd:ProductType>
  <cd:ProductType name="BinBits">
    <cd:ProductDefinition id="BinBits" format="binary" version="1"/>
  </cd:ProductType>
  <cd:ProductType name="BinTruncTime">
    <cd:ProductDefinition id="BinTruncTime" format="binary" version="1"/>
  </cd:ProductType>
  <cd:ProductType name="BinBadSizeExpr">
    <cd:ProductDefinition id="BinBadSizeExpr" format="binary" version="1"/>
  </cd:ProductType>
</cd:ProductClass>
)xml";

  // 35 bytes total when count == 2:
  //   magic[4] + raw_hdr[2] + count[1] + opt_present[1] + opt_absent[0] +
  //   items[8] + empty_arr[0] + empty_rec[0] + choice.a[1] + cplx[8] +
  //   tstamp[10].
  constexpr std::string_view kBinRichXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinRich" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>4</cd:ByteSize>
        <cd:FixedValue>CODA</cd:FixedValue>
      </cd:Text>
    </cd:Field>
    <cd:Field name="raw_hdr">
      <cd:Raw>
        <cd:BitSize>16</cd:BitSize>
        <cd:FixedValue>AB</cd:FixedValue>
      </cd:Raw>
    </cd:Field>
    <cd:Field name="count">
      <cd:Integer>
        <cd:BitSize>8</cd:BitSize>
        <cd:NativeType>uint8</cd:NativeType>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="opt_present">
      <cd:Available>int(/count) == 2</cd:Available>
      <cd:Integer>
        <cd:BitSize>8</cd:BitSize>
        <cd:NativeType>uint8</cd:NativeType>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="opt_absent">
      <cd:Available>int(/count) == 9</cd:Available>
      <cd:Integer>
        <cd:BitSize>8</cd:BitSize>
        <cd:NativeType>uint8</cd:NativeType>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="items">
      <cd:Array>
        <cd:Dimension>int(/count)</cd:Dimension>
        <cd:Float>
          <cd:BitSize>32</cd:BitSize>
          <cd:NativeType>float</cd:NativeType>
          <cd:LittleEndian/>
        </cd:Float>
      </cd:Array>
    </cd:Field>
    <cd:Field name="empty_arr">
      <cd:Array>
        <cd:Dimension>0</cd:Dimension>
        <cd:Integer>
          <cd:BitSize>8</cd:BitSize>
          <cd:NativeType>uint8</cd:NativeType>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="empty_rec">
      <cd:Record/>
    </cd:Field>
    <cd:Field name="choice">
      <cd:Union>
        <cd:FieldExpression>0</cd:FieldExpression>
        <cd:Field name="a">
          <cd:Integer>
            <cd:BitSize>8</cd:BitSize>
            <cd:NativeType>uint8</cd:NativeType>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="b">
          <cd:Integer>
            <cd:BitSize>16</cd:BitSize>
            <cd:NativeType>uint16</cd:NativeType>
          </cd:Integer>
        </cd:Field>
      </cd:Union>
    </cd:Field>
    <cd:Field name="cplx">
      <cd:Complex>
        <cd:Float>
          <cd:BitSize>32</cd:BitSize>
          <cd:NativeType>float</cd:NativeType>
          <cd:LittleEndian/>
        </cd:Float>
      </cd:Complex>
    </cd:Field>
    <cd:Field name="tstamp">
      <cd:Time timeformat="time(str(.), &quot;yyyy-MM-dd&quot;)">
        <cd:Text>
          <cd:ByteSize>10</cd:ByteSize>
        </cd:Text>
      </cd:Time>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  constexpr std::string_view kBinBitsXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinBits" format="binary">
  <cd:Integer>
    <cd:BitSize>5</cd:BitSize>
    <cd:NativeType>uint8</cd:NativeType>
  </cd:Integer>
</cd:ProductDefinition>
)xml";

  constexpr std::string_view kBinTruncTimeXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinTruncTime" format="binary">
  <cd:Time timeformat="time(str(.), &quot;yyyy-MM-dd&quot;)">
    <cd:Text>
      <cd:ByteSize>10</cd:ByteSize>
    </cd:Text>
  </cd:Time>
</cd:ProductDefinition>
)xml";

  constexpr std::string_view kBinBadSizeExprXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinBadSizeExpr" format="binary">
  <cd:Record>
    <cd:Field name="tag">
      <cd:Text>
        <cd:ByteSize>3</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="dyn">
      <cd:Text>
        <cd:ByteSize>int(str(/tag))</cd:ByteSize>
      </cd:Text>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  const std::array<std::pair<std::string_view, std::string_view>, 4> entries = {
      {{"products/BinRich.xml", kBinRichXml},
       {"products/BinBits.xml", kBinBitsXml},
       {"products/BinTruncTime.xml", kBinTruncTimeXml},
       {"products/BinBadSizeExpr.xml", kBinBadSizeExprXml}},
  };

  ScopedCodadefSession env("coda_check_bin_rich");
  ASSERT_NO_FATAL_FAILURE(env.InitWithCodadef(kIndexXml, entries));

  const std::vector<uint8_t> valid_bytes = {
      'C',  'O',  'D',  'A',             // magic
      'A',  'B',                         // raw_hdr
      2,                                 // count
      99,                                // opt_present
      0x00, 0x00, 0x80, 0x3f,            // items[0] = 1.0f
      0x00, 0x00, 0x00, 0x40,            // items[1] = 2.0f
      42,                                // choice.a
      0x00, 0x00, 0x40, 0x40,            // cplx.real = 3.0f
      0x00, 0x00, 0x80, 0x40,            // cplx.imaginary = 4.0f
      '2',  '0',  '2',  '0',  '-', '0',  // tstamp
      '1',  '-',  '1',  '5',
  };
  ASSERT_EQ(35u, valid_bytes.size());

  // 1. Valid 35-byte binary product passes both quick and full checks.
  const TempFile valid_file("coda_check_bin_valid.dat");
  ASSERT_TRUE(valid_file.WriteBytes(valid_bytes));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(valid_file.c_str(), "CheckClass",
                                         "BinRich", 1, product.OutParam()));
    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 0, CollectFinding, &findings));
    EXPECT_THAT(findings, IsEmpty());
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_THAT(findings, IsEmpty());
  }

  // 2. Trailing extra byte triggers file-size mismatch in both modes.
  std::vector<uint8_t> extra_bytes = valid_bytes;
  extra_bytes.push_back(0x00);
  const TempFile extra_file("coda_check_bin_extra.dat");
  ASSERT_TRUE(extra_file.WriteBytes(extra_bytes));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(extra_file.c_str(), "CheckClass",
                                         "BinRich", 1, product.OutParam()));
    std::vector<CheckFinding> quick_findings;
    EXPECT_EQ(CODA_SUCCESS, coda_product_check(product.get(), 0, CollectFinding,
                                               &quick_findings));
    EXPECT_THAT(quick_findings,
                ElementsAre(AllOf(
                    Field(&CheckFinding::path, "<null>"),
                    Field(&CheckFinding::message,
                          "incorrect file size (actual size 36 does not match "
                          "calculated file size 35)"))));

    std::vector<CheckFinding> full_findings;
    EXPECT_EQ(CODA_SUCCESS, coda_product_check(product.get(), 1, CollectFinding,
                                               &full_findings));
    EXPECT_THAT(full_findings,
                ElementsAre(AllOf(
                    Field(&CheckFinding::path, "<null>"),
                    Field(&CheckFinding::message,
                          "incorrect file size (actual size 36 does not match "
                          "calculated file size 35)"))));
  }

  // 3. Corrupt fixed text, fixed raw bytes, and datetime in a 35-byte file.
  std::vector<uint8_t> corrupt_bytes = valid_bytes;
  corrupt_bytes[0] = 'X';
  corrupt_bytes[5] = 'Z';
  std::memcpy(&corrupt_bytes[25], "not-a-date", 10);
  const TempFile corrupt_file("coda_check_bin_corrupt.dat");
  ASSERT_TRUE(corrupt_file.WriteBytes(corrupt_bytes));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(corrupt_file.c_str(), "CheckClass",
                                         "BinRich", 1, product.OutParam()));
    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_THAT(
        findings,
        ElementsAre(
            AllOf(Field(&CheckFinding::path, "/magic"),
                  Field(&CheckFinding::message,
                        "string data does not match fixed value")),
            AllOf(Field(&CheckFinding::path, "/raw_hdr"),
                  Field(&CheckFinding::message,
                        "data does not match fixed value (value differs)")),
            AllOf(Field(&CheckFinding::path, "/tstamp"),
                  Field(&CheckFinding::message,
                        HasSubstr("incorrect year value")))));
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  // 4. Bit-unaligned calculated size (5 bits in 1-byte file) formats ":5".
  const TempFile bits_file("coda_check_bin_bits.dat");
  ASSERT_TRUE(bits_file.WriteBytes(std::to_array<uint8_t>({0x1f})));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(bits_file.c_str(), "CheckClass",
                                         "BinBits", 1, product.OutParam()));
    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 0, CollectFinding, &findings));
    EXPECT_THAT(findings,
                ElementsAre(AllOf(
                    Field(&CheckFinding::path, "<null>"),
                    Field(&CheckFinding::message,
                          "incorrect file size (actual size 1 does not match "
                          "calculated file size 0:5)"))));
  }

  // 5. Truncated files cause OOB read (-1) in integer/text/raw/time fields.
  const TempFile trunc_text_file("coda_check_bin_trunc_text.dat");
  ASSERT_TRUE(trunc_text_file.WriteBytes(std::to_array<uint8_t>({'C', 'O'})));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(trunc_text_file.c_str(), "CheckClass",
                                         "BinRich", 1, product.OutParam()));
    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }

  const TempFile trunc_raw_file("coda_check_bin_trunc_raw.dat");
  ASSERT_TRUE(trunc_raw_file.WriteBytes(
      std::to_array<uint8_t>({'C', 'O', 'D', 'A', 'A'})));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(trunc_raw_file.c_str(), "CheckClass",
                                         "BinRich", 1, product.OutParam()));
    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }

  const TempFile trunc_num_file("coda_check_bin_trunc_num.dat");
  ASSERT_TRUE(trunc_num_file.WriteBytes(
      std::to_array<uint8_t>({'C', 'O', 'D', 'A', 'A', 'B'})));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(trunc_num_file.c_str(), "CheckClass",
                                         "BinRich", 1, product.OutParam()));
    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }

  const TempFile trunc_arr_file("coda_check_bin_trunc_arr.dat");
  ASSERT_TRUE(trunc_arr_file.WriteBytes(
      std::to_array<uint8_t>({'C', 'O', 'D', 'A', 'A', 'B', 2, 99, 0x00})));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(trunc_arr_file.c_str(), "CheckClass",
                                         "BinRich", 1, product.OutParam()));
    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }

  const TempFile trunc_time_file("coda_check_bin_trunc_time.dat");
  ASSERT_TRUE(
      trunc_time_file.WriteBytes(std::to_array<uint8_t>({'2', '0', '2', '0'})));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS,
              coda_open_as(trunc_time_file.c_str(), "CheckClass",
                           "BinTruncTime", 1, product.OutParam()));
    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }

  // 6. Failing field size expression returns -1 from both quick check
  //    (restoring use_fast_size_expressions) and full check.
  const TempFile bad_expr_file("coda_check_bin_bad_expr.dat");
  ASSERT_TRUE(bad_expr_file.WriteText("abc"));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS,
              coda_open_as(bad_expr_file.c_str(), "CheckClass",
                           "BinBadSizeExpr", 1, product.OutParam()));
    std::vector<CheckFinding> findings;
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_fast_size_expressions(1));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 0, CollectFinding, &findings));
    EXPECT_EQ(1, coda_get_option_use_fast_size_expressions());
    EXPECT_EQ(CODA_ERROR_INVALID_FORMAT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_EQ(CODA_ERROR_INVALID_FORMAT, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
  }
}

TEST(CodaCheckTest, BinaryRecordSizeExpressionMatchMismatchAndError) {
  constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="RecSizeClass">
  <cd:ProductType name="Match">
    <cd:ProductDefinition id="RecMatch" format="binary" version="1"/>
  </cd:ProductType>
  <cd:ProductType name="MismatchByte">
    <cd:ProductDefinition id="RecMismatchByte" format="binary" version="1"/>
  </cd:ProductType>
  <cd:ProductType name="MismatchBits">
    <cd:ProductDefinition id="RecMismatchBits" format="binary" version="1"/>
  </cd:ProductType>
  <cd:ProductType name="ExprError">
    <cd:ProductDefinition id="RecExprError" format="binary" version="1"/>
  </cd:ProductType>
</cd:ProductClass>
)xml";

  constexpr std::string_view kRecMatchXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="RecMatch" format="binary">
  <cd:Record>
    <cd:BitSize>16</cd:BitSize>
    <cd:Field name="a"><cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>uint8</cd:NativeType></cd:Integer></cd:Field>
    <cd:Field name="b"><cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>uint8</cd:NativeType></cd:Integer></cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  constexpr std::string_view kRecMismatchByteXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="RecMismatchByte" format="binary">
  <cd:Record>
    <cd:BitSize>24</cd:BitSize>
    <cd:Field name="a"><cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>uint8</cd:NativeType></cd:Integer></cd:Field>
    <cd:Field name="b"><cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>uint8</cd:NativeType></cd:Integer></cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  constexpr std::string_view kRecMismatchBitsXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="RecMismatchBits" format="binary">
  <cd:Record>
    <cd:BitSize>13</cd:BitSize>
    <cd:Field name="a"><cd:Integer><cd:BitSize>5</cd:BitSize><cd:NativeType>uint8</cd:NativeType></cd:Integer></cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  constexpr std::string_view kRecExprErrorXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="RecExprError" format="binary">
  <cd:Record>
    <cd:BitSize>int(str(./tag))</cd:BitSize>
    <cd:Field name="tag">
      <cd:Text><cd:ByteSize>3</cd:ByteSize></cd:Text>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  const std::array<std::pair<std::string_view, std::string_view>, 4> entries = {
      {{"products/RecMatch.xml", kRecMatchXml},
       {"products/RecMismatchByte.xml", kRecMismatchByteXml},
       {"products/RecMismatchBits.xml", kRecMismatchBitsXml},
       {"products/RecExprError.xml", kRecExprErrorXml}},
  };

  ScopedCodadefSession env("coda_check_rec_size");
  ASSERT_NO_FATAL_FAILURE(env.InitWithCodadef(kIndexXml, entries));

  const TempFile two_byte_file("coda_check_rec_two.dat");
  ASSERT_TRUE(two_byte_file.WriteBytes(std::to_array<uint8_t>({1, 2})));

  // 1. Matching record size expression succeeds and preserves fast-size option.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_fast_size_expressions(0));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(two_byte_file.c_str(), "RecSizeClass",
                                         "Match", 1, product.OutParam()));
    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 0, CollectFinding, &findings));
    EXPECT_EQ(0, coda_get_option_use_fast_size_expressions());
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_EQ(0, coda_get_option_use_fast_size_expressions());
    EXPECT_THAT(findings, IsEmpty());
  }

  // 2. Byte-aligned mismatch in record size_expr reports both byte sizes.
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS,
              coda_open_as(two_byte_file.c_str(), "RecSizeClass",
                           "MismatchByte", 1, product.OutParam()));
    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_THAT(
        findings,
        ElementsAre(AllOf(
            Field(&CheckFinding::path, "/"),
            Field(&CheckFinding::message,
                  "invalid result for record size expression (actual record "
                  "size 2 does not match expression result 3)"))));
  }

  // 3. Bit-unaligned mismatch formats both ":5" bit remainders.
  const TempFile one_byte_file("coda_check_rec_one.dat");
  ASSERT_TRUE(one_byte_file.WriteBytes(std::to_array<uint8_t>({0x1f})));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS,
              coda_open_as(one_byte_file.c_str(), "RecSizeClass",
                           "MismatchBits", 1, product.OutParam()));
    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_THAT(
        findings,
        ElementsAre(
            AllOf(Field(&CheckFinding::path, "/"),
                  Field(&CheckFinding::message,
                        "invalid result for record size expression (actual "
                        "record size 0:5 does not match expression result "
                        "1:5)")),
            AllOf(Field(&CheckFinding::path, "<null>"),
                  Field(&CheckFinding::message,
                        "incorrect file size (actual size 1 does not match "
                        "calculated file size 0:5)"))));
  }

  // 4. Failing record size_expr is skipped in quick check (which disables fast
  //    size expressions) and emits a callback finding in full check while
  //    preserving use_fast_size_expressions.
  const TempFile bad_tag_file("coda_check_rec_bad_tag.dat");
  ASSERT_TRUE(bad_tag_file.WriteText("abc"));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(bad_tag_file.c_str(), "RecSizeClass",
                                         "ExprError", 1, product.OutParam()));
    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_fast_size_expressions(1));
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 0, CollectFinding, &findings));
    EXPECT_EQ(1, coda_get_option_use_fast_size_expressions());
    EXPECT_THAT(findings, IsEmpty());

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_EQ(1, coda_get_option_use_fast_size_expressions());
    EXPECT_THAT(findings,
                ElementsAre(AllOf(Field(&CheckFinding::path, "/"),
                                  Field(&CheckFinding::message,
                                        HasSubstr("ascii integer")))));
    coda_set_error(CODA_SUCCESS, nullptr);
  }
}

TEST(CodaCheckTest, AsciiProductNumberFormatErrorsAndLineSeparators) {
  constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="AsciiClass">
  <cd:ProductType name="AsciiNum">
    <cd:ProductDefinition id="AsciiNum" format="ascii" version="1"/>
  </cd:ProductType>
  <cd:ProductType name="AsciiEol">
    <cd:ProductDefinition id="AsciiEol" format="ascii" version="1"/>
  </cd:ProductType>
</cd:ProductClass>
)xml";

  // Include a trailing separator because coda-ascii-cursor.c's
  // get_bit_size_boundary() uses `current_bit_offset + read_bit_size >=
  // bit_size`, so a fixed-size ASCII integer at exact EOF triggers OOB read.
  constexpr std::string_view kAsciiNumXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="AsciiNum" format="ascii">
  <cd:Record>
    <cd:Field name="num">
      <cd:Integer>
        <cd:ByteSize>4</cd:ByteSize>
        <cd:NativeType>int32</cd:NativeType>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="eol"><cd:AsciiLineSeparator/></cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  constexpr std::string_view kAsciiEolXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="AsciiEol" format="ascii">
  <cd:Record>
    <cd:Field name="f1"><cd:Text><cd:ByteSize>2</cd:ByteSize></cd:Text></cd:Field>
    <cd:Field name="eol1"><cd:AsciiLineSeparator/></cd:Field>
    <cd:Field name="f2"><cd:Text><cd:ByteSize>2</cd:ByteSize></cd:Text></cd:Field>
    <cd:Field name="eol2"><cd:AsciiLineSeparator/></cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  const std::array<std::pair<std::string_view, std::string_view>, 2> entries = {
      {{"products/AsciiNum.xml", kAsciiNumXml},
       {"products/AsciiEol.xml", kAsciiEolXml}},
  };

  ScopedCodadefSession env("coda_check_ascii");
  ASSERT_NO_FATAL_FAILURE(env.InitWithCodadef(kIndexXml, entries));

  // 1. Non-numeric ASCII integer triggers CODA_ERROR_INVALID_FORMAT callback.
  const TempFile bad_num_file("coda_check_ascii_badnum.txt");
  ASSERT_TRUE(bad_num_file.WriteText("abcd\n"));
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(bad_num_file.c_str(), "AsciiClass",
                                         "AsciiNum", 1, product.OutParam()));
    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_THAT(findings,
                ElementsAre(AllOf(Field(&CheckFinding::path, "/num"),
                                  Field(&CheckFinding::message,
                                        HasSubstr("ascii integer")))));
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  // 2. LF, CR, and CRLF line separator validation (both valid and mismatched).
  struct EolCase {
    std::string_view label;
    std::string_view content;
    std::string_view expected_message;
  };
  constexpr EolCase kEolCases[] = {
      {.label = "lf_ok", .content = "aa\nbb\n", .expected_message = ""},
      {.label = "lf_bad",
       .content = "aa\nbbX",
       .expected_message = "invalid end of line sequence (expected newline)"},
      {.label = "cr_ok", .content = "aa\rbb\r", .expected_message = ""},
      {.label = "cr_bad",
       .content = "aa\rbbX",
       .expected_message =
           "invalid end of line sequence (expected carriage return)"},
      {.label = "crlf_ok", .content = "aa\r\nbb\r\n", .expected_message = ""},
      {.label = "crlf_bad",
       .content = "aa\r\nbbXX",
       .expected_message =
           "invalid end of line sequence (expected carriage return followed by "
           "newline)"},
  };

  for (const EolCase& tc : kEolCases) {
    SCOPED_TRACE(tc.label);
    const TempFile f(absl::StrCat("coda_check_eol_", tc.label, ".txt"));
    ASSERT_TRUE(f.WriteText(tc.content));
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(f.c_str(), "AsciiClass", "AsciiEol", 1,
                                         product.OutParam()));
    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    if (tc.expected_message.empty()) {
      EXPECT_THAT(findings, IsEmpty());
    } else {
      EXPECT_THAT(findings,
                  ElementsAre(AllOf(
                      Field(&CheckFinding::path, "/eol2"),
                      Field(&CheckFinding::message, tc.expected_message))));
    }
  }
}

TEST(CodaCheckTest, XmlProductWithAttributesAndDefinitionChecksContent) {
  constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="XmlClass">
  <cd:ProductType name="XmlProd">
    <cd:ProductDefinition id="XmlProd" format="xml" version="1"/>
  </cd:ProductType>
</cd:ProductClass>
)xml";

  constexpr std::string_view kXmlProdDef = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="XmlProd" format="xml">
  <cd:Record>
    <cd:Field name="root">
      <cd:Record>
        <cd:Field name="bad_int">
          <cd:Integer format="ascii">
            <cd:NativeType>int32</cd:NativeType>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="bad_time">
          <cd:Time timeformat="time(str(.), &quot;yyyy-MM-dd&quot;)">
            <cd:Text/>
          </cd:Time>
        </cd:Field>
        <cd:Field name="bad_fixed">
          <cd:Text>
            <cd:ByteSize>4</cd:ByteSize>
            <cd:FixedValue>GOOD</cd:FixedValue>
          </cd:Text>
        </cd:Field>
        <cd:Attribute name="attr">
          <cd:Text/>
        </cd:Attribute>
      </cd:Record>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  const std::array<std::pair<std::string_view, std::string_view>, 1> entries = {
      {{"products/XmlProd.xml", kXmlProdDef}},
  };
  ScopedCodadefSession env("coda_check_xml");
  ASSERT_NO_FATAL_FAILURE(env.InitWithCodadef(kIndexXml, entries));

  const TempFile xml_file("coda_check_sample.xml");
  ASSERT_TRUE(
      xml_file.WriteText("<?xml version=\"1.0\"?>\n"
                         "<root attr=\"v\">\n"
                         "  <bad_int>notanint</bad_int>\n"
                         "  <bad_time>bad-date</bad_time>\n"
                         "  <bad_fixed>FAIL</bad_fixed>\n"
                         "</root>\n"));

  // 1. Plain self-describing XML open checks attributes and elements cleanly.
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(xml_file.c_str(), product.OutParam()));
    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 0, CollectFinding, &findings));
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_THAT(findings, IsEmpty());
  }

  // 2. XML opened with .codadef reports bad int, bad datetime, and fixed text.
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(xml_file.c_str(), "XmlClass",
                                         "XmlProd", 1, product.OutParam()));
    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_THAT(
        findings,
        ElementsAre(
            AllOf(Field(&CheckFinding::path, "/root/bad_int"),
                  Field(&CheckFinding::message, HasSubstr("ascii integer"))),
            AllOf(Field(&CheckFinding::path, "/root/bad_time"),
                  Field(&CheckFinding::message,
                        HasSubstr("incorrect year value"))),
            AllOf(Field(&CheckFinding::path, "/root/bad_fixed"),
                  Field(&CheckFinding::message,
                        "string data does not match fixed value"))));
    coda_set_error(CODA_SUCCESS, nullptr);
  }
}

TEST(CodaCheckTest, NetcdfProductCheckedAgainstDefinitionDetectsMismatches) {
  constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="NcClass">
  <cd:ProductType name="NcMatch">
    <cd:ProductDefinition id="NcMatch" format="netcdf" version="1"/>
  </cd:ProductType>
  <cd:ProductType name="NcMismatch">
    <cd:ProductDefinition id="NcMismatch" format="netcdf" version="1"/>
  </cd:ProductType>
</cd:ProductClass>
)xml";

  constexpr std::string_view kNcMatchXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="NcMatch" format="netcdf">
  <cd:Record>
    <cd:Field name="count"><cd:Integer><cd:NativeType>int32</cd:NativeType></cd:Integer></cd:Field>
    <cd:Field name="dyn_arr_ok">
      <cd:Array>
        <cd:Dimension>int(/count) + 1</cd:Dimension>
        <cd:Integer><cd:NativeType>int32</cd:NativeType></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="opt_present">
      <cd:Optional/>
      <cd:Integer><cd:NativeType>int32</cd:NativeType></cd:Integer>
    </cd:Field>
    <cd:Field name="opt_absent_ok">
      <cd:Optional/>
      <cd:Available>int(/count) == 9</cd:Available>
      <cd:Integer><cd:NativeType>int32</cd:NativeType></cd:Integer>
    </cd:Field>
    <cd:Attribute name="gattr">
      <cd:Text>
        <cd:ByteSize>2</cd:ByteSize>
        <cd:FixedValue>ok</cd:FixedValue>
      </cd:Text>
    </cd:Attribute>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  constexpr std::string_view kNcMismatchXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="NcMismatch" format="netcdf">
  <cd:Record>
    <cd:Field name="count"><cd:Integer><cd:NativeType>int32</cd:NativeType></cd:Integer></cd:Field>
    <cd:Field name="wrong_class"><cd:Record/></cd:Field>
    <cd:Field name="wrong_ndims">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Integer><cd:NativeType>int32</cd:NativeType></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="wrong_static_dim">
      <cd:Array>
        <cd:Dimension>4</cd:Dimension>
        <cd:Integer><cd:NativeType>int32</cd:NativeType></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="wrong_expr_dim">
      <cd:Array>
        <cd:Dimension>int(/count) + 5</cd:Dimension>
        <cd:Integer><cd:NativeType>int32</cd:NativeType></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="err_expr_dim">
      <cd:Array>
        <cd:Dimension>int(/missing)</cd:Dimension>
        <cd:Integer><cd:NativeType>int32</cd:NativeType></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="missing_req">
      <cd:Integer><cd:NativeType>int32</cd:NativeType></cd:Integer>
    </cd:Field>
    <cd:Field name="unexpected_avail">
      <cd:Optional/>
      <cd:Available>false</cd:Available>
      <cd:Integer><cd:NativeType>int32</cd:NativeType></cd:Integer>
    </cd:Field>
    <cd:Field name="err_avail_expr">
      <cd:Optional/>
      <cd:Available>int(/missing) == 1</cd:Available>
      <cd:Integer><cd:NativeType>int32</cd:NativeType></cd:Integer>
    </cd:Field>
    <cd:Field name="rn_var" real_name="expected_rn">
      <cd:Integer><cd:NativeType>int32</cd:NativeType></cd:Integer>
    </cd:Field>
    <cd:Field name="wrong_read_type">
      <cd:Integer><cd:NativeType>int16</cd:NativeType></cd:Integer>
    </cd:Field>
    <cd:Field name="bad_attr_parent">
      <cd:Integer><cd:NativeType>int32</cd:NativeType></cd:Integer>
    </cd:Field>
    <cd:Field name="bad_arr_base">
      <cd:Array>
        <cd:Dimension>3</cd:Dimension>
        <cd:Text/>
      </cd:Array>
    </cd:Field>
    <cd:Field name="fixed_attr_var">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:Attribute name="units">
          <cd:Text>
            <cd:ByteSize>4</cd:ByteSize>
            <cd:FixedValue>ABCD</cd:FixedValue>
          </cd:Text>
        </cd:Attribute>
      </cd:Integer>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  const std::array<std::pair<std::string_view, std::string_view>, 2> entries = {
      {{"products/NcMatch.xml", kNcMatchXml},
       {"products/NcMismatch.xml", kNcMismatchXml}},
  };
  ScopedCodadefSession env("coda_check_nc");
  ASSERT_NO_FATAL_FAILURE(env.InitWithCodadef(kIndexXml, entries));

  std::vector<uint8_t> be_int2;
  AppendBeInt32(be_int2, 2);
  std::vector<uint8_t> be_int7;
  AppendBeInt32(be_int7, 7);
  std::vector<uint8_t> be_arr3;
  AppendBeInt32(be_arr3, 1);
  AppendBeInt32(be_arr3, 2);
  AppendBeInt32(be_arr3, 3);
  std::vector<uint8_t> be_arr2x3;
  for (int32_t v = 1; v <= 6; ++v) {
    AppendBeInt32(be_arr2x3, v);
  }

  // 1. Valid NetCDF product matching NcMatch definition produces zero findings.
  {
    NetcdfBuilder nb;
    nb.AddDim({.name = "d3", .size = 3});
    nb.AddGlobalAttr({.name = "gattr",
                      .type = NetcdfType::kChar,
                      .count = 2,
                      .raw_values = {'o', 'k'}});
    nb.AddVar({.name = "count",
               .dim_ids = {},
               .attrs = {},
               .type = NetcdfType::kInt,
               .vsize = 4,
               .data = be_int2});
    nb.AddVar({.name = "dyn_arr_ok",
               .dim_ids = {0},
               .attrs = {},
               .type = NetcdfType::kInt,
               .vsize = 12,
               .data = be_arr3});
    nb.AddVar({.name = "opt_present",
               .dim_ids = {},
               .attrs = {},
               .type = NetcdfType::kInt,
               .vsize = 4,
               .data = be_int7});

    const TempFile nc_file("coda_check_nc_ok.nc");
    ASSERT_TRUE(nc_file.WriteBytes(nb.Build()));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(nc_file.c_str(), "NcClass", "NcMatch",
                                         1, product.OutParam()));
    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_THAT(findings, IsEmpty());
  }

  // 2. Comprehensive NetCDF product against NcMismatch covers all
  //    check_definition mismatch branches and text fixed-value length mismatch.
  {
    NetcdfBuilder nb;
    nb.AddDim({.name = "d3", .size = 3});
    nb.AddDim({.name = "d2", .size = 2});
    nb.AddVar({.name = "count",
               .dim_ids = {},
               .attrs = {},
               .type = NetcdfType::kInt,
               .vsize = 4,
               .data = be_int2});
    nb.AddVar({.name = "wrong_class",
               .dim_ids = {},
               .attrs = {},
               .type = NetcdfType::kInt,
               .vsize = 4,
               .data = be_int2});
    nb.AddVar({.name = "wrong_ndims",
               .dim_ids = {1, 0},
               .attrs = {},
               .type = NetcdfType::kInt,
               .vsize = 24,
               .data = be_arr2x3});
    nb.AddVar({.name = "wrong_static_dim",
               .dim_ids = {0},
               .attrs = {},
               .type = NetcdfType::kInt,
               .vsize = 12,
               .data = be_arr3});
    nb.AddVar({.name = "wrong_expr_dim",
               .dim_ids = {0},
               .attrs = {},
               .type = NetcdfType::kInt,
               .vsize = 12,
               .data = be_arr3});
    nb.AddVar({.name = "err_expr_dim",
               .dim_ids = {0},
               .attrs = {},
               .type = NetcdfType::kInt,
               .vsize = 12,
               .data = be_arr3});
    nb.AddVar({.name = "unexpected_avail",
               .dim_ids = {},
               .attrs = {},
               .type = NetcdfType::kInt,
               .vsize = 4,
               .data = be_int7});
    nb.AddVar({.name = "rn_var",
               .dim_ids = {},
               .attrs = {},
               .type = NetcdfType::kInt,
               .vsize = 4,
               .data = be_int7});
    nb.AddVar({.name = "wrong_read_type",
               .dim_ids = {},
               .attrs = {},
               .type = NetcdfType::kInt,
               .vsize = 4,
               .data = be_int7});
    nb.AddVar({.name = "bad_attr_parent",
               .dim_ids = {},
               .attrs = {{.name = "extra_attr",
                          .type = NetcdfType::kChar,
                          .count = 1,
                          .raw_values = {'x'}}},
               .type = NetcdfType::kInt,
               .vsize = 4,
               .data = be_int7});
    nb.AddVar({.name = "bad_arr_base",
               .dim_ids = {0},
               .attrs = {},
               .type = NetcdfType::kInt,
               .vsize = 12,
               .data = be_arr3});
    nb.AddVar({.name = "fixed_attr_var",
               .dim_ids = {},
               .attrs = {{.name = "units",
                          .type = NetcdfType::kChar,
                          .count = 3,
                          .raw_values = {'a', 'b', 'c'}}},
               .type = NetcdfType::kInt,
               .vsize = 4,
               .data = be_int7});
    nb.AddVar({.name = "extra_in_product",
               .dim_ids = {},
               .attrs = {},
               .type = NetcdfType::kInt,
               .vsize = 4,
               .data = be_int7});

    const TempFile nc_file("coda_check_nc_bad.nc");
    ASSERT_TRUE(nc_file.WriteBytes(nb.Build()));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(nc_file.c_str(), "NcClass",
                                         "NcMismatch", 1, product.OutParam()));
    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));

    EXPECT_THAT(
        findings,
        Contains(AllOf(
            Field(&CheckFinding::path, "/"),
            Field(&CheckFinding::message,
                  "field 'missing_req' availability (unavailable) does not "
                  "match definition (available)"))));
    EXPECT_THAT(
        findings,
        Contains(AllOf(
            Field(&CheckFinding::path, "/"),
            Field(&CheckFinding::message,
                  "field 'unexpected_avail' availability (available) does not "
                  "match definition (unavailable)"))));
    EXPECT_THAT(
        findings,
        Contains(
            AllOf(Field(&CheckFinding::path, "/"),
                  Field(&CheckFinding::message,
                        HasSubstr("while evaluating definition expression for "
                                  "availability of field 'err_avail_expr'")))));
    EXPECT_THAT(
        findings,
        Contains(
            AllOf(Field(&CheckFinding::path, "/"),
                  Field(&CheckFinding::message,
                        "real name for field 'rn_var' (rn_var) does not match "
                        "definition (expected_rn)"))));
    EXPECT_THAT(
        findings,
        Contains(AllOf(
            Field(&CheckFinding::path, "/"),
            Field(&CheckFinding::message,
                  "field 'extra_in_product' availability (available) does not "
                  "match definition (not allowed)"))));
    EXPECT_THAT(
        findings,
        Contains(
            AllOf(Field(&CheckFinding::path, "/wrong_class"),
                  Field(&CheckFinding::message,
                        "type (integer) does not match definition (record)"))));
    EXPECT_THAT(
        findings,
        Contains(AllOf(
            Field(&CheckFinding::path, "/wrong_ndims"),
            Field(&CheckFinding::message,
                  "number of dimensions (2) does not match definition (1)"))));
    EXPECT_THAT(
        findings,
        Contains(
            AllOf(Field(&CheckFinding::path, "/wrong_static_dim"),
                  Field(&CheckFinding::message,
                        "size of dim[0] (3) does not match definition (4)"))));
    EXPECT_THAT(
        findings,
        Contains(
            AllOf(Field(&CheckFinding::path, "/wrong_expr_dim"),
                  Field(&CheckFinding::message,
                        "size of dim[0] (3) does not match definition (7)"))));
    EXPECT_THAT(
        findings,
        Contains(
            AllOf(Field(&CheckFinding::path, "/err_expr_dim"),
                  Field(&CheckFinding::message,
                        HasSubstr("while evaluating definition expression for "
                                  "dimension 0")))));
    EXPECT_THAT(
        findings,
        Contains(AllOf(
            Field(&CheckFinding::path, "/wrong_read_type"),
            Field(&CheckFinding::message,
                  "read type (int32) does not match definition (int16)"))));
    EXPECT_THAT(findings,
                Contains(AllOf(
                    Field(&CheckFinding::path, "/bad_arr_base[0]"),
                    Field(&CheckFinding::message,
                          "type (integer) does not match definition (text)"))));
    EXPECT_THAT(
        findings,
        Contains(AllOf(
            Field(&CheckFinding::path, "/fixed_attr_var@/units"),
            Field(&CheckFinding::message,
                  "string data does not match fixed value (length differs)"))));
    coda_set_error(CODA_SUCCESS, nullptr);
  }
}

TEST(CodaCheckTest, Hdf5ProductArrayFastPathAndDefinitionChecks) {
  constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="H5Class">
  <cd:ProductType name="H5Bad">
    <cd:ProductDefinition id="H5Bad" format="hdf5" version="1"/>
  </cd:ProductType>
</cd:ProductClass>
)xml";

  constexpr std::string_view kH5BadXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="H5Bad" format="hdf5">
  <cd:Record>
    <cd:Field name="int_vec">
      <cd:Array>
        <cd:Dimension>4</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int16</cd:NativeType>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="dbl_vec">
      <cd:Array>
        <cd:Dimension>3</cd:Dimension>
        <cd:Float>
          <cd:NativeType>double</cd:NativeType>
        </cd:Float>
      </cd:Array>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  const std::array<std::pair<std::string_view, std::string_view>, 1> entries = {
      {{"products/H5Bad.xml", kH5BadXml}},
  };
  ScopedCodadefSession env("coda_check_h5");
  ASSERT_NO_FATAL_FAILURE(env.InitWithCodadef(kIndexXml, entries));

  const TempFile h5_file("coda_check_sample.h5");
  {
    ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> int_dims = {4};
    constexpr std::array<int32_t, 4> int_data = {10, 20, 30, 40};
    ScopedH5Space int_space(H5Screate_simple(1, int_dims.data(), nullptr));
    ASSERT_TRUE(int_space.valid());
    ScopedH5Dataset int_dset(H5Dcreate2(file.get(), "int_vec", H5T_NATIVE_INT32,
                                        int_space.get(), H5P_DEFAULT,
                                        H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(int_dset.valid());
    ASSERT_GE(H5Dwrite(int_dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, int_data.data()),
              0);

    ScopedH5Space attr_space(H5Screate(H5S_SCALAR));
    ASSERT_TRUE(attr_space.valid());
    ScopedH5Attr attr(H5Acreate2(int_dset.get(), "tag", H5T_NATIVE_INT32,
                                 attr_space.get(), H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(attr.valid());
    constexpr int32_t tag_val = 7;
    ASSERT_GE(H5Awrite(attr.get(), H5T_NATIVE_INT32, &tag_val), 0);

    constexpr std::array<hsize_t, 1> dbl_dims = {3};
    constexpr std::array<double, 3> dbl_data = {1.5, 2.5, 3.5};
    ScopedH5Space dbl_space(H5Screate_simple(1, dbl_dims.data(), nullptr));
    ASSERT_TRUE(dbl_space.valid());
    ScopedH5Dataset dbl_dset(H5Dcreate2(file.get(), "dbl_vec",
                                        H5T_NATIVE_DOUBLE, dbl_space.get(),
                                        H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(dbl_dset.valid());
    ASSERT_GE(H5Dwrite(dbl_dset.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, dbl_data.data()),
              0);
  }

  // 1. Plain HDF5 open exercises the multi-element HDF5 fast-read path.
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 0, CollectFinding, &findings));
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_THAT(findings, IsEmpty());
  }

  // 2. HDF5 with mismatched .codadef catches element read-type mismatch on the
  //    first element while still executing the bulk array read.
  {
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open_as(h5_file.c_str(), "H5Class", "H5Bad", 1,
                                         product.OutParam()));
    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_THAT(
        findings,
        ElementsAre(
            AllOf(Field(&CheckFinding::path, "/int_vec@"),
                  Field(&CheckFinding::message,
                        "field 'tag' availability (available) does not match "
                        "definition (not allowed)")),
            AllOf(Field(&CheckFinding::path, "/int_vec[0]"),
                  Field(&CheckFinding::message,
                        "read type (int32) does not match definition "
                        "(int16)"))));
  }
}

TEST(CodaCheckTest,
     SyntheticProductsCoverNegativeSizesMemBlockAndSpecialTypes) {
  ScopedCodadefSession env("coda_check_synth");
  ASSERT_NO_FATAL_FAILURE(env.InitEmpty());

  // 1. Unvalidated binary text/raw with bit_size = -1 trigger negative size
  //    findings in check_data.
  {
    std::vector<ScopedType> owned_types;
    coda_type_record* root = coda_type_record_new(coda_format_binary);
    ASSERT_NE(nullptr, root);
    TrackType(root, owned_types);

    coda_type_text* neg_text = coda_type_text_new(coda_format_binary);
    ASSERT_NE(nullptr, neg_text);
    TrackType(neg_text, owned_types);
    coda_type_record_field* f_text = coda_type_record_field_new("neg_text");
    ASSERT_NE(nullptr, f_text);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_field_set_type(
                  f_text, reinterpret_cast<coda_type*>(neg_text)));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_add_field(root, f_text));

    coda_type_raw* neg_raw = coda_type_raw_new(coda_format_binary);
    ASSERT_NE(nullptr, neg_raw);
    TrackType(neg_raw, owned_types);
    coda_type_record_field* f_raw = coda_type_record_field_new("neg_raw");
    ASSERT_NE(nullptr, f_raw);
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                                f_raw, reinterpret_cast<coda_type*>(neg_raw)));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_add_field(root, f_raw));

    auto* bin_prod = static_cast<coda_bin_product*>(
        std::calloc(1, sizeof(coda_bin_product)));
    ASSERT_NE(nullptr, bin_prod);
    bin_prod->format = coda_format_binary;
    bin_prod->fd = -1;
    bin_prod->use_mmap = 0;
    bin_prod->file_size = 0;
    bin_prod->root_type = reinterpret_cast<coda_dynamic_type*>(root);
    ScopedProduct product(reinterpret_cast<coda_product*>(bin_prod));

    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_THAT(findings,
                Contains(AllOf(Field(&CheckFinding::path, "/neg_text"),
                               Field(&CheckFinding::message,
                                     "string length is negative"))));
    EXPECT_THAT(
        findings,
        Contains(AllOf(Field(&CheckFinding::path, "/neg_raw"),
                       Field(&CheckFinding::message, "bit size is negative"))));
  }

  // 2. coda_backend_memory tag_mem_data inside binary product triggers block
  //    size mismatch (both bit-unaligned and byte-aligned). Use a size
  //    expression so definition->bit_size == -1 and coda_mem_data_new()
  //    accepts the 4-byte buffer length.
  for (const int64_t def_bits : {int64_t{12}, int64_t{16}}) {
    std::vector<ScopedType> owned_types;
    coda_type_number* def =
        coda_type_number_new(coda_format_binary, coda_integer_class);
    ASSERT_NE(nullptr, def);
    TrackType(def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_number_set_endianness(def, kHostEndianness));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_read_type(reinterpret_cast<coda_type*>(def),
                                      coda_native_type_uint16));
    coda_expression* bit_size_expr = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string((def_bits == 12) ? "12" : "16",
                                          &bit_size_expr));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_bit_size_expression(
                  reinterpret_cast<coda_type*>(def), bit_size_expr));

    auto* bin_prod = static_cast<coda_bin_product*>(
        std::calloc(1, sizeof(coda_bin_product)));
    ASSERT_NE(nullptr, bin_prod);
    bin_prod->format = coda_format_binary;
    bin_prod->fd = -1;
    bin_prod->use_mmap = 0;
    bin_prod->file_size = 4;
    ScopedProduct product(reinterpret_cast<coda_product*>(bin_prod));

    constexpr std::array<uint8_t, 4> buf = {0x01, 0x02, 0x03, 0x04};
    coda_mem_data* mem_data =
        coda_mem_data_new(reinterpret_cast<coda_type*>(def), nullptr,
                          product.get(), 4, buf.data());
    ASSERT_NE(nullptr, mem_data);
    bin_prod->root_type = reinterpret_cast<coda_dynamic_type*>(mem_data);
    const absl::Cleanup free_mem_prod = [&] {
      coda_dynamic_type_delete(bin_prod->root_type);
      bin_prod->root_type = nullptr;
      std::free(const_cast<uint8_t*>(bin_prod->mem_ptr));
      bin_prod->mem_ptr = nullptr;
    };

    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    if (def_bits == 12) {
      EXPECT_THAT(
          findings,
          Contains(
              AllOf(Field(&CheckFinding::path, "/"),
                    Field(&CheckFinding::message,
                          "incorrect block size (actual size 4 does not match "
                          "calculated size 1:4)"))));
    } else {
      EXPECT_THAT(
          findings,
          Contains(
              AllOf(Field(&CheckFinding::path, "/"),
                    Field(&CheckFinding::message,
                          "incorrect block size (actual size 4 does not match "
                          "calculated size 2)"))));
    }
  }

  // 3. Synthetic sp3 product with a coda_product_definition covers:
  //    - special_type != definition_special_type and matching special_type
  //    - coda_raw_class fixed value length mismatch & match under !size_check
  //    - coda_text_class & coda_raw_class size_expr CODA_ERROR_PRODUCT vs
  //      CODA_ERROR_EXPRESSION error paths under !size_check.
  {
    std::vector<ScopedType> owned_types;

    auto* sp3_prod =
        static_cast<coda_product*>(std::calloc(1, sizeof(coda_product)));
    ASSERT_NE(nullptr, sp3_prod);
    sp3_prod->format = coda_format_sp3;
    ScopedProduct product(sp3_prod);

    coda_type_record* prod_rec_def = coda_type_record_new(coda_format_sp3);
    ASSERT_NE(nullptr, prod_rec_def);
    TrackType(prod_rec_def, owned_types);
    coda_mem_record* prod_rec = coda_mem_record_new(prod_rec_def, nullptr);
    ASSERT_NE(nullptr, prod_rec);
    sp3_prod->root_type = reinterpret_cast<coda_dynamic_type*>(prod_rec);

    // Build product definition record.
    coda_type_record* def_root = coda_type_record_new(coda_format_sp3);
    ASSERT_NE(nullptr, def_root);
    TrackType(def_root, owned_types);
    coda_product_definition* prod_def =
        coda_product_definition_new("SynthSp3", coda_format_sp3, 1);
    ASSERT_NE(nullptr, prod_def);
    const absl::Cleanup free_prod_def = [&] {
      sp3_prod->product_definition = nullptr;
      coda_product_definition_delete(prod_def);
    };
    ASSERT_EQ(CODA_SUCCESS,
              coda_product_definition_set_root_type(
                  prod_def, reinterpret_cast<coda_type*>(def_root)));
    sp3_prod->product_definition = prod_def;

    auto add_def_field = [&](const char* name, coda_type* ftype) {
      coda_type_record_field* field = coda_type_record_field_new(name);
      ASSERT_NE(nullptr, field);
      ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(field, ftype));
      ASSERT_EQ(CODA_SUCCESS, coda_type_record_add_field(def_root, field));
    };

    // Helper time type used in product for t1 and t2.
    coda_type_text* time_str_def = coda_type_text_new(coda_format_sp3);
    ASSERT_NE(nullptr, time_str_def);
    TrackType(time_str_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS, coda_type_set_read_type(
                                reinterpret_cast<coda_type*>(time_str_def),
                                coda_native_type_string));
    coda_expression* time_expr = nullptr;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string(
                                "time(str(.),\"yyyy-MM-dd\")", &time_expr));
    coda_type_special* prod_time_def =
        coda_type_time_new(coda_format_sp3, time_expr);
    ASSERT_NE(nullptr, prod_time_def);
    TrackType(prod_time_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_time_set_base_type(
                  prod_time_def, reinterpret_cast<coda_type*>(time_str_def)));

    // t1: product is time, definition is complex -> special_type mismatch.
    coda_mem_data* t1_base =
        coda_mem_string_new(time_str_def, nullptr, sp3_prod, "2020-01-01");
    ASSERT_NE(nullptr, t1_base);
    coda_mem_special* t1_val = coda_mem_time_new(
        prod_time_def, nullptr, reinterpret_cast<coda_dynamic_type*>(t1_base));
    ASSERT_NE(nullptr, t1_val);
    ASSERT_EQ(
        CODA_SUCCESS,
        coda_mem_record_add_field(
            prod_rec, "t1", reinterpret_cast<coda_dynamic_type*>(t1_val), 1));

    coda_type_special* def_cplx = coda_type_complex_new(coda_format_sp3);
    ASSERT_NE(nullptr, def_cplx);
    TrackType(def_cplx, owned_types);
    coda_type_number* def_cplx_flt =
        coda_type_number_new(coda_format_sp3, coda_real_class);
    ASSERT_NE(nullptr, def_cplx_flt);
    TrackType(def_cplx_flt, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_complex_set_type(
                  def_cplx, reinterpret_cast<coda_type*>(def_cplx_flt)));
    add_def_field("t1", reinterpret_cast<coda_type*>(def_cplx));

    // t2: product is time, definition is time -> special_type match.
    coda_mem_data* t2_base =
        coda_mem_string_new(time_str_def, nullptr, sp3_prod, "2020-01-02");
    ASSERT_NE(nullptr, t2_base);
    coda_mem_special* t2_val = coda_mem_time_new(
        prod_time_def, nullptr, reinterpret_cast<coda_dynamic_type*>(t2_base));
    ASSERT_NE(nullptr, t2_val);
    ASSERT_EQ(
        CODA_SUCCESS,
        coda_mem_record_add_field(
            prod_rec, "t2", reinterpret_cast<coda_dynamic_type*>(t2_val), 1));
    add_def_field("t2", reinterpret_cast<coda_type*>(prod_time_def));

    // r_len_diff: product raw length 2, definition fixed raw length 4.
    coda_type_raw* prod_raw_def = coda_type_raw_new(coda_format_sp3);
    ASSERT_NE(nullptr, prod_raw_def);
    TrackType(prod_raw_def, owned_types);
    constexpr std::array<uint8_t, 2> raw2 = {0x01, 0x02};
    coda_mem_data* r_len_diff =
        coda_mem_raw_new(prod_raw_def, nullptr, sp3_prod, 2, raw2.data());
    ASSERT_NE(nullptr, r_len_diff);
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(
                  prod_rec, "r_len_diff",
                  reinterpret_cast<coda_dynamic_type*>(r_len_diff), 1));

    coda_type_raw* def_raw_fixed = coda_type_raw_new(coda_format_sp3);
    ASSERT_NE(nullptr, def_raw_fixed);
    TrackType(def_raw_fixed, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_bit_size(
                  reinterpret_cast<coda_type*>(def_raw_fixed), 32));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_raw_set_fixed_value(def_raw_fixed, 4, "ABCD"));
    add_def_field("r_len_diff", reinterpret_cast<coda_type*>(def_raw_fixed));

    // r_ok: product raw length 4 matching fixed value "ABCD".
    constexpr std::array<uint8_t, 4> raw4 = {'A', 'B', 'C', 'D'};
    coda_mem_data* r_ok =
        coda_mem_raw_new(prod_raw_def, nullptr, sp3_prod, 4, raw4.data());
    ASSERT_NE(nullptr, r_ok);
    ASSERT_EQ(CODA_SUCCESS, coda_mem_record_add_field(
                                prod_rec, "r_ok",
                                reinterpret_cast<coda_dynamic_type*>(r_ok), 1));
    add_def_field("r_ok", reinterpret_cast<coda_type*>(def_raw_fixed));

    // neg_expr_text & neg_expr_raw: size_expr evaluating to -1 triggers
    // CODA_ERROR_PRODUCT inside check_data (!size_check) and invokes callback.
    coda_type_text* neg_expr_text_def = coda_type_text_new(coda_format_sp3);
    ASSERT_NE(nullptr, neg_expr_text_def);
    TrackType(neg_expr_text_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS, coda_type_set_read_type(
                                reinterpret_cast<coda_type*>(neg_expr_text_def),
                                coda_native_type_string));
    coda_expression* neg_expr1 = nullptr;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("-1", &neg_expr1));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_byte_size_expression(
                  reinterpret_cast<coda_type*>(neg_expr_text_def), neg_expr1));
    coda_mem_data* neg_expr_text_val =
        coda_mem_string_new(neg_expr_text_def, nullptr, sp3_prod, "x");
    ASSERT_NE(nullptr, neg_expr_text_val);
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(
                  prod_rec, "neg_expr_text",
                  reinterpret_cast<coda_dynamic_type*>(neg_expr_text_val), 1));
    add_def_field("neg_expr_text",
                  reinterpret_cast<coda_type*>(neg_expr_text_def));

    coda_type_raw* neg_expr_raw_def = coda_type_raw_new(coda_format_sp3);
    ASSERT_NE(nullptr, neg_expr_raw_def);
    TrackType(neg_expr_raw_def, owned_types);
    coda_expression* neg_expr2 = nullptr;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("-1", &neg_expr2));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_byte_size_expression(
                  reinterpret_cast<coda_type*>(neg_expr_raw_def), neg_expr2));
    coda_mem_data* neg_expr_raw_val =
        coda_mem_raw_new(neg_expr_raw_def, nullptr, sp3_prod, 1, raw2.data());
    ASSERT_NE(nullptr, neg_expr_raw_val);
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(
                  prod_rec, "neg_expr_raw",
                  reinterpret_cast<coda_dynamic_type*>(neg_expr_raw_val), 1));
    add_def_field("neg_expr_raw",
                  reinterpret_cast<coda_type*>(neg_expr_raw_def));

    std::vector<CheckFinding> findings;
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_THAT(
        findings,
        ElementsAre(
            AllOf(Field(&CheckFinding::path, "/t1"),
                  Field(&CheckFinding::message,
                        "special type (time) does not match definition "
                        "(complex)")),
            AllOf(Field(&CheckFinding::path, "/r_len_diff"),
                  Field(&CheckFinding::message,
                        "data does not match fixed value (length differs)")),
            AllOf(Field(&CheckFinding::path, "/neg_expr_text"),
                  Field(&CheckFinding::message,
                        HasSubstr("calculated size is negative"))),
            AllOf(Field(&CheckFinding::path, "/neg_expr_raw"),
                  Field(&CheckFinding::message,
                        HasSubstr("calculated size is negative")))));

    // Add a raw field with a failing size expression (non-product error) and
    // confirm coda_product_check returns -1.
    coda_type_raw* bad_expr_raw_def = coda_type_raw_new(coda_format_sp3);
    ASSERT_NE(nullptr, bad_expr_raw_def);
    TrackType(bad_expr_raw_def, owned_types);
    coda_expression* bad_expr = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("int(/missing)", &bad_expr));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_byte_size_expression(
                  reinterpret_cast<coda_type*>(bad_expr_raw_def), bad_expr));
    coda_mem_data* bad_expr_raw_val =
        coda_mem_raw_new(bad_expr_raw_def, nullptr, sp3_prod, 1, raw2.data());
    ASSERT_NE(nullptr, bad_expr_raw_val);
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(
                  prod_rec, "bad_expr_raw",
                  reinterpret_cast<coda_dynamic_type*>(bad_expr_raw_val), 1));
    add_def_field("bad_expr_raw",
                  reinterpret_cast<coda_type*>(bad_expr_raw_def));

    findings.clear();
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_EQ(CODA_ERROR_INVALID_NAME, coda_errno);
  }

  // 4. Synthetic sp3 text field with failing size expression (non-product
  // error)
  //    returns -1 from check_data.
  {
    std::vector<ScopedType> owned_types;
    auto* sp3_prod =
        static_cast<coda_product*>(std::calloc(1, sizeof(coda_product)));
    ASSERT_NE(nullptr, sp3_prod);
    sp3_prod->format = coda_format_sp3;
    ScopedProduct product(sp3_prod);

    coda_type_text* bad_text_def = coda_type_text_new(coda_format_sp3);
    ASSERT_NE(nullptr, bad_text_def);
    TrackType(bad_text_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS, coda_type_set_read_type(
                                reinterpret_cast<coda_type*>(bad_text_def),
                                coda_native_type_string));
    coda_expression* bad_expr = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("int(/missing)", &bad_expr));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_byte_size_expression(
                  reinterpret_cast<coda_type*>(bad_text_def), bad_expr));
    coda_mem_data* bad_text_val =
        coda_mem_string_new(bad_text_def, nullptr, sp3_prod, "x");
    ASSERT_NE(nullptr, bad_text_val);
    sp3_prod->root_type = reinterpret_cast<coda_dynamic_type*>(bad_text_val);

    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  }
}

TEST(CodaCheckTest, BinaryEdgeErrorBranchesAndSpecialBaseCheck) {
  constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="EdgeClass">
  <cd:ProductType name="BinBadArrDim">
    <cd:ProductDefinition id="BinBadArrDim" format="binary" version="1"/>
  </cd:ProductType>
  <cd:ProductType name="BinBadElemSize">
    <cd:ProductDefinition id="BinBadElemSize" format="binary" version="1"/>
  </cd:ProductType>
  <cd:ProductType name="BinBadAvail0">
    <cd:ProductDefinition id="BinBadAvail0" format="binary" version="1"/>
  </cd:ProductType>
  <cd:ProductType name="BinBadAvail1">
    <cd:ProductDefinition id="BinBadAvail1" format="binary" version="1"/>
  </cd:ProductType>
  <cd:ProductType name="BinTruncComplex">
    <cd:ProductDefinition id="BinTruncComplex" format="binary" version="1"/>
  </cd:ProductType>
  <cd:ProductType name="BinMaxDepthArr">
    <cd:ProductDefinition id="BinMaxDepthArr" format="binary" version="1"/>
  </cd:ProductType>
</cd:ProductClass>
)xml";

  constexpr std::string_view kBinBadArrDimXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinBadArrDim" format="binary">
  <cd:Record>
    <cd:Field name="tag"><cd:Text><cd:ByteSize>3</cd:ByteSize></cd:Text></cd:Field>
    <cd:Field name="arr">
      <cd:Array>
        <cd:Dimension>int(str(/tag))</cd:Dimension>
        <cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>uint8</cd:NativeType></cd:Integer>
      </cd:Array>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  constexpr std::string_view kBinBadElemSizeXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinBadElemSize" format="binary">
  <cd:Record>
    <cd:Field name="tag"><cd:Text><cd:ByteSize>3</cd:ByteSize></cd:Text></cd:Field>
    <cd:Field name="arr">
      <cd:Array>
        <cd:Dimension>1</cd:Dimension>
        <cd:Record>
          <cd:Field name="dyn"><cd:Text><cd:ByteSize>int(str(/tag))</cd:ByteSize></cd:Text></cd:Field>
        </cd:Record>
      </cd:Array>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  constexpr std::string_view kBinBadAvail0Xml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinBadAvail0" format="binary">
  <cd:Record>
    <cd:Field name="a">
      <cd:Available>int(/missing) == 1</cd:Available>
      <cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>uint8</cd:NativeType></cd:Integer>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  constexpr std::string_view kBinBadAvail1Xml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinBadAvail1" format="binary">
  <cd:Record>
    <cd:Field name="a"><cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>uint8</cd:NativeType></cd:Integer></cd:Field>
    <cd:Field name="b">
      <cd:Available>int(/missing) == 1</cd:Available>
      <cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>uint8</cd:NativeType></cd:Integer>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  constexpr std::string_view kBinTruncComplexXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinTruncComplex" format="binary">
  <cd:Complex>
    <cd:Float><cd:BitSize>32</cd:BitSize><cd:NativeType>float</cd:NativeType><cd:LittleEndian/></cd:Float>
  </cd:Complex>
</cd:ProductDefinition>
)xml";

  // 31 nested records place the leaf array at cursor depth CODA_CURSOR_MAXDEPTH
  // (32), causing coda_cursor_goto_first_array_element to fail.
  std::string max_depth_xml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinMaxDepthArr" format="binary">
)xml";
  for (int i = 0; i < CODA_CURSOR_MAXDEPTH - 1; ++i) {
    max_depth_xml += "<cd:Record><cd:Field name=\"c\">\n";
  }
  max_depth_xml += R"xml(<cd:Array>
  <cd:Dimension>1</cd:Dimension>
  <cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>uint8</cd:NativeType></cd:Integer>
</cd:Array>
)xml";
  for (int i = 0; i < CODA_CURSOR_MAXDEPTH - 1; ++i) {
    max_depth_xml += "</cd:Field></cd:Record>\n";
  }
  max_depth_xml += "</cd:ProductDefinition>\n";

  const std::array<std::pair<std::string_view, std::string_view>, 6> entries = {
      {{"products/BinBadArrDim.xml", kBinBadArrDimXml},
       {"products/BinBadElemSize.xml", kBinBadElemSizeXml},
       {"products/BinBadAvail0.xml", kBinBadAvail0Xml},
       {"products/BinBadAvail1.xml", kBinBadAvail1Xml},
       {"products/BinTruncComplex.xml", kBinTruncComplexXml},
       {"products/BinMaxDepthArr.xml", max_depth_xml}},
  };
  ScopedCodadefSession env("coda_check_bin_edge");
  ASSERT_NO_FATAL_FAILURE(env.InitWithCodadef(kIndexXml, entries));

  struct BinaryFailureCase {
    const char* product_type;
    std::string_view bytes;
    int expected_errno;
  };
  constexpr BinaryFailureCase kCases[] = {
      {.product_type = "BinBadArrDim",
       .bytes = "abc",
       .expected_errno = CODA_ERROR_INVALID_FORMAT},
      {.product_type = "BinBadElemSize",
       .bytes = "abc",
       .expected_errno = CODA_ERROR_INVALID_FORMAT},
      {.product_type = "BinBadAvail0",
       .bytes = "a",
       .expected_errno = CODA_ERROR_INVALID_NAME},
      {.product_type = "BinBadAvail1",
       .bytes = "ab",
       .expected_errno = CODA_ERROR_INVALID_NAME},
      {.product_type = "BinTruncComplex",
       .bytes = "ab",
       .expected_errno = CODA_ERROR_OUT_OF_BOUNDS_READ},
      {.product_type = "BinMaxDepthArr",
       .bytes = "a",
       .expected_errno = CODA_ERROR_INVALID_ARGUMENT},
  };

  for (const BinaryFailureCase& tc : kCases) {
    SCOPED_TRACE(tc.product_type);
    const TempFile f(absl::StrCat("coda_check_edge_", tc.product_type, ".dat"));
    ASSERT_TRUE(f.WriteText(tc.bytes));
    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS,
              coda_open_as(f.c_str(), "EdgeClass", tc.product_type, 1,
                           product.OutParam()));
    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_EQ(tc.expected_errno, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
  }
}

TEST(CodaCheckTest, Hdf5CorruptChunkReadReturnsHdf5Error) {
  ScopedCodadefSession env("coda_check_h5_corrupt");
  ASSERT_NO_FATAL_FAILURE(env.InitEmpty());

  const TempFile h5_file("coda_check_corrupt_chunk.h5");
  {
    ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());

    constexpr std::array<hsize_t, 1> dims = {4};
    constexpr std::array<int32_t, 4> data = {0x44444444, 0x55555555, 0x66666666,
                                             0x77777777};
    ScopedH5Plist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_chunk(dcpl.get(), 1, dims.data()), 0);
    ASSERT_GE(H5Pset_fletcher32(dcpl.get()), 0);

    ScopedH5Space space(H5Screate_simple(1, dims.data(), nullptr));
    ASSERT_TRUE(space.valid());
    ScopedH5Dataset dset(H5Dcreate2(file.get(), "chk", H5T_NATIVE_INT32,
                                    space.get(), H5P_DEFAULT, dcpl.get(),
                                    H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
    ASSERT_GE(H5Dwrite(dset.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, data.data()),
              0);
    ASSERT_GE(H5Fflush(file.get(), H5F_SCOPE_LOCAL), 0);

    // Corrupt one payload byte in the raw chunk without updating its Fletcher32
    // checksum so coda_open succeeds while H5Dread fails with CODA_ERROR_HDF5.
    constexpr std::array<hsize_t, 1> origin = {0};
    hsize_t raw_size = 0;
    ASSERT_GE(H5Dget_chunk_storage_size(dset.get(), origin.data(), &raw_size),
              0);
    ASSERT_EQ(dims[0] * sizeof(int32_t) + 4, raw_size);

    std::vector<uint8_t> raw_chunk(static_cast<size_t>(raw_size));
    size_t buf_size = raw_chunk.size();
    uint32_t filter_mask = 0;
    ASSERT_GE(H5Dread_chunk2(dset.get(), H5P_DEFAULT, origin.data(),
                             &filter_mask, raw_chunk.data(), &buf_size),
              0);
    raw_chunk[0] ^= 0x01;
    ASSERT_GE(H5Dwrite_chunk(dset.get(), H5P_DEFAULT, filter_mask,
                             origin.data(), raw_chunk.size(), raw_chunk.data()),
              0);
  }

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  std::vector<CheckFinding> findings;
  coda_set_error(CODA_SUCCESS, nullptr);
  int rc = 0;
  {
    // Upstream coda-check.c:685 returns -1 without freeing `values`.
    const absl::LeakCheckDisabler leak_disabler;
    rc = coda_product_check(product.get(), 1, CollectFinding, &findings);
  }
  EXPECT_EQ(-1, rc);
  EXPECT_EQ(CODA_ERROR_HDF5, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
}

TEST(CodaCheckTest, SyntheticEdgeCasesCoverRemainingCheckDataErrors) {
  ScopedCodadefSession env("coda_check_synth_edges");
  ASSERT_NO_FATAL_FAILURE(env.InitEmpty());

  auto make_sp3_product = []() -> coda_product* {
    auto* p = static_cast<coda_product*>(std::calloc(1, sizeof(coda_product)));
    if (p != nullptr) {
      p->format = coda_format_sp3;
    }
    return p;
  };

  auto wrap_in_records = [](coda_dynamic_type* inner, int depth,
                            std::vector<ScopedType>& owned_types,
                            coda_format fmt = coda_format_sp3) {
    coda_dynamic_type* cur = inner;
    for (int i = 0; i < depth; ++i) {
      coda_type_record* rec_def = coda_type_record_new(fmt);
      if (rec_def == nullptr) {
        coda_dynamic_type_delete(cur);
        return static_cast<coda_dynamic_type*>(nullptr);
      }
      TrackType(rec_def, owned_types);
      coda_mem_record* rec = coda_mem_record_new(rec_def, nullptr);
      if (rec == nullptr) {
        coda_dynamic_type_delete(cur);
        return static_cast<coda_dynamic_type*>(nullptr);
      }
      if (coda_mem_record_add_field(rec, "c", cur, 1) != CODA_SUCCESS) {
        coda_dynamic_type_delete(cur);
        coda_dynamic_type_delete(reinterpret_cast<coda_dynamic_type*>(rec));
        return static_cast<coda_dynamic_type*>(nullptr);
      }
      cur = reinterpret_cast<coda_dynamic_type*>(rec);
    }
    return cur;
  };

  // 1. Null root_type causes coda_cursor_get_type failure in check_data.
  {
    ScopedProduct product(make_sp3_product());
    ASSERT_TRUE(product);
    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  }

  // 2. Failing dim_expr in check_definition (coda_array_class) returns -1.
  {
    std::vector<ScopedType> owned_types;
    ScopedProduct product(make_sp3_product());
    ASSERT_TRUE(product);

    coda_type_number* u8_def =
        coda_type_number_new(coda_format_sp3, coda_integer_class);
    ASSERT_NE(nullptr, u8_def);
    TrackType(u8_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_read_type(reinterpret_cast<coda_type*>(u8_def),
                                      coda_native_type_uint8));

    coda_type_array* prod_arr_def = coda_type_array_new(coda_format_sp3);
    ASSERT_NE(nullptr, prod_arr_def);
    TrackType(prod_arr_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_array_set_base_type(
                  prod_arr_def, reinterpret_cast<coda_type*>(u8_def)));
    coda_expression* bad_dim = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("int(/missing)", &bad_dim));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_array_add_variable_dimension(prod_arr_def, bad_dim));

    coda_mem_data* mem_arr =
        coda_mem_data_new(reinterpret_cast<coda_type*>(prod_arr_def), nullptr,
                          product.get(), 0, nullptr);
    ASSERT_NE(nullptr, mem_arr);
    product.get()->root_type = reinterpret_cast<coda_dynamic_type*>(mem_arr);

    coda_type_array* def_arr = coda_type_array_new(coda_format_sp3);
    ASSERT_NE(nullptr, def_arr);
    TrackType(def_arr, owned_types);
    ASSERT_EQ(CODA_SUCCESS, coda_type_array_set_base_type(
                                def_arr, reinterpret_cast<coda_type*>(u8_def)));
    ASSERT_EQ(CODA_SUCCESS, coda_type_array_add_fixed_dimension(def_arr, 1));

    coda_product_definition* prod_def =
        coda_product_definition_new("BadArrDef", coda_format_sp3, 1);
    ASSERT_NE(nullptr, prod_def);
    const absl::Cleanup free_prod_def = [&] {
      product.get()->product_definition = nullptr;
      coda_product_definition_delete(prod_def);
    };
    ASSERT_EQ(CODA_SUCCESS,
              coda_product_definition_set_root_type(
                  prod_def, reinterpret_cast<coda_type*>(def_arr)));
    product.get()->product_definition = prod_def;

    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 0, CollectFinding, &findings));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  }

  // 3. Failing available_expr in check_definition (coda_record_class) returns
  //    -1.
  {
    std::vector<ScopedType> owned_types;
    ScopedProduct product(make_sp3_product());
    ASSERT_TRUE(product);

    coda_type_number* u8_def =
        coda_type_number_new(coda_format_sp3, coda_integer_class);
    ASSERT_NE(nullptr, u8_def);
    TrackType(u8_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_read_type(reinterpret_cast<coda_type*>(u8_def),
                                      coda_native_type_uint8));

    coda_type_record* prod_rec_def = coda_type_record_new(coda_format_sp3);
    ASSERT_NE(nullptr, prod_rec_def);
    TrackType(prod_rec_def, owned_types);
    coda_type_record_field* f = coda_type_record_field_new("f");
    ASSERT_NE(nullptr, f);
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                                f, reinterpret_cast<coda_type*>(u8_def)));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_optional(f));
    coda_expression* bad_avail = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("int(/missing) == 1", &bad_avail));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_field_set_available_expression(f, bad_avail));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_add_field(prod_rec_def, f));

    coda_mem_data* mem_rec =
        coda_mem_data_new(reinterpret_cast<coda_type*>(prod_rec_def), nullptr,
                          product.get(), 0, nullptr);
    ASSERT_NE(nullptr, mem_rec);
    product.get()->root_type = reinterpret_cast<coda_dynamic_type*>(mem_rec);

    coda_type_record* def_rec = coda_type_record_new(coda_format_sp3);
    ASSERT_NE(nullptr, def_rec);
    TrackType(def_rec, owned_types);
    coda_type_record_field* df = coda_type_record_field_new("f");
    ASSERT_NE(nullptr, df);
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                                df, reinterpret_cast<coda_type*>(u8_def)));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_add_field(def_rec, df));

    coda_product_definition* prod_def =
        coda_product_definition_new("BadRecDef", coda_format_sp3, 1);
    ASSERT_NE(nullptr, prod_def);
    const absl::Cleanup free_prod_def = [&] {
      product.get()->product_definition = nullptr;
      coda_product_definition_delete(prod_def);
    };
    ASSERT_EQ(CODA_SUCCESS,
              coda_product_definition_set_root_type(
                  prod_def, reinterpret_cast<coda_type*>(def_rec)));
    product.get()->product_definition = prod_def;

    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 0, CollectFinding, &findings));
    EXPECT_EQ(CODA_ERROR_INVALID_NAME, coda_errno);
  }

  // 4. Attribute sub-element returning -1 propagates through check_data.
  {
    std::vector<ScopedType> owned_types;
    ScopedProduct product(make_sp3_product());
    ASSERT_TRUE(product);

    coda_type_raw* bad_raw_def = coda_type_raw_new(coda_format_sp3);
    ASSERT_NE(nullptr, bad_raw_def);
    TrackType(bad_raw_def, owned_types);
    coda_expression* bad_expr = nullptr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("int(/missing)", &bad_expr));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_byte_size_expression(
                  reinterpret_cast<coda_type*>(bad_raw_def), bad_expr));

    coda_type_record* attrs_def = coda_type_record_new(coda_format_sp3);
    ASSERT_NE(nullptr, attrs_def);
    TrackType(attrs_def, owned_types);
    coda_type_record_field* af = coda_type_record_field_new("bad_attr");
    ASSERT_NE(nullptr, af);
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                                af, reinterpret_cast<coda_type*>(bad_raw_def)));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_add_field(attrs_def, af));

    coda_type_record* root_def = coda_type_record_new(coda_format_sp3);
    ASSERT_NE(nullptr, root_def);
    TrackType(root_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_attributes(reinterpret_cast<coda_type*>(root_def),
                                       attrs_def));

    constexpr std::array<uint8_t, 1> raw1 = {'x'};
    coda_mem_data* bad_raw_val =
        coda_mem_raw_new(bad_raw_def, nullptr, product.get(), 1, raw1.data());
    ASSERT_NE(nullptr, bad_raw_val);
    coda_mem_record* attrs = coda_mem_record_new(attrs_def, nullptr);
    ASSERT_NE(nullptr, attrs);
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(
                  attrs, "bad_attr",
                  reinterpret_cast<coda_dynamic_type*>(bad_raw_val), 0));

    coda_mem_record* root = coda_mem_record_new(
        root_def, reinterpret_cast<coda_dynamic_type*>(attrs));
    ASSERT_NE(nullptr, root);
    product.get()->root_type = reinterpret_cast<coda_dynamic_type*>(root);

    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_EQ(CODA_ERROR_INVALID_NAME, coda_errno);
  }

  // 5. Leaf at CODA_CURSOR_MAXDEPTH with attributes fails
  //    coda_cursor_goto_attributes.
  {
    std::vector<ScopedType> owned_types;
    ScopedProduct product(make_sp3_product());
    ASSERT_TRUE(product);

    coda_type_record* leaf_attr_def = coda_type_record_new(coda_format_sp3);
    ASSERT_NE(nullptr, leaf_attr_def);
    TrackType(leaf_attr_def, owned_types);
    coda_type_text* leaf_def = coda_type_text_new(coda_format_sp3);
    ASSERT_NE(nullptr, leaf_def);
    TrackType(leaf_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_read_type(reinterpret_cast<coda_type*>(leaf_def),
                                      coda_native_type_string));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_attributes(reinterpret_cast<coda_type*>(leaf_def),
                                       leaf_attr_def));

    coda_mem_record* leaf_attrs = coda_mem_record_new(leaf_attr_def, nullptr);
    ASSERT_NE(nullptr, leaf_attrs);
    coda_mem_data* leaf = coda_mem_string_new(
        leaf_def, reinterpret_cast<coda_dynamic_type*>(leaf_attrs),
        product.get(), "ok");
    ASSERT_NE(nullptr, leaf);

    coda_dynamic_type* wrapped =
        wrap_in_records(reinterpret_cast<coda_dynamic_type*>(leaf),
                        CODA_CURSOR_MAXDEPTH - 1, owned_types);
    ASSERT_NE(nullptr, wrapped);
    product.get()->root_type = wrapped;

    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  }

  // 6. HDF5-format array whose first element is at CODA_CURSOR_MAXDEPTH with
  //    attributes fails inside the read_check=0 element check_data call.
  {
    std::vector<ScopedType> owned_types;
    ScopedProduct product(make_sp3_product());
    ASSERT_TRUE(product);

    coda_type_record* elem_attr_def = coda_type_record_new(coda_format_hdf5);
    ASSERT_NE(nullptr, elem_attr_def);
    TrackType(elem_attr_def, owned_types);
    coda_type_number* h5_int_def =
        coda_type_number_new(coda_format_hdf5, coda_integer_class);
    ASSERT_NE(nullptr, h5_int_def);
    TrackType(h5_int_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_read_type(reinterpret_cast<coda_type*>(h5_int_def),
                                      coda_native_type_int32));
    ASSERT_EQ(CODA_SUCCESS, coda_type_set_bit_size(
                                reinterpret_cast<coda_type*>(h5_int_def), 32));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_attributes(reinterpret_cast<coda_type*>(h5_int_def),
                                       elem_attr_def));

    coda_type_array* h5_arr_def = coda_type_array_new(coda_format_hdf5);
    ASSERT_NE(nullptr, h5_arr_def);
    TrackType(h5_arr_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_array_set_base_type(
                  h5_arr_def, reinterpret_cast<coda_type*>(h5_int_def)));
    ASSERT_EQ(CODA_SUCCESS, coda_type_array_add_fixed_dimension(h5_arr_def, 2));

    coda_mem_array* mem_arr = coda_mem_array_new(h5_arr_def, nullptr);
    ASSERT_NE(nullptr, mem_arr);
    for (int i = 0; i < 2; ++i) {
      coda_mem_record* e_attrs = coda_mem_record_new(elem_attr_def, nullptr);
      ASSERT_NE(nullptr, e_attrs);
      coda_mem_data* elem = coda_mem_int32_new(
          h5_int_def, reinterpret_cast<coda_dynamic_type*>(e_attrs),
          product.get(), i);
      ASSERT_NE(nullptr, elem);
      ASSERT_EQ(CODA_SUCCESS,
                coda_mem_array_set_element(
                    mem_arr, i, reinterpret_cast<coda_dynamic_type*>(elem)));
    }

    coda_dynamic_type* wrapped = wrap_in_records(
        reinterpret_cast<coda_dynamic_type*>(mem_arr), CODA_CURSOR_MAXDEPTH - 2,
        owned_types, coda_format_hdf5);
    ASSERT_NE(nullptr, wrapped);
    product.get()->root_type = wrapped;

    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  }

  // 7. HDF5-format memory array with bit_size=-1 triggers CODA_ERROR_PRODUCT
  //    inside coda_cursor_read_double_array and reports via callbackfunc.
  {
    std::vector<ScopedType> owned_types;
    ScopedProduct product(make_sp3_product());
    ASSERT_TRUE(product);

    coda_type_number* h5_int_def =
        coda_type_number_new(coda_format_hdf5, coda_integer_class);
    ASSERT_NE(nullptr, h5_int_def);
    TrackType(h5_int_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_read_type(reinterpret_cast<coda_type*>(h5_int_def),
                                      coda_native_type_int32));

    coda_type_array* h5_arr_def = coda_type_array_new(coda_format_hdf5);
    ASSERT_NE(nullptr, h5_arr_def);
    TrackType(h5_arr_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_array_set_base_type(
                  h5_arr_def, reinterpret_cast<coda_type*>(h5_int_def)));
    ASSERT_EQ(CODA_SUCCESS, coda_type_array_add_fixed_dimension(h5_arr_def, 2));

    coda_mem_array* mem_arr = coda_mem_array_new(h5_arr_def, nullptr);
    ASSERT_NE(nullptr, mem_arr);
    constexpr std::array<uint8_t, 4> kZero4 = {0, 0, 0, 0};
    for (int i = 0; i < 2; ++i) {
      coda_mem_data* elem =
          coda_mem_data_new(reinterpret_cast<coda_type*>(h5_int_def), nullptr,
                            product.get(), 4, kZero4.data());
      ASSERT_NE(nullptr, elem);
      ASSERT_EQ(CODA_SUCCESS,
                coda_mem_array_set_element(
                    mem_arr, i, reinterpret_cast<coda_dynamic_type*>(elem)));
    }
    product.get()->root_type = reinterpret_cast<coda_dynamic_type*>(mem_arr);

    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_THAT(findings,
                ElementsAre(AllOf(Field(&CheckFinding::path, "/"),
                                  Field(&CheckFinding::message,
                                        HasSubstr("invalid bit size (-1)")))));
  }

  // 8. Array element 1 whose record size_expr fails during
  //    coda_cursor_goto_next_array_element returns -1 from check_data.
  {
    std::vector<ScopedType> owned_types;
    ScopedProduct product(make_sp3_product());
    ASSERT_TRUE(product);

    coda_type_text* tag_def = coda_type_text_new(coda_format_binary);
    ASSERT_NE(nullptr, tag_def);
    TrackType(tag_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_read_type(reinterpret_cast<coda_type*>(tag_def),
                                      coda_native_type_string));
    ASSERT_EQ(CODA_SUCCESS, coda_type_set_byte_size(
                                reinterpret_cast<coda_type*>(tag_def), 1));

    coda_type_record* elem_rec_def = coda_type_record_new(coda_format_binary);
    ASSERT_NE(nullptr, elem_rec_def);
    TrackType(elem_rec_def, owned_types);
    coda_expression* rec_size_expr = nullptr;
    ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("int(str(./tag)) * 8",
                                                        &rec_size_expr));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_bit_size_expression(
                  reinterpret_cast<coda_type*>(elem_rec_def), rec_size_expr));
    coda_type_record_field* tag_field = coda_type_record_field_new("tag");
    ASSERT_NE(nullptr, tag_field);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_field_set_type(
                  tag_field, reinterpret_cast<coda_type*>(tag_def)));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_add_field(elem_rec_def, tag_field));

    coda_type_array* arr_def = coda_type_array_new(coda_format_binary);
    ASSERT_NE(nullptr, arr_def);
    TrackType(arr_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_array_set_base_type(
                  arr_def, reinterpret_cast<coda_type*>(elem_rec_def)));
    ASSERT_EQ(CODA_SUCCESS, coda_type_array_add_fixed_dimension(arr_def, 3));

    constexpr std::array<uint8_t, 3> kBuf = {'1', 'x', '1'};
    coda_mem_data* mem_arr = coda_mem_data_new(
        reinterpret_cast<coda_type*>(arr_def), nullptr, product.get(),
        static_cast<long>(kBuf.size()), kBuf.data());
    ASSERT_NE(nullptr, mem_arr);
    product.get()->root_type = reinterpret_cast<coda_dynamic_type*>(mem_arr);

    const ScopedCodaOptions restore_options;
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_fast_size_expressions(1));

    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }

  // 9. Self-describing product node with runtime attributes when its
  //    definition has no attributes clears *definition for child attribute
  //    checks (coda-check.c:583-595).
  {
    std::vector<ScopedType> owned_types;
    ScopedProduct product(make_sp3_product());
    ASSERT_TRUE(product);

    coda_type_record* data_rec_def = coda_type_record_new(coda_format_sp3);
    ASSERT_NE(nullptr, data_rec_def);
    TrackType(data_rec_def, owned_types);
    coda_type_record* data_attr_def = coda_type_record_new(coda_format_sp3);
    ASSERT_NE(nullptr, data_attr_def);
    TrackType(data_attr_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_attributes(
                  reinterpret_cast<coda_type*>(data_rec_def), data_attr_def));

    UniqueDynamicType<coda_mem_record> mem_attrs(
        coda_mem_record_new(data_attr_def, nullptr));
    ASSERT_NE(nullptr, mem_attrs.get());
    UniqueDynamicType<coda_mem_record> mem_root(coda_mem_record_new(
        data_rec_def, reinterpret_cast<coda_dynamic_type*>(mem_attrs.get())));
    ASSERT_NE(nullptr, mem_root.get());
    mem_attrs.release();
    product.get()->root_type =
        reinterpret_cast<coda_dynamic_type*>(mem_root.release());

    coda_type_record* def_root_type = coda_type_record_new(coda_format_sp3);
    ASSERT_NE(nullptr, def_root_type);
    TrackType(def_root_type, owned_types);
    coda_product_definition prod_def{};
    prod_def.format = coda_format_sp3;
    prod_def.root_type = reinterpret_cast<coda_type*>(def_root_type);
    product.get()->product_definition = &prod_def;
    const absl::Cleanup clear_prod_def = [&] {
      product.get()->product_definition = nullptr;
    };

    std::vector<CheckFinding> findings;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(CODA_SUCCESS,
              coda_product_check(product.get(), 1, CollectFinding, &findings));
    EXPECT_THAT(findings, ::testing::IsEmpty());
  }
}

// TODO(schwehr): 88.3% reachable ceiling in libcoda/coda-check.c (507/574);
// remaining 67 missed lines are 9 OOM checks, 1 assert(0)/exit(1) default, 21
// dead XML-with-size_check lines, 4 unreachable NULL checks, and 32 redundant
// defensive cursor guards.

}  // namespace
}  // namespace stcorp_coda
