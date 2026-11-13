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
// Tests CODA binary cursor bit-size, string-length, element-count, scalar,
// bit-packed, endianness, float/NaN/Inf, conversion, special-type, array,
// partial-array, and error paths (libcoda/coda-bin-cursor.c).
//
// google3 build caveats:
//   - Compiled on little-endian LP64 Linux (WORDS_BIGENDIAN and WIN32 branches
//     are compiled out).
//   - coda_bin_cursor_read_int64() line 638 shifts `1 << bit_size` on 32-bit
//     int during negative sign extension; tests use bit_size < 31 for negative
//     sub-64-bit int64 values to avoid triggering UBSan signed shift overflow.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-bin-cursor.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-bin.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-bin.h"
#include "third_party/stcorp_coda/libcoda/coda-definition.h"
#include "third_party/stcorp_coda/libcoda/coda-type.h"
}  // extern "C"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"

namespace stcorp_coda {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::HasSubstr;

struct ProductDefDeleter {
  void operator()(coda_product_definition* def) const {
    coda_product_definition_delete(def);
  }
};
using ScopedProductDefinition =
    std::unique_ptr<coda_product_definition, ProductDefDeleter>;

struct BinProductDeleter {
  void operator()(coda_product* product) const { coda_bin_close(product); }
};
using ScopedBinProduct = std::unique_ptr<coda_product, BinProductDeleter>;

struct TypeDeleter {
  void operator()(coda_type* type) const { coda_type_release(type); }
};
using ScopedType = std::unique_ptr<coda_type, TypeDeleter>;

struct CodadefEnv {
  ScopedCodaOptions restore_options;
  ScopedEnvVar restore_def{"CODA_DEFINITION"};
  ScopedEnvVar restore_leap{"CODA_LEAP_SECOND_TABLE"};
  TempFile codadef_file;
  ScopedCodaSession session;

  explicit CodadefEnv(std::string_view name) : codadef_file(name) {
    restore_def.Unset();
    restore_leap.Unset();
  }

  ~CodadefEnv() {
    session.Reset();
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  bool InitWithXml(std::string_view magic, std::string_view prod_id,
                   std::string_view product_xml) {
    const std::string index_xml = absl::StrFormat(
        R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="BinCursorClass">
  <cd:ProductType name="BinCursorType">
    <cd:ProductDefinition id="%s" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">%s</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)xml",
        prod_id, magic);
    const std::string entry_name = absl::StrFormat("products/%s.xml", prod_id);
    const std::array<std::pair<std::string_view, std::string_view>, 1> entries =
        {{{entry_name, product_xml}}};
    if (!codadef_file.WriteBytes(BuildCodadefBytes(index_xml, entries))) {
      return false;
    }
    if (coda_set_definition_path(codadef_file.c_str()) != CODA_SUCCESS) {
      return false;
    }
    return session.Init() == CODA_SUCCESS;
  }
};

// Encodes MSB-first bit streams matching CODA's read_bits() convention.
class BitWriter {
 public:
  void WriteBits(uint64_t value, int num_bits) {
    for (int i = num_bits - 1; i >= 0; --i) {
      if (bit_pos_ == 0) {
        bytes_.push_back(0);
      }
      if (((value >> i) & 1ULL) != 0) {
        bytes_.back() |= static_cast<uint8_t>(1U << (7 - bit_pos_));
      }
      bit_pos_ = (bit_pos_ + 1) & 7;
    }
  }

  void WriteLeBytes(uint64_t value, int num_bytes) {
    for (int i = 0; i < num_bytes; ++i) {
      WriteBits((value >> (8 * i)) & 0xFFU, 8);
    }
  }

  void WriteBeBytes(uint64_t value, int num_bytes) {
    for (int i = num_bytes - 1; i >= 0; --i) {
      WriteBits((value >> (8 * i)) & 0xFFU, 8);
    }
  }

  void WriteString(std::string_view s) {
    for (char c : s) {
      WriteBits(static_cast<uint8_t>(c), 8);
    }
  }

  void PadToByte() {
    if (bit_pos_ != 0) {
      bit_pos_ = 0;
    }
  }

  const std::vector<uint8_t>& bytes() const { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
  int bit_pos_ = 0;
};

void AppendString(std::vector<uint8_t>& out, std::string_view s) {
  out.insert(out.end(), s.begin(), s.end());
}

void AppendLe16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void AppendBe16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t v) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
  }
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
  }
}

void AppendLe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
  }
}

void AppendBe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
  }
}

void AppendLeFloat(std::vector<uint8_t>& out, float v) {
  AppendLe32(out, std::bit_cast<uint32_t>(v));
}

void AppendLeDouble(std::vector<uint8_t>& out, double v) {
  AppendLe64(out, std::bit_cast<uint64_t>(v));
}

coda_cursor GotoField(const coda_cursor& root, const char* name) {
  coda_cursor c = root;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&c, name))
      << "field=" << name;
  return c;
}

bool AddRecordField(coda_type_record* record, const char* name,
                    coda_type* field_type) {
  coda_type_record_field* field = coda_type_record_field_new(name);
  if (field == nullptr) {
    return false;
  }
  if (coda_type_record_field_set_type(field, field_type) != CODA_SUCCESS ||
      coda_type_record_add_field(record, field) != CODA_SUCCESS) {
    coda_type_record_field_delete(field);
    return false;
  }
  return true;
}

TEST(CodaBinCursorTest, GetBitSizeFixedDynamicCompoundSpecialAndErrors) {
  CodadefEnv env("coda_bin_cursor_bitsize.codadef");
  constexpr std::string_view kXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinSizeProd" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text><cd:ByteSize>8</cd:ByteSize></cd:Text>
    </cd:Field>
    <cd:Field name="dyn_bits">
      <cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer>
    </cd:Field>
    <cd:Field name="dyn_bytes">
      <cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer>
    </cd:Field>
    <cd:Field name="f32_bits">
      <cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer>
    </cd:Field>
    <cd:Field name="i32_bits">
      <cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer>
    </cd:Field>
    <cd:Field name="zero">
      <cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer>
    </cd:Field>
    <cd:Field name="neg_val">
      <cd:Integer><cd:NativeType>int8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer>
    </cd:Field>
    <cd:Field name="fixed_i32">
      <cd:Integer><cd:NativeType>int32</cd:NativeType><cd:BitSize>32</cd:BitSize></cd:Integer>
    </cd:Field>
    <cd:Field name="fixed_raw11">
      <cd:Raw><cd:BitSize>11</cd:BitSize></cd:Raw>
    </cd:Field>
    <cd:Field name="pad5">
      <cd:Raw><cd:BitSize>5</cd:BitSize></cd:Raw>
    </cd:Field>
    <cd:Field name="fixed_rec">
      <cd:Record>
        <cd:Field name="a"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
        <cd:Field name="b"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
      </cd:Record>
    </cd:Field>
    <cd:Field name="fixed_arr">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension>
        <cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="dyn_rec">
      <cd:Record>
        <cd:Field name="len"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
        <!-- size_expr evaluates with cursor on payload, so ../len reaches sibling len. -->
        <cd:Field name="payload"><cd:Text><cd:ByteSize>int(../len)</cd:ByteSize></cd:Text></cd:Field>
      </cd:Record>
    </cd:Field>
    <cd:Field name="sized_rec">
      <cd:Record>
        <cd:BitSize>(1 + int(/dyn_bytes)) * 8</cd:BitSize>
        <cd:Field name="a"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
        <cd:Field name="b"><cd:Text><cd:ByteSize>int(/dyn_bytes)</cd:ByteSize></cd:Text></cd:Field>
      </cd:Record>
    </cd:Field>
    <cd:Field name="dyn_arr">
      <cd:Array><cd:Dimension>int(/dyn_bytes)</cd:Dimension>
        <cd:Integer><cd:NativeType>uint16</cd:NativeType><cd:BitSize>16</cd:BitSize></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="dyn_int">
      <cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>int(/dyn_bits)</cd:BitSize></cd:Integer>
    </cd:Field>
    <cd:Field name="dyn_real">
      <cd:Float><cd:NativeType>float</cd:NativeType><cd:BitSize>int(/f32_bits)</cd:BitSize></cd:Float>
    </cd:Field>
    <cd:Field name="dyn_text">
      <cd:Text><cd:ByteSize>int(/dyn_bytes)</cd:ByteSize></cd:Text>
    </cd:Field>
    <cd:Field name="dyn_raw">
      <cd:Raw><cd:BitSize>int(/dyn_bits)</cd:BitSize></cd:Raw>
    </cd:Field>
    <cd:Field name="dyn_time_ok">
      <cd:Time timeformat="float(.)">
        <cd:Integer><cd:NativeType>int32</cd:NativeType><cd:BitSize>int(/i32_bits)</cd:BitSize></cd:Integer>
      </cd:Time>
    </cd:Field>
    <cd:Field name="bad_expr_text">
      <cd:Text><cd:ByteSize>1 / int(/zero)</cd:ByteSize></cd:Text>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
    <cd:Field name="neg_expr_text">
      <cd:Text><cd:ByteSize>int(/neg_val)</cd:ByteSize></cd:Text>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
    <cd:Field name="bad_expr_time">
      <cd:Time timeformat="float(.)">
        <cd:Integer><cd:NativeType>int32</cd:NativeType><cd:BitSize>1 / int(/zero)</cd:BitSize></cd:Integer>
      </cd:Time>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";
  ASSERT_TRUE(env.InitWithXml("BINSIZE1", "BinSizeProd", kXml));
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  std::vector<uint8_t> data;
  AppendString(data, "BINSIZE1");
  data.push_back(5);                                  // dyn_bits
  data.push_back(3);                                  // dyn_bytes
  data.push_back(32);                                 // f32_bits
  data.push_back(32);                                 // i32_bits
  data.push_back(0);                                  // zero
  data.push_back(static_cast<uint8_t>(int8_t{-4}));   // neg_val
  AppendBe32(data, 42);                               // fixed_i32
  data.insert(data.end(), {0xAB, 0xCD});              // fixed_raw11 + pad5
  data.insert(data.end(), {1, 2});                    // fixed_rec
  data.insert(data.end(), {10, 20, 30, 40, 50, 60});  // fixed_arr
  data.push_back(2);                                  // dyn_rec.len
  AppendString(data, "hi");                           // dyn_rec.payload
  data.push_back(7);                                  // sized_rec.a
  AppendString(data, "xyz");                          // sized_rec.b
  data.insert(data.end(), 64, 0);  // dyn_arr + trailing fields

  TempFile dat_file("coda_bin_cursor_bitsize.dat");
  ASSERT_TRUE(dat_file.WriteBytes(data));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(dat_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  int64_t bit_size = -999;
  const coda_cursor c_i32 = GotoField(root, "fixed_i32");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_bit_size(&c_i32, &bit_size));
  EXPECT_EQ(32, bit_size);

  const coda_cursor c_raw11 = GotoField(root, "fixed_raw11");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_bit_size(&c_raw11, &bit_size));
  EXPECT_EQ(11, bit_size);

  const coda_cursor c_frec = GotoField(root, "fixed_rec");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_bit_size(&c_frec, &bit_size));
  EXPECT_EQ(16, bit_size);

  const coda_cursor c_farr = GotoField(root, "fixed_arr");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_bit_size(&c_farr, &bit_size));
  EXPECT_EQ(48, bit_size);

  const coda_cursor c_drec = GotoField(root, "dyn_rec");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_bit_size(&c_drec, &bit_size));
  EXPECT_EQ(24, bit_size);

  // Exercise sized_rec with both fast_size_expressions=1 and 0.
  const coda_cursor c_srec = GotoField(root, "sized_rec");
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_fast_size_expressions(1));
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_bit_size(&c_srec, &bit_size));
  EXPECT_EQ(32, bit_size);
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_fast_size_expressions(0));
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_bit_size(&c_srec, &bit_size));
  EXPECT_EQ(32, bit_size);
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_fast_size_expressions(1));

  const coda_cursor c_darr = GotoField(root, "dyn_arr");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_bit_size(&c_darr, &bit_size));
  EXPECT_EQ(48, bit_size);

  const coda_cursor c_dint = GotoField(root, "dyn_int");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_bit_size(&c_dint, &bit_size));
  EXPECT_EQ(5, bit_size);

  const coda_cursor c_dreal = GotoField(root, "dyn_real");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_bit_size(&c_dreal, &bit_size));
  EXPECT_EQ(32, bit_size);

  const coda_cursor c_dtext = GotoField(root, "dyn_text");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_bit_size(&c_dtext, &bit_size));
  EXPECT_EQ(24, bit_size);

  const coda_cursor c_draw = GotoField(root, "dyn_raw");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_bit_size(&c_draw, &bit_size));
  EXPECT_EQ(5, bit_size);

  const coda_cursor c_dtime = GotoField(root, "dyn_time_ok");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_bit_size(&c_dtime, &bit_size));
  EXPECT_EQ(32, bit_size);

  // Error branches in coda_bin_cursor_get_bit_size().
  const coda_cursor c_bad_text = GotoField(root, "bad_expr_text");
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_bin_cursor_get_bit_size(&c_bad_text, &bit_size));
  EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);

  const coda_cursor c_neg_text = GotoField(root, "neg_expr_text");
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_bin_cursor_get_bit_size(&c_neg_text, &bit_size));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("calculated size is negative"));

  const coda_cursor c_bad_time = GotoField(root, "bad_expr_time");
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_bin_cursor_get_bit_size(&c_bad_time, &bit_size));
  EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);

  // Unsized types (bit_size == -1, size_expr == NULL) via direct type API.
  ScopedProductDefinition def(
      coda_product_definition_new("UnsizedBin", coda_format_binary, 1));
  ASSERT_NE(nullptr, def);
  ScopedType unsized_rec(
      reinterpret_cast<coda_type*>(coda_type_record_new(coda_format_binary)));
  ASSERT_NE(nullptr, unsized_rec);
  auto* rec = reinterpret_cast<coda_type_record*>(unsized_rec.get());

  ScopedType t_int(reinterpret_cast<coda_type*>(
      coda_type_number_new(coda_format_binary, coda_integer_class)));
  ScopedType t_real(reinterpret_cast<coda_type*>(
      coda_type_number_new(coda_format_binary, coda_real_class)));
  ScopedType t_text(
      reinterpret_cast<coda_type*>(coda_type_text_new(coda_format_binary)));
  ScopedType t_raw(
      reinterpret_cast<coda_type*>(coda_type_raw_new(coda_format_binary)));
  ASSERT_NE(nullptr, t_int);
  ASSERT_NE(nullptr, t_real);
  ASSERT_NE(nullptr, t_text);
  ASSERT_NE(nullptr, t_raw);
  ASSERT_TRUE(AddRecordField(rec, "u_int", t_int.get()));
  ASSERT_TRUE(AddRecordField(rec, "u_real", t_real.get()));
  ASSERT_TRUE(AddRecordField(rec, "u_text", t_text.get()));
  ASSERT_TRUE(AddRecordField(rec, "u_raw", t_raw.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_product_definition_set_root_type(
                              def.get(), unsized_rec.get()));

  coda_product* raw_bin = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_bin_open(dat_file.c_str(), static_cast<int64_t>(data.size()),
                          &raw_bin));
  ScopedBinProduct bin_prod(raw_bin);
  coda_product* raw_ptr = bin_prod.get();
  ASSERT_EQ(CODA_SUCCESS, coda_bin_reopen_with_definition(&raw_ptr, def.get()));

  coda_cursor u_root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&u_root, bin_prod.get()));
  for (const char* name : {"u_int", "u_real", "u_text", "u_raw"}) {
    const coda_cursor c = GotoField(u_root, name);
    bit_size = 999;
    EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_bit_size(&c, &bit_size));
    EXPECT_EQ(-1, bit_size);
  }
}

