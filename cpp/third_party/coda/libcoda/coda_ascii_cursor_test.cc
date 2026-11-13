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
// Tests CODA ASCII number parsers, asciilines initialization/caching, bit-size
// and string-length queries, boundary checks, ASCII mappings, special text
// types, scalar/array/partial-array reads, and error paths
// (libcoda/coda-ascii-cursor.c, libcoda/coda-ascii.c).
//
// google3 build caveats:
//   - Upstream coda-ascii-cursor.c:611-625 allocates `asciiline` via
//     coda_type_text_new() and retains it in coda_type_array_set_base_type()
//     without releasing the initial reference on success (72 B leak per
//     product on first asciiline setup); callers that initialize asciilines
//     wrap that call in a narrow absl::LeakCheckDisabler.
//   - Upstream coda-ascii-cursor.c:106 checks
//     `current_bit_offset + read_bit_size >= bit_size` for ascii/binary files,
//     so valid ASCII files include a trailing byte (newline) after fixed-size
//     terminal fields.
//   - Upstream coda-ascii-cursor.c:1607 passes `sizeof(int32_t)` instead of
//     `sizeof(int64_t)` to transpose_array() in
//     coda_ascii_cursor_read_int64_array(); Fortran-order transposition for
//     int64 arrays is therefore exercised on a 1D array here.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-ascii-cursor.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-ascii.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-ascii.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-ascii-internal.h

#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-ascbin.h"
#include "third_party/stcorp_coda/libcoda/coda-ascii-internal.h"
#include "third_party/stcorp_coda/libcoda/coda-ascii.h"
#include "third_party/stcorp_coda/libcoda/coda-mem-internal.h"
#include "third_party/stcorp_coda/libcoda/coda-type.h"
}  // extern "C"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <limits>
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
#include "third_party/absl/strings/str_format.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"

namespace stcorp_coda {
namespace {

using ::testing::DoubleEq;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::FloatEq;

struct TypeDeleter {
  void operator()(coda_type* type) const { coda_type_release(type); }
};
using ScopedType = std::unique_ptr<coda_type, TypeDeleter>;

struct FieldDeleter {
  void operator()(coda_type_record_field* field) const {
    coda_type_record_field_delete(field);
  }
};
using ScopedRecordField = std::unique_ptr<coda_type_record_field, FieldDeleter>;

template <typename T>
coda_type* TrackType(T* raw_type, std::vector<ScopedType>& owned_types) {
  auto* base = reinterpret_cast<coda_type*>(raw_type);
  owned_types.emplace_back(base);
  return base;
}

constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="AsciiCursorTestClass">
  <cd:ProductType name="ScalarProd">
    <cd:ProductDefinition id="scalar_prod" format="ascii" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">ASC_SCALAR</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="TimeDynProd">
    <cd:ProductDefinition id="time_dyn_prod" format="ascii" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">ASC_TDYN</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="MapProd">
    <cd:ProductDefinition id="map_prod" format="ascii" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">ASC_MAP</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="SpecProd">
    <cd:ProductDefinition id="spec_prod" format="ascii" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">ASC_SPEC</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="ArrProd">
    <cd:ProductDefinition id="arr_prod" format="ascii" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">ASC_ARR</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="ArrErrProd">
    <cd:ProductDefinition id="arr_err_prod" format="ascii" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">ERR_ARR</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="UniProd">
    <cd:ProductDefinition id="uni_prod" format="ascii" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">ASC_UNI</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="BinAscProd">
    <cd:ProductDefinition id="bin_asc_prod" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">BIN_ASC</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="XmlAscProd">
    <cd:ProductDefinition id="xml_asc_prod" format="xml" version="1">
      <cd:DetectionRule>
        <cd:MatchData path="/XmlAsc"/>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="EmptyAscProd">
    <cd:ProductDefinition id="empty_asc_prod" format="ascii" version="1">
      <cd:DetectionRule>
        <cd:MatchSize size="0"/>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)xml";

constexpr std::string_view kScalarProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="scalar_prod" format="ascii">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>10</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="s1">
      <cd:AsciiWhiteSpace/>
    </cd:Field>
    <cd:Field name="i8">
      <cd:Integer>
        <cd:NativeType>int8</cd:NativeType>
        <cd:ByteSize>4</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="u8">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:ByteSize>4</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="i16">
      <cd:Integer>
        <cd:NativeType>int16</cd:NativeType>
        <cd:ByteSize>6</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="u16">
      <cd:Integer>
        <cd:NativeType>uint16</cd:NativeType>
        <cd:ByteSize>6</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="i32">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:ByteSize>11</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="u32">
      <cd:Integer>
        <cd:NativeType>uint32</cd:NativeType>
        <cd:ByteSize>11</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="i64">
      <cd:Integer>
        <cd:NativeType>int64</cd:NativeType>
        <cd:ByteSize>20</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="u64">
      <cd:Integer>
        <cd:NativeType>uint64</cd:NativeType>
        <cd:ByteSize>21</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="f32">
      <cd:Float>
        <cd:NativeType>float</cd:NativeType>
        <cd:ByteSize>8</cd:ByteSize>
      </cd:Float>
    </cd:Field>
    <cd:Field name="f64">
      <cd:Float>
        <cd:NativeType>double</cd:NativeType>
        <cd:ByteSize>10</cd:ByteSize>
      </cd:Float>
    </cd:Field>
    <cd:Field name="ch">
      <cd:Text>
        <cd:ByteSize>1</cd:ByteSize>
        <cd:NativeType>char</cd:NativeType>
      </cd:Text>
    </cd:Field>
    <cd:Field name="dyn_i32">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="s2">
      <cd:AsciiWhiteSpace/>
    </cd:Field>
    <cd:Field name="dyn_u32">
      <cd:Integer>
        <cd:NativeType>uint32</cd:NativeType>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="s3">
      <cd:AsciiWhiteSpace/>
    </cd:Field>
    <cd:Field name="dyn_f64">
      <cd:Float>
        <cd:NativeType>double</cd:NativeType>
      </cd:Float>
    </cd:Field>
    <cd:Field name="s4">
      <cd:AsciiWhiteSpace/>
    </cd:Field>
    <cd:Field name="scaled_i16">
      <cd:Integer>
        <cd:NativeType>int16</cd:NativeType>
        <cd:ByteSize>6</cd:ByteSize>
        <cd:Conversion numerator="2" denominator="4" offset="10" invalid="-999"/>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="scaled_i16_inv">
      <cd:Integer>
        <cd:NativeType>int16</cd:NativeType>
        <cd:ByteSize>6</cd:ByteSize>
        <cd:Conversion numerator="2" denominator="4" offset="10" invalid="-999"/>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="scaled_f64">
      <cd:Float>
        <cd:NativeType>double</cd:NativeType>
        <cd:ByteSize>8</cd:ByteSize>
        <cd:Conversion numerator="3" denominator="2" offset="-1" invalid="-999"/>
      </cd:Float>
    </cd:Field>
    <cd:Field name="text_len">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:ByteSize>3</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="dyn_text">
      <cd:Text>
        <cd:ByteSize>int(/text_len)</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="time_fixed">
      <cd:Time timeformat="ascii_ccsds_datetime_ymd1"/>
    </cd:Field>
    <cd:Field name="s5">
      <cd:AsciiWhiteSpace/>
    </cd:Field>
    <cd:Field name="time_line">
      <cd:Time timeformat="time(str(.),&quot;yyyy-MM-dd'T'HH:mm:ss&quot;)">
        <cd:AsciiLine/>
      </cd:Time>
    </cd:Field>
    <cd:Field name="sep">
      <cd:AsciiLineSeparator/>
    </cd:Field>
    <cd:Field name="cplx_dyn">
      <cd:Complex>
        <cd:Float>
          <cd:NativeType>double</cd:NativeType>
        </cd:Float>
      </cd:Complex>
    </cd:Field>
    <cd:Field name="s6">
      <cd:AsciiWhiteSpace/>
    </cd:Field>
    <cd:Field name="raw4">
      <cd:Raw>
        <cd:BitSize>32</cd:BitSize>
      </cd:Raw>
    </cd:Field>
    <cd:Field name="raw_bits">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:ByteSize>3</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="raw_expr">
      <cd:Raw>
        <cd:BitSize>int(/raw_bits)</cd:BitSize>
      </cd:Raw>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

// Exercises coda_ascii_cursor_get_bit_size() for coda_special_time whose
// base_type is ascii_text_default with dynamic size_expr (bit_size < 0).
constexpr std::string_view kTimeDynProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="time_dyn_prod" format="ascii">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>8</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="len">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:ByteSize>3</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="ts">
      <cd:Time timeformat="time(str(.),&quot;yyyy-MM-dd'T'HH:mm:ss&quot;)">
        <cd:Text>
          <cd:ByteSize>int(/len)</cd:ByteSize>
        </cd:Text>
      </cd:Time>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

constexpr std::string_view kMapProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="map_prod" format="ascii">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>7</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="dyn_i_hit">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:Mapping string="NA" value="-999"/>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="s1">
      <cd:AsciiWhiteSpace/>
    </cd:Field>
    <cd:Field name="dyn_i_miss">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:Mapping string="NA" value="-999"/>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="s2">
      <cd:AsciiWhiteSpace/>
    </cd:Field>
    <cd:Field name="def_i_miss">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:ByteSize>4</cd:ByteSize>
        <cd:Mapping string="NA" value="-999"/>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="s3">
      <cd:AsciiWhiteSpace/>
    </cd:Field>
    <cd:Field name="u64_hit">
      <cd:Integer>
        <cd:NativeType>uint64</cd:NativeType>
        <cd:Mapping string="NONE" value="777"/>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="s4">
      <cd:AsciiWhiteSpace/>
    </cd:Field>
    <cd:Field name="u64_miss">
      <cd:Integer>
        <cd:NativeType>uint64</cd:NativeType>
        <cd:Mapping string="NONE" value="777"/>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="s5">
      <cd:AsciiWhiteSpace/>
    </cd:Field>
    <cd:Field name="dyn_f_nan">
      <cd:Float>
        <cd:NativeType>double</cd:NativeType>
        <cd:Mapping string="MISSING" value="nan"/>
        <cd:Mapping string="PINF" value="inf"/>
      </cd:Float>
    </cd:Field>
    <cd:Field name="s6">
      <cd:AsciiWhiteSpace/>
    </cd:Field>
    <cd:Field name="dyn_f_miss">
      <cd:Float>
        <cd:NativeType>double</cd:NativeType>
        <cd:Mapping string="MISSING" value="nan"/>
      </cd:Float>
    </cd:Field>
    <cd:Field name="s7">
      <cd:AsciiWhiteSpace/>
    </cd:Field>
    <cd:Field name="def_f_miss">
      <cd:Float>
        <cd:NativeType>double</cd:NativeType>
        <cd:ByteSize>5</cd:ByteSize>
        <cd:Mapping string="NA" value="-1.5"/>
      </cd:Float>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

constexpr std::string_view kSpecProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="spec_prod" format="ascii">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>8</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="header_sep">
      <cd:AsciiLineSeparator/>
    </cd:Field>
    <cd:Field name="ws">
      <cd:AsciiWhiteSpace/>
    </cd:Field>
    <cd:Field name="ws_zero">
      <cd:AsciiWhiteSpace/>
    </cd:Field>
    <cd:Field name="line1">
      <cd:AsciiLine/>
    </cd:Field>
    <cd:Field name="sep1">
      <cd:AsciiLineSeparator/>
    </cd:Field>
    <cd:Field name="line2">
      <cd:AsciiLine/>
    </cd:Field>
    <cd:Field name="sep2">
      <cd:AsciiLineSeparator/>
    </cd:Field>
    <cd:Field name="line3">
      <cd:AsciiLine/>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

constexpr std::string_view kArrProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="arr_prod" format="ascii">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>7</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="cnt">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:ByteSize>2</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="arr_i8">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Dimension>3</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int8</cd:NativeType>
          <cd:ByteSize>4</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="arr_u8">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Dimension>3</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>uint8</cd:NativeType>
          <cd:ByteSize>4</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="arr_i16">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Dimension>3</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int16</cd:NativeType>
          <cd:ByteSize>4</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="arr_u16">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Dimension>3</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>uint16</cd:NativeType>
          <cd:ByteSize>4</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="arr_i32">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Dimension>3</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int32</cd:NativeType>
          <cd:ByteSize>4</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="arr_u32">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Dimension>3</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>uint32</cd:NativeType>
          <cd:ByteSize>4</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="arr_i64">
      <cd:Array>
        <cd:Dimension>4</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int64</cd:NativeType>
          <cd:ByteSize>4</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="arr_u64">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Dimension>3</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>uint64</cd:NativeType>
          <cd:ByteSize>4</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="arr_f32">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Dimension>3</cd:Dimension>
        <cd:Float>
          <cd:NativeType>float</cd:NativeType>
          <cd:ByteSize>5</cd:ByteSize>
        </cd:Float>
      </cd:Array>
    </cd:Field>
    <cd:Field name="arr_f64">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Dimension>3</cd:Dimension>
        <cd:Float>
          <cd:NativeType>double</cd:NativeType>
          <cd:ByteSize>5</cd:ByteSize>
        </cd:Float>
      </cd:Array>
    </cd:Field>
    <cd:Field name="arr_ch">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Dimension>3</cd:Dimension>
        <cd:Text>
          <cd:ByteSize>1</cd:ByteSize>
          <cd:NativeType>char</cd:NativeType>
        </cd:Text>
      </cd:Array>
    </cd:Field>
    <cd:Field name="arr_dyn">
      <cd:Array>
        <cd:Dimension>int(/cnt)</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int32</cd:NativeType>
          <cd:ByteSize>3</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

constexpr std::string_view kArrErrProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="arr_err_prod" format="ascii">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>7</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="arr_i8">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int8</cd:NativeType>
          <cd:ByteSize>3</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
      <cd:BitOffset>56</cd:BitOffset>
    </cd:Field>
    <cd:Field name="arr_u8">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>uint8</cd:NativeType>
          <cd:ByteSize>3</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
      <cd:BitOffset>56</cd:BitOffset>
    </cd:Field>
    <cd:Field name="arr_i16">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int16</cd:NativeType>
          <cd:ByteSize>3</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
      <cd:BitOffset>56</cd:BitOffset>
    </cd:Field>
    <cd:Field name="arr_u16">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>uint16</cd:NativeType>
          <cd:ByteSize>3</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
      <cd:BitOffset>56</cd:BitOffset>
    </cd:Field>
    <cd:Field name="arr_i32">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int32</cd:NativeType>
          <cd:ByteSize>3</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
      <cd:BitOffset>56</cd:BitOffset>
    </cd:Field>
    <cd:Field name="arr_u32">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>uint32</cd:NativeType>
          <cd:ByteSize>3</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
      <cd:BitOffset>56</cd:BitOffset>
    </cd:Field>
    <cd:Field name="arr_i64">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int64</cd:NativeType>
          <cd:ByteSize>3</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
      <cd:BitOffset>56</cd:BitOffset>
    </cd:Field>
    <cd:Field name="arr_u64">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>uint64</cd:NativeType>
          <cd:ByteSize>3</cd:ByteSize>
        </cd:Integer>
      </cd:Array>
      <cd:BitOffset>56</cd:BitOffset>
    </cd:Field>
    <cd:Field name="arr_f32">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Float>
          <cd:NativeType>float</cd:NativeType>
          <cd:ByteSize>3</cd:ByteSize>
        </cd:Float>
      </cd:Array>
      <cd:BitOffset>56</cd:BitOffset>
    </cd:Field>
    <cd:Field name="arr_f64">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Float>
          <cd:NativeType>double</cd:NativeType>
          <cd:ByteSize>3</cd:ByteSize>
        </cd:Float>
      </cd:Array>
      <cd:BitOffset>56</cd:BitOffset>
    </cd:Field>
    <cd:Field name="arr_ch">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Text>
          <cd:ByteSize>1</cd:ByteSize>
          <cd:NativeType>char</cd:NativeType>
        </cd:Text>
      </cd:Array>
      <cd:BitOffset>104</cd:BitOffset>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

constexpr std::string_view kUniProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="uni_prod" format="ascii">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>7</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="sel">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:ByteSize>2</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="denom">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:ByteSize>2</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="neg_len">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:ByteSize>3</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="bit_off">
      <cd:Integer>
        <cd:NativeType>int32</cd:NativeType>
        <cd:ByteSize>2</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="dyn_rec">
      <cd:Record>
        <cd:Field name="a">
          <cd:Text>
            <cd:ByteSize>3</cd:ByteSize>
          </cd:Text>
        </cd:Field>
        <cd:Field name="b">
          <cd:Text>
            <cd:ByteSize>int(/sel) + 2</cd:ByteSize>
          </cd:Text>
        </cd:Field>
      </cd:Record>
    </cd:Field>
    <cd:Field name="uni">
      <cd:Union>
        <cd:FieldExpression>int(/sel)</cd:FieldExpression>
        <cd:Field name="as_int">
          <cd:Integer>
            <cd:NativeType>int32</cd:NativeType>
            <cd:ByteSize>3</cd:ByteSize>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="as_text">
          <cd:Text>
            <cd:ByteSize>5</cd:ByteSize>
          </cd:Text>
        </cd:Field>
      </cd:Union>
    </cd:Field>
    <cd:Field name="neg_sized_text">
      <cd:Text>
        <cd:ByteSize>int(/neg_len)</cd:ByteSize>
      </cd:Text>
      <cd:BitOffset>128</cd:BitOffset>
    </cd:Field>
    <cd:Field name="bad_expr_text">
      <cd:Text>
        <cd:ByteSize>1 / int(/denom)</cd:ByteSize>
      </cd:Text>
      <cd:BitOffset>128</cd:BitOffset>
    </cd:Field>
    <cd:Field name="unaligned_bit_len_rec">
      <cd:Record>
        <cd:BitSize>int(/bit_off)</cd:BitSize>
      </cd:Record>
      <cd:BitOffset>128</cd:BitOffset>
    </cd:Field>
    <cd:Field name="unaligned_line">
      <cd:AsciiLine/>
      <cd:BitOffset>int(/bit_off)</cd:BitOffset>
    </cd:Field>
    <cd:Field name="unaligned_ws">
      <cd:AsciiWhiteSpace/>
      <cd:BitOffset>int(/bit_off)</cd:BitOffset>
    </cd:Field>
    <cd:Field name="unaligned_sep">
      <cd:AsciiLineSeparator/>
      <cd:BitOffset>int(/bit_off)</cd:BitOffset>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

constexpr std::string_view kBinAscProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="bin_asc_prod" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>7</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="pad3">
      <cd:Raw>
        <cd:BitSize>3</cd:BitSize>
      </cd:Raw>
    </cd:Field>
    <cd:Field name="unaligned_i64">
      <cd:Integer format="ascii">
        <cd:NativeType>int64</cd:NativeType>
        <cd:ByteSize>2</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="unaligned_u64">
      <cd:Integer format="ascii">
        <cd:NativeType>uint64</cd:NativeType>
        <cd:ByteSize>2</cd:ByteSize>
      </cd:Integer>
      <cd:BitOffset>59</cd:BitOffset>
    </cd:Field>
    <cd:Field name="unaligned_f64">
      <cd:Float format="ascii">
        <cd:NativeType>double</cd:NativeType>
        <cd:ByteSize>2</cd:ByteSize>
      </cd:Float>
      <cd:BitOffset>59</cd:BitOffset>
    </cd:Field>
    <cd:Field name="unaligned_ch">
      <cd:Text format="ascii">
        <cd:ByteSize>1</cd:ByteSize>
        <cd:NativeType>char</cd:NativeType>
      </cd:Text>
      <cd:BitOffset>59</cd:BitOffset>
    </cd:Field>
    <cd:Field name="unaligned_str">
      <cd:Text format="ascii">
        <cd:ByteSize>2</cd:ByteSize>
      </cd:Text>
      <cd:BitOffset>59</cd:BitOffset>
    </cd:Field>
    <cd:Field name="unaligned_line">
      <cd:AsciiLine format="ascii"/>
      <cd:BitOffset>59</cd:BitOffset>
    </cd:Field>
    <cd:Field name="aligned_line">
      <cd:AsciiLine format="ascii"/>
      <cd:BitOffset>64</cd:BitOffset>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

constexpr std::string_view kXmlAscProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="xml_asc_prod" format="xml">
  <cd:Record>
    <cd:Field name="XmlAsc">
      <cd:Record>
        <cd:Field name="num">
          <cd:Integer format="ascii">
            <cd:NativeType>int32</cd:NativeType>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="flt">
          <cd:Float format="ascii">
            <cd:NativeType>double</cd:NativeType>
          </cd:Float>
        </cd:Field>
        <cd:Field name="line">
          <cd:AsciiLine format="ascii"/>
        </cd:Field>
        <cd:Field name="ws">
          <cd:AsciiWhiteSpace format="ascii"/>
        </cd:Field>
      </cd:Record>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

constexpr std::string_view kEmptyAscProdXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="empty_asc_prod" format="ascii">
  <cd:Record/>
</cd:ProductDefinition>
)xml";

