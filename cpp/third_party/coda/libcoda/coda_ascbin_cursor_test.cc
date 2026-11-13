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
// Tests CODA ASCII/binary cursor navigation, offset calculation, union
// selection, array subscripting, dynamic bit-size computation, special-type
// unwrapping, and error paths (libcoda/coda-ascbin-cursor.c).
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-ascbin-cursor.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-ascbin.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-cursor.c

#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-ascbin.h"
#include "third_party/stcorp_coda/libcoda/coda-type.h"
}  // extern "C"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"

namespace stcorp_coda {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

struct TypeDeleter {
  void operator()(coda_type* type) const { coda_type_release(type); }
};
using ScopedType = std::unique_ptr<coda_type, TypeDeleter>;

constexpr std::string_view kIndexXml = R"(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="AscbinTestClass">
  <cd:ProductType name="FixedBin">
    <cd:ProductDefinition id="fixed_bin" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">ABIN</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="DynRecBin">
    <cd:ProductDefinition id="dyn_rec_bin" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">DYN1</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="UnionBin">
    <cd:ProductDefinition id="union_bin" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">UNI1</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="ArrayBin">
    <cd:ProductDefinition id="array_bin" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">ARR1</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="ErrBin">
    <cd:ProductDefinition id="err_bin" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">ERR1</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="AscProd">
    <cd:ProductDefinition id="asc_prod" format="ascii" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">ASC1</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)";

// Fixed-layout binary product with bit-packed fields, raw bytes, and a complex
// special type.
constexpr std::string_view kFixedBinXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="fixed_bin" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>4</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="bits3">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>3</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="bits5">
      <cd:Integer>
        <cd:NativeType>int8</cd:NativeType>
        <cd:BitSize>5</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="u16">
      <cd:Integer>
        <cd:NativeType>uint16</cd:NativeType>
        <cd:BitSize>16</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="raw2">
      <cd:Raw>
        <cd:BitSize>16</cd:BitSize>
      </cd:Raw>
    </cd:Field>
    <cd:Field name="cplx">
      <cd:Complex>
        <cd:Float>
          <cd:NativeType>float</cd:NativeType>
          <cd:BitSize>32</cd:BitSize>
        </cd:Float>
      </cd:Complex>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";

// Dynamic record product exercising bit_offset_expr, available_expr, chained
// dynamic offsets, fixed-offset-after-dynamic records, and record size_expr.
constexpr std::string_view kDynRecBinXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="dyn_rec_bin" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>4</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="flag">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="opt_flag">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="len1">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="len2">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="offset_bytes">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="signed_val">
      <cd:Integer>
        <cd:NativeType>int8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="sub">
      <cd:Record>
        <cd:Field name="first_opt">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
          <cd:Available>int(/flag) == 1</cd:Available>
          <cd:BitOffset>0</cd:BitOffset>
        </cd:Field>
        <cd:Field name="dyn_text1">
          <cd:Text>
            <cd:ByteSize>int(/len1)</cd:ByteSize>
          </cd:Text>
          <cd:BitOffset>8</cd:BitOffset>
        </cd:Field>
        <cd:Field name="dyn_text2">
          <cd:Text>
            <cd:ByteSize>int(/len2)</cd:ByteSize>
          </cd:Text>
        </cd:Field>
        <cd:Field name="opt_mid">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
          <cd:Available>int(/opt_flag) == 1</cd:Available>
        </cd:Field>
        <cd:Field name="tail_u8">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="opt_at_expr">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
          <cd:Available>int(/flag) == 1</cd:Available>
          <cd:BitOffset>int(/offset_bytes) * 8</cd:BitOffset>
        </cd:Field>
        <cd:Field name="after_expr">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:Field>
      </cd:Record>
      <cd:BitOffset>80</cd:BitOffset>
    </cd:Field>
    <cd:Field name="fixed_avail_rec">
      <cd:Record>
        <cd:Field name="a">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="b">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
          <cd:Available>int(/flag) == 1</cd:Available>
        </cd:Field>
        <cd:Field name="c">
          <cd:Text>
            <cd:ByteSize>int(/len1)</cd:ByteSize>
          </cd:Text>
        </cd:Field>
      </cd:Record>
      <cd:BitOffset>80</cd:BitOffset>
    </cd:Field>
    <cd:Field name="sized_rec">
      <cd:Record>
        <cd:BitSize>int(/signed_val) * 8</cd:BitSize>
        <cd:Field name="v">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="dyn_tail">
          <cd:Text>
            <cd:ByteSize>int(/len1)</cd:ByteSize>
          </cd:Text>
        </cd:Field>
      </cd:Record>
      <cd:BitOffset>80</cd:BitOffset>
    </cd:Field>
    <cd:Field name="empty_rec">
      <cd:Record/>
      <cd:BitOffset>80</cd:BitOffset>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";

// Union product exercising dynamic-size unions, fixed-size unions, and
// non-byte-aligned union offsets.
constexpr std::string_view kUnionBinXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="union_bin" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>4</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="tag">
      <cd:Integer>
        <cd:NativeType>int8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="len">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="dyn_union">
      <cd:Union>
        <cd:FieldExpression>int(/tag)</cd:FieldExpression>
        <cd:Field name="as_u8">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="as_u16">
          <cd:Integer>
            <cd:NativeType>uint16</cd:NativeType>
            <cd:BitSize>16</cd:BitSize>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="as_text">
          <cd:Text>
            <cd:ByteSize>int(/len)</cd:ByteSize>
          </cd:Text>
        </cd:Field>
      </cd:Union>
      <cd:BitOffset>48</cd:BitOffset>
    </cd:Field>
    <cd:Field name="fixed_union">
      <cd:Union>
        <cd:FieldExpression>int(/tag)</cd:FieldExpression>
        <cd:Field name="f0">
          <cd:Integer>
            <cd:NativeType>uint16</cd:NativeType>
            <cd:BitSize>16</cd:BitSize>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="f1">
          <cd:Integer>
            <cd:NativeType>int16</cd:NativeType>
            <cd:BitSize>16</cd:BitSize>
          </cd:Integer>
        </cd:Field>
      </cd:Union>
      <cd:BitOffset>80</cd:BitOffset>
    </cd:Field>
    <cd:Field name="pad3">
      <cd:Raw>
        <cd:BitSize>3</cd:BitSize>
      </cd:Raw>
      <cd:BitOffset>96</cd:BitOffset>
    </cd:Field>
    <cd:Field name="unaligned_union">
      <cd:Union>
        <cd:FieldExpression>int(/tag)</cd:FieldExpression>
        <cd:Field name="b0">
          <cd:Raw>
            <cd:BitSize>5</cd:BitSize>
          </cd:Raw>
        </cd:Field>
        <cd:Field name="b1">
          <cd:Raw>
            <cd:BitSize>5</cd:BitSize>
          </cd:Raw>
        </cd:Field>
      </cd:Union>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";

// Array product exercising fixed 1D, mixed fixed/variable 2D, zero-sized, and
// variable-size record-element arrays.
constexpr std::string_view kArrayBinXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="array_bin" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>4</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="cols">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="count">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="arr_fixed">
      <cd:Array>
        <cd:Dimension>3</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int16</cd:NativeType>
          <cd:BitSize>16</cd:BitSize>
          <cd:LittleEndian/>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="arr_mixed_2d">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Dimension>int(/cols)</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>uint8</cd:NativeType>
          <cd:BitSize>8</cd:BitSize>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="arr_var_rec">
      <cd:Array>
        <cd:Dimension>int(/count)</cd:Dimension>
        <cd:Record>
          <cd:Field name="elen">
            <cd:Integer>
              <cd:NativeType>uint8</cd:NativeType>
              <cd:BitSize>8</cd:BitSize>
            </cd:Integer>
          </cd:Field>
          <cd:Field name="etext">
            <cd:Text>
              <cd:ByteSize>int(../elen)</cd:ByteSize>
            </cd:Text>
          </cd:Field>
        </cd:Record>
      </cd:Array>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";

// Error-path binary product whose expressions divide by /denom (set to 0 in the
// file) or read negative /signed_dim (set to -2) to trigger runtime failures.
constexpr std::string_view kErrBinXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="err_bin" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>4</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="denom">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="signed_dim">
      <cd:Integer>
        <cd:NativeType>int8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="r_expr_avail_and_off">
      <cd:Record>
        <cd:Field name="f0">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
          <cd:Available>1 / int(/denom) == 1</cd:Available>
          <cd:BitOffset>0</cd:BitOffset>
        </cd:Field>
        <cd:Field name="f1">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
          <cd:BitOffset>8 / int(/denom)</cd:BitOffset>
        </cd:Field>
        <cd:Field name="f2">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:Field>
      </cd:Record>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
    <cd:Field name="r_next_avail_with_expr_off">
      <cd:Record>
        <cd:Field name="f0">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="f1">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
          <cd:Available>1 / int(/denom) == 1</cd:Available>
          <cd:BitOffset>8</cd:BitOffset>
        </cd:Field>
      </cd:Record>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
    <cd:Field name="r_next_off_err">
      <cd:Record>
        <cd:Field name="f0">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="f1">
          <cd:Text>
            <cd:ByteSize>int(/denom)</cd:ByteSize>
          </cd:Text>
          <cd:BitOffset>8 / int(/denom)</cd:BitOffset>
        </cd:Field>
      </cd:Record>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
    <cd:Field name="r_fixed_off_avail_err">
      <cd:Record>
        <cd:Field name="f0">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="f1">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
          <cd:Available>1 / int(/denom) == 1</cd:Available>
        </cd:Field>
        <cd:Field name="f2">
          <cd:Text>
            <cd:ByteSize>int(/denom)</cd:ByteSize>
          </cd:Text>
        </cd:Field>
      </cd:Record>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
    <cd:Field name="r_dyn_walk_err">
      <cd:Record>
        <cd:Field name="bad_size">
          <cd:Text>
            <cd:ByteSize>1 / int(/denom)</cd:ByteSize>
          </cd:Text>
        </cd:Field>
        <cd:Field name="mid">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="after_bad">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:Field>
      </cd:Record>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
    <cd:Field name="r_dyn_walk_avail_err">
      <cd:Record>
        <cd:Field name="good_var">
          <cd:Text>
            <cd:ByteSize>int(/denom)</cd:ByteSize>
          </cd:Text>
        </cd:Field>
        <cd:Field name="mid_avail_err">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
          <cd:Available>1 / int(/denom) == 1</cd:Available>
        </cd:Field>
        <cd:Field name="tail">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:Field>
      </cd:Record>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
    <cd:Field name="r_last_field_size_err">
      <cd:Record>
        <cd:Field name="ok">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="bad_last">
          <cd:Text>
            <cd:ByteSize>1 / int(/denom)</cd:ByteSize>
          </cd:Text>
        </cd:Field>
      </cd:Record>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
    <cd:Field name="r_size_expr_err">
      <cd:Record>
        <cd:BitSize>1 / int(/denom)</cd:BitSize>
        <cd:Field name="ok">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:Field>
      </cd:Record>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
    <cd:Field name="u_expr_err">
      <cd:Union>
        <cd:FieldExpression>1 / int(/denom)</cd:FieldExpression>
        <cd:Field name="u0">
          <cd:Integer>
            <cd:NativeType>uint8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="u1">
          <cd:Integer>
            <cd:NativeType>uint16</cd:NativeType>
            <cd:BitSize>16</cd:BitSize>
          </cd:Integer>
        </cd:Field>
      </cd:Union>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
    <cd:Field name="u_field_size_err">
      <cd:Union>
        <cd:FieldExpression>0</cd:FieldExpression>
        <cd:Field name="bad_u0">
          <cd:Text>
            <cd:ByteSize>1 / int(/denom)</cd:ByteSize>
          </cd:Text>
        </cd:Field>
        <cd:Field name="u1">
          <cd:Integer>
            <cd:NativeType>uint16</cd:NativeType>
            <cd:BitSize>16</cd:BitSize>
          </cd:Integer>
        </cd:Field>
      </cd:Union>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
    <cd:Field name="arr_bad_dim_expr">
      <cd:Array>
        <cd:Dimension>1 / int(/denom)</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>uint8</cd:NativeType>
          <cd:BitSize>8</cd:BitSize>
        </cd:Integer>
      </cd:Array>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
    <cd:Field name="arr_neg_dim">
      <cd:Array>
        <cd:Dimension>int(/signed_dim)</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>uint8</cd:NativeType>
          <cd:BitSize>8</cd:BitSize>
        </cd:Integer>
      </cd:Array>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
    <cd:Field name="arr_bad_elem_size">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Text>
          <cd:ByteSize>1 / int(/denom)</cd:ByteSize>
        </cd:Text>
      </cd:Array>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";

// ASCII product exercising AsciiLine, AsciiLineSeparator, ASCII Integer,
// ASCII Time special type, and ASCII Array.
constexpr std::string_view kAscProdXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="asc_prod" format="ascii">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>4</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="line">
      <cd:AsciiLine/>
    </cd:Field>
    <cd:Field name="sep1">
      <cd:AsciiLineSeparator/>
    </cd:Field>
    <cd:Field name="int_val">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:ByteSize>4</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="ts">
      <cd:Time timeformat="ascii_envisat_datetime"/>
    </cd:Field>
    <cd:Field name="sep2">
      <cd:AsciiLineSeparator/>
    </cd:Field>
    <cd:Field name="arr">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int16</cd:NativeType>
          <cd:ByteSize>3</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="sep3">
      <cd:AsciiLineSeparator/>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";

std::vector<uint8_t> BuildTestCodadefBytes() {
  const std::array<std::pair<std::string_view, std::string_view>, 6> kEntries =
      {{
          {"products/fixed_bin.xml", kFixedBinXml},
          {"products/dyn_rec_bin.xml", kDynRecBinXml},
          {"products/union_bin.xml", kUnionBinXml},
          {"products/array_bin.xml", kArrayBinXml},
          {"products/err_bin.xml", kErrBinXml},
          {"products/asc_prod.xml", kAscProdXml},
      }};
  return BuildCodadefBytes(kIndexXml, kEntries);
}

// Helper fixture that loads the shared synthetic .codadef dictionary and opens
// synthesized product files cleanly in ::testing::TempDir().
class CodaAscbinCursorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    restore_def_.emplace("CODA_DEFINITION");
    restore_leap_.emplace("CODA_LEAP_SECOND_TABLE");
    restore_def_->Unset();
    restore_leap_->Unset();
    restore_options_.emplace();