TEST(CodaBinCursorTest, GetStringLengthAndNumElements) {
  CodadefEnv env("coda_bin_cursor_strlen_numelem.codadef");
  constexpr std::string_view kXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinMetaProd" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text><cd:ByteSize>8</cd:ByteSize></cd:Text>
    </cd:Field>
    <cd:Field name="str_len">
      <cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer>
    </cd:Field>
    <cd:Field name="zero">
      <cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer>
    </cd:Field>
    <cd:Field name="fixed_str">
      <cd:Text><cd:ByteSize>5</cd:ByteSize></cd:Text>
    </cd:Field>
    <cd:Field name="dyn_str">
      <cd:Text><cd:ByteSize>int(/str_len)</cd:ByteSize></cd:Text>
    </cd:Field>
    <cd:Field name="empty_str">
      <cd:Text><cd:ByteSize>0</cd:ByteSize></cd:Text>
    </cd:Field>
    <cd:Field name="arr">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>int(/str_len)</cd:Dimension>
        <cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="bad_str">
      <cd:Text><cd:ByteSize>1 / int(/zero)</cd:ByteSize></cd:Text>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";
  ASSERT_TRUE(env.InitWithXml("BINMETA1", "BinMetaProd", kXml));
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  std::vector<uint8_t> data;
  AppendString(data, "BINMETA1");
  data.push_back(4);  // str_len
  data.push_back(0);  // zero
  AppendString(data, "hello");
  AppendString(data, "coda");
  data.insert(data.end(), 8, 1);

  TempFile dat_file("coda_bin_cursor_meta.dat");
  ASSERT_TRUE(dat_file.WriteBytes(data));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(dat_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  long len = -999;
  const coda_cursor c_fixed = GotoField(root, "fixed_str");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_string_length(&c_fixed, &len));
  EXPECT_EQ(5, len);

  const coda_cursor c_dyn = GotoField(root, "dyn_str");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_string_length(&c_dyn, &len));
  EXPECT_EQ(4, len);

  const coda_cursor c_empty = GotoField(root, "empty_str");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_string_length(&c_empty, &len));
  EXPECT_EQ(0, len);

  const coda_cursor c_bad = GotoField(root, "bad_str");
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_bin_cursor_get_string_length(&c_bad, &len));
  EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);

  long num_elements = -1;
  EXPECT_EQ(CODA_SUCCESS,
            coda_bin_cursor_get_num_elements(&root, &num_elements));
  EXPECT_EQ(8, num_elements);

  const coda_cursor c_arr = GotoField(root, "arr");
  EXPECT_EQ(CODA_SUCCESS,
            coda_bin_cursor_get_num_elements(&c_arr, &num_elements));
  EXPECT_EQ(8, num_elements);

  EXPECT_EQ(CODA_SUCCESS,
            coda_bin_cursor_get_num_elements(&c_fixed, &num_elements));
  EXPECT_EQ(1, num_elements);

  // Unsized text returns length == -1 with CODA_SUCCESS.
  ScopedProductDefinition def(
      coda_product_definition_new("UnsizedText", coda_format_binary, 1));
  ASSERT_NE(nullptr, def);
  ScopedType root_type(
      reinterpret_cast<coda_type*>(coda_type_record_new(coda_format_binary)));
  ScopedType t_text(
      reinterpret_cast<coda_type*>(coda_type_text_new(coda_format_binary)));
  ASSERT_TRUE(
      AddRecordField(reinterpret_cast<coda_type_record*>(root_type.get()),
                     "utext", t_text.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_definition_set_root_type(def.get(), root_type.get()));

  coda_product* raw_bin = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_bin_open(dat_file.c_str(), static_cast<int64_t>(data.size()),
                          &raw_bin));
  ScopedBinProduct bin_prod(raw_bin);
  coda_product* raw_ptr = bin_prod.get();
  ASSERT_EQ(CODA_SUCCESS, coda_bin_reopen_with_definition(&raw_ptr, def.get()));

  coda_cursor u_root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&u_root, bin_prod.get()));
  const coda_cursor c_utext = GotoField(u_root, "utext");
  len = 999;
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_get_string_length(&c_utext, &len));
  EXPECT_EQ(-1, len);
}