std::vector<uint8_t> BuildTestCodadefBytes() {
  const std::array<std::pair<std::string_view, std::string_view>, 10> kEntries =
      {{
          {"products/scalar_prod.xml", kScalarProdXml},
          {"products/time_dyn_prod.xml", kTimeDynProdXml},
          {"products/map_prod.xml", kMapProdXml},
          {"products/spec_prod.xml", kSpecProdXml},
          {"products/arr_prod.xml", kArrProdXml},
          {"products/arr_err_prod.xml", kArrErrProdXml},
          {"products/uni_prod.xml", kUniProdXml},
          {"products/bin_asc_prod.xml", kBinAscProdXml},
          {"products/xml_asc_prod.xml", kXmlAscProdXml},
          {"products/empty_asc_prod.xml", kEmptyAscProdXml},
      }};
  return BuildCodadefBytes(kIndexXml, kEntries);
}

// Allocates a synthetic coda_product using coda_format_sp3 so coda_close()
// invokes coda_sp3_close() without requiring an OS file descriptor.
ScopedProduct MakeSyntheticMemProduct() {
  auto* product =
      static_cast<coda_product*>(std::calloc(1, sizeof(coda_product)));
  if (product != nullptr) {
    product->format = coda_format_sp3;
  }
  return ScopedProduct(product);
}

class CodaAsciiCursorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    restore_def_.emplace("CODA_DEFINITION");
    restore_leap_.emplace("CODA_LEAP_SECOND_TABLE");
    restore_def_->Unset();
    restore_leap_->Unset();
    restore_options_.emplace();

    codadef_file_.emplace("coda_ascii_cursor_test.codadef");
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
    ASSERT_NO_FATAL_FAILURE(OpenProductBytes(
        filename,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()),
                                 text.size()),
        cursor));
  }

  std::optional<ScopedEnvVar> restore_def_;
  std::optional<ScopedEnvVar> restore_leap_;
  std::optional<ScopedCodaOptions> restore_options_;
  std::optional<TempFile> codadef_file_;
  ScopedCodaSession session_;
  std::optional<TempFile> product_file_;
  ScopedProduct product_;
};

// Direct parser tests (coda_ascii_parse_int64/uint64/double).