    codadef_file_.emplace("coda_ascbin_cursor_test.codadef");
    ASSERT_TRUE(codadef_file_->WriteBytes(BuildTestCodadefBytes()));
    ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file_->c_str()));
    ASSERT_EQ(CODA_SUCCESS, session_.Init());
  }

  void TearDown() override {
    product_.Reset();
    session_.Reset();
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  }

  void OpenProductBytes(std::string_view filename,
                        std::span<const uint8_t> bytes, coda_cursor* cursor) {
    product_.Reset();
    product_file_.emplace(filename);
    ASSERT_TRUE(product_file_->WriteBytes(bytes));
    ASSERT_EQ(CODA_SUCCESS,
              coda_open(product_file_->c_str(), product_.OutParam()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(cursor, product_.get()));
  }

  void OpenProductText(std::string_view filename, std::string_view text,
                       coda_cursor* cursor) {
    OpenProductBytes(
        filename,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()),
                                 text.size()),
        cursor);
  }

  std::optional<ScopedEnvVar> restore_def_;
  std::optional<ScopedEnvVar> restore_leap_;
  std::optional<ScopedCodaOptions> restore_options_;
  std::optional<TempFile> codadef_file_;
  ScopedCodaSession session_;
  std::optional<TempFile> product_file_;
  ScopedProduct product_;
};