TEST(CodaBinCursorTest, ReadIntegersAllWidthsEndiannessBitLengthsAndModes) {
  CodadefEnv env("coda_bin_cursor_ints.codadef");
  constexpr std::string_view kXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinIntsProd" format="binary">
  <cd:Record>
    <cd:Field name="magic"><cd:Text><cd:ByteSize>8</cd:ByteSize></cd:Text></cd:Field>
    <cd:Field name="zero"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="one"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="bits5"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="bits11"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="bits19"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="bits24"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="bits40"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="over8"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="over16"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="over32"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="over64"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="i8_neg"><cd:Integer><cd:NativeType>int8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="i8_min"><cd:Integer><cd:NativeType>int8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u8_max"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="i16_be"><cd:Integer><cd:NativeType>int16</cd:NativeType><cd:BitSize>16</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="i16_le"><cd:Integer><cd:NativeType>int16</cd:NativeType><cd:BitSize>16</cd:BitSize><cd:LittleEndian/></cd:Integer></cd:Field>
    <cd:Field name="u16_be"><cd:Integer><cd:NativeType>uint16</cd:NativeType><cd:BitSize>16</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u16_le"><cd:Integer><cd:NativeType>uint16</cd:NativeType><cd:BitSize>16</cd:BitSize><cd:LittleEndian/></cd:Integer></cd:Field>
    <cd:Field name="i32_be"><cd:Integer><cd:NativeType>int32</cd:NativeType><cd:BitSize>32</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="i32_le"><cd:Integer><cd:NativeType>int32</cd:NativeType><cd:BitSize>32</cd:BitSize><cd:LittleEndian/></cd:Integer></cd:Field>
    <cd:Field name="u32_be"><cd:Integer><cd:NativeType>uint32</cd:NativeType><cd:BitSize>32</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u32_le"><cd:Integer><cd:NativeType>uint32</cd:NativeType><cd:BitSize>32</cd:BitSize><cd:LittleEndian/></cd:Integer></cd:Field>
    <cd:Field name="i64_be"><cd:Integer><cd:NativeType>int64</cd:NativeType><cd:BitSize>64</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="i64_le"><cd:Integer><cd:NativeType>int64</cd:NativeType><cd:BitSize>64</cd:BitSize><cd:LittleEndian/></cd:Integer></cd:Field>
    <cd:Field name="u64_be"><cd:Integer><cd:NativeType>uint64</cd:NativeType><cd:BitSize>64</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u64_le"><cd:Integer><cd:NativeType>uint64</cd:NativeType><cd:BitSize>64</cd:BitSize><cd:LittleEndian/></cd:Integer></cd:Field>
    <cd:Field name="pad3"><cd:Raw><cd:BitSize>3</cd:BitSize></cd:Raw></cd:Field>
    <cd:Field name="u_i8"><cd:Integer><cd:NativeType>int8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u_u8"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u_i16_be"><cd:Integer><cd:NativeType>int16</cd:NativeType><cd:BitSize>16</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u_i16_le"><cd:Integer><cd:NativeType>int16</cd:NativeType><cd:BitSize>16</cd:BitSize><cd:LittleEndian/></cd:Integer></cd:Field>
    <cd:Field name="u_u16_be"><cd:Integer><cd:NativeType>uint16</cd:NativeType><cd:BitSize>16</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u_u16_le"><cd:Integer><cd:NativeType>uint16</cd:NativeType><cd:BitSize>16</cd:BitSize><cd:LittleEndian/></cd:Integer></cd:Field>
    <cd:Field name="u_i32_be"><cd:Integer><cd:NativeType>int32</cd:NativeType><cd:BitSize>32</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u_i32_le"><cd:Integer><cd:NativeType>int32</cd:NativeType><cd:BitSize>32</cd:BitSize><cd:LittleEndian/></cd:Integer></cd:Field>
    <cd:Field name="u_u32_be"><cd:Integer><cd:NativeType>uint32</cd:NativeType><cd:BitSize>32</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u_u32_le"><cd:Integer><cd:NativeType>uint32</cd:NativeType><cd:BitSize>32</cd:BitSize><cd:LittleEndian/></cd:Integer></cd:Field>
    <cd:Field name="u_i64_be"><cd:Integer><cd:NativeType>int64</cd:NativeType><cd:BitSize>64</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u_i64_le"><cd:Integer><cd:NativeType>int64</cd:NativeType><cd:BitSize>64</cd:BitSize><cd:LittleEndian/></cd:Integer></cd:Field>
    <cd:Field name="u_u64_be"><cd:Integer><cd:NativeType>uint64</cd:NativeType><cd:BitSize>64</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u_u64_le"><cd:Integer><cd:NativeType>uint64</cd:NativeType><cd:BitSize>64</cd:BitSize><cd:LittleEndian/></cd:Integer></cd:Field>
    <cd:Field name="i8_5_pos"><cd:Integer><cd:NativeType>int8</cd:NativeType><cd:BitSize>5</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="i8_5_neg"><cd:Integer><cd:NativeType>int8</cd:NativeType><cd:BitSize>5</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u8_5"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>5</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="i16_5_neg"><cd:Integer><cd:NativeType>int16</cd:NativeType><cd:BitSize>5</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="i16_11_pos"><cd:Integer><cd:NativeType>int16</cd:NativeType><cd:BitSize>11</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="i16_11_neg"><cd:Integer><cd:NativeType>int16</cd:NativeType><cd:BitSize>11</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u16_5"><cd:Integer><cd:NativeType>uint16</cd:NativeType><cd:BitSize>5</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u16_11"><cd:Integer><cd:NativeType>uint16</cd:NativeType><cd:BitSize>11</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="i32_5_neg"><cd:Integer><cd:NativeType>int32</cd:NativeType><cd:BitSize>5</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="i32_19_pos"><cd:Integer><cd:NativeType>int32</cd:NativeType><cd:BitSize>19</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="i32_19_neg"><cd:Integer><cd:NativeType>int32</cd:NativeType><cd:BitSize>19</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u32_5"><cd:Integer><cd:NativeType>uint32</cd:NativeType><cd:BitSize>5</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u32_19"><cd:Integer><cd:NativeType>uint32</cd:NativeType><cd:BitSize>19</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="i64_5_neg"><cd:Integer><cd:NativeType>int64</cd:NativeType><cd:BitSize>5</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="i64_24_neg"><cd:Integer><cd:NativeType>int64</cd:NativeType><cd:BitSize>24</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="i64_40_pos"><cd:Integer><cd:NativeType>int64</cd:NativeType><cd:BitSize>40</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u64_5"><cd:Integer><cd:NativeType>uint64</cd:NativeType><cd:BitSize>5</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="u64_40"><cd:Integer><cd:NativeType>uint64</cd:NativeType><cd:BitSize>40</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="dyn_i8"><cd:Integer><cd:NativeType>int8</cd:NativeType><cd:BitSize>int(/bits5)</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="dyn_i8_bytes"><cd:Integer><cd:NativeType>int8</cd:NativeType><cd:ByteSize>int(/one)</cd:ByteSize></cd:Integer></cd:Field>
    <cd:Field name="dyn_u8"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>int(/bits5)</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="dyn_i16"><cd:Integer><cd:NativeType>int16</cd:NativeType><cd:BitSize>int(/bits11)</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="dyn_u16"><cd:Integer><cd:NativeType>uint16</cd:NativeType><cd:BitSize>int(/bits11)</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="dyn_i32"><cd:Integer><cd:NativeType>int32</cd:NativeType><cd:BitSize>int(/bits19)</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="dyn_u32"><cd:Integer><cd:NativeType>uint32</cd:NativeType><cd:BitSize>int(/bits19)</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="dyn_i64"><cd:Integer><cd:NativeType>int64</cd:NativeType><cd:BitSize>int(/bits24)</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="dyn_u64"><cd:Integer><cd:NativeType>uint64</cd:NativeType><cd:BitSize>int(/bits40)</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="bad_i8"><cd:Integer><cd:NativeType>int8</cd:NativeType><cd:BitSize>1 / int(/zero)</cd:BitSize></cd:Integer><cd:BitOffset>64</cd:BitOffset></cd:Field>
    <cd:Field name="bad_u8"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>1 / int(/zero)</cd:BitSize></cd:Integer><cd:BitOffset>64</cd:BitOffset></cd:Field>
    <cd:Field name="bad_i16"><cd:Integer><cd:NativeType>int16</cd:NativeType><cd:BitSize>1 / int(/zero)</cd:BitSize></cd:Integer><cd:BitOffset>64</cd:BitOffset></cd:Field>
    <cd:Field name="bad_u16"><cd:Integer><cd:NativeType>uint16</cd:NativeType><cd:BitSize>1 / int(/zero)</cd:BitSize></cd:Integer><cd:BitOffset>64</cd:BitOffset></cd:Field>
    <cd:Field name="bad_i32"><cd:Integer><cd:NativeType>int32</cd:NativeType><cd:BitSize>1 / int(/zero)</cd:BitSize></cd:Integer><cd:BitOffset>64</cd:BitOffset></cd:Field>
    <cd:Field name="bad_u32"><cd:Integer><cd:NativeType>uint32</cd:NativeType><cd:BitSize>1 / int(/zero)</cd:BitSize></cd:Integer><cd:BitOffset>64</cd:BitOffset></cd:Field>
    <cd:Field name="bad_i64"><cd:Integer><cd:NativeType>int64</cd:NativeType><cd:BitSize>1 / int(/zero)</cd:BitSize></cd:Integer><cd:BitOffset>64</cd:BitOffset></cd:Field>
    <cd:Field name="bad_u64"><cd:Integer><cd:NativeType>uint64</cd:NativeType><cd:BitSize>1 / int(/zero)</cd:BitSize></cd:Integer><cd:BitOffset>64</cd:BitOffset></cd:Field>
    <cd:Field name="over_i8"><cd:Integer><cd:NativeType>int8</cd:NativeType><cd:BitSize>int(/over8)</cd:BitSize></cd:Integer><cd:BitOffset>64</cd:BitOffset></cd:Field>
    <cd:Field name="over_u8"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>int(/over8)</cd:BitSize></cd:Integer><cd:BitOffset>64</cd:BitOffset></cd:Field>
    <cd:Field name="over_i16"><cd:Integer><cd:NativeType>int16</cd:NativeType><cd:BitSize>int(/over16)</cd:BitSize></cd:Integer><cd:BitOffset>64</cd:BitOffset></cd:Field>
    <cd:Field name="over_u16"><cd:Integer><cd:NativeType>uint16</cd:NativeType><cd:BitSize>int(/over16)</cd:BitSize></cd:Integer><cd:BitOffset>64</cd:BitOffset></cd:Field>
    <cd:Field name="over_i32"><cd:Integer><cd:NativeType>int32</cd:NativeType><cd:BitSize>int(/over32)</cd:BitSize></cd:Integer><cd:BitOffset>64</cd:BitOffset></cd:Field>
    <cd:Field name="over_u32"><cd:Integer><cd:NativeType>uint32</cd:NativeType><cd:BitSize>int(/over32)</cd:BitSize></cd:Integer><cd:BitOffset>64</cd:BitOffset></cd:Field>
    <cd:Field name="over_i64"><cd:Integer><cd:NativeType>int64</cd:NativeType><cd:BitSize>int(/over64)</cd:BitSize></cd:Integer><cd:BitOffset>64</cd:BitOffset></cd:Field>
    <cd:Field name="over_u64"><cd:Integer><cd:NativeType>uint64</cd:NativeType><cd:BitSize>int(/over64)</cd:BitSize></cd:Integer><cd:BitOffset>64</cd:BitOffset></cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";
  ASSERT_TRUE(env.InitWithXml("BININTS1", "BinIntsProd", kXml));
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  BitWriter bw;
  bw.WriteString("BININTS1");
  bw.WriteBits(0, 8);   // zero
  bw.WriteBits(1, 8);   // one
  bw.WriteBits(5, 8);   // bits5
  bw.WriteBits(11, 8);  // bits11
  bw.WriteBits(19, 8);  // bits19
  bw.WriteBits(24, 8);  // bits24
  bw.WriteBits(40, 8);  // bits40
  bw.WriteBits(9, 8);   // over8
  bw.WriteBits(17, 8);  // over16
  bw.WriteBits(33, 8);  // over32
  bw.WriteBits(65, 8);  // over64
  const size_t kHeaderBytes = bw.bytes().size();

  // Byte-aligned full-width scalars.
  bw.WriteBits(static_cast<uint8_t>(int8_t{-42}), 8);
  bw.WriteBits(static_cast<uint8_t>(std::numeric_limits<int8_t>::min()), 8);
  bw.WriteBits(0xFFU, 8);
  bw.WriteBeBytes(static_cast<uint16_t>(int16_t{-12345}), 2);
  bw.WriteLeBytes(static_cast<uint16_t>(int16_t{-23456}), 2);
  bw.WriteBeBytes(0xBEEFU, 2);
  bw.WriteLeBytes(0xCAFEU, 2);
  bw.WriteBeBytes(static_cast<uint32_t>(int32_t{-0x12345678}), 4);
  bw.WriteLeBytes(static_cast<uint32_t>(int32_t{-0x23456789}), 4);
  bw.WriteBeBytes(0xDEADBEEFU, 4);
  bw.WriteLeBytes(0xCAFEBABEU, 4);
  bw.WriteBeBytes(static_cast<uint64_t>(int64_t{-0x0102030405060708LL}), 8);
  bw.WriteLeBytes(static_cast<uint64_t>(int64_t{-0x1122334455667788LL}), 8);
  bw.WriteBeBytes(0xFEDCBA9876543210ULL, 8);
  bw.WriteLeBytes(0x0123456789ABCDEFULL, 8);

  // Unaligned full-width scalars starting after 3 pad bits.
  bw.WriteBits(0b101, 3);  // pad3
  bw.WriteBits(static_cast<uint8_t>(int8_t{-17}), 8);
  bw.WriteBits(0xA5U, 8);
  bw.WriteBeBytes(static_cast<uint16_t>(int16_t{-1234}), 2);
  bw.WriteLeBytes(static_cast<uint16_t>(int16_t{-2345}), 2);
  bw.WriteBeBytes(0xABCDU, 2);
  bw.WriteLeBytes(0x1234U, 2);
  bw.WriteBeBytes(static_cast<uint32_t>(int32_t{-100000}), 4);
  bw.WriteLeBytes(static_cast<uint32_t>(int32_t{-200000}), 4);
  bw.WriteBeBytes(0x89ABCDEFU, 4);
  bw.WriteLeBytes(0x13579BDFU, 4);
  bw.WriteBeBytes(static_cast<uint64_t>(int64_t{-9000000000LL}), 8);
  bw.WriteLeBytes(static_cast<uint64_t>(int64_t{-8000000000LL}), 8);
  bw.WriteBeBytes(0x1122334455667788ULL, 8);
  bw.WriteLeBytes(0x8877665544332211ULL, 8);

  // Non-byte bit lengths (positive and negative).
  bw.WriteBits(10, 5);                // i8_5_pos = 10
  bw.WriteBits(0b11001, 5);           // i8_5_neg = -7
  bw.WriteBits(25, 5);                // u8_5 = 25
  bw.WriteBits(0b11001, 5);           // i16_5_neg = -7
  bw.WriteBits(0x2A3, 11);            // i16_11_pos = 675
  bw.WriteBits(0x5A3, 11);            // i16_11_neg = -605
  bw.WriteBits(21, 5);                // u16_5 = 21
  bw.WriteBits(0x5A3, 11);            // u16_11 = 1443
  bw.WriteBits(0b11011, 5);           // i32_5_neg = -5
  bw.WriteBits(123456, 19);           // i32_19_pos = 123456
  bw.WriteBits(0x61DC0, 19);          // i32_19_neg = -123456
  bw.WriteBits(19, 5);                // u32_5 = 19
  bw.WriteBits(400000, 19);           // u32_19 = 400000
  bw.WriteBits(0b11101, 5);           // i64_5_neg = -3
  bw.WriteBits(0x800001, 24);         // i64_24_neg = -8388607
  bw.WriteBits(0x123456789ALL, 40);   // i64_40_pos = 0x123456789ALL
  bw.WriteBits(29, 5);                // u64_5 = 29
  bw.WriteBits(0xABCDEF1234ULL, 40);  // u64_40 = 0xABCDEF1234ULL

  // Dynamic valid bit-sized fields.
  bw.WriteBits(0b11001, 5);                            // dyn_i8 = -7
  bw.WriteBits(static_cast<uint8_t>(int8_t{-42}), 8);  // dyn_i8_bytes = -42
  bw.WriteBits(25, 5);                                 // dyn_u8 = 25
  bw.WriteBits(0x5A3, 11);                             // dyn_i16 = -605
  bw.WriteBits(0x5A3, 11);                             // dyn_u16 = 1443
  bw.WriteBits(0x61DC0, 19);                           // dyn_i32 = -123456
  bw.WriteBits(400000, 19);                            // dyn_u32 = 400000
  bw.WriteBits(0x800001, 24);                          // dyn_i64 = -8388607
  bw.WriteBits(0xABCDEF1234ULL, 40);  // dyn_u64 = 0xABCDEF1234ULL
  bw.PadToByte();

  TempFile dat_file("coda_bin_cursor_ints.dat");
  ASSERT_TRUE(dat_file.WriteBytes(bw.bytes()));

  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(dat_file.c_str(), product.OutParam()));
    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

    auto read_i8 = [&](const char* name) {
      int8_t v = 0;
      const coda_cursor c = GotoField(root, name);
      EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_int8(&c, &v)) << name;
      return v;
    };
    auto read_u8 = [&](const char* name) {
      uint8_t v = 0;
      const coda_cursor c = GotoField(root, name);
      EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_uint8(&c, &v)) << name;
      return v;
    };
    auto read_i16 = [&](const char* name) {
      int16_t v = 0;
      const coda_cursor c = GotoField(root, name);
      EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_int16(&c, &v)) << name;
      return v;
    };
    auto read_u16 = [&](const char* name) {
      uint16_t v = 0;
      const coda_cursor c = GotoField(root, name);
      EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_uint16(&c, &v)) << name;
      return v;
    };
    auto read_i32 = [&](const char* name) {
      int32_t v = 0;
      const coda_cursor c = GotoField(root, name);
      EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_int32(&c, &v)) << name;
      return v;
    };
    auto read_u32 = [&](const char* name) {
      uint32_t v = 0;
      const coda_cursor c = GotoField(root, name);
      EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_uint32(&c, &v)) << name;
      return v;
    };
    auto read_i64 = [&](const char* name) {
      int64_t v = 0;
      const coda_cursor c = GotoField(root, name);
      EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_int64(&c, &v)) << name;
      return v;
    };
    auto read_u64 = [&](const char* name) {
      uint64_t v = 0;
      const coda_cursor c = GotoField(root, name);
      EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_uint64(&c, &v)) << name;
      return v;
    };

    // 1. Byte-aligned full-width big and little endian.
    EXPECT_EQ(-42, read_i8("i8_neg"));
    EXPECT_EQ(std::numeric_limits<int8_t>::min(), read_i8("i8_min"));
    EXPECT_EQ(0xFFU, read_u8("u8_max"));
    EXPECT_EQ(-12345, read_i16("i16_be"));
    EXPECT_EQ(-23456, read_i16("i16_le"));
    EXPECT_EQ(0xBEEFU, read_u16("u16_be"));
    EXPECT_EQ(0xCAFEU, read_u16("u16_le"));
    EXPECT_EQ(-0x12345678, read_i32("i32_be"));
    EXPECT_EQ(-0x23456789, read_i32("i32_le"));
    EXPECT_EQ(0xDEADBEEFU, read_u32("u32_be"));
    EXPECT_EQ(0xCAFEBABEU, read_u32("u32_le"));
    EXPECT_EQ(-0x0102030405060708LL, read_i64("i64_be"));
    EXPECT_EQ(-0x1122334455667788LL, read_i64("i64_le"));
    EXPECT_EQ(0xFEDCBA9876543210ULL, read_u64("u64_be"));
    EXPECT_EQ(0x0123456789ABCDEFULL, read_u64("u64_le"));

    // 2. Unaligned full-width big and little endian.
    EXPECT_EQ(-17, read_i8("u_i8"));
    EXPECT_EQ(0xA5U, read_u8("u_u8"));
    EXPECT_EQ(-1234, read_i16("u_i16_be"));
    EXPECT_EQ(-2345, read_i16("u_i16_le"));
    EXPECT_EQ(0xABCDU, read_u16("u_u16_be"));
    EXPECT_EQ(0x1234U, read_u16("u_u16_le"));
    EXPECT_EQ(-100000, read_i32("u_i32_be"));
    EXPECT_EQ(-200000, read_i32("u_i32_le"));
    EXPECT_EQ(0x89ABCDEFU, read_u32("u_u32_be"));
    EXPECT_EQ(0x13579BDFU, read_u32("u_u32_le"));
    EXPECT_EQ(-9000000000LL, read_i64("u_i64_be"));
    EXPECT_EQ(-8000000000LL, read_i64("u_i64_le"));
    EXPECT_EQ(0x1122334455667788ULL, read_u64("u_u64_be"));
    EXPECT_EQ(0x8877665544332211ULL, read_u64("u_u64_le"));

    // 3. Non-byte bit lengths with sign extension.
    EXPECT_EQ(10, read_i8("i8_5_pos"));
    EXPECT_EQ(-7, read_i8("i8_5_neg"));
    EXPECT_EQ(25, read_u8("u8_5"));
    EXPECT_EQ(-7, read_i16("i16_5_neg"));
    EXPECT_EQ(675, read_i16("i16_11_pos"));
    EXPECT_EQ(-605, read_i16("i16_11_neg"));
    EXPECT_EQ(21, read_u16("u16_5"));
    EXPECT_EQ(1443, read_u16("u16_11"));
    EXPECT_EQ(-5, read_i32("i32_5_neg"));
    EXPECT_EQ(123456, read_i32("i32_19_pos"));
    EXPECT_EQ(-123456, read_i32("i32_19_neg"));
    EXPECT_EQ(19U, read_u32("u32_5"));
    EXPECT_EQ(400000U, read_u32("u32_19"));
    EXPECT_EQ(-3, read_i64("i64_5_neg"));
    EXPECT_EQ(-8388607, read_i64("i64_24_neg"));
    EXPECT_EQ(0x123456789ALL, read_i64("i64_40_pos"));
    EXPECT_EQ(29ULL, read_u64("u64_5"));
    EXPECT_EQ(0xABCDEF1234ULL, read_u64("u64_40"));

    // 4. Dynamic valid bit sizes.
    EXPECT_EQ(-7, read_i8("dyn_i8"));
    EXPECT_EQ(-42, read_i8("dyn_i8_bytes"));
    EXPECT_EQ(25, read_u8("dyn_u8"));
    EXPECT_EQ(-605, read_i16("dyn_i16"));
    EXPECT_EQ(1443, read_u16("dyn_u16"));
    EXPECT_EQ(-123456, read_i32("dyn_i32"));
    EXPECT_EQ(400000U, read_u32("dyn_u32"));
    EXPECT_EQ(-8388607, read_i64("dyn_i64"));
    EXPECT_EQ(0xABCDEF1234ULL, read_u64("dyn_u64"));

    // 5. Dynamic bit-size evaluation failures and out-of-range bit sizes.
    int8_t v_i8 = 0;
    uint8_t v_u8 = 0;
    int16_t v_i16 = 0;
    uint16_t v_u16 = 0;
    int32_t v_i32 = 0;
    uint32_t v_u32 = 0;
    int64_t v_i64 = 0;
    uint64_t v_u64 = 0;

    auto expect_err = [&](const char* name, auto read_fn, auto* dst,
                          int expected_err) {
      coda_set_error(CODA_SUCCESS, nullptr);
      const coda_cursor c = GotoField(root, name);
      EXPECT_EQ(-1, read_fn(&c, dst)) << name;
      EXPECT_EQ(expected_err, coda_errno) << name;
    };

    expect_err("bad_i8", coda_bin_cursor_read_int8, &v_i8,
               CODA_ERROR_EXPRESSION);
    expect_err("bad_u8", coda_bin_cursor_read_uint8, &v_u8,
               CODA_ERROR_EXPRESSION);
    expect_err("bad_i16", coda_bin_cursor_read_int16, &v_i16,
               CODA_ERROR_EXPRESSION);
    expect_err("bad_u16", coda_bin_cursor_read_uint16, &v_u16,
               CODA_ERROR_EXPRESSION);
    expect_err("bad_i32", coda_bin_cursor_read_int32, &v_i32,
               CODA_ERROR_EXPRESSION);
    expect_err("bad_u32", coda_bin_cursor_read_uint32, &v_u32,
               CODA_ERROR_EXPRESSION);
    expect_err("bad_i64", coda_bin_cursor_read_int64, &v_i64,
               CODA_ERROR_EXPRESSION);
    expect_err("bad_u64", coda_bin_cursor_read_uint64, &v_u64,
               CODA_ERROR_EXPRESSION);

    expect_err("over_i8", coda_bin_cursor_read_int8, &v_i8, CODA_ERROR_PRODUCT);
    expect_err("over_u8", coda_bin_cursor_read_uint8, &v_u8,
               CODA_ERROR_PRODUCT);
    expect_err("over_i16", coda_bin_cursor_read_int16, &v_i16,
               CODA_ERROR_PRODUCT);
    expect_err("over_u16", coda_bin_cursor_read_uint16, &v_u16,
               CODA_ERROR_PRODUCT);
    expect_err("over_i32", coda_bin_cursor_read_int32, &v_i32,
               CODA_ERROR_PRODUCT);
    expect_err("over_u32", coda_bin_cursor_read_uint32, &v_u32,
               CODA_ERROR_PRODUCT);
    expect_err("over_i64", coda_bin_cursor_read_int64, &v_i64,
               CODA_ERROR_PRODUCT);
    expect_err("over_u64", coda_bin_cursor_read_uint64, &v_u64,
               CODA_ERROR_PRODUCT);
  }

  // 6. Truncated file OOB read paths (both read_bytes and read_bits paths).
  TempFile trunc_file("coda_bin_cursor_ints_trunc.dat");
  const std::vector<uint8_t> trunc_bytes(bw.bytes().begin(),
                                         bw.bytes().begin() + kHeaderBytes);
  ASSERT_TRUE(trunc_file.WriteBytes(trunc_bytes));

  for (const int use_mmap : {1, 0}) {
    SCOPED_TRACE(use_mmap);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    ScopedProduct trunc_prod;
    ASSERT_EQ(CODA_SUCCESS,
              coda_open(trunc_file.c_str(), trunc_prod.OutParam()));
    coda_cursor t_root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&t_root, trunc_prod.get()));

    int8_t v_i8 = 0;
    uint8_t v_u8 = 0;
    int16_t v_i16 = 0;
    uint16_t v_u16 = 0;
    int32_t v_i32 = 0;
    uint32_t v_u32 = 0;
    int64_t v_i64 = 0;
    uint64_t v_u64 = 0;

    auto expect_oob = [&](const char* name, auto read_fn, auto* dst) {
      coda_set_error(CODA_SUCCESS, nullptr);
      const coda_cursor c = GotoField(t_root, name);
      EXPECT_EQ(-1, read_fn(&c, dst)) << name;
      EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno) << name;
    };

    for (const char* name : {"i8_neg", "u_i8"}) {
      expect_oob(name, coda_bin_cursor_read_int8, &v_i8);
    }
    for (const char* name : {"u8_max", "u_u8"}) {
      expect_oob(name, coda_bin_cursor_read_uint8, &v_u8);
    }
    for (const char* name : {"i16_be", "u_i16_be"}) {
      expect_oob(name, coda_bin_cursor_read_int16, &v_i16);
    }
    for (const char* name : {"u16_be", "u_u16_be"}) {
      expect_oob(name, coda_bin_cursor_read_uint16, &v_u16);
    }
    for (const char* name : {"i32_be", "u_i32_be"}) {
      expect_oob(name, coda_bin_cursor_read_int32, &v_i32);
    }
    for (const char* name : {"u32_be", "u_u32_be"}) {
      expect_oob(name, coda_bin_cursor_read_uint32, &v_u32);
    }
    for (const char* name : {"i64_be", "u_i64_be"}) {
      expect_oob(name, coda_bin_cursor_read_int64, &v_i64);
    }
    for (const char* name : {"u64_be", "u_u64_be"}) {
      expect_oob(name, coda_bin_cursor_read_uint64, &v_u64);
    }
  }

  // 7. Unsized integers (bit_size == -1, size_expr == NULL) fail with PRODUCT.
  ScopedProductDefinition def(
      coda_product_definition_new("UnsizedInts", coda_format_binary, 1));
  ASSERT_NE(nullptr, def);
  ScopedType root_type(
      reinterpret_cast<coda_type*>(coda_type_record_new(coda_format_binary)));
  auto* rec = reinterpret_cast<coda_type_record*>(root_type.get());

  auto add_unsized_int = [&](const char* name, coda_native_type nt) {
    ScopedType t(reinterpret_cast<coda_type*>(
        coda_type_number_new(coda_format_binary, coda_integer_class)));
    ASSERT_NE(nullptr, t);
    ASSERT_EQ(CODA_SUCCESS, coda_type_set_read_type(t.get(), nt));
    ASSERT_TRUE(AddRecordField(rec, name, t.get()));
  };
  ASSERT_NO_FATAL_FAILURE(add_unsized_int("ui8", coda_native_type_int8));
  ASSERT_NO_FATAL_FAILURE(add_unsized_int("uu8", coda_native_type_uint8));
  ASSERT_NO_FATAL_FAILURE(add_unsized_int("ui16", coda_native_type_int16));
  ASSERT_NO_FATAL_FAILURE(add_unsized_int("uu16", coda_native_type_uint16));
  ASSERT_NO_FATAL_FAILURE(add_unsized_int("ui32", coda_native_type_int32));
  ASSERT_NO_FATAL_FAILURE(add_unsized_int("uu32", coda_native_type_uint32));
  ASSERT_NO_FATAL_FAILURE(add_unsized_int("ui64", coda_native_type_int64));
  ASSERT_NO_FATAL_FAILURE(add_unsized_int("uu64", coda_native_type_uint64));
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_definition_set_root_type(def.get(), root_type.get()));

  coda_product* raw_bin = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_bin_open(dat_file.c_str(),
                          static_cast<int64_t>(bw.bytes().size()), &raw_bin));
  ScopedBinProduct bin_prod(raw_bin);
  coda_product* raw_ptr = bin_prod.get();
  ASSERT_EQ(CODA_SUCCESS, coda_bin_reopen_with_definition(&raw_ptr, def.get()));

  coda_cursor u_root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&u_root, bin_prod.get()));
  int8_t v_i8 = 0;
  uint8_t v_u8 = 0;
  int16_t v_i16 = 0;
  uint16_t v_u16 = 0;
  int32_t v_i32 = 0;
  uint32_t v_u32 = 0;
  int64_t v_i64 = 0;
  uint64_t v_u64 = 0;
  auto expect_unsized = [&](const char* name, auto read_fn, auto* dst) {
    coda_set_error(CODA_SUCCESS, nullptr);
    const coda_cursor c = GotoField(u_root, name);
    EXPECT_EQ(-1, read_fn(&c, dst)) << name;
    EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno) << name;
  };
  expect_unsized("ui8", coda_bin_cursor_read_int8, &v_i8);
  expect_unsized("uu8", coda_bin_cursor_read_uint8, &v_u8);
  expect_unsized("ui16", coda_bin_cursor_read_int16, &v_i16);
  expect_unsized("uu16", coda_bin_cursor_read_uint16, &v_u16);
  expect_unsized("ui32", coda_bin_cursor_read_int32, &v_i32);
  expect_unsized("uu32", coda_bin_cursor_read_uint32, &v_u32);
  expect_unsized("ui64", coda_bin_cursor_read_int64, &v_i64);
  expect_unsized("uu64", coda_bin_cursor_read_uint64, &v_u64);
}