TEST(CodaAsciiCursorStandaloneTest, ParseInt64ValidAndInvalidInputs) {
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  struct ValidCase {
    std::string_view input;
    int ignore_trailing;
    int64_t expected_value;
    long expected_consumed;
  };
  constexpr ValidCase kValidCases[] = {
      {.input = "0",
       .ignore_trailing = 0,
       .expected_value = 0,
       .expected_consumed = 1},
      {.input = " \t +42 \t ",
       .ignore_trailing = 0,
       .expected_value = 42,
       .expected_consumed = 9},
      {.input = "\t-12345 ",
       .ignore_trailing = 0,
       .expected_value = -12345,
       .expected_consumed = 8},
      {.input = "9223372036854775807",
       .ignore_trailing = 0,
       .expected_value = std::numeric_limits<int64_t>::max(),
       .expected_consumed = 19},
      {.input = "-9223372036854775807",
       .ignore_trailing = 0,
       .expected_value = -std::numeric_limits<int64_t>::max(),
       .expected_consumed = 20},
      {.input = "  +987xyz",
       .ignore_trailing = 1,
       .expected_value = 987,
       .expected_consumed = 6},
  };

  for (const ValidCase& tc : kValidCases) {
    int64_t value = -1;
    const long consumed = coda_ascii_parse_int64(
        tc.input.data(), static_cast<long>(tc.input.size()), &value,
        tc.ignore_trailing);
    EXPECT_EQ(tc.expected_consumed, consumed) << tc.input;
    EXPECT_EQ(tc.expected_value, value) << tc.input;
  }

  struct InvalidCase {
    const char* input;
    long length;
    int ignore_trailing;
  };
  constexpr InvalidCase kInvalidCases[] = {
      {.input = "", .length = 0, .ignore_trailing = 0},
      {.input = " \t ", .length = 3, .ignore_trailing = 0},
      {.input = "+", .length = 1, .ignore_trailing = 1},
      {.input = "-", .length = 1, .ignore_trailing = 1},
      {.input = "abc", .length = 3, .ignore_trailing = 1},
      {.input = "9223372036854775808", .length = 19, .ignore_trailing = 0},
      {.input = "42abc", .length = 5, .ignore_trailing = 0},
      {.input = "42 99", .length = 5, .ignore_trailing = 0},
  };

  for (const InvalidCase& tc : kInvalidCases) {
    int64_t value = -999;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_ascii_parse_int64(tc.input, tc.length, &value,
                                         tc.ignore_trailing))
        << tc.input;
    EXPECT_EQ(CODA_ERROR_INVALID_FORMAT, coda_errno) << tc.input;
  }
}

TEST(CodaAsciiCursorStandaloneTest, ParseUint64ValidAndInvalidInputs) {
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  struct ValidCase {
    std::string_view input;
    int ignore_trailing;
    uint64_t expected_value;
    long expected_consumed;
  };
  constexpr ValidCase kValidCases[] = {
      {.input = "0",
       .ignore_trailing = 0,
       .expected_value = 0,
       .expected_consumed = 1},
      {.input = " \t+1024 \t",
       .ignore_trailing = 0,
       .expected_value = 1024,
       .expected_consumed = 9},
      {.input = "18446744073709551615",
       .ignore_trailing = 0,
       .expected_value = std::numeric_limits<uint64_t>::max(),
       .expected_consumed = 20},
      {.input = "+55abc",
       .ignore_trailing = 1,
       .expected_value = 55,
       .expected_consumed = 3},
  };

  for (const ValidCase& tc : kValidCases) {
    uint64_t value = 999;
    const long consumed = coda_ascii_parse_uint64(
        tc.input.data(), static_cast<long>(tc.input.size()), &value,
        tc.ignore_trailing);
    EXPECT_EQ(tc.expected_consumed, consumed) << tc.input;
    EXPECT_EQ(tc.expected_value, value) << tc.input;
  }

  struct InvalidCase {
    const char* input;
    long length;
    int ignore_trailing;
  };
  constexpr InvalidCase kInvalidCases[] = {
      {.input = "", .length = 0, .ignore_trailing = 0},
      {.input = " \t", .length = 2, .ignore_trailing = 0},
      {.input = "+", .length = 1, .ignore_trailing = 1},
      {.input = "-1", .length = 2, .ignore_trailing = 1},
      {.input = "xyz", .length = 3, .ignore_trailing = 1},
      {.input = "18446744073709551616", .length = 20, .ignore_trailing = 0},
      {.input = "42x", .length = 3, .ignore_trailing = 0},
      {.input = "42 7", .length = 4, .ignore_trailing = 0},
  };

  for (const InvalidCase& tc : kInvalidCases) {
    uint64_t value = 999;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_ascii_parse_uint64(tc.input, tc.length, &value,
                                          tc.ignore_trailing))
        << tc.input;
    EXPECT_EQ(CODA_ERROR_INVALID_FORMAT, coda_errno) << tc.input;
  }
}

TEST(CodaAsciiCursorStandaloneTest, ParseDoubleValidSpecialsAndInvalidInputs) {
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  struct ValidCase {
    std::string_view input;
    int ignore_trailing;
    double expected_value;
    long expected_consumed;
  };
  constexpr ValidCase kValidCases[] = {
      {.input = "42",
       .ignore_trailing = 0,
       .expected_value = 42.0,
       .expected_consumed = 2},
      {.input = " \t -12.5 \t",
       .ignore_trailing = 0,
       .expected_value = -12.5,
       .expected_consumed = 10},
      {.input = "+.25",
       .ignore_trailing = 0,
       .expected_value = 0.25,
       .expected_consumed = 4},
      {.input = "1.5e2",
       .ignore_trailing = 0,
       .expected_value = 150.0,
       .expected_consumed = 5},
      {.input = "2.5D-1",
       .ignore_trailing = 0,
       .expected_value = 0.25,
       .expected_consumed = 6},
      {.input = "-3.0d+2abc",
       .ignore_trailing = 1,
       .expected_value = -300.0,
       .expected_consumed = 7},
      {.input = "1.0E-2000",
       .ignore_trailing = 0,
       .expected_value = 0.0,
       .expected_consumed = 9},
  };

  for (const ValidCase& tc : kValidCases) {
    double value = -999.0;
    const long consumed = coda_ascii_parse_double(
        tc.input.data(), static_cast<long>(tc.input.size()), &value,
        tc.ignore_trailing);
    EXPECT_EQ(tc.expected_consumed, consumed) << tc.input;
    EXPECT_THAT(value, DoubleEq(tc.expected_value)) << tc.input;
  }

  // NaN / Inf cases including exponent overflow to +Inf.
  double val = 0.0;
  EXPECT_EQ(3, coda_ascii_parse_double("NaN", 3, &val, 0));
  EXPECT_EQ(1, coda_isNaN(val));

  EXPECT_EQ(4, coda_ascii_parse_double(" nanx", 5, &val, 1));
  EXPECT_EQ(1, coda_isNaN(val));

  EXPECT_EQ(4, coda_ascii_parse_double("+Inf", 4, &val, 0));
  EXPECT_EQ(1, coda_isPlusInf(val));

  EXPECT_EQ(5, coda_ascii_parse_double("\t-inf", 5, &val, 0));
  EXPECT_EQ(1, coda_isMinInf(val));

  EXPECT_EQ(8, coda_ascii_parse_double("1.0e2000", 8, &val, 0));
  EXPECT_EQ(1, coda_isPlusInf(val));

  struct InvalidCase {
    const char* input;
    long length;
    int ignore_trailing;
  };
  constexpr InvalidCase kInvalidCases[] = {
      {.input = "", .length = 0, .ignore_trailing = 0},
      {.input = ".", .length = 1, .ignore_trailing = 0},
      {.input = "+.", .length = 2, .ignore_trailing = 1},
      {.input = "+nan", .length = 4, .ignore_trailing = 1},
      {.input = "nanx", .length = 4, .ignore_trailing = 0},
      {.input = "infx", .length = 4, .ignore_trailing = 0},
      {.input = "abc", .length = 3, .ignore_trailing = 1},
      {.input = "1.0e", .length = 4, .ignore_trailing = 1},
      {.input = "1.0D+", .length = 5, .ignore_trailing = 1},
      {.input = "1.5xyz", .length = 6, .ignore_trailing = 0},
      {.input = "1.5 2.0", .length = 7, .ignore_trailing = 0},
  };

  for (const InvalidCase& tc : kInvalidCases) {
    double dst = -999.0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_ascii_parse_double(tc.input, tc.length, &dst,
                                          tc.ignore_trailing))
        << tc.input;
    EXPECT_EQ(CODA_ERROR_INVALID_FORMAT, coda_errno) << tc.input;
  }
}

// Asciilines setup, caching, >4096 block boundaries, LF/CR/CRLF, and errors.

TEST_F(CodaAsciiCursorTest, AsciilinesLargeLfCrCrlfBuffersAndCaching) {
  // 1. LF file > ASCII_PARSE_BLOCK_SIZE (4096) with > BLOCK_SIZE (16) lines.
  std::string lf_text = "ASC_SPEC_IGNORE\n";
  constexpr int kNumLfLines = 30;
  for (int i = 1; i < kNumLfLines; ++i) {
    std::string line = absl::StrFormat("line_%02d_", i);
    line.append(160, static_cast<char>('a' + (i % 26)));
    line.push_back('\n');
    lf_text.append(line);
  }
  // Final line without trailing EOL to cover lastline_ending == eol_unknown.
  lf_text.append("last_line_no_eol");
  ASSERT_GT(lf_text.size(), 4096U);

  coda_cursor root_cursor;
  ASSERT_NO_FATAL_FAILURE(
      OpenProductText("large_lf.asc", lf_text, &root_cursor));

  coda_cursor lines_cursor;
  {
    // TODO(schwehr): coda-ascii-cursor.c:611-625 allocates `asciiline` via
    // coda_type_text_new() and retains it in coda_type_array_set_base_type()
    // without releasing the initial reference on success.
    const absl::LeakCheckDisabler disabler;
    ASSERT_EQ(CODA_SUCCESS,
              coda_ascii_cursor_set_asciilines(&lines_cursor, product_.get()));
  }
  // Second call on the same product reuses cached product_file->asciilines.
  coda_cursor cached_cursor;
  ASSERT_EQ(CODA_SUCCESS,
            coda_ascii_cursor_set_asciilines(&cached_cursor, product_.get()));

  long num_lines = 0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_num_elements(&cached_cursor, &num_lines));
  EXPECT_EQ(kNumLfLines + 1, num_lines);

  for (const long idx : {0L, 1L, 15L, static_cast<long>(kNumLfLines)}) {
    coda_cursor elem = cached_cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_array_element_by_index(&elem, idx));
    long str_len = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&elem, &str_len));
    std::string buf(static_cast<size_t>(str_len) + 1, '\0');
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&elem, buf.data(),
                                      static_cast<long>(buf.size())));
    buf.resize(static_cast<size_t>(str_len));
    if (idx == 0) {
      EXPECT_EQ("ASC_SPEC_IGNORE\n", buf);
    } else if (idx == kNumLfLines) {
      EXPECT_EQ("last_line_no_eol", buf);
    } else {
      EXPECT_TRUE(buf.starts_with(absl::StrFormat("line_%02ld_", idx)));
      EXPECT_TRUE(buf.ends_with("\n"));
    }
  }

  // Also test public expression `str(asciiline[0])` on the same product.
  ScopedExpr expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("str(asciiline[0])", expr.OutParam()));
  ScopedCodaString eval_str;
  long eval_len = 0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_eval_string(expr.get(), &root_cursor,
                                        eval_str.OutParam(), &eval_len));
  EXPECT_STREQ("ASC_SPEC_IGNORE\n", eval_str.get());

  // 2. CRLF spanning the 4096-byte ASCII_PARSE_BLOCK_SIZE boundary:
  //    byte 4095 == '\r', byte 4096 == '\n'.
  std::string crlf_text = "ASC_SPEC";
  crlf_text.append(4087, 'x');
  crlf_text.append("\r\nsecond_crlf_line\r\n");
  ASSERT_NO_FATAL_FAILURE(
      OpenProductText("large_crlf.asc", crlf_text, &root_cursor));
  {
    const absl::LeakCheckDisabler disabler;
    ASSERT_EQ(CODA_SUCCESS,
              coda_ascii_cursor_set_asciilines(&lines_cursor, product_.get()));
  }
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_num_elements(&lines_cursor, &num_lines));
  EXPECT_EQ(2, num_lines);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element_by_index(&lines_cursor, 0));
  int64_t bit_size = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&lines_cursor, &bit_size));
  EXPECT_EQ(4097 * 8, bit_size);

  // 3. CR spanning the 4096-byte boundary (byte 4095 == '\r', byte 4096 == 'y')
  //    and trailing '\r' at EOF.
  std::string cr_text = "ASC_SPEC";
  cr_text.append(4087, 'z');
  cr_text.append("\ry_after_cr\r");
  ASSERT_NO_FATAL_FAILURE(
      OpenProductText("large_cr.asc", cr_text, &root_cursor));
  {
    const absl::LeakCheckDisabler disabler;
    ASSERT_EQ(CODA_SUCCESS,
              coda_ascii_cursor_set_asciilines(&lines_cursor, product_.get()));
  }
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_num_elements(&lines_cursor, &num_lines));
  EXPECT_EQ(2, num_lines);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element_by_index(&lines_cursor, 1));
  char line2[16] = {};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&lines_cursor, line2, sizeof(line2)));
  EXPECT_STREQ("y_after_cr\r", line2);
}