TEST_F(CodaAscbinCursorTest, BitPackedFieldsFixedOffsetsAndAttributes) {
  // Byte 4: bits3 = 0b101 (5) | bits5 = 0b11110 (-2 in 5-bit signed) -> 0xBE.
  // Bytes 5..6: u16 = 0x1234. Bytes 7..8: raw2 = {0xAA, 0x55}.
  // Bytes 9..16: complex float32 BE {1.5f (0x3FC00000), -2.25f (0xC0100000)}.
  constexpr auto kBytes = std::to_array<uint8_t>({
      'A',
      'B',
      'I',
      'N',
      0xBE,
      0x12,
      0x34,
      0xAA,
      0x55,
      0x3F,
      0xC0,
      0x00,
      0x00,
      0xC0,
      0x10,
      0x00,
      0x00,
  });

  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(OpenProductBytes("fixed_bin.dat", kBytes, &cursor));

  // 1. Verify root cursor properties and fixed record size.
  coda_product* prod_ptr = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_product_file(&cursor, &prod_ptr));
  EXPECT_EQ(product_.get(), prod_ptr);

  int depth = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_depth(&cursor, &depth));
  EXPECT_EQ(0, depth);

  coda_format fmt = coda_format_ascii;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_format(&cursor, &fmt));
  EXPECT_EQ(coda_format_binary, fmt);

  int64_t bit_size = -1;
  int64_t byte_size = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_byte_size(&cursor, &byte_size));
  EXPECT_EQ(136, bit_size);
  EXPECT_EQ(17, byte_size);

  long num_fields = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_fields));
  EXPECT_EQ(6, num_fields);

  int has_ascii = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_ascii_content(&cursor, &has_ascii));
  EXPECT_EQ(0, has_ascii);

  // 2. Reject out-of-range record field indices.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_record_field_by_index(&cursor, -1));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_record_field_by_index(&cursor, 6));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  // 3. Walk all fields via goto_first_record_field / goto_next_record_field.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_record_field(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_ascii_content(&cursor, &has_ascii));
  EXPECT_EQ(1, has_ascii);
  long str_len = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &str_len));
  EXPECT_EQ(4, str_len);
  std::array<char, 5> magic_buf{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&cursor, magic_buf.data(),
                                                  magic_buf.size()));
  EXPECT_STREQ("ABIN", magic_buf.data());

  // Field 1: bits3 at bit offset 32 (byte 4, bit 0), size 3.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  int64_t bit_off = -1;
  int64_t byte_off = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_bit_offset(&cursor, &bit_off));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_byte_offset(&cursor, &byte_off));
  EXPECT_EQ(32, bit_off);
  EXPECT_EQ(4, byte_off);
  uint8_t u8 = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&cursor, &u8));
  EXPECT_EQ(5, u8);

  // Non-text binary field rejects get_string_length with
  // CODA_ERROR_INVALID_TYPE.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_string_length(&cursor, &str_len));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  // Field 2: bits5 at bit offset 35 (byte 4, bit 3), size 5.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_bit_offset(&cursor, &bit_off));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_byte_offset(&cursor, &byte_off));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_byte_size(&cursor, &byte_size));
  EXPECT_EQ(35, bit_off);
  EXPECT_EQ(4, byte_off);
  EXPECT_EQ(5, bit_size);
  EXPECT_EQ(1, byte_size);
  int8_t s8 = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&cursor, &s8));
  EXPECT_EQ(-2, s8);

  // Field 3: u16 at bit offset 40.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  uint16_t u16 = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&cursor, &u16));
  EXPECT_EQ(0x1234, u16);

  // Field 4: raw2 at bit offset 56.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  std::array<uint8_t, 2> raw_buf{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_bytes(&cursor, raw_buf.data(), 0, 2));
  EXPECT_THAT(raw_buf, ElementsAre(0xAA, 0x55));

  // Field 5: cplx (special complex -> base record with real/imaginary).
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  coda_type_class tc = coda_record_class;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &tc));
  EXPECT_EQ(coda_special_class, tc);
  double re = 0.0;
  double im = 0.0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_complex_double_split(&cursor, &re, &im));
  EXPECT_DOUBLE_EQ(1.5, re);
  EXPECT_DOUBLE_EQ(-2.25, im);

  coda_cursor cplx_base = cursor;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_use_base_type_of_special_type(&cplx_base));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cplx_base, &tc));
  EXPECT_EQ(coda_record_class, tc);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_num_elements(&cplx_base, &num_fields));
  EXPECT_EQ(2, num_fields);

  // Stepping past the last field fails with CODA_ERROR_INVALID_INDEX.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_record_field(&cursor));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  // 4. Virtual attributes record on binary cursor.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  int has_attrs = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&cursor, &has_attrs));
  EXPECT_EQ(0, has_attrs);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_depth(&cursor, &depth));
  EXPECT_EQ(1, depth);
  long attr_idx = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&cursor, &attr_idx));
  EXPECT_EQ(-1, attr_idx);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_bit_offset(&cursor, &bit_off));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_byte_offset(&cursor, &byte_off));
  EXPECT_EQ(-1, bit_off);
  EXPECT_EQ(-1, byte_off);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_fields));
  EXPECT_EQ(0, num_fields);
}