TEST(CodaBinCursorTest, ReadFloatDoubleCharStringBitsBytesAndConversions) {
  CodadefEnv env("coda_bin_cursor_other.codadef");
  constexpr std::string_view kXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinOtherProd" format="binary">
  <cd:Record>
    <cd:Field name="magic"><cd:Text><cd:ByteSize>8</cd:ByteSize></cd:Text></cd:Field>
    <cd:Field name="zero"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="str_len"><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Field>
    <cd:Field name="f32_be"><cd:Float><cd:NativeType>float</cd:NativeType><cd:BitSize>32</cd:BitSize></cd:Float></cd:Field>
    <cd:Field name="f32_le"><cd:Float><cd:NativeType>float</cd:NativeType><cd:BitSize>32</cd:BitSize><cd:LittleEndian/></cd:Float></cd:Field>
    <cd:Field name="f32_nan"><cd:Float><cd:NativeType>float</cd:NativeType><cd:BitSize>32</cd:BitSize><cd:LittleEndian/></cd:Float></cd:Field>
    <cd:Field name="f32_pinf"><cd:Float><cd:NativeType>float</cd:NativeType><cd:BitSize>32</cd:BitSize></cd:Float></cd:Field>
    <cd:Field name="f64_be"><cd:Float><cd:NativeType>double</cd:NativeType><cd:BitSize>64</cd:BitSize></cd:Float></cd:Field>
    <cd:Field name="f64_le"><cd:Float><cd:NativeType>double</cd:NativeType><cd:BitSize>64</cd:BitSize><cd:LittleEndian/></cd:Float></cd:Field>
    <cd:Field name="f64_ninf"><cd:Float><cd:NativeType>double</cd:NativeType><cd:BitSize>64</cd:BitSize></cd:Float></cd:Field>
    <cd:Field name="f64_nan"><cd:Float><cd:NativeType>double</cd:NativeType><cd:BitSize>64</cd:BitSize><cd:LittleEndian/></cd:Float></cd:Field>
    <cd:Field name="conv_i16">
      <cd:Integer>
        <cd:NativeType>int16</cd:NativeType>
        <cd:BitSize>16</cd:BitSize>
        <cd:LittleEndian/>
        <cd:Conversion numerator="2" denominator="4" offset="10" invalid="-99">
          <cd:Unit>m</cd:Unit>
        </cd:Conversion>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="conv_inv">
      <cd:Integer>
        <cd:NativeType>int16</cd:NativeType>
        <cd:BitSize>16</cd:BitSize>
        <cd:LittleEndian/>
        <cd:Conversion numerator="2" denominator="4" offset="10" invalid="-99">
          <cd:Unit>m</cd:Unit>
        </cd:Conversion>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="ch_aligned"><cd:Text><cd:ByteSize>1</cd:ByteSize><cd:NativeType>char</cd:NativeType></cd:Text></cd:Field>
    <cd:Field name="str_fixed"><cd:Text><cd:ByteSize>6</cd:ByteSize></cd:Text></cd:Field>
    <cd:Field name="str_dyn"><cd:Text><cd:ByteSize>int(/str_len)</cd:ByteSize></cd:Text></cd:Field>
    <cd:Field name="str_empty"><cd:Text><cd:ByteSize>0</cd:ByteSize></cd:Text></cd:Field>
    <cd:Field name="raw_aligned"><cd:Raw><cd:BitSize>64</cd:BitSize></cd:Raw></cd:Field>
    <cd:Field name="pad3"><cd:Raw><cd:BitSize>3</cd:BitSize></cd:Raw></cd:Field>
    <cd:Field name="f32_unaligned"><cd:Float><cd:NativeType>float</cd:NativeType><cd:BitSize>32</cd:BitSize><cd:LittleEndian/></cd:Float></cd:Field>
    <cd:Field name="f64_unaligned"><cd:Float><cd:NativeType>double</cd:NativeType><cd:BitSize>64</cd:BitSize></cd:Float></cd:Field>
    <cd:Field name="ch_unaligned"><cd:Text><cd:ByteSize>1</cd:ByteSize><cd:NativeType>char</cd:NativeType></cd:Text></cd:Field>
    <cd:Field name="str_unaligned"><cd:Text><cd:ByteSize>4</cd:ByteSize></cd:Text></cd:Field>
    <cd:Field name="raw_unaligned"><cd:Raw><cd:BitSize>24</cd:BitSize></cd:Raw></cd:Field>
    <cd:Field name="bad_str"><cd:Text><cd:ByteSize>1 / int(/zero)</cd:ByteSize></cd:Text><cd:BitOffset>64</cd:BitOffset></cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";
  ASSERT_TRUE(env.InitWithXml("BINOTHR1", "BinOtherProd", kXml));
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  BitWriter bw;
  bw.WriteString("BINOTHR1");
  bw.WriteBits(0, 8);  // zero
  bw.WriteBits(4, 8);  // str_len
  const size_t kHeaderBytes = bw.bytes().size();

  bw.WriteBeBytes(std::bit_cast<uint32_t>(-12.5f), 4);
  bw.WriteLeBytes(std::bit_cast<uint32_t>(3.25f), 4);
  bw.WriteLeBytes(
      std::bit_cast<uint32_t>(std::numeric_limits<float>::quiet_NaN()), 4);
  bw.WriteBeBytes(
      std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity()), 4);
  bw.WriteBeBytes(std::bit_cast<uint64_t>(25.125), 8);
  bw.WriteLeBytes(std::bit_cast<uint64_t>(-6.75), 8);
  bw.WriteBeBytes(
      std::bit_cast<uint64_t>(-std::numeric_limits<double>::infinity()), 8);
  bw.WriteLeBytes(
      std::bit_cast<uint64_t>(std::numeric_limits<double>::quiet_NaN()), 8);
  bw.WriteLeBytes(static_cast<uint16_t>(int16_t{10}), 2);  // 10 * 2/4 + 10 = 15
  bw.WriteLeBytes(static_cast<uint16_t>(int16_t{-99}), 2);  // invalid -> NaN
  bw.WriteBits('K', 8);
  bw.WriteString("coda!!");
  bw.WriteString("test");
  bw.WriteBeBytes(0x1122334455667788ULL, 8);  // raw_aligned

  // Unaligned section (3 pad bits).
  bw.WriteBits(0b101, 3);
  bw.WriteLeBytes(std::bit_cast<uint32_t>(7.5f), 4);
  bw.WriteBeBytes(std::bit_cast<uint64_t>(-9.25), 8);
  bw.WriteBits('Q', 8);
  bw.WriteString("bits");
  bw.WriteBeBytes(0xAABBCCULL, 3);  // raw_unaligned (24 bits)
  bw.PadToByte();

  TempFile dat_file("coda_bin_cursor_other.dat");
  ASSERT_TRUE(dat_file.WriteBytes(bw.bytes()));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(dat_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // 1. Float and double (aligned, unaligned, BE/LE, NaN, +/-Inf).
  float fv = 0.0f;
  double dv = 0.0;
  const coda_cursor c_f32_be = GotoField(root, "f32_be");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_float(&c_f32_be, &fv));
  EXPECT_FLOAT_EQ(-12.5f, fv);
  const coda_cursor c_f32_le = GotoField(root, "f32_le");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_float(&c_f32_le, &fv));
  EXPECT_FLOAT_EQ(3.25f, fv);
  const coda_cursor c_f32_nan = GotoField(root, "f32_nan");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_float(&c_f32_nan, &fv));
  EXPECT_TRUE(std::isnan(fv));
  const coda_cursor c_f32_pinf = GotoField(root, "f32_pinf");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_float(&c_f32_pinf, &fv));
  EXPECT_TRUE(std::isinf(fv) && fv > 0.0f);
  const coda_cursor c_f32_un = GotoField(root, "f32_unaligned");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_float(&c_f32_un, &fv));
  EXPECT_FLOAT_EQ(7.5f, fv);

  const coda_cursor c_f64_be = GotoField(root, "f64_be");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_double(&c_f64_be, &dv));
  EXPECT_DOUBLE_EQ(25.125, dv);
  const coda_cursor c_f64_le = GotoField(root, "f64_le");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_double(&c_f64_le, &dv));
  EXPECT_DOUBLE_EQ(-6.75, dv);
  const coda_cursor c_f64_ninf = GotoField(root, "f64_ninf");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_double(&c_f64_ninf, &dv));
  EXPECT_TRUE(std::isinf(dv) && dv < 0.0);
  const coda_cursor c_f64_nan = GotoField(root, "f64_nan");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_double(&c_f64_nan, &dv));
  EXPECT_TRUE(std::isnan(dv));
  const coda_cursor c_f64_un = GotoField(root, "f64_unaligned");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_double(&c_f64_un, &dv));
  EXPECT_DOUBLE_EQ(-9.25, dv);

  // 2. Conversions via coda_cursor_read_double/int16 with perform_conversions
  // 1/0.
  const coda_cursor c_conv = GotoField(root, "conv_i16");
  const coda_cursor c_cinv = GotoField(root, "conv_inv");
  ASSERT_EQ(1, coda_get_option_perform_conversions());
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_conv, &dv));
  EXPECT_DOUBLE_EQ(15.0, dv);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c_conv, &fv));
  EXPECT_FLOAT_EQ(15.0f, fv);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_cinv, &dv));
  EXPECT_TRUE(std::isnan(dv));
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(0));
  int16_t raw_i16 = 0;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&c_conv, &raw_i16));
  EXPECT_EQ(10, raw_i16);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_cinv, &dv));
  EXPECT_DOUBLE_EQ(-99.0, dv);
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(1));

  // 3. Char and string (aligned, unaligned, dynamic, empty, truncated dst).
  char ch = '\0';
  const coda_cursor c_ch_a = GotoField(root, "ch_aligned");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_char(&c_ch_a, &ch));
  EXPECT_EQ('K', ch);
  const coda_cursor c_ch_u = GotoField(root, "ch_unaligned");
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_char(&c_ch_u, &ch));
  EXPECT_EQ('Q', ch);

  char sbuf[16]{};
  const coda_cursor c_str_fix = GotoField(root, "str_fixed");
  EXPECT_EQ(CODA_SUCCESS,
            coda_bin_cursor_read_string(&c_str_fix, sbuf, sizeof(sbuf)));
  EXPECT_STREQ("coda!!", sbuf);
  char trunc_buf[5]{};
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_string(&c_str_fix, trunc_buf,
                                                      sizeof(trunc_buf)));
  EXPECT_STREQ("coda", trunc_buf);
  char one_byte[1]{'X'};
  EXPECT_EQ(CODA_SUCCESS, coda_bin_cursor_read_string(&c_str_fix, one_byte, 1));
  EXPECT_EQ('\0', one_byte[0]);

  const coda_cursor c_str_dyn = GotoField(root, "str_dyn");
  EXPECT_EQ(CODA_SUCCESS,
            coda_bin_cursor_read_string(&c_str_dyn, sbuf, sizeof(sbuf)));
  EXPECT_STREQ("test", sbuf);

  const coda_cursor c_str_emp = GotoField(root, "str_empty");
  sbuf[0] = 'X';
  EXPECT_EQ(CODA_SUCCESS,
            coda_bin_cursor_read_string(&c_str_emp, sbuf, sizeof(sbuf)));
  EXPECT_EQ('\0', sbuf[0]);

  const coda_cursor c_str_un = GotoField(root, "str_unaligned");
  EXPECT_EQ(CODA_SUCCESS,
            coda_bin_cursor_read_string(&c_str_un, sbuf, sizeof(sbuf)));
  EXPECT_STREQ("bits", sbuf);

  const coda_cursor c_str_bad = GotoField(root, "bad_str");
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_bin_cursor_read_string(&c_str_bad, sbuf, sizeof(sbuf)));
  EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);

  // 4. Bits and bytes (aligned and unaligned).
  const coda_cursor c_raw_a = GotoField(root, "raw_aligned");
  std::array<uint8_t, 4> bytes4{};
  EXPECT_EQ(CODA_SUCCESS,
            coda_bin_cursor_read_bytes(&c_raw_a, bytes4.data(), 2, 4));
  EXPECT_THAT(bytes4, ElementsAre(0x33, 0x44, 0x55, 0x66));

  std::array<uint8_t, 5> bits5{};
  EXPECT_EQ(CODA_SUCCESS,
            coda_bin_cursor_read_bits(&c_raw_a, bits5.data(), 4, 36));
  EXPECT_THAT(bits5, ElementsAre(0x01, 0x22, 0x33, 0x44, 0x55));

  std::array<uint8_t, 5> bits5_shifted{};
  EXPECT_EQ(CODA_SUCCESS,
            coda_bin_cursor_read_bits(&c_raw_a, bits5_shifted.data(), 3, 36));
  EXPECT_THAT(bits5_shifted, ElementsAre(0x08, 0x91, 0x19, 0xA2, 0x2A));

  const coda_cursor c_raw_u = GotoField(root, "raw_unaligned");
  std::array<uint8_t, 2> bytes2{};
  EXPECT_EQ(CODA_SUCCESS,
            coda_bin_cursor_read_bytes(&c_raw_u, bytes2.data(), 1, 2));
  EXPECT_THAT(bytes2, ElementsAre(0xBB, 0xCC));

  // 5. Truncated file OOB read paths for float, double, char, string, bits,
  //    bytes.
  TempFile trunc_file("coda_bin_cursor_other_trunc.dat");
  ASSERT_TRUE(trunc_file.WriteBytes(
      std::span<const uint8_t>(bw.bytes().data(), kHeaderBytes)));
  ScopedProduct trunc_prod;
  ASSERT_EQ(CODA_SUCCESS, coda_open(trunc_file.c_str(), trunc_prod.OutParam()));
  coda_cursor t_root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&t_root, trunc_prod.get()));

  for (const char* name : {"f32_be", "f32_unaligned"}) {
    coda_set_error(CODA_SUCCESS, nullptr);
    const coda_cursor c = GotoField(t_root, name);
    EXPECT_EQ(-1, coda_bin_cursor_read_float(&c, &fv));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }
  for (const char* name : {"f64_be", "f64_unaligned"}) {
    coda_set_error(CODA_SUCCESS, nullptr);
    const coda_cursor c = GotoField(t_root, name);
    EXPECT_EQ(-1, coda_bin_cursor_read_double(&c, &dv));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }
  for (const char* name : {"ch_aligned", "ch_unaligned"}) {
    coda_set_error(CODA_SUCCESS, nullptr);
    const coda_cursor c = GotoField(t_root, name);
    EXPECT_EQ(-1, coda_bin_cursor_read_char(&c, &ch));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }
  {
    coda_set_error(CODA_SUCCESS, nullptr);
    const coda_cursor c = GotoField(t_root, "str_fixed");
    EXPECT_EQ(-1, coda_bin_cursor_read_string(&c, sbuf, sizeof(sbuf)));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }
  {
    coda_set_error(CODA_SUCCESS, nullptr);
    const coda_cursor c = GotoField(t_root, "raw_aligned");
    EXPECT_EQ(-1, coda_bin_cursor_read_bytes(&c, bytes4.data(), 0, 4));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
    EXPECT_EQ(-1, coda_bin_cursor_read_bits(&c, bits5.data(), 0, 36));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }

  // 6. Unsized text and non-byte-rounded dynamic text bit_size errors.
  ScopedProductDefinition def(
      coda_product_definition_new("BadTextDef", coda_format_binary, 1));
  ASSERT_NE(nullptr, def);
  ScopedType root_type(
      reinterpret_cast<coda_type*>(coda_type_record_new(coda_format_binary)));
  auto* rec = reinterpret_cast<coda_type_record*>(root_type.get());

  ScopedType unsized_text(
      reinterpret_cast<coda_type*>(coda_type_text_new(coda_format_binary)));
  ScopedType odd_text(
      reinterpret_cast<coda_type*>(coda_type_text_new(coda_format_binary)));
  ScopedExpr odd_expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("6 + 7", odd_expr.OutParam()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_set_bit_size_expression(odd_text.get(), odd_expr.get()));
  odd_expr.release();
  ASSERT_TRUE(AddRecordField(rec, "unsized", unsized_text.get()));
  ASSERT_TRUE(AddRecordField(rec, "odd_bits", odd_text.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_definition_set_root_type(def.get(), root_type.get()));

  coda_product* raw_bin = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_bin_open(dat_file.c_str(),
                          static_cast<int64_t>(bw.bytes().size()), &raw_bin));
  ScopedBinProduct bin_prod(raw_bin);
  coda_product* raw_ptr = bin_prod.get();
  ASSERT_EQ(CODA_SUCCESS, coda_bin_reopen_with_definition(&raw_ptr, def.get()));

  coda_cursor b_root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&b_root, bin_prod.get()));

  coda_set_error(CODA_SUCCESS, nullptr);
  const coda_cursor c_unsized = GotoField(b_root, "unsized");
  EXPECT_EQ(-1, coda_bin_cursor_read_string(&c_unsized, sbuf, sizeof(sbuf)));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  const coda_cursor c_odd = GotoField(b_root, "odd_bits");
  EXPECT_EQ(-1, coda_bin_cursor_read_string(&c_odd, sbuf, sizeof(sbuf)));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("product error"));
}