TEST_F(CodaAsciiCursorTest, AsciilinesErrorPathsAndEmptyFile) {
  coda_cursor root_cursor;
  coda_cursor lines_cursor;

  // 1. Mixed EOLs cause coda_ascii_init_asciilines() to fail.
  ASSERT_NO_FATAL_FAILURE(
      OpenProductText("mixed_eol.asc", "ASC_SPEC\nline2\r\n", &root_cursor));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_ascii_cursor_set_asciilines(&lines_cursor, product_.get()));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);

  // 2. Empty ASCII file initializes num_asciilines == 0; reading line bit size
  //    reports CODA_ERROR_OUT_OF_BOUNDS_READ.
  ASSERT_NO_FATAL_FAILURE(OpenProductText("empty.asc", "", &root_cursor));
  {
    const absl::LeakCheckDisabler disabler;
    ASSERT_EQ(CODA_SUCCESS,
              coda_ascii_cursor_set_asciilines(&lines_cursor, product_.get()));
  }
  long num_lines = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_num_elements(&lines_cursor, &num_lines));
  EXPECT_EQ(0, num_lines);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&lines_cursor, 0));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  // Disable boundary checks to step onto element 0 and hit empty-file check in
  // coda_ascii_cursor_get_bit_size().
  coda_set_option_perform_boundary_checks(0);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element_by_index(&lines_cursor, 0));
  int64_t bit_size = 0;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_ascii_cursor_get_bit_size(&lines_cursor, &bit_size));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

  // 3. `asciiline` expression on a binary product fails with
  //    CODA_ERROR_EXPRESSION.
  ASSERT_NO_FATAL_FAILURE(
      OpenProductText("bin_for_expr.bin", "BIN_ASCAA\n", &root_cursor));
  ScopedExpr expr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_expression_from_string("str(asciiline[0])", expr.OutParam()));
  ScopedCodaString eval_str;
  long eval_len = 0;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_expression_eval_string(expr.get(), &root_cursor,
                                            eval_str.OutParam(), &eval_len));
  EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);
}

// Scalar reads, conversions, dynamic sizes, time, complex, raw bits/bytes,
// and mmap=0 vs mmap=1 I/O modes.

TEST_F(CodaAsciiCursorTest, ScalarReadsConversionsAndMmapToggle) {
  // Construct payload matching kScalarProdXml field-for-field.
  std::string payload;
  payload.append("ASC_SCALAR");             // magic (10)
  payload.append(" ");                      // s1 (1)
  payload.append("-128");                   // i8 (4)
  payload.append(" 255");                   // u8 (4)
  payload.append("-32768");                 // i16 (6)
  payload.append(" 65535");                 // u16 (6)
  payload.append("-2147483648");            // i32 (11)
  payload.append(" 4294967295");            // u32 (11)
  payload.append(" 9223372036854775807");   // i64 (20)
  payload.append(" 18446744073709551615");  // u64 (21)
  payload.append("  -12.5 ");               // f32 (8)
  payload.append("  1.25e+2 ");             // f64 (10)
  payload.append("Z");                      // ch (1)
  payload.append(" -77");                   // dyn_i32 (4)
  payload.append(" ");                      // s2 (1)
  payload.append("88");                     // dyn_u32 (2)
  payload.append(" ");                      // s3 (1)
  payload.append("3.5");                    // dyn_f64 (3)
  payload.append(" ");                      // s4 (1)
  payload.append("+00100");                 // scaled_i16 (6)
  payload.append("  -999");                 // scaled_i16_inv (6)
  payload.append("    4.0 ");               // scaled_f64 (8)
  payload.append("  5");                    // text_len (3)
  payload.append("HELLO");                  // dyn_text (5)
  payload.append("2000-01-01T00:00:10");    // time_fixed (19)
  payload.append(" ");                      // s5 (1)
  payload.append("2000-01-01T00:00:20");    // time_line (19)
  payload.append("\n");                     // sep (1)
  payload.append("1.5 2.5");                // cplx_dyn (7)
  payload.append(" ");                      // s6 (1)
  payload.append("RAW!");                   // raw4 (4)
  payload.append(" 16");                    // raw_bits (3)
  payload.append("AB");                     // raw_expr (2)
  payload.append("\n");                     // trailing newline

  for (const int use_mmap : {1, 0}) {
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_use_mmap(use_mmap));

    coda_cursor root;
    ASSERT_NO_FATAL_FAILURE(OpenProductText("scalars.asc", payload, &root));

    long root_num_elems = 0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_ascii_cursor_get_num_elements(&root, &root_num_elems));
    EXPECT_EQ(33, root_num_elems);

    auto goto_field = [&](const char* name) {
      coda_cursor c = root;
      EXPECT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&c, name))
          << name;
      return c;
    };

    coda_cursor c = goto_field("i8");
    int8_t i8_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&c, &i8_val));
    EXPECT_EQ(-128, i8_val);
    long scalar_elems = 0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_ascii_cursor_get_num_elements(&c, &scalar_elems));
    EXPECT_EQ(1, scalar_elems);

    c = goto_field("u8");
    uint8_t u8_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&c, &u8_val));
    EXPECT_EQ(255U, u8_val);

    c = goto_field("i16");
    int16_t i16_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&c, &i16_val));
    EXPECT_EQ(-32768, i16_val);

    c = goto_field("u16");
    uint16_t u16_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&c, &u16_val));
    EXPECT_EQ(65535U, u16_val);

    c = goto_field("i32");
    int32_t i32_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &i32_val));
    EXPECT_EQ(std::numeric_limits<int32_t>::min(), i32_val);

    c = goto_field("u32");
    uint32_t u32_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint32(&c, &u32_val));
    EXPECT_EQ(std::numeric_limits<uint32_t>::max(), u32_val);

    c = goto_field("i64");
    int64_t i64_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c, &i64_val));
    EXPECT_EQ(std::numeric_limits<int64_t>::max(), i64_val);

    c = goto_field("u64");
    uint64_t u64_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint64(&c, &u64_val));
    EXPECT_EQ(std::numeric_limits<uint64_t>::max(), u64_val);

    c = goto_field("f32");
    float f32_val = 0.0f;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c, &f32_val));
    EXPECT_THAT(f32_val, FloatEq(-12.5f));

    c = goto_field("f64");
    double f64_val = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &f64_val));
    EXPECT_THAT(f64_val, DoubleEq(125.0));

    // Dynamic-size numbers (size determined by parsing).
    c = goto_field("dyn_i32");
    int64_t bits = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bits));
    EXPECT_EQ(32, bits);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &i32_val));
    EXPECT_EQ(-77, i32_val);

    c = goto_field("dyn_u32");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bits));
    EXPECT_EQ(16, bits);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint32(&c, &u32_val));
    EXPECT_EQ(88U, u32_val);

    c = goto_field("dyn_f64");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bits));
    EXPECT_EQ(24, bits);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &f64_val));
    EXPECT_THAT(f64_val, DoubleEq(3.5));

    // Toggle coda_option_perform_conversions between 1 and 0.
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(1));
    c = goto_field("scaled_i16");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &f64_val));
    EXPECT_THAT(f64_val, DoubleEq(60.0));
    c = goto_field("scaled_i16_inv");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &f64_val));
    EXPECT_EQ(1, coda_isNaN(f64_val));
    c = goto_field("scaled_f64");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &f64_val));
    EXPECT_THAT(f64_val, DoubleEq(5.0));

    ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(0));
    c = goto_field("scaled_i16");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&c, &i16_val));
    EXPECT_EQ(100, i16_val);
    c = goto_field("scaled_i16_inv");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&c, &i16_val));
    EXPECT_EQ(-999, i16_val);
    c = goto_field("scaled_f64");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &f64_val));
    EXPECT_THAT(f64_val, DoubleEq(4.0));
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(1));

    // Char and dynamic string (plus buffer truncation).
    c = goto_field("ch");
    char ch = '\0';
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&c, &ch));
    EXPECT_EQ('Z', ch);

    c = goto_field("dyn_text");
    long str_len = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&c, &str_len));
    EXPECT_EQ(5, str_len);
    char full_text[6] = {};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&c, full_text, sizeof(full_text)));
    EXPECT_STREQ("HELLO", full_text);
    char trunc_text[4] = {};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&c, trunc_text, sizeof(trunc_text)));
    EXPECT_STREQ("HEL", trunc_text);
    char empty_dst[1] = {'X'};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&c, empty_dst, sizeof(empty_dst)));
    EXPECT_EQ('\0', empty_dst[0]);

    // Fixed and special-line ASCII Time fields.
    c = goto_field("time_fixed");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bits));
    EXPECT_EQ(19 * 8, bits);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &f64_val));
    EXPECT_THAT(f64_val, DoubleEq(10.0));

    c = goto_field("time_line");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bits));
    EXPECT_EQ(19 * 8, bits);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &f64_val));
    EXPECT_THAT(f64_val, DoubleEq(20.0));

    // Dynamic-element Complex special type.
    c = goto_field("cplx_dyn");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bits));
    EXPECT_EQ(7 * 8, bits);
    double cplx[2] = {0.0, 0.0};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_complex_double_pair(&c, cplx));
    EXPECT_THAT(cplx, ElementsAre(DoubleEq(1.5), DoubleEq(2.5)));

    // Raw fixed & dynamic-bit-size fields via read_bytes and read_bits.
    c = goto_field("raw4");
    std::array<uint8_t, 4> raw4_buf{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_bytes(&c, raw4_buf.data(), 0, 4));
    EXPECT_THAT(raw4_buf, ElementsAre('R', 'A', 'W', '!'));
    std::array<uint8_t, 2> mid_bits{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_bits(&c, mid_bits.data(), 8, 16));
    EXPECT_THAT(mid_bits, ElementsAre('A', 'W'));

    c = goto_field("raw_expr");
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bits));
    EXPECT_EQ(16, bits);
    std::array<uint8_t, 2> raw2_buf{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_bytes(&c, raw2_buf.data(), 0, 2));
    EXPECT_THAT(raw2_buf, ElementsAre('A', 'B'));
  }
}

TEST_F(CodaAsciiCursorTest, TimeWithDynamicDefaultTextBaseType) {
  coda_cursor root;
  ASSERT_NO_FATAL_FAILURE(OpenProductText(
      "time_dyn.asc", "ASC_TDYN 192000-01-01T00:00:30\n", &root));
  coda_cursor ts = root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&ts, "ts"));

  int64_t bit_size = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&ts, &bit_size));
  // coda_special_time with ascii_text_default base type returns boundary size
  // (remaining bytes up to EOF: 19 chars + '\n' = 20 bytes).
  EXPECT_EQ(20 * 8, bit_size);

  double sec = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&ts, &sec));
  EXPECT_THAT(sec, DoubleEq(30.0));
}

// ASCII integer/float mappings (dynamic, default_bit_size, uint64, NaN/Inf).