TEST_F(CodaAscbinCursorTest, DynamicOffsetsAvailabilityAndSizeExprAvailable) {
  // Header: magic="DYN1", flag=1, opt_flag=1, len1=2, len2=3, offset_bytes=9,
  // signed_val=5.
  // sub (starts at byte 10):
  //   [0] first_opt=0x11, [1..2] "AB", [3..5] "CDE", [6] opt_mid=0x22,
  //   [7] tail_u8=0x33, [8] pad=0x00, [9] opt_at_expr=0x44, [10]
  //   after_expr=0x55.
  constexpr auto kBytes = std::to_array<uint8_t>({
      'D', 'Y', 'N', '1', 1,   1,    2,    3,    9,    5,    0x11,
      'A', 'B', 'C', 'D', 'E', 0x22, 0x33, 0x00, 0x44, 0x55,
  });

  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(OpenProductBytes("dyn_avail.dat", kBytes, &cursor));

  // 1. Jump directly to sub fields via index (covers bit_offset_expr and the
  //    chained while-loop over dynamic offsets).
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "sub"));

  int64_t bit_size = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(80, bit_size);

  // Field 4 (tail_u8) skips backward through opt_mid and dyn_text2 to
  // dyn_text1.
  coda_cursor field_cursor = cursor;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_index(&field_cursor, 4));
  uint8_t val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&field_cursor, &val));
  EXPECT_EQ(0x33, val);

  // Field 5 (opt_at_expr) uses BitOffset = offset_bytes * 8 = 72 -> byte 19.
  field_cursor = cursor;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_index(&field_cursor, 5));
  int64_t byte_off = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_file_byte_offset(&field_cursor, &byte_off));
  EXPECT_EQ(19, byte_off);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&field_cursor, &val));
  EXPECT_EQ(0x44, val);

  // Stepping 5 -> 6 (after_expr) lands at byte 20.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&field_cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&field_cursor, &val));
  EXPECT_EQ(0x55, val);

  // 2. Check field availability queries on sub.
  int available = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_available_status(
                              &cursor, 0, &available));
  EXPECT_EQ(1, available);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_available_status(
                              &cursor, 1, &available));
  EXPECT_EQ(1, available);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_record_field_available_status(&cursor, -1,
                                                              &available));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_record_field_available_status(&cursor, 7,
                                                              &available));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  // 3. Walk sub sequentially via goto_first_record_field +
  // goto_next_record_field.
  field_cursor = cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_record_field(&field_cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&field_cursor, &val));
  EXPECT_EQ(0x11, val);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&field_cursor));
  std::array<char, 3> txt1{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&field_cursor, txt1.data(), txt1.size()));
  EXPECT_STREQ("AB", txt1.data());
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&field_cursor));
  std::array<char, 4> txt2{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&field_cursor, txt2.data(), txt2.size()));
  EXPECT_STREQ("CDE", txt2.data());
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&field_cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&field_cursor, &val));
  EXPECT_EQ(0x22, val);

  // 4. fixed_avail_rec has field[1].bit_offset == 8 >= 0 and field[2] dynamic.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "fixed_avail_rec"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(32, bit_size);
  field_cursor = cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_record_field(&field_cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&field_cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&field_cursor, &val));
  EXPECT_EQ('A', val);

  // 5. sized_rec compares fast size_expr (signed_val*8 = 40) vs field walk
  // (24).
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "sized_rec"));
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_fast_size_expressions(1));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(40, bit_size);
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_fast_size_expressions(0));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(24, bit_size);

  // 6. empty_rec (bit_size == -1 on empty coda_type_record -> 0 bits).
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "empty_rec"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(0, bit_size);
  long empty_fields = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &empty_fields));
  EXPECT_EQ(0, empty_fields);
}