TEST(CodaBinCursorTest, ReadArraysAndPartialArraysBinaryAndAsciiBaseTypes) {
  CodadefEnv env("coda_bin_cursor_arrays.codadef");
  constexpr std::string_view kXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinArrProd" format="binary">
  <cd:Record>
    <cd:Field name="magic"><cd:Text><cd:ByteSize>8</cd:ByteSize></cd:Text></cd:Field>
    <cd:Field name="b_i8"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Integer><cd:NativeType>int8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Array></cd:Field>
    <cd:Field name="b_u8"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Integer><cd:NativeType>uint8</cd:NativeType><cd:BitSize>8</cd:BitSize></cd:Integer></cd:Array></cd:Field>
    <cd:Field name="b_i16"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Integer><cd:NativeType>int16</cd:NativeType><cd:BitSize>16</cd:BitSize><cd:LittleEndian/></cd:Integer></cd:Array></cd:Field>
    <cd:Field name="b_u16"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Integer><cd:NativeType>uint16</cd:NativeType><cd:BitSize>16</cd:BitSize></cd:Integer></cd:Array></cd:Field>
    <cd:Field name="b_i32"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Integer><cd:NativeType>int32</cd:NativeType><cd:BitSize>32</cd:BitSize><cd:LittleEndian/></cd:Integer></cd:Array></cd:Field>
    <cd:Field name="b_u32"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Integer><cd:NativeType>uint32</cd:NativeType><cd:BitSize>32</cd:BitSize></cd:Integer></cd:Array></cd:Field>
    <cd:Field name="b_i64"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Integer><cd:NativeType>int64</cd:NativeType><cd:BitSize>64</cd:BitSize><cd:LittleEndian/></cd:Integer></cd:Array></cd:Field>
    <cd:Field name="b_u64"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Integer><cd:NativeType>uint64</cd:NativeType><cd:BitSize>64</cd:BitSize></cd:Integer></cd:Array></cd:Field>
    <cd:Field name="b_f32"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Float><cd:NativeType>float</cd:NativeType><cd:BitSize>32</cd:BitSize><cd:LittleEndian/></cd:Float></cd:Array></cd:Field>
    <cd:Field name="b_f64"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Float><cd:NativeType>double</cd:NativeType><cd:BitSize>64</cd:BitSize><cd:LittleEndian/></cd:Float></cd:Array></cd:Field>
    <cd:Field name="b_ch"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Text><cd:ByteSize>1</cd:ByteSize><cd:NativeType>char</cd:NativeType></cd:Text></cd:Array></cd:Field>
    <cd:Field name="a_i8"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Integer format="ascii"><cd:ByteSize>4</cd:ByteSize><cd:NativeType>int8</cd:NativeType></cd:Integer></cd:Array></cd:Field>
    <cd:Field name="a_u8"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Integer format="ascii"><cd:ByteSize>4</cd:ByteSize><cd:NativeType>uint8</cd:NativeType></cd:Integer></cd:Array></cd:Field>
    <cd:Field name="a_i16"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Integer format="ascii"><cd:ByteSize>6</cd:ByteSize><cd:NativeType>int16</cd:NativeType></cd:Integer></cd:Array></cd:Field>
    <cd:Field name="a_u16"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Integer format="ascii"><cd:ByteSize>6</cd:ByteSize><cd:NativeType>uint16</cd:NativeType></cd:Integer></cd:Array></cd:Field>
    <cd:Field name="a_i32"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Integer format="ascii"><cd:ByteSize>6</cd:ByteSize><cd:NativeType>int32</cd:NativeType></cd:Integer></cd:Array></cd:Field>
    <cd:Field name="a_u32"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Integer format="ascii"><cd:ByteSize>6</cd:ByteSize><cd:NativeType>uint32</cd:NativeType></cd:Integer></cd:Array></cd:Field>
    <cd:Field name="a_i64"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Integer format="ascii"><cd:ByteSize>6</cd:ByteSize><cd:NativeType>int64</cd:NativeType></cd:Integer></cd:Array></cd:Field>
    <cd:Field name="a_u64"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Integer format="ascii"><cd:ByteSize>6</cd:ByteSize><cd:NativeType>uint64</cd:NativeType></cd:Integer></cd:Array></cd:Field>
    <cd:Field name="a_f32"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Float format="ascii"><cd:ByteSize>6</cd:ByteSize><cd:NativeType>float</cd:NativeType></cd:Float></cd:Array></cd:Field>
    <cd:Field name="a_f64"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Float format="ascii"><cd:ByteSize>6</cd:ByteSize><cd:NativeType>double</cd:NativeType></cd:Float></cd:Array></cd:Field>
    <cd:Field name="a_ch"><cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension><cd:Text format="ascii"><cd:ByteSize>1</cd:ByteSize><cd:NativeType>char</cd:NativeType></cd:Text></cd:Array></cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";
  ASSERT_TRUE(env.InitWithXml("BINARRS1", "BinArrProd", kXml));
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  constexpr std::array<int8_t, 6> kI8 = {-10, 20, -30, 40, -50, 60};
  constexpr std::array<uint8_t, 6> kU8 = {10, 20, 30, 40, 50, 60};
  constexpr std::array<int16_t, 6> kI16 = {-100, 200, -300, 400, -500, 600};
  constexpr std::array<uint16_t, 6> kU16 = {1000, 2000, 3000, 4000, 5000, 6000};
  constexpr std::array<int32_t, 6> kI32 = {-10, 20, -30, 40, -50, 60};
  constexpr std::array<uint32_t, 6> kU32 = {100, 200, 300, 400, 500, 600};
  constexpr std::array<int64_t, 6> kI64 = {-1000, 2000,  -3000,
                                           4000,  -5000, 6000};
  constexpr std::array<uint64_t, 6> kU64 = {1111, 2222, 3333, 4444, 5555, 6666};
  constexpr std::array<float, 6> kF32 = {1.5f, -2.5f, 3.5f, -4.5f, 5.5f, -6.5f};
  constexpr std::array<double, 6> kF64 = {10.5,  -20.5, 30.5,
                                          -40.5, 50.5,  -60.5};
  constexpr std::array<char, 6> kCh = {'A', 'B', 'C', 'D', 'E', 'F'};

  std::vector<uint8_t> bin;
  AppendString(bin, "BINARRS1");
  for (int8_t v : kI8) bin.push_back(static_cast<uint8_t>(v));
  for (uint8_t v : kU8) bin.push_back(v);
  for (int16_t v : kI16) AppendLe16(bin, static_cast<uint16_t>(v));
  for (uint16_t v : kU16) AppendBe16(bin, v);
  for (int32_t v : kI32) AppendLe32(bin, static_cast<uint32_t>(v));
  for (uint32_t v : kU32) AppendBe32(bin, v);
  for (int64_t v : kI64) AppendLe64(bin, static_cast<uint64_t>(v));
  for (uint64_t v : kU64) AppendBe64(bin, v);
  for (float v : kF32) AppendLeFloat(bin, v);
  for (double v : kF64) AppendLeDouble(bin, v);
  for (char v : kCh) bin.push_back(static_cast<uint8_t>(v));

  // Embedded ASCII-formatted arrays inside the binary product.
  AppendString(bin, " -10  20 -30  40 -50  60");              // a_i8
  AppendString(bin, "  10  20  30  40  50  60");              // a_u8
  AppendString(bin, "  -100   200  -300   400  -500   600");  // a_i16
  AppendString(bin, "  1000  2000  3000  4000  5000  6000");  // a_u16
  AppendString(bin, "   -10    20   -30    40   -50    60");  // a_i32
  AppendString(bin, "   100   200   300   400   500   600");  // a_u32
  AppendString(bin, " -1000  2000 -3000  4000 -5000  6000");  // a_i64
  AppendString(bin, "  1111  2222  3333  4444  5555  6666");  // a_u64
  AppendString(bin, "   1.5  -2.5   3.5  -4.5   5.5  -6.5");  // a_f32
  AppendString(bin, "  10.5 -20.5  30.5 -40.5  50.5 -60.5");  // a_f64
  AppendString(bin, "ABCDEF");                                // a_ch
  // Trailing newline keeps ascii boundary check (< bit_size) satisfied.
  bin.push_back('\n');

  TempFile dat_file("coda_bin_cursor_arrays.dat");
  ASSERT_TRUE(dat_file.WriteBytes(bin));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(dat_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  auto to_fortran_2x3 = [](const auto& src) {
    using T = typename std::decay_t<decltype(src)>::value_type;
    return std::array<T, 6>{src[0], src[3], src[1], src[4], src[2], src[5]};
  };

  auto check_both = [&](const char* bin_name, const char* asc_name, auto arr_fn,
                        auto partial_fn, const auto& expected, auto sentinel) {
    using T = typename std::decay_t<decltype(expected)>::value_type;
    for (const char* name : {bin_name, asc_name}) {
      SCOPED_TRACE(name);
      const coda_cursor c = GotoField(root, name);

      std::vector<T> buf(6, sentinel);
      ASSERT_EQ(CODA_SUCCESS, arr_fn(&c, buf.data(), coda_array_ordering_c));
      EXPECT_THAT(buf, ElementsAreArray(expected));

      std::fill(buf.begin(), buf.end(), sentinel);
      ASSERT_EQ(CODA_SUCCESS,
                arr_fn(&c, buf.data(), coda_array_ordering_fortran));
      EXPECT_THAT(buf, ElementsAreArray(to_fortran_2x3(expected)));

      std::vector<T> pbuf(3, sentinel);
      ASSERT_EQ(CODA_SUCCESS, partial_fn(&c, 2, 3, pbuf.data()));
      EXPECT_THAT(pbuf, ElementsAre(expected[2], expected[3], expected[4]));

      std::vector<T> zero_buf(1, sentinel);
      ASSERT_EQ(CODA_SUCCESS, partial_fn(&c, 0, 0, zero_buf.data()));
      EXPECT_EQ(sentinel, zero_buf[0]);
    }
  };

  ASSERT_NO_FATAL_FAILURE(
      check_both("b_i8", "a_i8", coda_bin_cursor_read_int8_array,
                 coda_bin_cursor_read_int8_partial_array, kI8, int8_t{-99}));
  ASSERT_NO_FATAL_FAILURE(
      check_both("b_u8", "a_u8", coda_bin_cursor_read_uint8_array,
                 coda_bin_cursor_read_uint8_partial_array, kU8, uint8_t{99}));
  ASSERT_NO_FATAL_FAILURE(check_both(
      "b_i16", "a_i16", coda_bin_cursor_read_int16_array,
      coda_bin_cursor_read_int16_partial_array, kI16, int16_t{-999}));
  ASSERT_NO_FATAL_FAILURE(check_both(
      "b_u16", "a_u16", coda_bin_cursor_read_uint16_array,
      coda_bin_cursor_read_uint16_partial_array, kU16, uint16_t{999}));
  ASSERT_NO_FATAL_FAILURE(check_both(
      "b_i32", "a_i32", coda_bin_cursor_read_int32_array,
      coda_bin_cursor_read_int32_partial_array, kI32, int32_t{-999}));
  ASSERT_NO_FATAL_FAILURE(check_both(
      "b_u32", "a_u32", coda_bin_cursor_read_uint32_array,
      coda_bin_cursor_read_uint32_partial_array, kU32, uint32_t{999}));
  ASSERT_NO_FATAL_FAILURE(check_both(
      "b_i64", "a_i64", coda_bin_cursor_read_int64_array,
      coda_bin_cursor_read_int64_partial_array, kI64, int64_t{-999}));
  ASSERT_NO_FATAL_FAILURE(check_both(
      "b_u64", "a_u64", coda_bin_cursor_read_uint64_array,
      coda_bin_cursor_read_uint64_partial_array, kU64, uint64_t{999}));
  ASSERT_NO_FATAL_FAILURE(
      check_both("b_f32", "a_f32", coda_bin_cursor_read_float_array,
                 coda_bin_cursor_read_float_partial_array, kF32, -999.0f));
  ASSERT_NO_FATAL_FAILURE(
      check_both("b_f64", "a_f64", coda_bin_cursor_read_double_array,
                 coda_bin_cursor_read_double_partial_array, kF64, -999.0));
  ASSERT_NO_FATAL_FAILURE(
      check_both("b_ch", "a_ch", coda_bin_cursor_read_char_array,
                 coda_bin_cursor_read_char_partial_array, kCh, '?'));

  // Verify boundary_checks option (0 disables partial-array bounds checks).
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));
  const coda_cursor c_bi8 = GotoField(root, "b_i8");
  int8_t dummy_i8 = -99;
  EXPECT_EQ(CODA_SUCCESS,
            coda_cursor_read_int8_partial_array(&c_bi8, 99, 0, &dummy_i8));
  EXPECT_EQ(-99, dummy_i8);
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(1));

  // Truncated file triggers read_array() and read_partial_array() failures for
  // both binary and ASCII base types.
  TempFile trunc_file("coda_bin_cursor_arrays_trunc.dat");
  ASSERT_TRUE(trunc_file.WriteBytes(std::span<const uint8_t>(bin.data(), 8)));
  ScopedProduct trunc_prod;
  ASSERT_EQ(CODA_SUCCESS, coda_open(trunc_file.c_str(), trunc_prod.OutParam()));
  coda_cursor t_root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&t_root, trunc_prod.get()));

  auto check_oob = [&](const char* bin_name, const char* asc_name, auto arr_fn,
                       auto partial_fn, auto sentinel) {
    using T = decltype(sentinel);
    std::array<T, 6> buf{};
    for (const char* name : {bin_name, asc_name}) {
      coda_set_error(CODA_SUCCESS, nullptr);
      const coda_cursor c = GotoField(t_root, name);
      EXPECT_EQ(-1, arr_fn(&c, buf.data(), coda_array_ordering_c)) << name;
      EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno) << name;
      EXPECT_EQ(-1, partial_fn(&c, 0, 1, buf.data())) << name;
      EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno) << name;
    }
  };

  check_oob("b_i8", "a_i8", coda_bin_cursor_read_int8_array,
            coda_bin_cursor_read_int8_partial_array, int8_t{0});
  check_oob("b_u8", "a_u8", coda_bin_cursor_read_uint8_array,
            coda_bin_cursor_read_uint8_partial_array, uint8_t{0});
  check_oob("b_i16", "a_i16", coda_bin_cursor_read_int16_array,
            coda_bin_cursor_read_int16_partial_array, int16_t{0});
  check_oob("b_u16", "a_u16", coda_bin_cursor_read_uint16_array,
            coda_bin_cursor_read_uint16_partial_array, uint16_t{0});
  check_oob("b_i32", "a_i32", coda_bin_cursor_read_int32_array,
            coda_bin_cursor_read_int32_partial_array, int32_t{0});
  check_oob("b_u32", "a_u32", coda_bin_cursor_read_uint32_array,
            coda_bin_cursor_read_uint32_partial_array, uint32_t{0});
  check_oob("b_i64", "a_i64", coda_bin_cursor_read_int64_array,
            coda_bin_cursor_read_int64_partial_array, int64_t{0});
  check_oob("b_u64", "a_u64", coda_bin_cursor_read_uint64_array,
            coda_bin_cursor_read_uint64_partial_array, uint64_t{0});
  check_oob("b_f32", "a_f32", coda_bin_cursor_read_float_array,
            coda_bin_cursor_read_float_partial_array, 0.0f);
  check_oob("b_f64", "a_f64", coda_bin_cursor_read_double_array,
            coda_bin_cursor_read_double_partial_array, 0.0);
  check_oob("b_ch", "a_ch", coda_bin_cursor_read_char_array,
            coda_bin_cursor_read_char_partial_array, '\0');
}