TEST_F(CodaAsciiCursorTest, AsciiIntegerAndFloatMappings) {
  const std::string payload =
      "ASC_MAP"
      "NA "
      "42 "
      "0099 "
      "NONE "
      "123 "
      "MISSING "
      "2.5 "
      "+3.25\n";

  coda_cursor root;
  ASSERT_NO_FATAL_FAILURE(OpenProductText("mappings.asc", payload, &root));

  auto goto_field = [&](const char* name) {
    coda_cursor c = root;
    EXPECT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&c, name))
        << name;
    return c;
  };

  int32_t i32 = 0;
  int64_t bits = 0;
  coda_cursor c = goto_field("dyn_i_hit");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bits));
  EXPECT_EQ(16, bits);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &i32));
  EXPECT_EQ(-999, i32);

  c = goto_field("dyn_i_miss");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bits));
  EXPECT_EQ(16, bits);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &i32));
  EXPECT_EQ(42, i32);

  c = goto_field("def_i_miss");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bits));
  EXPECT_EQ(32, bits);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &i32));
  EXPECT_EQ(99, i32);

  uint64_t u64 = 0;
  c = goto_field("u64_hit");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint64(&c, &u64));
  EXPECT_EQ(777ULL, u64);

  c = goto_field("u64_miss");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint64(&c, &u64));
  EXPECT_EQ(123ULL, u64);

  double f64 = 0.0;
  c = goto_field("dyn_f_nan");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bits));
  EXPECT_EQ(56, bits);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &f64));
  EXPECT_EQ(1, coda_isNaN(f64));

  c = goto_field("dyn_f_miss");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &f64));
  EXPECT_THAT(f64, DoubleEq(2.5));

  c = goto_field("def_f_miss");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bits));
  EXPECT_EQ(40, bits);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &f64));
  EXPECT_THAT(f64, DoubleEq(3.25));
}

// Special text types: AsciiWhiteSpace, AsciiLine, AsciiLineSeparator
// (LF/CR/CRLF).

TEST_F(CodaAsciiCursorTest, SpecialTextTypesAcrossLfCrCrlfAndErrors) {
  struct EolCase {
    std::string_view eol;
    int64_t expected_sep_bits;
  };
  constexpr EolCase kEolCases[] = {
      {.eol = "\n", .expected_sep_bits = 8},
      {.eol = "\r", .expected_sep_bits = 8},
      {.eol = "\r\n", .expected_sep_bits = 16},
  };

  for (const EolCase& tc : kEolCases) {
    std::string text = "ASC_SPEC";
    text.append(tc.eol);
    text.append(" \t alpha");
    text.append(tc.eol);
    text.append("beta");
    text.append(tc.eol);
    text.append("gamma");
    text.append(tc.eol);

    coda_cursor root;
    ASSERT_NO_FATAL_FAILURE(OpenProductText("spec.asc", text, &root));

    // header_sep runs before any AsciiLine, exercising eol_unknown detection.
    coda_cursor header_sep = root;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&header_sep, "header_sep"));
    int64_t sep_bits = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&header_sep, &sep_bits));
    EXPECT_EQ(tc.expected_sep_bits, sep_bits);

    auto read_str_field = [&](const char* name) {
      coda_cursor c = root;
      EXPECT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&c, name));
      long len = -1;
      EXPECT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&c, &len));
      std::string out(static_cast<size_t>(len) + 1, '\0');
      EXPECT_EQ(CODA_SUCCESS,
                coda_cursor_read_string(&c, out.data(),
                                        static_cast<long>(out.size())));
      out.resize(static_cast<size_t>(len));
      return out;
    };

    EXPECT_EQ(" \t ", read_str_field("ws"));
    EXPECT_EQ("", read_str_field("ws_zero"));
    EXPECT_EQ("alpha", read_str_field("line1"));

    coda_cursor sep1 = root;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&sep1, "sep1"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&sep1, &sep_bits));
    EXPECT_EQ(tc.expected_sep_bits, sep_bits);

    EXPECT_EQ("beta", read_str_field("line2"));
    coda_cursor sep2 = root;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&sep2, "sep2"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&sep2, &sep_bits));
    EXPECT_EQ(tc.expected_sep_bits, sep_bits);

    EXPECT_EQ("gamma", read_str_field("line3"));
  }

  // Invalid line separator character ('X') and '\r' at exact EOF (eol_unknown).
  coda_cursor root;
  ASSERT_NO_FATAL_FAILURE(
      OpenProductText("spec_bad_sep.asc", "ASC_SPECX", &root));
  coda_cursor header_sep = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&header_sep, "header_sep"));
  int64_t sep_bits = 0;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_bit_size(&header_sep, &sep_bits));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);

  ASSERT_NO_FATAL_FAILURE(
      OpenProductText("spec_cr_eof.asc", "ASC_SPEC\r", &root));
  header_sep = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&header_sep, "header_sep"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&header_sep, &sep_bits));
  EXPECT_EQ(8, sep_bits);
}

// All 11 typed array & partial-array readers (C and Fortran order + error
// paths).

template <typename T>
struct AsciiArrayTraits;

template <>
struct AsciiArrayTraits<int8_t> {
  static constexpr const char* kField = "arr_i8";
  static constexpr std::array<int8_t, 6> kCOrder = {-10, 20, -30, 40, -50, 60};
  static constexpr std::array<int8_t, 6> kFortranOrder = {-10, 40,  20,
                                                          -50, -30, 60};
  static std::string Format() { return " -10  20 -30  40 -50  60"; }
  static int ReadAll(const coda_cursor* c, int8_t* dst,
                     coda_array_ordering ord) {
    return coda_ascii_cursor_read_int8_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         int8_t* dst) {
    return coda_ascii_cursor_read_int8_partial_array(c, off, len, dst);
  }
};

template <>
struct AsciiArrayTraits<uint8_t> {
  static constexpr const char* kField = "arr_u8";
  static constexpr std::array<uint8_t, 6> kCOrder = {10, 20, 30, 40, 50, 250};
  static constexpr std::array<uint8_t, 6> kFortranOrder = {10, 40, 20,
                                                           50, 30, 250};
  static std::string Format() { return "  10  20  30  40  50 250"; }
  static int ReadAll(const coda_cursor* c, uint8_t* dst,
                     coda_array_ordering ord) {
    return coda_ascii_cursor_read_uint8_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         uint8_t* dst) {
    return coda_ascii_cursor_read_uint8_partial_array(c, off, len, dst);
  }
};

template <>
struct AsciiArrayTraits<int16_t> {
  static constexpr const char* kField = "arr_i16";
  static constexpr std::array<int16_t, 6> kCOrder = {-100, 200,  -300,
                                                     400,  -500, 600};
  static constexpr std::array<int16_t, 6> kFortranOrder = {-100, 400,  200,
                                                           -500, -300, 600};
  static std::string Format() { return "-100 200-300 400-500 600"; }
  static int ReadAll(const coda_cursor* c, int16_t* dst,
                     coda_array_ordering ord) {
    return coda_ascii_cursor_read_int16_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         int16_t* dst) {
    return coda_ascii_cursor_read_int16_partial_array(c, off, len, dst);
  }
};

template <>
struct AsciiArrayTraits<uint16_t> {
  static constexpr const char* kField = "arr_u16";
  static constexpr std::array<uint16_t, 6> kCOrder = {100, 200, 300,
                                                      400, 500, 600};
  static constexpr std::array<uint16_t, 6> kFortranOrder = {100, 400, 200,
                                                            500, 300, 600};
  static std::string Format() { return " 100 200 300 400 500 600"; }
  static int ReadAll(const coda_cursor* c, uint16_t* dst,
                     coda_array_ordering ord) {
    return coda_ascii_cursor_read_uint16_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         uint16_t* dst) {
    return coda_ascii_cursor_read_uint16_partial_array(c, off, len, dst);
  }
};

template <>
struct AsciiArrayTraits<int32_t> {
  static constexpr const char* kField = "arr_i32";
  static constexpr std::array<int32_t, 6> kCOrder = {-101, 202,  -303,
                                                     404,  -505, 606};
  static constexpr std::array<int32_t, 6> kFortranOrder = {-101, 404,  202,
                                                           -505, -303, 606};
  static std::string Format() { return "-101 202-303 404-505 606"; }
  static int ReadAll(const coda_cursor* c, int32_t* dst,
                     coda_array_ordering ord) {
    return coda_ascii_cursor_read_int32_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         int32_t* dst) {
    return coda_ascii_cursor_read_int32_partial_array(c, off, len, dst);
  }
};

template <>
struct AsciiArrayTraits<uint32_t> {
  static constexpr const char* kField = "arr_u32";
  static constexpr std::array<uint32_t, 6> kCOrder = {111, 222, 333,
                                                      444, 555, 666};
  static constexpr std::array<uint32_t, 6> kFortranOrder = {111, 444, 222,
                                                            555, 333, 666};
  static std::string Format() { return " 111 222 333 444 555 666"; }
  static int ReadAll(const coda_cursor* c, uint32_t* dst,
                     coda_array_ordering ord) {
    return coda_ascii_cursor_read_uint32_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         uint32_t* dst) {
    return coda_ascii_cursor_read_uint32_partial_array(c, off, len, dst);
  }
};

template <>
struct AsciiArrayTraits<uint64_t> {
  static constexpr const char* kField = "arr_u64";
  static constexpr std::array<uint64_t, 6> kCOrder = {1001, 1002, 1003,
                                                      1004, 1005, 1006};
  static constexpr std::array<uint64_t, 6> kFortranOrder = {1001, 1004, 1002,
                                                            1005, 1003, 1006};
  static std::string Format() { return "100110021003100410051006"; }
  static int ReadAll(const coda_cursor* c, uint64_t* dst,
                     coda_array_ordering ord) {
    return coda_ascii_cursor_read_uint64_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         uint64_t* dst) {
    return coda_ascii_cursor_read_uint64_partial_array(c, off, len, dst);
  }
};

template <>
struct AsciiArrayTraits<float> {
  static constexpr const char* kField = "arr_f32";
  static constexpr std::array<float, 6> kCOrder = {1.5f,  -2.5f, 3.5f,
                                                   -4.5f, 5.5f,  -6.5f};
  static constexpr std::array<float, 6> kFortranOrder = {1.5f, -4.5f, -2.5f,
                                                         5.5f, 3.5f,  -6.5f};
  static std::string Format() { return "  1.5 -2.5  3.5 -4.5  5.5 -6.5"; }
  static int ReadAll(const coda_cursor* c, float* dst,
                     coda_array_ordering ord) {
    return coda_ascii_cursor_read_float_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len, float* dst) {
    return coda_ascii_cursor_read_float_partial_array(c, off, len, dst);
  }
};

template <>
struct AsciiArrayTraits<double> {
  static constexpr const char* kField = "arr_f64";
  static constexpr std::array<double, 6> kCOrder = {1.25,  -2.25, 3.25,
                                                    -4.25, 5.25,  -6.25};
  static constexpr std::array<double, 6> kFortranOrder = {1.25, -4.25, -2.25,
                                                          5.25, 3.25,  -6.25};
  static std::string Format() { return " 1.25-2.25 3.25-4.25 5.25-6.25"; }
  static int ReadAll(const coda_cursor* c, double* dst,
                     coda_array_ordering ord) {
    return coda_ascii_cursor_read_double_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len,
                         double* dst) {
    return coda_ascii_cursor_read_double_partial_array(c, off, len, dst);
  }
};

template <>
struct AsciiArrayTraits<char> {
  static constexpr const char* kField = "arr_ch";
  static constexpr std::array<char, 6> kCOrder = {'A', 'B', 'C', 'D', 'E', 'F'};
  static constexpr std::array<char, 6> kFortranOrder = {'A', 'D', 'B',
                                                        'E', 'C', 'F'};
  static std::string Format() { return "ABCDEF"; }
  static int ReadAll(const coda_cursor* c, char* dst, coda_array_ordering ord) {
    return coda_ascii_cursor_read_char_array(c, dst, ord);
  }
  static int ReadPartial(const coda_cursor* c, long off, long len, char* dst) {
    return coda_ascii_cursor_read_char_partial_array(c, off, len, dst);
  }
};

template <typename T>
void VerifyArrayType(const coda_cursor& root) {
  coda_cursor c = root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(
                              &c, AsciiArrayTraits<T>::kField));

  std::array<T, 6> out{};
  ASSERT_EQ(CODA_SUCCESS, AsciiArrayTraits<T>::ReadAll(&c, out.data(),
                                                       coda_array_ordering_c));
  EXPECT_THAT(out, ElementsAreArray(AsciiArrayTraits<T>::kCOrder));

  out.fill(T{});
  ASSERT_EQ(CODA_SUCCESS, AsciiArrayTraits<T>::ReadAll(
                              &c, out.data(), coda_array_ordering_fortran));
  EXPECT_THAT(out, ElementsAreArray(AsciiArrayTraits<T>::kFortranOrder));

  std::array<T, 3> partial{};
  ASSERT_EQ(CODA_SUCCESS,
            AsciiArrayTraits<T>::ReadPartial(&c, 2, 3, partial.data()));
  EXPECT_THAT(partial, ElementsAre(AsciiArrayTraits<T>::kCOrder[2],
                                   AsciiArrayTraits<T>::kCOrder[3],
                                   AsciiArrayTraits<T>::kCOrder[4]));
}