TEST_F(CodaAscbinCursorTest,
       DynamicOffsetsUnavailableFieldsAndNegativeSizeExpr) {
  // flag=0, opt_flag=0, len1=2, len2=3, offset_bytes=9, signed_val=-1.
  constexpr auto kBytes = std::to_array<uint8_t>({
      'D',
      'Y',
      'N',
      '1',
      0,
      0,
      2,
      3,
      9,
      0xFF,
      0x11,
      'A',
      'B',
      'C',
      'D',
      'E',
      0x33,
      0x44,
  });

  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(OpenProductBytes("dyn_unavail.dat", kBytes, &cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "sub"));

  // 1. Bit size excludes unavailable fields (0 + 16 + 24 + 0 + 8 + 0 + 8 = 56).
  int64_t bit_size = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(56, bit_size);

  // 2. Field 0 unavailable with bit_offset_expr -> rel_bit_offset = 0, no_data.
  //    no_data uses coda_backend_memory, so get_file_byte_offset returns -1.
  coda_cursor f = cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_index(&f, 0));
  coda_special_type st = coda_special_time;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&f, &st));
  EXPECT_EQ(coda_special_no_data, st);
  int64_t byte_off = 0;
  EXPECT_EQ(-1, coda_cursor_get_file_byte_offset(&f, &byte_off));
  EXPECT_EQ(-1, byte_off);

  // 3. Field 4 (tail_u8) skips unavailable opt_mid -> byte 16.
  f = cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_index(&f, 4));
  uint8_t val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&f, &val));
  EXPECT_EQ(0x33, val);

  // 4. Field 5 (opt_at_expr) unavailable via index -> no_data (-1 byte offset).
  f = cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_index(&f, 5));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&f, &st));
  EXPECT_EQ(coda_special_no_data, st);
  byte_off = 0;
  EXPECT_EQ(-1, coda_cursor_get_file_byte_offset(&f, &byte_off));
  EXPECT_EQ(-1, byte_off);

  // 5. Stepping 4 -> 5 -> 6 via goto_next_record_field keeps offset at byte 16
  //    across unavailable opt_at_expr so after_expr lands at byte 16.
  f = cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_index(&f, 4));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&f));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&f, &st));
  EXPECT_EQ(coda_special_no_data, st);
  byte_off = 0;
  EXPECT_EQ(-1, coda_cursor_get_file_byte_offset(&f, &byte_off));
  EXPECT_EQ(-1, byte_off);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&f));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_byte_offset(&f, &byte_off));
  EXPECT_EQ(16, byte_off);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&f, &val));
  EXPECT_EQ(0x33, val);

  int available = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_available_status(
                              &cursor, 0, &available));
  EXPECT_EQ(0, available);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_available_status(
                              &cursor, 3, &available));
  EXPECT_EQ(0, available);

  // 6. fixed_avail_rec with unavailable field[1] (bit_offset >= 0).
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "fixed_avail_rec"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(24, bit_size);
  f = cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_index(&f, 1));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&f, &st));
  EXPECT_EQ(coda_special_no_data, st);
  f = cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_record_field(&f));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&f));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&f, &st));
  EXPECT_EQ(coda_special_no_data, st);

  // 7. sized_rec with negative size_expr (-1 * 8 = -8) returns
  // CODA_ERROR_PRODUCT.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "sized_rec"));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("negative"));
  coda_set_error(CODA_SUCCESS, nullptr);
}