TEST(CodaBinCursorTest, ReadSpecialTypesTimeVsfComplexAndNavigation) {
  CodadefEnv env("coda_bin_cursor_special.codadef");
  constexpr std::string_view kXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinSpecProd" format="binary">
  <cd:Record>
    <cd:Field name="magic"><cd:Text><cd:ByteSize>8</cd:ByteSize></cd:Text></cd:Field>
    <cd:Field name="t_envisat"><cd:Time timeformat="binary_envisat_datetime"/></cd:Field>
    <cd:Field name="t_eps"><cd:Time timeformat="binary_eps_datetime"/></cd:Field>
    <cd:Field name="vsf">
      <cd:VSFInteger>
        <cd:ScaleFactor><cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>int8</cd:NativeType></cd:Integer></cd:ScaleFactor>
        <cd:Integer><cd:BitSize>16</cd:BitSize><cd:NativeType>int16</cd:NativeType><cd:LittleEndian/></cd:Integer>
      </cd:VSFInteger>
    </cd:Field>
    <cd:Field name="cplx">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>2</cd:Dimension>
        <cd:Complex>
          <cd:Float><cd:BitSize>64</cd:BitSize><cd:NativeType>double</cd:NativeType><cd:LittleEndian/></cd:Float>
        </cd:Complex>
      </cd:Array>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";
  ASSERT_TRUE(env.InitWithXml("BINSPEC1", "BinSpecProd", kXml));
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  std::vector<uint8_t> bin;
  AppendString(bin, "BINSPEC1");
  // t_envisat: days=1, seconds=3600, microseconds=500000 -> 90000.5 s
  AppendBe32(bin, 1);
  AppendBe32(bin, 3600);
  AppendBe32(bin, 500000);
  // t_eps: days=2 (uint16 BE), milliseconds=500 (uint32 BE) -> 172800.5 s
  AppendBe16(bin, 2);
  AppendBe32(bin, 500);
  // vsf: scale=2 (int8), val=1234 (int16 LE) -> 12.34
  bin.push_back(2);
  AppendLe16(bin, 1234);
  // cplx[2][2]: (1,10), (2,20), (3,30), (4,40)
  for (int i = 1; i <= 4; ++i) {
    AppendLeDouble(bin, static_cast<double>(i));
    AppendLeDouble(bin, static_cast<double>(i * 10));
  }

  TempFile dat_file("coda_bin_cursor_special.dat");
  ASSERT_TRUE(dat_file.WriteBytes(bin));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(dat_file.c_str(), product.OutParam()));

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  // Navigate fields sequentially using goto_record_field_by_index / next.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_index(&cursor, 1));
  double t_env = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &t_env));
  EXPECT_DOUBLE_EQ(90000.5, t_env);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  double t_eps = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &t_eps));
  EXPECT_DOUBLE_EQ(172800.5, t_eps);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  double vsf_val = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &vsf_val));
  EXPECT_DOUBLE_EQ(12.34, vsf_val);

  // Unwrap VSF base record and inspect raw scale_factor & value.
  coda_cursor vsf_base = cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_use_base_type_of_special_type(&vsf_base));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_record_field(&vsf_base));
  int8_t sf = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&vsf_base, &sf));
  EXPECT_EQ(2, sf);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&vsf_base));
  int16_t raw_v = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&vsf_base, &raw_v));
  EXPECT_EQ(1234, raw_v);

  // Complex array pair and split reads in C and Fortran orderings.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/cplx"));
  std::array<double, 8> pairs{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_complex_double_pairs_array(
                              &cursor, pairs.data(), coda_array_ordering_c));
  EXPECT_THAT(pairs, ElementsAre(1, 10, 2, 20, 3, 30, 4, 40));

  std::array<double, 4> re{};
  std::array<double, 4> im{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_complex_double_split_array(
                &cursor, re.data(), im.data(), coda_array_ordering_fortran));
  EXPECT_THAT(re, ElementsAre(1, 3, 2, 4));
  EXPECT_THAT(im, ElementsAre(10, 30, 20, 40));

  const std::array<long, 2> subs = {1, 0};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element(&cursor, 2, subs.data()));
  std::array<double, 2> pair{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_complex_double_pair(&cursor, pair.data()));
  EXPECT_THAT(pair, ElementsAre(3.0, 30.0));

  double s_re = 0.0;
  double s_im = 0.0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_complex_double_split(&cursor, &s_re, &s_im));
  EXPECT_DOUBLE_EQ(3.0, s_re);
  EXPECT_DOUBLE_EQ(30.0, s_im);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));

  // Verify bypass_special_types option unwraps VSF directly to record.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_bypass_special_types(1));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/vsf"));
  coda_type_class tc = coda_special_class;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &tc));
  EXPECT_EQ(coda_record_class, tc);
}

// TODO(schwehr): 98.3% line coverage in libcoda/coda-bin-cursor.c (692/704);
// remaining 12 missed lines (102 and the 11 scalar endian-conversion error
// branches) are OOM/defensive checks already validated earlier in cursor/read
// paths.

}  // namespace
}  // namespace stcorp_coda