TEST_F(CodaAsciiCursorTest, TypedArrayAndPartialArrayReadsAllTypes) {
  std::string payload = "ASC_ARR";
  payload.append(" 3");  // cnt
  payload.append(AsciiArrayTraits<int8_t>::Format());
  payload.append(AsciiArrayTraits<uint8_t>::Format());
  payload.append(AsciiArrayTraits<int16_t>::Format());
  payload.append(AsciiArrayTraits<uint16_t>::Format());
  payload.append(AsciiArrayTraits<int32_t>::Format());
  payload.append(AsciiArrayTraits<uint32_t>::Format());
  payload.append("-901 902-903 904");  // arr_i64 (4 * 4 bytes)
  payload.append(AsciiArrayTraits<uint64_t>::Format());
  payload.append(AsciiArrayTraits<float>::Format());
  payload.append(AsciiArrayTraits<double>::Format());
  payload.append(AsciiArrayTraits<char>::Format());
  payload.append(" 11 22 33");  // arr_dyn (3 * 3 bytes)
  payload.append("\n");

  coda_cursor root;
  ASSERT_NO_FATAL_FAILURE(OpenProductText("arrays.asc", payload, &root));

  VerifyArrayType<int8_t>(root);
  VerifyArrayType<uint8_t>(root);
  VerifyArrayType<int16_t>(root);
  VerifyArrayType<uint16_t>(root);
  VerifyArrayType<int32_t>(root);
  VerifyArrayType<uint32_t>(root);
  VerifyArrayType<uint64_t>(root);
  VerifyArrayType<float>(root);
  VerifyArrayType<double>(root);
  VerifyArrayType<char>(root);

  // int64 array: 1D array avoids the upstream sizeof(int32_t) transpose quirk
  // at coda-ascii-cursor.c:1607 while still exercising both ordering branches.
  coda_cursor c_i64 = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&c_i64, "arr_i64"));
  std::array<int64_t, 4> i64_out{};
  ASSERT_EQ(CODA_SUCCESS, coda_ascii_cursor_read_int64_array(
                              &c_i64, i64_out.data(), coda_array_ordering_c));
  EXPECT_THAT(i64_out, ElementsAre(-901, 902, -903, 904));
  i64_out.fill(0);
  ASSERT_EQ(CODA_SUCCESS,
            coda_ascii_cursor_read_int64_array(&c_i64, i64_out.data(),
                                               coda_array_ordering_fortran));
  EXPECT_THAT(i64_out, ElementsAre(-901, 902, -903, 904));
  std::array<int64_t, 2> i64_partial{};
  ASSERT_EQ(CODA_SUCCESS, coda_ascii_cursor_read_int64_partial_array(
                              &c_i64, 1, 2, i64_partial.data()));
  EXPECT_THAT(i64_partial, ElementsAre(902, -903));

  // Variable-dimension ASCII array bit size, num elements, and boundary checks.
  coda_cursor c_dyn = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&c_dyn, "arr_dyn"));
  int64_t dyn_bits = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c_dyn, &dyn_bits));
  EXPECT_EQ(9 * 8, dyn_bits);
  long dyn_elems = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&c_dyn, &dyn_elems));
  EXPECT_EQ(3, dyn_elems);
  std::array<int32_t, 3> dyn_vals{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(&c_dyn, dyn_vals.data(),
                                                       coda_array_ordering_c));
  EXPECT_THAT(dyn_vals, ElementsAre(11, 22, 33));

  coda_set_option_perform_boundary_checks(1);
  coda_set_error(CODA_SUCCESS, nullptr);
  coda_cursor oob = c_dyn;
  EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&oob, 5));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
}

TEST_F(CodaAsciiCursorTest, TypedArrayErrorPropagation) {
  // Byte 7..12 ("badbad") fails numeric parsing; byte 13 ("X") is valid for
  // arr_ch at BitOffset 104, whose 2nd element exceeds EOF and fails OOB.
  coda_cursor root;
  ASSERT_NO_FATAL_FAILURE(
      OpenProductText("arr_err.asc", "ERR_ARRbadbadX", &root));

  auto expect_array_fail = [&](const char* field_name, auto read_fn) {
    coda_cursor c = root;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&c, field_name))
        << field_name;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, read_fn(&c)) << field_name;
    EXPECT_NE(CODA_SUCCESS, coda_errno) << field_name;
  };

  std::array<int8_t, 2> i8{};
  std::array<uint8_t, 2> u8{};
  std::array<int16_t, 2> i16{};
  std::array<uint16_t, 2> u16{};
  std::array<int32_t, 2> i32{};
  std::array<uint32_t, 2> u32{};
  std::array<int64_t, 2> i64{};
  std::array<uint64_t, 2> u64{};
  std::array<float, 2> f32{};
  std::array<double, 2> f64{};
  std::array<char, 2> ch{};

  expect_array_fail("arr_i8", [&](const coda_cursor* c) {
    return coda_ascii_cursor_read_int8_array(c, i8.data(),
                                             coda_array_ordering_c);
  });
  expect_array_fail("arr_u8", [&](const coda_cursor* c) {
    return coda_ascii_cursor_read_uint8_array(c, u8.data(),
                                              coda_array_ordering_c);
  });
  expect_array_fail("arr_i16", [&](const coda_cursor* c) {
    return coda_ascii_cursor_read_int16_array(c, i16.data(),
                                              coda_array_ordering_c);
  });
  expect_array_fail("arr_u16", [&](const coda_cursor* c) {
    return coda_ascii_cursor_read_uint16_array(c, u16.data(),
                                               coda_array_ordering_c);
  });
  expect_array_fail("arr_i32", [&](const coda_cursor* c) {
    return coda_ascii_cursor_read_int32_array(c, i32.data(),
                                              coda_array_ordering_c);
  });
  expect_array_fail("arr_u32", [&](const coda_cursor* c) {
    return coda_ascii_cursor_read_uint32_array(c, u32.data(),
                                               coda_array_ordering_c);
  });
  expect_array_fail("arr_i64", [&](const coda_cursor* c) {
    return coda_ascii_cursor_read_int64_array(c, i64.data(),
                                              coda_array_ordering_c);
  });
  expect_array_fail("arr_u64", [&](const coda_cursor* c) {
    return coda_ascii_cursor_read_uint64_array(c, u64.data(),
                                               coda_array_ordering_c);
  });
  expect_array_fail("arr_f32", [&](const coda_cursor* c) {
    return coda_ascii_cursor_read_float_array(c, f32.data(),
                                              coda_array_ordering_c);
  });
  expect_array_fail("arr_f64", [&](const coda_cursor* c) {
    return coda_ascii_cursor_read_double_array(c, f64.data(),
                                               coda_array_ordering_c);
  });
  expect_array_fail("arr_ch", [&](const coda_cursor* c) {
    return coda_ascii_cursor_read_char_array(c, ch.data(),
                                             coda_array_ordering_c);
  });
}

// Records, unions, expression errors, overflow errors, OOB, and unaligned
// reads.

TEST_F(CodaAsciiCursorTest, RecordUnionSizeErrorsAndUnalignedAsciiPaths) {
  // Header layout:
  //   magic (7)   = "ASC_UNI"
  //   sel (2)     = " 1"     -> selects uni field 1 ("as_text"), dyn_rec.b
  //   len=3 denom (2)   = " 0"     -> division by zero in bad_expr_text neg_len
  //   (3) = " -4"    -> negative calculated size in neg_sized_text bit_off (2)
  //   = " 3"     -> non-byte-aligned offset/size (3 bits) dyn_rec (6) =
  //   "abcxyz" -> 3 + 3 bytes uni (5)     = "UNION"  -> 5 bytes
  const std::string payload = "ASC_UNI 1 0 -4 3abcxyzUNION\n";
  coda_cursor root;
  ASSERT_NO_FATAL_FAILURE(OpenProductText("uni.asc", payload, &root));

  coda_cursor dyn_rec = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&dyn_rec, "dyn_rec"));
  int64_t bits = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&dyn_rec, &bits));
  EXPECT_EQ(6 * 8, bits);
  char rec_text[7] = {};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&dyn_rec, rec_text, sizeof(rec_text)));
  EXPECT_STREQ("abcxyz", rec_text);

  coda_cursor uni = root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&uni, "uni"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_available_union_field(&uni));
  char uni_text[6] = {};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&uni, uni_text, sizeof(uni_text)));
  EXPECT_STREQ("UNION", uni_text);

  auto expect_field_error = [&](const char* name, int expected_errno, auto fn) {
    coda_cursor c = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&c, name))
        << name << ": " << coda_errno << " "
        << coda_errno_to_string(coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, fn(&c)) << name;
    EXPECT_EQ(expected_errno, coda_errno) << name;
  };

  expect_field_error("neg_sized_text", CODA_ERROR_PRODUCT,
                     [&](const coda_cursor* c) {
                       return coda_ascii_cursor_get_bit_size(c, &bits);
                     });
  expect_field_error("bad_expr_text", CODA_ERROR_EXPRESSION,
                     [&](const coda_cursor* c) {
                       long len = 0;
                       return coda_ascii_cursor_get_string_length(c, &len);
                     });
  char tmp_str[8] = {};
  expect_field_error(
      "unaligned_bit_len_rec", CODA_ERROR_PRODUCT, [&](const coda_cursor* c) {
        return coda_ascii_cursor_read_string(c, tmp_str, sizeof(tmp_str));
      });
  expect_field_error(
      "bad_expr_text", CODA_ERROR_EXPRESSION, [&](const coda_cursor* c) {
        return coda_ascii_cursor_read_string(c, tmp_str, sizeof(tmp_str));
      });
  expect_field_error("unaligned_line", CODA_ERROR_PRODUCT,
                     [&](const coda_cursor* c) {
                       return coda_ascii_cursor_get_bit_size(c, &bits);
                     });
  expect_field_error("unaligned_ws", CODA_ERROR_PRODUCT,
                     [&](const coda_cursor* c) {
                       return coda_ascii_cursor_get_bit_size(c, &bits);
                     });
  expect_field_error("unaligned_sep", CODA_ERROR_PRODUCT,
                     [&](const coda_cursor* c) {
                       return coda_ascii_cursor_get_bit_size(c, &bits);
                     });

  // read_bits / read_bytes negative and OOB arguments.
  coda_cursor magic = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&magic, "magic"));
  std::array<uint8_t, 4> byte_buf{};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_ascii_cursor_read_bits(&magic, byte_buf.data(), 0, 5));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_ascii_cursor_read_bits(&magic, byte_buf.data(), 3, 8));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_ascii_cursor_read_bytes(&magic, byte_buf.data(), -1, 1));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_ascii_cursor_read_bytes(&magic, byte_buf.data(), 1000, 1));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
}