TEST_F(CodaAscbinCursorTest, UnionNavigationAvailabilityAndRangeErrors) {
  // 1. Valid union selection tag=1 (u16) and tag=2 (text of len=3).
  constexpr auto kTag1Bytes = std::to_array<uint8_t>({
      'U',
      'N',
      'I',
      '1',
      1,
      3,
      0x12,
      0x34,
      0x00,
      0x00,
      0xAB,
      0xCD,
      0xE0,
      0x00,
  });
  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(
      OpenProductBytes("union_tag1.dat", kTag1Bytes, &cursor));

  // Non-union record rejects union-only APIs.
  long union_idx = -1;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_cursor_get_available_union_field_index(&cursor, &union_idx));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_available_union_field(&cursor));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "dyn_union"));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_available_union_field_index(&cursor, &union_idx));
  EXPECT_EQ(1, union_idx);

  int64_t bit_size = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(16, bit_size);

  int avail = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_record_field_available_status(&cursor, 0, &avail));
  EXPECT_EQ(0, avail);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_record_field_available_status(&cursor, 1, &avail));
  EXPECT_EQ(1, avail);

  // Navigate to active union field.
  coda_cursor u = cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_available_union_field(&u));
  uint16_t u16 = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&u, &u16));
  EXPECT_EQ(0x1234, u16);

  // Navigate union fields by index and goto_next_record_field: inactive fields
  // yield coda_special_no_data.
  u = cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_index(&u, 0));
  coda_special_type st = coda_special_time;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&u, &st));
  EXPECT_EQ(coda_special_no_data, st);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&u));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&u, &u16));
  EXPECT_EQ(0x1234, u16);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&u));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&u, &st));
  EXPECT_EQ(coda_special_no_data, st);

  // fixed_union (bit_size == 16 >= 0) also resolves active field.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "fixed_union"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(16, bit_size);

  // 2. Dynamic text branch (tag=2) computes bit size from active field.
  constexpr auto kTag2Bytes = std::to_array<uint8_t>({
      'U',
      'N',
      'I',
      '1',
      2,
      3,
      'X',
      'Y',
      'Z',
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
  });
  ASSERT_NO_FATAL_FAILURE(
      OpenProductBytes("union_tag2.dat", kTag2Bytes, &cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "dyn_union"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(24, bit_size);

  // 3. Out-of-range union indices (-1 and >= num_fields) including unaligned
  //    bit_offset (byte 12, bit 3) fail with CODA_ERROR_PRODUCT.
  for (const int8_t bad_tag : {int8_t{-1}, int8_t{3}}) {
    const auto bad_bytes = std::to_array<uint8_t>({
        'U',
        'N',
        'I',
        '1',
        static_cast<uint8_t>(bad_tag),
        1,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    });
    ASSERT_NO_FATAL_FAILURE(
        OpenProductBytes("union_bad_tag.dat", bad_bytes, &cursor));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(
                                &cursor, "unaligned_union"));
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_cursor_get_available_union_field_index(&cursor, &union_idx));
    EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_available_union_field(&cursor));
    EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
  }
}

TEST_F(CodaAscbinCursorTest, FixedMixed2dAndVariableRecordArrays) {
  // cols=3, count=3.
  // arr_fixed[3] int16 LE: {10, -20, 300}
  // arr_mixed_2d[2][3] uint8: {1, 2, 3, 4, 5, 6}
  // arr_var_rec[3]: {len=2,"hi"}, {len=3,"abc"}, {len=1,"z"}
  constexpr auto kBytes = std::to_array<uint8_t>({
      'A', 'R', 'R', '1', 3, 3,   10,  0, 0xEC, 0xFF, 0x2C, 0x01, 1,   2,
      3,   4,   5,   6,   2, 'h', 'i', 3, 'a',  'b',  'c',  1,    'z',
  });

  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(OpenProductBytes("array_bin.dat", kBytes, &cursor));

  // 1. Fixed 1D array.
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "arr_fixed"));
  long num_elems = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elems));
  EXPECT_EQ(3, num_elems);
  int num_dims = -1;
  std::array<long, CODA_MAX_NUM_DIMS> dims{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&cursor, &num_dims, dims.data()));
  EXPECT_EQ(1, num_dims);
  EXPECT_EQ(3, dims[0]);
  int64_t bit_size = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(48, bit_size);

  // Subscript errors: wrong rank, negative index, >= dim.
  const std::array<long, 2> two_subs = {0, 0};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&cursor, 2, two_subs.data()));
  EXPECT_EQ(CODA_ERROR_ARRAY_NUM_DIMS_MISMATCH, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  long bad_sub = -1;
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&cursor, 1, &bad_sub));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  bad_sub = 3;
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&cursor, 1, &bad_sub));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&cursor, -1));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&cursor, 3));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  // Step through all 3 elements and reject stepping past the end.
  coda_cursor elem = cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&elem));
  int16_t s16 = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&elem, &s16));
  EXPECT_EQ(10, s16);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&elem));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&elem, &s16));
  EXPECT_EQ(-20, s16);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&elem));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&elem, &s16));
  EXPECT_EQ(300, s16);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&elem));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  // 2. Mixed fixed/variable 2D array (2 x cols=3).
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "arr_mixed_2d"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elems));
  EXPECT_EQ(6, num_elems);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&cursor, &num_dims, dims.data()));
  EXPECT_EQ(2, num_dims);
  EXPECT_EQ(2, dims[0]);
  EXPECT_EQ(3, dims[1]);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(48, bit_size);

  elem = cursor;
  const std::array<long, 2> subs = {1, 2};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element(&elem, 2, subs.data()));
  uint8_t u8 = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&elem, &u8));
  EXPECT_EQ(6, u8);

  // Disable boundary checks to exercise the unchecked navigation path.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));
  elem = cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 4));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&elem, &u8));
  EXPECT_EQ(5, u8);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&elem));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&elem, &u8));
  EXPECT_EQ(6, u8);
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(1));

  // 3. Variable-size record-element array (count=3).
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "arr_var_rec"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(72, bit_size);

  // goto_array_element walks elements 0 and 1 to reach element 2 ("z").
  elem = cursor;
  const long sub2 = 2;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element(&elem, 1, &sub2));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&elem, "etext"));
  std::array<char, 2> text_buf1{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&elem, text_buf1.data(), text_buf1.size()));
  EXPECT_STREQ("z", text_buf1.data());

  // goto_array_element_by_index walks element 0 to reach element 1 ("abc").
  elem = cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 1));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&elem, "etext"));
  std::array<char, 4> text_buf3{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&elem, text_buf3.data(), text_buf3.size()));
  EXPECT_STREQ("abc", text_buf3.data());

  // goto_first_array_element + goto_next_array_element on variable elements.
  elem = cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&elem));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&elem));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&elem, "etext"));
  text_buf3 = {};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&elem, text_buf3.data(), text_buf3.size()));
  EXPECT_STREQ("abc", text_buf3.data());

  // 4. Zero-element variable array reports 0 bits.
  constexpr auto kEmptyArrayBytes = std::to_array<uint8_t>({
      'A',
      'R',
      'R',
      '1',
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
  });
  ASSERT_NO_FATAL_FAILURE(
      OpenProductBytes("array_empty.dat", kEmptyArrayBytes, &cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "arr_mixed_2d"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elems));
  EXPECT_EQ(0, num_elems);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(0, bit_size);
}