TEST_F(CodaAsciiCursorTest,
       UnalignedAsciiFieldsInBinaryProductAndNonAsciiLine) {
  // "BIN_ASC" (7) + 1 byte ('A') for pad3 & unaligned fields + "line\n" at
  // byte 8.
  const std::string payload = "BIN_ASCAline\n";
  coda_cursor root;
  ASSERT_NO_FATAL_FAILURE(OpenProductText("bin_asc.bin", payload, &root));

  auto expect_unaligned = [&](const char* name, int expected_errno, auto fn) {
    coda_cursor c = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&c, name))
        << name << ": " << coda_errno << " "
        << coda_errno_to_string(coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, fn(&c)) << name;
    EXPECT_EQ(expected_errno, coda_errno) << name;
  };

  int64_t i64 = 0;
  uint64_t u64 = 0;
  double f64 = 0.0;
  char ch = '\0';
  char str[8] = {};
  uint8_t byte_buf[2] = {};
  int64_t bits = 0;

  expect_unaligned("unaligned_i64", CODA_ERROR_FILE_READ,
                   [&](const coda_cursor* c) {
                     return coda_ascii_cursor_read_int64(c, &i64);
                   });
  expect_unaligned("unaligned_u64", CODA_ERROR_FILE_READ,
                   [&](const coda_cursor* c) {
                     return coda_ascii_cursor_read_uint64(c, &u64);
                   });
  expect_unaligned("unaligned_f64", CODA_ERROR_FILE_READ,
                   [&](const coda_cursor* c) {
                     return coda_ascii_cursor_read_double(c, &f64);
                   });
  expect_unaligned("unaligned_ch", CODA_ERROR_PRODUCT,
                   [&](const coda_cursor* c) {
                     return coda_ascii_cursor_read_char(c, &ch);
                   });
  expect_unaligned("unaligned_str", CODA_ERROR_PRODUCT,
                   [&](const coda_cursor* c) {
                     return coda_ascii_cursor_read_string(c, str, sizeof(str));
                   });
  expect_unaligned("unaligned_str", CODA_ERROR_PRODUCT,
                   [&](const coda_cursor* c) {
                     return coda_ascii_cursor_read_bytes(c, byte_buf, 0, 1);
                   });
  expect_unaligned("unaligned_line", CODA_ERROR_PRODUCT,
                   [&](const coda_cursor* c) {
                     return coda_ascii_cursor_get_bit_size(c, &bits);
                   });

  // Aligned AsciiLine inside binary product uses the non-asciiline-cache
  // branch.
  coda_cursor line = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&line, "aligned_line"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&line, &bits));
  EXPECT_EQ(4 * 8, bits);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&line, str, sizeof(str)));
  EXPECT_STREQ("line", str);
}

TEST_F(CodaAsciiCursorTest, ScalarOverflowUnderflowParseFailuresAndOobReads) {
  // Fixed-size fields containing out-of-range numbers and non-numeric strings.
  std::string payload = "ASC_SCALAR ";
  // i8 (4): "+128", u8 (4): " 256", i16 (6): " 32768", u16 (6): " 65536",
  // i32 (11): " 2147483648", u32 (11): " 4294967296",
  // i64 (20): "bad_i64_value       ", u64 (21): "bad_u64_value        ",
  // f32 (8): "bad_f32 ", f64 (10): "bad_f64   ".
  payload.append("+128");
  payload.append(" 256");
  payload.append(" 32768");
  payload.append(" 65536");
  payload.append(" 2147483648");
  payload.append(" 4294967296");
  payload.append("bad_i64_value       ");
  payload.append("bad_u64_value        ");
  payload.append("bad_f32 ");
  payload.append("bad_f64   ");
  payload.append("Z");   // ch
  payload.append("99");  // dyn_i32 (non-EOF)
  payload.append(" ");
  payload.append("badu");  // dyn_u32
  payload.append(" ");
  payload.append("badf");  // dyn_f64
  payload.append("\n");

  coda_cursor root;
  ASSERT_NO_FATAL_FAILURE(OpenProductText("scalar_errs.asc", payload, &root));

  auto expect_read_err = [&](const char* name, int expected_errno, auto fn) {
    coda_cursor c = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&c, name))
        << name << ": " << coda_errno << " "
        << coda_errno_to_string(coda_errno);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, fn(&c)) << name;
    EXPECT_EQ(expected_errno, coda_errno) << name;
  };

  int8_t i8 = 0;
  uint8_t u8 = 0;
  int16_t i16 = 0;
  uint16_t u16 = 0;
  int32_t i32 = 0;
  uint32_t u32 = 0;
  int64_t i64 = 0;
  uint64_t u64 = 0;
  float f32 = 0.0f;
  double f64 = 0.0;

  expect_read_err("i8", CODA_ERROR_PRODUCT, [&](const coda_cursor* c) {
    return coda_cursor_read_int8(c, &i8);
  });
  expect_read_err("u8", CODA_ERROR_PRODUCT, [&](const coda_cursor* c) {
    return coda_cursor_read_uint8(c, &u8);
  });
  expect_read_err("i16", CODA_ERROR_PRODUCT, [&](const coda_cursor* c) {
    return coda_cursor_read_int16(c, &i16);
  });
  expect_read_err("u16", CODA_ERROR_PRODUCT, [&](const coda_cursor* c) {
    return coda_cursor_read_uint16(c, &u16);
  });
  expect_read_err("i32", CODA_ERROR_PRODUCT, [&](const coda_cursor* c) {
    return coda_cursor_read_int32(c, &i32);
  });
  expect_read_err("u32", CODA_ERROR_PRODUCT, [&](const coda_cursor* c) {
    return coda_cursor_read_uint32(c, &u32);
  });
  expect_read_err("i64", CODA_ERROR_INVALID_FORMAT, [&](const coda_cursor* c) {
    return coda_cursor_read_int64(c, &i64);
  });
  expect_read_err("u64", CODA_ERROR_INVALID_FORMAT, [&](const coda_cursor* c) {
    return coda_cursor_read_uint64(c, &u64);
  });
  expect_read_err("f32", CODA_ERROR_INVALID_FORMAT, [&](const coda_cursor* c) {
    return coda_cursor_read_float(c, &f32);
  });
  expect_read_err("f64", CODA_ERROR_INVALID_FORMAT, [&](const coda_cursor* c) {
    return coda_cursor_read_double(c, &f64);
  });

  // Dynamic numbers with invalid content fail in both get_bit_size & read.
  // Test dyn_f64 in a separate file where preceding dyn_u32 is valid so
  // navigating to dyn_f64 succeeds.
  int64_t bits = 0;
  expect_read_err(
      "dyn_u32", CODA_ERROR_INVALID_FORMAT,
      [&](const coda_cursor* c) { return coda_cursor_get_bit_size(c, &bits); });

  std::string dyn_f64_payload = "ASC_SCALAR ";
  dyn_f64_payload.append("-128");                   // i8 (4)
  dyn_f64_payload.append("   1");                   // u8 (4)
  dyn_f64_payload.append("     1");                 // i16 (6)
  dyn_f64_payload.append("     1");                 // u16 (6)
  dyn_f64_payload.append("          1");            // i32 (11)
  dyn_f64_payload.append("          1");            // u32 (11)
  dyn_f64_payload.append("                   1");   // i64 (20)
  dyn_f64_payload.append("                    1");  // u64 (21)
  dyn_f64_payload.append("     1.0");               // f32 (8)
  dyn_f64_payload.append("       1.0");             // f64 (10)
  dyn_f64_payload.append("Z");                      // ch (1)
  dyn_f64_payload.append("1 2 badf\n");             // dyn_i32, dyn_u32, dyn_f64
  ASSERT_NO_FATAL_FAILURE(
      OpenProductText("scalar_dyn_f64_err.asc", dyn_f64_payload, &root));
  expect_read_err(
      "dyn_f64", CODA_ERROR_INVALID_FORMAT,
      [&](const coda_cursor* c) { return coda_cursor_get_bit_size(c, &bits); });

  // Also test int8..int32 underflow (below minimum) and truncated OOB file.
  std::string underflow = "ASC_SCALAR ";
  underflow.append("-129");
  underflow.append("   0");
  underflow.append("-32769");
  underflow.append("     0");
  underflow.append("-2147483649");
  underflow.append("\n");
  ASSERT_NO_FATAL_FAILURE(
      OpenProductText("scalar_underflow.asc", underflow, &root));

  expect_read_err("i8", CODA_ERROR_PRODUCT, [&](const coda_cursor* c) {
    return coda_cursor_read_int8(c, &i8);
  });
  expect_read_err("i16", CODA_ERROR_PRODUCT, [&](const coda_cursor* c) {
    return coda_cursor_read_int16(c, &i16);
  });
  expect_read_err("i32", CODA_ERROR_PRODUCT, [&](const coda_cursor* c) {
    return coda_cursor_read_int32(c, &i32);
  });
  expect_read_err(
      "u64", CODA_ERROR_OUT_OF_BOUNDS_READ,
      [&](const coda_cursor* c) { return coda_cursor_read_uint64(c, &u64); });

  // Truncated file where fixed-size scalars and char exceed EOF.
  ASSERT_NO_FATAL_FAILURE(
      OpenProductText("scalar_short.asc", "ASC_SCALAR 1\n", &root));
  expect_read_err(
      "i8", CODA_ERROR_OUT_OF_BOUNDS_READ,
      [&](const coda_cursor* c) { return coda_cursor_read_int8(c, &i8); });
  expect_read_err(
      "u8", CODA_ERROR_OUT_OF_BOUNDS_READ,
      [&](const coda_cursor* c) { return coda_cursor_read_uint8(c, &u8); });
  expect_read_err(
      "i16", CODA_ERROR_OUT_OF_BOUNDS_READ,
      [&](const coda_cursor* c) { return coda_cursor_read_int16(c, &i16); });
  expect_read_err(
      "u16", CODA_ERROR_OUT_OF_BOUNDS_READ,
      [&](const coda_cursor* c) { return coda_cursor_read_uint16(c, &u16); });
  expect_read_err(
      "i32", CODA_ERROR_OUT_OF_BOUNDS_READ,
      [&](const coda_cursor* c) { return coda_cursor_read_int32(c, &i32); });
  expect_read_err(
      "u32", CODA_ERROR_OUT_OF_BOUNDS_READ,
      [&](const coda_cursor* c) { return coda_cursor_read_uint32(c, &u32); });
  expect_read_err(
      "i64", CODA_ERROR_OUT_OF_BOUNDS_READ,
      [&](const coda_cursor* c) { return coda_cursor_read_int64(c, &i64); });
  expect_read_err(
      "f32", CODA_ERROR_OUT_OF_BOUNDS_READ,
      [&](const coda_cursor* c) { return coda_cursor_read_float(c, &f32); });
  expect_read_err(
      "f64", CODA_ERROR_OUT_OF_BOUNDS_READ,
      [&](const coda_cursor* c) { return coda_cursor_read_double(c, &f64); });
  expect_read_err("ch", CODA_ERROR_OUT_OF_BOUNDS_READ,
                  [&](const coda_cursor* c) {
                    char ch = '\0';
                    return coda_cursor_read_char(c, &ch);
                  });
  // dyn_i32 has dynamic size (bit_size < 0) past EOF so get_bit_size() hits
  // get_bit_size_boundary() and reports CODA_ERROR_OUT_OF_BOUNDS_READ.
  expect_read_err(
      "dyn_i32", CODA_ERROR_OUT_OF_BOUNDS_READ,
      [&](const coda_cursor* c) { return coda_cursor_get_bit_size(c, &bits); });

  // Fixed-size text right at EOF without trailing newline triggers
  // get_bit_size_boundary's `>=` check.
  ASSERT_NO_FATAL_FAILURE(
      OpenProductText("scalar_exact_magic.asc", "ASC_SCALAR", &root));
  char magic[11] = {};
  expect_read_err("magic", CODA_ERROR_OUT_OF_BOUNDS_READ,
                  [&](const coda_cursor* c) {
                    return coda_cursor_read_string(c, magic, sizeof(magic));
                  });
}

// XML-wrapped ASCII fields (coda_mem_data with coda_format_ascii).

TEST_F(CodaAsciiCursorTest, XmlProductWithAsciiTypedFields) {
  const std::string xml_data =
      "<?xml version=\"1.0\"?>\n"
      "<XmlAsc><num> -42</num><flt>6.25</flt>"
      "<line>first\nsecond</line><ws> \t x</ws></XmlAsc>";
  coda_cursor root;
  ASSERT_NO_FATAL_FAILURE(OpenProductText("xml_asc.xml", xml_data, &root));

  coda_cursor xml_asc = root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&xml_asc, "XmlAsc"));

  coda_cursor c = xml_asc;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&c, "num"));
  int32_t num = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c, &num));
  EXPECT_EQ(-42, num);

  c = xml_asc;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&c, "flt"));
  double flt = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &flt));
  EXPECT_THAT(flt, DoubleEq(6.25));

  c = xml_asc;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&c, "line"));
  int64_t bits = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bits));
  EXPECT_EQ(5 * 8, bits);
  char line_buf[16] = {};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&c, line_buf, sizeof(line_buf)));
  EXPECT_STREQ("first", line_buf);

  c = xml_asc;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&c, "ws"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &bits));
  EXPECT_EQ(3 * 8, bits);
}