TEST_F(CodaAscbinCursorTest, AsciiProductNavigationSizesAndSpecialTime) {
  // Trailing '\n' (sep3) keeps the final arr[1] read strictly before EOF
  // because get_bit_size_boundary in coda-ascii-cursor.c uses `>=`.
  constexpr std::string_view kAsciiData =
      "ASC1hello\n"
      "  4201-JAN-2020 00:00:00.000000\n"
      " 10 20\n";

  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(OpenProductText("asc_prod.txt", kAsciiData, &cursor));

  int has_ascii = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_ascii_content(&cursor, &has_ascii));
  EXPECT_EQ(1, has_ascii);

  int64_t bit_size = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(static_cast<int64_t>(kAsciiData.size()) * 8, bit_size);

  long str_len = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &str_len));
  EXPECT_EQ(static_cast<long>(kAsciiData.size()), str_len);

  std::string full(static_cast<size_t>(str_len) + 1, '\0');
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&cursor, full.data(), str_len + 1));
  full.resize(static_cast<size_t>(str_len));
  EXPECT_EQ(kAsciiData, full);

  // Special ASCII Time field: has_ascii_content == 1, unwraps to 27-byte text.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&cursor, "ts"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_ascii_content(&cursor, &has_ascii));
  EXPECT_EQ(1, has_ascii);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &str_len));
  EXPECT_EQ(27, str_len);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_use_base_type_of_special_type(&cursor));
  coda_type_class tc = coda_special_class;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &tc));
  EXPECT_EQ(coda_text_class, tc);

  // ASCII array dimensions, element count, and element reads.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "arr"));
  long num_elems = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elems));
  EXPECT_EQ(2, num_elems);
  int num_dims = -1;
  std::array<long, CODA_MAX_NUM_DIMS> dims{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&cursor, &num_dims, dims.data()));
  EXPECT_EQ(1, num_dims);
  EXPECT_EQ(2, dims[0]);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&cursor));
  int32_t v = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &v));
  EXPECT_EQ(10, v);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &v));
  EXPECT_EQ(20, v);
}

TEST_F(CodaAscbinCursorTest, RecordByteSizeExpressionConvertsBytesToBits) {
  // Construct a binary coda_type_record with byte_size_expr (bit_size == -8)
  // directly, because .codadef XML only exposes <cd:BitSize> for records.
  ScopedType rec_type(
      reinterpret_cast<coda_type*>(coda_type_record_new(coda_format_binary)));
  ASSERT_NE(nullptr, rec_type.get());

  coda_expression* expr = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("5", &expr));
  if (coda_type_set_byte_size_expression(rec_type.get(), expr) !=
      CODA_SUCCESS) {
    coda_expression_delete(expr);
    FAIL() << "coda_type_set_byte_size_expression failed";
  }

  coda_product synth_product{};
  synth_product.format = coda_format_binary;
  synth_product.root_type =
      reinterpret_cast<coda_dynamic_type*>(rec_type.get());

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS,
            coda_ascbin_cursor_set_product(&cursor, &synth_product));

  int64_t bit_size = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_ascbin_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(40, bit_size);
}