// Synthetic coda_mem_data products for empty mappings, non-ASCII line_with_eol,
// ascii_text_default without size_expr, and non-byte-aligned mem boundaries.

TEST_F(CodaAsciiCursorTest, SyntheticMemDataEmptyMappingsLineWithEolAndBounds) {
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };
  std::vector<ScopedType> owned_types;

  // 1. Empty-string integer & float mappings with 0-byte coda_mem_data.
  {
    ScopedProduct mem_prod = MakeSyntheticMemProduct();
    ASSERT_NE(nullptr, mem_prod.get());

    coda_type_number* int_type =
        coda_type_number_new(coda_format_ascii, coda_integer_class);
    ASSERT_NE(nullptr, int_type);
    TrackType(int_type, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_read_type(reinterpret_cast<coda_type*>(int_type),
                                      coda_native_type_int32));
    coda_ascii_integer_mapping* int_map =
        coda_ascii_integer_mapping_new("", -77);
    ASSERT_NE(nullptr, int_map);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_number_add_ascii_integer_mapping(int_type, int_map));

    coda_type_number* flt_type =
        coda_type_number_new(coda_format_ascii, coda_real_class);
    ASSERT_NE(nullptr, flt_type);
    TrackType(flt_type, owned_types);
    coda_ascii_float_mapping* flt_map = coda_ascii_float_mapping_new("", -3.25);
    ASSERT_NE(nullptr, flt_map);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_number_add_ascii_float_mapping(flt_type, flt_map));

    coda_type_text* def_text = coda_type_text_new(coda_format_ascii);
    ASSERT_NE(nullptr, def_text);
    TrackType(def_text, owned_types);

    coda_type_record* root_def = coda_type_record_new(coda_format_xml);
    ASSERT_NE(nullptr, root_def);
    TrackType(root_def, owned_types);
    coda_mem_record* root_mem = coda_mem_record_new(root_def, nullptr);
    ASSERT_NE(nullptr, root_mem);
    mem_prod.get()->root_type = reinterpret_cast<coda_dynamic_type*>(root_mem);

    coda_mem_data* int_mem =
        coda_mem_data_new(reinterpret_cast<coda_type*>(int_type), nullptr,
                          mem_prod.get(), 0, nullptr);
    ASSERT_NE(nullptr, int_mem);
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(
                  root_mem, "emp_int",
                  reinterpret_cast<coda_dynamic_type*>(int_mem), 1));

    coda_mem_data* flt_mem =
        coda_mem_data_new(reinterpret_cast<coda_type*>(flt_type), nullptr,
                          mem_prod.get(), 0, nullptr);
    ASSERT_NE(nullptr, flt_mem);
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(
                  root_mem, "emp_flt",
                  reinterpret_cast<coda_dynamic_type*>(flt_mem), 1));

    const std::string text_bytes = "hello";
    coda_mem_data* txt_mem =
        coda_mem_data_new(reinterpret_cast<coda_type*>(def_text), nullptr,
                          mem_prod.get(), static_cast<long>(text_bytes.size()),
                          reinterpret_cast<const uint8_t*>(text_bytes.data()));
    ASSERT_NE(nullptr, txt_mem);
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(
                  root_mem, "def_txt",
                  reinterpret_cast<coda_dynamic_type*>(txt_mem), 1));
    mem_prod.get()->file_size = mem_prod.get()->mem_size;

    coda_cursor cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, mem_prod.get()));

    coda_cursor c_int = cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&c_int, "emp_int"));
    int64_t bits = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_ascii_cursor_get_bit_size(&c_int, &bits));
    EXPECT_EQ(0, bits);
    int32_t i32_val = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_ascii_cursor_read_int32(&c_int, &i32_val));
    EXPECT_EQ(-77, i32_val);

    coda_cursor c_flt = cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&c_flt, "emp_flt"));
    ASSERT_EQ(CODA_SUCCESS, coda_ascii_cursor_get_bit_size(&c_flt, &bits));
    EXPECT_EQ(0, bits);
    double f64_val = 0.0;
    ASSERT_EQ(CODA_SUCCESS, coda_ascii_cursor_read_double(&c_flt, &f64_val));
    EXPECT_THAT(f64_val, DoubleEq(-3.25));

    coda_cursor c_txt = cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&c_txt, "def_txt"));
    ASSERT_EQ(CODA_SUCCESS, coda_ascii_cursor_get_bit_size(&c_txt, &bits));
    EXPECT_EQ(5 * 8, bits);
  }

  // 2. ascii_text_line_with_eol in a non-ascii (coda_format_sp3) product.
  {
    ScopedProduct mem_prod = MakeSyntheticMemProduct();
    ASSERT_NE(nullptr, mem_prod.get());

    coda_type_text* line_eol = coda_type_text_new(coda_format_ascii);
    ASSERT_NE(nullptr, line_eol);
    TrackType(line_eol, owned_types);
    ASSERT_EQ(CODA_SUCCESS, coda_type_text_set_special_text_type(
                                line_eol, ascii_text_line_with_eol));

    coda_type_record* root_def = coda_type_record_new(coda_format_xml);
    ASSERT_NE(nullptr, root_def);
    TrackType(root_def, owned_types);
    coda_mem_record* root_mem = coda_mem_record_new(root_def, nullptr);
    ASSERT_NE(nullptr, root_mem);
    mem_prod.get()->root_type = reinterpret_cast<coda_dynamic_type*>(root_mem);

    struct LineCase {
      const char* name;
      std::string_view data;
      int64_t expected_bits;
    };
    constexpr LineCase kLineCases[] = {
        {.name = "lf", .data = "abc\ndef", .expected_bits = 4 * 8},
        {.name = "crlf", .data = "abc\r\ndef", .expected_bits = 5 * 8},
        {.name = "cr", .data = "abc\rdef", .expected_bits = 4 * 8},
    };
    for (const LineCase& tc : kLineCases) {
      coda_mem_data* mem =
          coda_mem_data_new(reinterpret_cast<coda_type*>(line_eol), nullptr,
                            mem_prod.get(), static_cast<long>(tc.data.size()),
                            reinterpret_cast<const uint8_t*>(tc.data.data()));
      ASSERT_NE(nullptr, mem);
      ASSERT_EQ(
          CODA_SUCCESS,
          coda_mem_record_add_field(
              root_mem, tc.name, reinterpret_cast<coda_dynamic_type*>(mem), 1));
    }
    mem_prod.get()->file_size = mem_prod.get()->mem_size;

    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, mem_prod.get()));
    for (const LineCase& tc : kLineCases) {
      coda_cursor c = root;
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_goto_record_field_by_name(&c, tc.name));
      int64_t bits = 0;
      ASSERT_EQ(CODA_SUCCESS, coda_ascii_cursor_get_bit_size(&c, &bits));
      EXPECT_EQ(tc.expected_bits, bits) << tc.name;
    }
  }

  // 3. Nested binary/ascii record inside a union wrapped in 1-byte
  //    coda_mem_data (never wrap a non-union record directly in coda_mem_data,
  //    since coda-ascbin-cursor.c:51 casts stack[n-1].type to
  //    coda_type_record*) to cover get_bit_size_boundary() non-byte-aligned
  //    offset and OOB error messages.
  {
    ScopedProduct mem_prod = MakeSyntheticMemProduct();
    ASSERT_NE(nullptr, mem_prod.get());

    coda_type_record* root_def = coda_type_record_new(coda_format_binary);
    ASSERT_NE(nullptr, root_def);
    TrackType(root_def, owned_types);
    coda_mem_record* root_mem = coda_mem_record_new(root_def, nullptr);
    ASSERT_NE(nullptr, root_mem);
    mem_prod.get()->root_type = reinterpret_cast<coda_dynamic_type*>(root_mem);

    coda_type_record* rec_def = coda_type_record_new(coda_format_binary);
    ASSERT_NE(nullptr, rec_def);
    TrackType(rec_def, owned_types);

    coda_type_raw* raw3 = coda_type_raw_new(coda_format_binary);
    ASSERT_NE(nullptr, raw3);
    TrackType(raw3, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_bit_size(reinterpret_cast<coda_type*>(raw3), 3));
    ScopedRecordField f0(coda_type_record_field_new("raw3"));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                                f0.get(), reinterpret_cast<coda_type*>(raw3)));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_add_field(rec_def, f0.get()));
    (void)f0.release();

    // 13-bit number at bit_offset 3 inside a 1-byte block (3 + 13 > 8).
    coda_type_number* num13 =
        coda_type_number_new(coda_format_binary, coda_integer_class);
    ASSERT_NE(nullptr, num13);
    TrackType(num13, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_bit_size(reinterpret_cast<coda_type*>(num13), 13));
    ScopedRecordField f1(coda_type_record_field_new("num13"));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                                f1.get(), reinterpret_cast<coda_type*>(num13)));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_add_field(rec_def, f1.get()));
    (void)f1.release();

    // Dynamic ASCII text field at bit_offset 19 (> 8, non-zero bit remainder).
    coda_type_raw* raw3b = coda_type_raw_new(coda_format_binary);
    ASSERT_NE(nullptr, raw3b);
    TrackType(raw3b, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_bit_size(reinterpret_cast<coda_type*>(raw3b), 3));
    ScopedRecordField f2(coda_type_record_field_new("raw3b"));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_type(
                                f2.get(), reinterpret_cast<coda_type*>(raw3b)));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_add_field(rec_def, f2.get()));
    (void)f2.release();

    coda_type_text* dyn_txt = coda_type_text_new(coda_format_ascii);
    ASSERT_NE(nullptr, dyn_txt);
    TrackType(dyn_txt, owned_types);
    ScopedRecordField f3(coda_type_record_field_new("dyn_txt"));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_field_set_type(
                  f3.get(), reinterpret_cast<coda_type*>(dyn_txt)));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_add_field(rec_def, f3.get()));
    (void)f3.release();

    coda_type_record* union_def = coda_type_union_new(coda_format_binary);
    ASSERT_NE(nullptr, union_def);
    TrackType(union_def, owned_types);
    ScopedExpr union_expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("0", union_expr.OutParam()));
    ASSERT_EQ(CODA_SUCCESS, coda_type_union_set_field_expression(
                                union_def, union_expr.get()));
    (void)union_expr.release();
    ScopedRecordField uf(coda_type_record_field_new("rec"));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_optional(uf.get()));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_field_set_type(
                  uf.get(), reinterpret_cast<coda_type*>(rec_def)));
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_add_field(union_def, uf.get()));
    (void)uf.release();

    const uint8_t one_byte = 0x5A;
    coda_mem_data* mem =
        coda_mem_data_new(reinterpret_cast<coda_type*>(union_def), nullptr,
                          mem_prod.get(), 1, &one_byte);
    ASSERT_NE(nullptr, mem);
    ASSERT_EQ(CODA_SUCCESS, coda_mem_record_add_field(
                                root_mem, "blk",
                                reinterpret_cast<coda_dynamic_type*>(mem), 1));
    mem_prod.get()->file_size = mem_prod.get()->mem_size;

    coda_cursor root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, mem_prod.get()));
    coda_cursor rec = root;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&rec, "blk"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&rec, "rec"));

    // num13: current_bit_offset = 3, read_bit_size = 13, block = 8 -> OOB.
    coda_cursor c_num = rec;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&c_num, "num13"));
    int64_t val = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_ascii_cursor_read_int64(&c_num, &val));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);

    // dyn_txt: current_bit_offset = 19 > block = 8 -> OOB with bit remainder.
    coda_cursor c_dyn = rec;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&c_dyn, "dyn_txt"));
    int64_t bits = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_ascii_cursor_get_bit_size(&c_dyn, &bits));
    EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  }
}

// TODO(schwehr): Remaining uncovered lines in coda-ascii-cursor.c are mostly
// defensive OOM checks (coda_ascii_cursor_set_asciilines), unreachable
// transpose_array OOM returns, and !has_dynamic_size length-mismatch guards in
// parse_integer_mapping/parse_float_mapping prevented by coda-type.c.

}  // namespace
}  // namespace stcorp_coda