TEST_F(CodaAscbinCursorTest, ExpressionEvaluationErrorPaths) {
  // denom=0 triggers division by zero (CODA_ERROR_EXPRESSION); signed_dim=-2
  // triggers negative array size (CODA_ERROR_PRODUCT).
  constexpr auto kBytes = std::to_array<uint8_t>({
      'E',
      'R',
      'R',
      '1',
      0,
      0xFE,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
  });

  coda_cursor root;
  ASSERT_NO_FATAL_FAILURE(OpenProductBytes("err_bin.dat", kBytes, &root));

  auto expect_expr_err = [](int status) {
    EXPECT_EQ(-1, status);
    EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  int64_t bit_size = -1;
  long num_elems = -1;
  int avail = -1;
  long union_idx = -1;
  int num_dims = -1;
  std::array<long, CODA_MAX_NUM_DIMS> dims{};

  // 1. r_expr_avail_and_off: available_expr with bit_offset_expr, failing
  //    bit_offset_expr, and recursive anchor failure.
  coda_cursor c = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&c, "r_expr_avail_and_off"));
  coda_set_error(CODA_SUCCESS, nullptr);
  expect_expr_err(coda_cursor_get_bit_size(&c, &bit_size));
  coda_cursor sub = c;
  expect_expr_err(coda_cursor_goto_record_field_by_index(&sub, 0));
  sub = c;
  expect_expr_err(coda_cursor_goto_record_field_by_index(&sub, 1));
  sub = c;
  expect_expr_err(coda_cursor_goto_record_field_by_index(&sub, 2));

  // 2. r_next_avail_with_expr_off: goto_next_record_field fails in
  //    get_next_relative_field_bit_offset on available_expr.
  c = root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(
                              &c, "r_next_avail_with_expr_off"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_record_field(&c));
  expect_expr_err(coda_cursor_goto_next_record_field(&c));

  // 3. r_next_off_err: goto_next_record_field and get_bit_size fail on
  //    bit_offset_expr of field[1].
  c = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&c, "r_next_off_err"));
  expect_expr_err(coda_cursor_get_bit_size(&c, &bit_size));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_record_field(&c));
  expect_expr_err(coda_cursor_goto_next_record_field(&c));

  // 4. r_fixed_off_avail_err: field[1] has bit_offset >= 0 and failing
  //    available_expr across index, next, get_bit_size, and available_status.
  c = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&c, "r_fixed_off_avail_err"));
  expect_expr_err(coda_cursor_get_bit_size(&c, &bit_size));
  expect_expr_err(coda_cursor_get_record_field_available_status(&c, 1, &avail));
  sub = c;
  expect_expr_err(coda_cursor_goto_record_field_by_index(&sub, 1));
  sub = c;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_record_field(&sub));
  expect_expr_err(coda_cursor_goto_next_record_field(&sub));

  // 5. r_dyn_walk_err: bad_size fails inside dynamic offset walk,
  //    goto_next_record_field, and get_bit_size.
  c = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&c, "r_dyn_walk_err"));
  expect_expr_err(coda_cursor_get_bit_size(&c, &bit_size));
  sub = c;
  expect_expr_err(coda_cursor_goto_record_field_by_index(&sub, 2));
  sub = c;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_record_field(&sub));
  expect_expr_err(coda_cursor_goto_next_record_field(&sub));

  // 6. r_dyn_walk_avail_err: intermediate available_expr fails during walk.
  c = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&c, "r_dyn_walk_avail_err"));
  expect_expr_err(coda_cursor_goto_record_field_by_index(&c, 2));

  // 7. r_last_field_size_err: last field bit_size fails inside get_bit_size.
  c = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&c, "r_last_field_size_err"));
  expect_expr_err(coda_cursor_get_bit_size(&c, &bit_size));

  // 8. r_size_expr_err: record size_expr fails inside get_bit_size.
  c = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&c, "r_size_expr_err"));
  expect_expr_err(coda_cursor_get_bit_size(&c, &bit_size));

  // 9. u_expr_err & u_field_size_err: union_field_expr and active-field
  //    bit_size error paths.
  c = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&c, "u_expr_err"));
  expect_expr_err(coda_cursor_get_available_union_field_index(&c, &union_idx));
  expect_expr_err(coda_cursor_goto_available_union_field(&c));
  expect_expr_err(coda_cursor_get_bit_size(&c, &bit_size));
  expect_expr_err(coda_cursor_get_record_field_available_status(&c, 0, &avail));
  sub = c;
  expect_expr_err(coda_cursor_goto_record_field_by_index(&sub, 0));

  c = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&c, "u_field_size_err"));
  expect_expr_err(coda_cursor_get_bit_size(&c, &bit_size));

  // 10. arr_bad_dim_expr: failing dim_expr across all array cursor functions.
  c = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&c, "arr_bad_dim_expr"));
  const long idx0 = 0;
  expect_expr_err(coda_cursor_goto_array_element(&c, 1, &idx0));
  expect_expr_err(coda_cursor_goto_array_element_by_index(&c, 0));
  expect_expr_err(coda_cursor_get_num_elements(&c, &num_elems));
  expect_expr_err(coda_cursor_get_array_dim(&c, &num_dims, dims.data()));
  expect_expr_err(coda_cursor_get_bit_size(&c, &bit_size));

  // 11. arr_neg_dim: negative dim_expr returns CODA_ERROR_PRODUCT.
  c = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&c, "arr_neg_dim"));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_num_elements(&c, &num_elems));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_array_dim(&c, &num_dims, dims.data()));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);

  // 12. arr_bad_elem_size: variable-size element failure during element walk,
  //     goto_next_array_element, and get_bit_size.
  c = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&c, "arr_bad_elem_size"));
  expect_expr_err(coda_cursor_get_bit_size(&c, &bit_size));
  const long idx1 = 1;
  sub = c;
  expect_expr_err(coda_cursor_goto_array_element(&sub, 1, &idx1));
  sub = c;
  expect_expr_err(coda_cursor_goto_array_element_by_index(&sub, 1));
  sub = c;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&sub));
  expect_expr_err(coda_cursor_goto_next_array_element(&sub));
}

TEST_F(CodaAscbinCursorTest, GotoNextArrayElementPropagatesNumElementsError) {
  // Enter element 0 with boundary checks disabled, then re-enable them so
  // coda_cursor_goto_next_array_element hits the coda_cursor_get_num_elements
  // failure inside the boundary check.
  constexpr auto kBytes = std::to_array<uint8_t>({
      'E',
      'R',
      'R',
      '1',
      0,
      0xFE,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
  });

  coda_cursor cursor;
  ASSERT_NO_FATAL_FAILURE(
      OpenProductBytes("err_next_elem.dat", kBytes, &cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "arr_bad_dim_expr"));

  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&cursor, 0));
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(1));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&cursor));
  EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);
  coda_set_error(CODA_SUCCESS, nullptr);
}

// TODO(schwehr): Lines 331 (union_field_expr failure inside
// coda_ascbin_cursor_goto_next_record_field after entering field 0), 768-769
// (default assert in coda_ascbin_cursor_get_bit_size), and 835-836 (default
// assert in coda_ascbin_cursor_get_num_elements) are defensive/unreachable via
// valid public cursor states.

}  // namespace
}  // namespace stcorp_coda
