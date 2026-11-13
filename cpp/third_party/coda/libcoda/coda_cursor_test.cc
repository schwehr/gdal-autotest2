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
// Tests CODA cursor initialization, navigation, type/metadata queries, path
// parsing/formatting, cursor comparison, depth limits, special-type bypassing,
// and backend dispatch across ascii, binary, memory, XML, HDF5, NetCDF, CDF,
// GRIB, RINEX, SP3, and disabled HDF4 (libcoda/coda-cursor.c).
//
// google3 build caveats:
//   - HAVE_HDF4 is undefined in config.h, so HDF4 operations fail with
//     CODA_ERROR_NO_HDF4_SUPPORT.
//   - CODA_CURSOR_MAXDEPTH is 32 in libcoda/coda.h.
//   - config.h defines H5_USE_16_API, so HDF5 file synthesis uses explicit
//     versioned HDF5 1.8+ APIs (H5Gcreate2, H5Dcreate2, H5Acreate2).
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-cursor.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-internal.h

#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-mem-internal.h"
#include "third_party/stcorp_coda/libcoda/coda-type.h"
}  // extern "C"

#include <array>
#include <bit>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/google/util/hdf5_test_util.h"
#include "third_party/stcorp_coda/google/util/netcdf_builder.h"
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

// Allocates a synthetic coda_product using coda_format_sp3 so coda_close()
// invokes coda_sp3_close() and frees root_type and mem_ptr without an OS fd.
ScopedProduct MakeSyntheticMemProduct() {
  auto* product =
      static_cast<coda_product*>(std::calloc(1, sizeof(coda_product)));
  if (product != nullptr) {
    product->format = coda_format_sp3;
  }
  return ScopedProduct(product);
}

// Print capture state for testing coda_cursor_print_path().
thread_local std::string* g_print_buffer = nullptr;
thread_local int g_print_call_count = 0;
thread_local int g_print_fail_on_call = 0;

int CapturePrint(const char* fmt, ...) {
  ++g_print_call_count;
  if (g_print_fail_on_call > 0 && g_print_call_count == g_print_fail_on_call) {
    return -1;
  }
  if (g_print_buffer == nullptr) {
    return -1;
  }
  va_list ap;
  va_start(ap, fmt);
  va_list ap_copy;
  va_copy(ap_copy, ap);
  const int needed = std::vsnprintf(nullptr, 0, fmt, ap_copy);
  va_end(ap_copy);
  if (needed < 0) {
    va_end(ap);
    return -1;
  }
  std::string chunk(static_cast<size_t>(needed) + 1, '\0');
  std::vsnprintf(chunk.data(), chunk.size(), fmt, ap);
  va_end(ap);
  chunk.resize(static_cast<size_t>(needed));
  g_print_buffer->append(chunk);
  return needed;
}

std::string PrintCursorPath(const coda_cursor& cursor) {
  std::string out;
  g_print_buffer = &out;
  g_print_call_count = 0;
  g_print_fail_on_call = 0;
  const absl::Cleanup clear_state = [] {
    g_print_buffer = nullptr;
    g_print_call_count = 0;
    g_print_fail_on_call = 0;
  };
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_print_path(&cursor, &CapturePrint));
  return out;
}

int PrintCursorPathFailingOnCall(const coda_cursor& cursor, int fail_on_call) {
  std::string out;
  g_print_buffer = &out;
  g_print_call_count = 0;
  g_print_fail_on_call = fail_on_call;
  const absl::Cleanup clear_state = [] {
    g_print_buffer = nullptr;
    g_print_call_count = 0;
    g_print_fail_on_call = 0;
  };
  return coda_cursor_print_path(&cursor, &CapturePrint);
}

void AppendLe16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void AppendLe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

void AppendLeDouble(std::vector<uint8_t>& out, double v) {
  AppendLe64(out, std::bit_cast<uint64_t>(v));
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendBe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
  }
}

void AppendText(std::vector<uint8_t>& out, std::string_view s) {
  out.insert(out.end(), s.begin(), s.end());
}

// Product definitions for binary and ascii cursor tests.
constexpr std::string_view kCursorIndexXml = R"(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="CursorTestClass">
  <cd:ProductType name="CursorBin">
    <cd:ProductDefinition id="cursor_bin" format="binary" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">CURSBIN1</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
  <cd:ProductType name="CursorAsc">
    <cd:ProductDefinition id="cursor_asc" format="ascii" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">CURSASC1</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)";

constexpr std::string_view kCursorBinXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="cursor_bin" format="binary" version="1">
  <cd:Record format="binary">
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>8</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="count">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="zero_count">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="hidden_flag">
      <cd:Hidden/>
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="opt_present">
      <cd:Optional/>
      <cd:Available>int(/count) &gt; 0</cd:Available>
      <cd:Integer>
        <cd:NativeType>uint16</cd:NativeType>
        <cd:BitSize>16</cd:BitSize>
        <cd:LittleEndian/>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="opt_absent">
      <cd:Optional/>
      <cd:Available>int(/count) == 0</cd:Available>
      <cd:Integer>
        <cd:NativeType>uint16</cd:NativeType>
        <cd:BitSize>16</cd:BitSize>
        <cd:LittleEndian/>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="bits5">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>5</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="bits3">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>3</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="bits9">
      <!-- coda_type_number_validate rejects <cd:LittleEndian/> when bit_size % 8 != 0. -->
      <cd:Integer>
        <cd:NativeType>uint16</cd:NativeType>
        <cd:BitSize>9</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="bits7">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>7</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="fixed_arr">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Dimension>3</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>int16</cd:NativeType>
          <cd:BitSize>16</cd:BitSize>
          <cd:LittleEndian/>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="var_arr">
      <cd:Array>
        <cd:Dimension>int(/count)</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>uint8</cd:NativeType>
          <cd:BitSize>8</cd:BitSize>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="empty_arr">
      <cd:Array>
        <cd:Dimension>int(/zero_count)</cd:Dimension>
        <cd:Integer>
          <cd:NativeType>uint8</cd:NativeType>
          <cd:BitSize>8</cd:BitSize>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="u_sel">
      <cd:Integer>
        <cd:NativeType>uint8</cd:NativeType>
        <cd:BitSize>8</cd:BitSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="u">
      <cd:Union>
        <cd:FieldExpression>int(/u_sel)</cd:FieldExpression>
        <cd:Field name="as_i32">
          <cd:Integer>
            <cd:NativeType>int32</cd:NativeType>
            <cd:BitSize>32</cd:BitSize>
            <cd:LittleEndian/>
          </cd:Integer>
        </cd:Field>
        <cd:Field name="as_str">
          <cd:Text>
            <cd:ByteSize>4</cd:ByteSize>
          </cd:Text>
        </cd:Field>
      </cd:Union>
    </cd:Field>
    <cd:Field name="u_special">
      <cd:Union>
        <cd:FieldExpression>0</cd:FieldExpression>
        <cd:Field name="sp_time">
          <cd:Time timeformat="binary_envisat_datetime"/>
        </cd:Field>
        <cd:Field name="sp_i8">
          <cd:Integer>
            <cd:NativeType>int8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:Field>
      </cd:Union>
    </cd:Field>
    <cd:Field name="label">
      <cd:Text>
        <cd:ByteSize>5</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="bin_complex">
      <cd:Complex>
        <cd:Float>
          <cd:NativeType>double</cd:NativeType>
          <cd:BitSize>64</cd:BitSize>
          <cd:LittleEndian/>
        </cd:Float>
      </cd:Complex>
    </cd:Field>
    <cd:Field name="bin_vsf">
      <cd:VSFInteger>
        <cd:ScaleFactor>
          <cd:Integer>
            <cd:NativeType>int8</cd:NativeType>
            <cd:BitSize>8</cd:BitSize>
          </cd:Integer>
        </cd:ScaleFactor>
        <cd:Integer>
          <cd:NativeType>int16</cd:NativeType>
          <cd:BitSize>16</cd:BitSize>
          <cd:LittleEndian/>
        </cd:Integer>
        <cd:Unit>Pa</cd:Unit>
      </cd:VSFInteger>
    </cd:Field>
    <cd:Field name="complex_arr">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Complex>
          <cd:Float>
            <cd:NativeType>double</cd:NativeType>
            <cd:BitSize>64</cd:BitSize>
            <cd:LittleEndian/>
          </cd:Float>
        </cd:Complex>
      </cd:Array>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";

constexpr std::string_view kCursorAscXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="cursor_asc" format="ascii" version="1">
  <cd:Record format="ascii">
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>8</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="count">
      <cd:Integer>
        <cd:NativeType>int64</cd:NativeType>
        <cd:ByteSize>3</cd:ByteSize>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="asc_time">
      <cd:Time timeformat="ascii_ccsds_datetime_ymd1"/>
    </cd:Field>
    <cd:Field name="arr">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Float>
          <cd:NativeType>double</cd:NativeType>
          <cd:ByteSize>5</cd:ByteSize>
        </cd:Float>
      </cd:Array>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";

std::vector<uint8_t> MakeCursorCodadefBytes() {
  const std::array<std::pair<std::string_view, std::string_view>, 2> entries = {
      {
          {"products/cursor_bin.xml", kCursorBinXml},
          {"products/cursor_asc.xml", kCursorAscXml},
      },
  };
  return BuildCodadefBytes(kCursorIndexXml, entries);
}

// Builds a binary payload matching kCursorBinXml (103 bytes total).
std::vector<uint8_t> MakeCursorBinBytes(uint8_t u_sel) {
  std::vector<uint8_t> data;
  AppendText(data, "CURSBIN1");  // magic (8)
  data.push_back(2);             // count = 2
  data.push_back(0);             // zero_count = 0
  data.push_back(7);             // hidden_flag = 7
  AppendLe16(data, 0x1234);      // opt_present (2)
  // opt_absent is unavailable -> 0 bytes.
  data.push_back(0xAB);  // bits5 (5 bits) + bits3 (3 bits)
  data.push_back(0x01);  // bits9 (9 bits) + bits7 (7 bits)
  data.push_back(0x00);
  for (int16_t v : {10, 20, 30, 40, 50, 60}) {
    AppendLe16(data, static_cast<uint16_t>(v));  // fixed_arr (12)
  }
  data.push_back(11);  // var_arr[0]
  data.push_back(22);  // var_arr[1]
  // empty_arr -> 0 bytes.
  data.push_back(u_sel);  // u_sel
  if (u_sel == 0) {
    AppendLe32(data, 123456);  // u.as_i32
  } else {
    AppendText(data, "ABCD");  // u.as_str
  }
  // u_special.sp_time (12 bytes: days, seconds, microseconds).
  AppendBe32(data, 100);
  AppendBe32(data, 3600);
  AppendBe32(data, 500);
  AppendText(data, "HELLO");                 // label (5)
  AppendLeDouble(data, 1.5);                 // bin_complex.real
  AppendLeDouble(data, -2.5);                // bin_complex.imaginary
  data.push_back(static_cast<uint8_t>(-2));  // bin_vsf.scale_factor
  AppendLe16(data, 420);                     // bin_vsf.value
  AppendLeDouble(data, 3.0);                 // complex_arr[0].real
  AppendLeDouble(data, 4.0);                 // complex_arr[0].imaginary
  AppendLeDouble(data, 5.0);                 // complex_arr[1].real
  AppendLeDouble(data, 6.0);                 // complex_arr[1].imaginary
  return data;
}

// Synthesizes a minimal valid CDF v3.0 single-file product with 3 zVariables:
//   0: "arr"   (CDF_INT4, 3 records, rec_varys=1, 0 dims -> 1D array of 3)
//   1: "txt"   (CDF_CHAR, 1 record,  rec_varys=0, num_elems=5 -> scalar string)
//   2: "epoch" (CDF_EPOCH, 1 record, rec_varys=0 -> scalar coda_special_time)
std::vector<uint8_t> MakeMinimalCdfV3Bytes() {
  constexpr uint64_t kCdrOffset = 8;
  constexpr uint64_t kCdrSize = 56;
  constexpr uint64_t kGdrOffset = kCdrOffset + kCdrSize;  // 64
  constexpr uint64_t kGdrSize = 84;
  constexpr uint64_t kVdr0Offset = kGdrOffset + kGdrSize;  // 148
  constexpr uint64_t kVdrSize = 344;
  constexpr uint64_t kVdr1Offset = kVdr0Offset + kVdrSize;  // 492
  constexpr uint64_t kVdr2Offset = kVdr1Offset + kVdrSize;  // 836
  constexpr uint64_t kTotalSize = kVdr2Offset + kVdrSize;   // 1180

  std::vector<uint8_t> cdf;
  cdf.reserve(kTotalSize);

  // CDF v3.0 magic (0xCDF30001, 0x0000FFFF).
  AppendBe32(cdf, 0xCDF30001U);
  AppendBe32(cdf, 0x0000FFFFU);

  // CDR (56 bytes at offset 8).
  AppendBe64(cdf, kCdrSize);
  AppendBe32(cdf, 1);  // CDR record type
  AppendBe64(cdf, kGdrOffset);
  AppendBe32(cdf, 3);  // version
  AppendBe32(cdf, 0);  // release
  AppendBe32(cdf, 6);  // encoding = IBMPC (little-endian)
  AppendBe32(cdf, 3);  // flags: bit0=row-major, bit1=single-file
  AppendBe64(cdf, 0);  // rfuA, rfuB
  AppendBe32(cdf, 0);  // increment
  AppendBe64(cdf, 0);  // padding to 56 bytes

  // GDR (84 bytes at offset 64).
  AppendBe64(cdf, kGdrSize);
  AppendBe32(cdf, 2);            // GDR record type
  AppendBe64(cdf, 0);            // rvdr_head
  AppendBe64(cdf, kVdr0Offset);  // zvdr_head
  AppendBe64(cdf, 0);            // adr_head
  AppendBe64(cdf, kTotalSize);   // eof
  AppendBe32(cdf, 0);            // nr_vars
  AppendBe32(cdf, 0);            // num_attr
  AppendBe32(cdf, 0);            // rMaxRec
  AppendBe32(cdf, 0);            // rnum_dims
  AppendBe32(cdf, 3);            // nz_vars
  cdf.resize(kVdr0Offset, 0);

  auto append_zvdr = [&](uint64_t next_offset, int32_t data_type,
                         int32_t max_rec, int32_t flags, int32_t num_elems,
                         int32_t num, std::string_view name) {
    const size_t start = cdf.size();
    AppendBe64(cdf, kVdrSize);
    AppendBe32(cdf, 8);  // zVDR record type
    AppendBe64(cdf, next_offset);
    AppendBe32(cdf, static_cast<uint32_t>(data_type));
    AppendBe32(cdf, static_cast<uint32_t>(max_rec));
    AppendBe64(cdf, 0);  // vxr_head
    AppendBe64(cdf, 0);  // vxr_tail
    AppendBe32(cdf, static_cast<uint32_t>(flags));
    AppendBe32(cdf, 0);  // srecords
    AppendBe32(cdf, 0);  // rfuB
    AppendBe32(cdf, 0);  // rfuC
    AppendBe32(cdf, 0);  // rfuF
    AppendBe32(cdf, static_cast<uint32_t>(num_elems));
    AppendBe32(cdf, static_cast<uint32_t>(num));
    AppendBe64(cdf, 0xFFFFFFFFFFFFFFFFULL);  // cpr_spr_offset = -1
    AppendBe32(cdf, 1);                      // blocking_factor
    const size_t name_pos = cdf.size();
    cdf.resize(name_pos + 256, 0);
    std::memcpy(cdf.data() + name_pos, name.data(), name.size());
    AppendBe32(cdf, 0);  // num_dims = 0
    EXPECT_EQ(start + kVdrSize, cdf.size());
  };

  append_zvdr(kVdr1Offset, 4, 2, 1, 1, 0, "arr");
  append_zvdr(kVdr2Offset, 51, 0, 0, 5, 1, "txt");
  append_zvdr(0, 31, 0, 0, 1, 2, "epoch");
  return cdf;
}

// Synthesizes a minimal valid GRIB1 message with a 2x2 lat/lon grid (87 bytes).
std::vector<uint8_t> MakeMinimalGrib1Bytes() {
  std::vector<uint8_t> grib;
  grib.reserve(87);
  // Section 0 (8 bytes): "GRIB" + 24-bit message length (87) + edition (1).
  AppendText(grib, "GRIB");
  grib.push_back(0);
  grib.push_back(0);
  grib.push_back(87);
  grib.push_back(1);

  // Section 1 (PDS, 28 bytes): section_size=28, table2=2, centre=98, gen=1,
  // gridDef=0, flags=0x80 (GDS present, no BMS), rest 0.
  grib.push_back(0);
  grib.push_back(0);
  grib.push_back(28);
  grib.push_back(2);
  grib.push_back(98);
  grib.push_back(1);
  grib.push_back(0);
  grib.push_back(0x80);
  grib.resize(8 + 28, 0);

  // Section 2 (GDS, 32 bytes): section_size=32, NV=0, PVL=255 (none),
  // dataRepresentationType=0 (lat/lon), Ni=2, Nj=2.
  grib.push_back(0);
  grib.push_back(0);
  grib.push_back(32);
  grib.push_back(0);
  grib.push_back(255);
  grib.push_back(0);
  grib.push_back(0);
  grib.push_back(2);  // Ni = 2
  grib.push_back(0);
  grib.push_back(2);  // Nj = 2
  grib.resize(8 + 28 + 32, 0);

  // Section 4 (BDS, 15 bytes = 11 header + 4 data bytes, bitsPerValue=8).
  grib.push_back(0);
  grib.push_back(0);
  grib.push_back(15);
  grib.push_back(0);  // flags = 0
  grib.push_back(0);
  grib.push_back(0);  // binaryScaleFactor = 0
  grib.push_back(0);
  grib.push_back(0);
  grib.push_back(0);
  grib.push_back(0);  // referenceValue = 0.0
  grib.push_back(8);  // bitsPerValue = 8
  grib.push_back(1);
  grib.push_back(2);
  grib.push_back(3);
  grib.push_back(4);

  // Section 5 (4 bytes): "7777".
  AppendText(grib, "7777");
  return grib;
}

std::string MakeMinimalSp3Text() {
  auto pad60 = [](std::string line) {
    if (line.size() < 60) {
      line.append(60 - line.size(), ' ');
    }
    line.push_back('\n');
    return line;
  };
  std::string out;
  out += pad60("#aP2026 01 01 00 00  0.00000000       0 ORBIT IGS   FIT TEST");
  out += pad60("## 2400      0.00000000   300.00000000 61041  0.000000000000");
  out += pad60("+    0");
  for (int i = 0; i < 4; ++i) {
    out += pad60("+        ");
  }
  for (int i = 0; i < 5; ++i) {
    out += pad60("++       ");
  }
  out += pad60("%c G     GPS");
  out += pad60("%c ");
  out += pad60("%f  0.0000000  0.000000000");
  for (int i = 0; i < 7; ++i) {
    out += pad60("/* ");
  }
  out += "EOF\n";
  return out;
}

std::string MakeMinimalRinexClockText() {
  auto pad80 = [](std::string_view prefix, std::string_view label) {
    std::string line(80, ' ');
    std::memcpy(line.data(), prefix.data(), prefix.size());
    std::memcpy(line.data() + 60, label.data(), label.size());
    line.push_back('\n');
    return line;
  };
  std::string out;
  out += pad80("     3.00           C                   G",
               "RINEX VERSION / TYPE");
  out += pad80("TESTPGM             TESTRUNBY           20260101 000000 UTC",
               "PGM / RUN BY / DATE");
  out += pad80("", "END OF HEADER");
  return out;
}

bool WriteMinimalHdf5File(const char* path) {
  ScopedH5File file(H5Fcreate(path, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
  if (!file) {
    return false;
  }

  // Root group attribute.
  ScopedH5Space scalar_space(H5Screate(H5S_SCALAR));
  if (!scalar_space) {
    return false;
  }
  ScopedH5Attr root_attr(H5Acreate2(file.get(), "root_attr", H5T_STD_I32LE,
                                    scalar_space.get(), H5P_DEFAULT,
                                    H5P_DEFAULT));
  const int32_t root_attr_val = 99;
  if (!root_attr ||
      H5Awrite(root_attr.get(), H5T_NATIVE_INT32, &root_attr_val) < 0) {
    return false;
  }

  // Subgroup "/grp" with two datasets so goto_next_record_field can step.
  ScopedH5Group grp(
      H5Gcreate2(file.get(), "grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  if (!grp) {
    return false;
  }

  // 1. Fixed-length string dataset "/grp/d_labels" (2 x 6-byte strings).
  ScopedH5Type str_type(H5Tcopy(H5T_C_S1));
  if (!str_type || H5Tset_size(str_type.get(), 6) < 0 ||
      H5Tset_strpad(str_type.get(), H5T_STR_NULLTERM) < 0) {
    return false;
  }
  const std::array<hsize_t, 1> str_dims = {2};
  ScopedH5Space str_space(H5Screate_simple(1, str_dims.data(), nullptr));
  if (!str_space) {
    return false;
  }
  ScopedH5Dataset d_labels(H5Dcreate2(grp.get(), "d_labels", str_type.get(),
                                      str_space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                      H5P_DEFAULT));
  const char labels_buf[2][6] = {"alpha", "bravo"};
  if (!d_labels || H5Dwrite(d_labels.get(), str_type.get(), H5S_ALL, H5S_ALL,
                            H5P_DEFAULT, labels_buf) < 0) {
    return false;
  }

  // 2. 2D int32 dataset "/grp/d_mat" (2 x 3) with attribute "units".
  const std::array<hsize_t, 2> mat_dims = {2, 3};
  ScopedH5Space mat_space(H5Screate_simple(2, mat_dims.data(), nullptr));
  if (!mat_space) {
    return false;
  }
  ScopedH5Dataset d_mat(H5Dcreate2(grp.get(), "d_mat", H5T_STD_I32LE,
                                   mat_space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
  const std::array<int32_t, 6> mat_vals = {1, 2, 3, 4, 5, 6};
  if (!d_mat || H5Dwrite(d_mat.get(), H5T_NATIVE_INT32, H5S_ALL, H5S_ALL,
                         H5P_DEFAULT, mat_vals.data()) < 0) {
    return false;
  }
  ScopedH5Attr mat_attr(H5Acreate2(d_mat.get(), "units", str_type.get(),
                                   scalar_space.get(), H5P_DEFAULT,
                                   H5P_DEFAULT));
  const char attr_str[6] = "kelvn";
  if (!mat_attr || H5Awrite(mat_attr.get(), str_type.get(), attr_str) < 0) {
    return false;
  }
  return true;
}

TEST(CodaCursorTest, NullAndInvalidCursorArgumentsReturnExpectedErrors) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  const absl::Cleanup cleanup_state = [] {
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  TempFile xml_file("coda_cursor_invalid_args.xml");
  ASSERT_TRUE(xml_file.WriteText(
      "<?xml version=\"1.0\"?><root><arr>1</arr><arr>2</arr></root>"));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(xml_file.c_str(), product.OutParam()));

  coda_cursor valid_root{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&valid_root, product.get()));

  // Uninitialized cursor (n == 0).
  coda_cursor zero_cursor{};

  long lval = 0;
  int ival = 0;
  int64_t i64val = 0;
  coda_product* pval = nullptr;
  coda_format fmt = coda_format_ascii;
  coda_type_class tc = coda_record_class;
  coda_native_type nt = coda_native_type_int8;
  coda_special_type st = coda_special_time;
  coda_type* type_ptr = nullptr;
  int num_dims = 0;
  std::array<long, CODA_MAX_NUM_DIMS> dims{};
  const long sub0 = 0;

  // coda_cursor_set_product
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_set_product(nullptr, product.get()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_set_product(&zero_cursor, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // Check NULL / uninitialized cursor rejection across all cursor APIs.
  for (coda_cursor* bad : {static_cast<coda_cursor*>(nullptr), &zero_cursor}) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto(bad, "/root"));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto(bad, "@"));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto(bad, "root"));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto(bad, ".."));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_first_record_field(bad));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_record_field_by_index(bad, 0));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_record_field_by_name(bad, "root"));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_next_record_field(bad));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_available_union_field(bad));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_first_array_element(bad));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element(bad, 1, &sub0));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(bad, 0));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_next_array_element(bad));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_attributes(bad));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_parent(bad));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_root(bad));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_use_base_type_of_special_type(bad));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_has_ascii_content(bad, &ival));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_has_attributes(bad, &ival));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_string_length(bad, &lval));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_bit_size(bad, &i64val));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_byte_size(bad, &i64val));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_num_elements(bad, &lval));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_product_file(bad, &pval));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_depth(bad, &ival));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_index(bad, &lval));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_file_bit_offset(bad, &i64val));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_file_byte_offset(bad, &i64val));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_format(bad, &fmt));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_type_class(bad, &tc));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_read_type(bad, &nt));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_special_type(bad, &st));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_type(bad, &type_ptr));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1,
              coda_cursor_get_record_field_index_from_name(bad, "root", &lval));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_record_field_available_status(bad, 0, &ival));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_available_union_field_index(bad, &lval));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_array_dim(bad, &num_dims, dims.data()));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  }

  // NULL output-argument checks and root-level parent/next/index checks.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_has_ascii_content(&valid_root, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_has_attributes(&valid_root, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_string_length(&valid_root, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_string_length(&valid_root, &lval));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_depth(&valid_root, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_index(&valid_root, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // At root (n == 1), get_index and goto_next_* and goto_parent fail.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_index(&valid_root, &lval));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_parent(&valid_root));
  EXPECT_EQ(CODA_ERROR_NO_PARENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_record_field(&valid_root));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&valid_root));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // Remaining NULL out-param checks.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_file_bit_offset(&valid_root, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_file_byte_offset(&valid_root, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_format(&valid_root, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_type_class(&valid_root, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_read_type(&valid_root, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_special_type(&valid_root, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_special_type(&valid_root, &st));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_type(&valid_root, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_use_base_type_of_special_type(&valid_root));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  // NULL out-params and type-mismatch checks on record vs array.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_record_field_available_status(&valid_root, 0,
                                                              nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_cursor_get_available_union_field_index(&valid_root, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_array_dim(&valid_root, nullptr, dims.data()));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_array_dim(&valid_root, &num_dims, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_array_dim(&valid_root, &num_dims, dims.data()));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&valid_root, 1, &sub0));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&valid_root, 0));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  // Move to "/root/arr" (an array) and verify record ops reject it.
  coda_cursor arr_cursor = valid_root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&arr_cursor, "/root/arr"));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_record_field_by_index(&arr_cursor, 0));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_available_union_field(&arr_cursor));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_cursor_get_record_field_available_status(&arr_cursor, 0, &ival));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_cursor_get_available_union_field_index(&arr_cursor, &lval));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  // Step to element [0] -> parent is array, so goto_next_record_field fails.
  coda_cursor elem_cursor = arr_cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&elem_cursor));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_record_field(&elem_cursor));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  // Step to attributes of array -> goto_next_array_element fails (index == -1).
  coda_cursor attr_cursor = arr_cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&attr_cursor));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&attr_cursor));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  // Step to attributes of root record -> goto_next_record_field fails
  // (index == -1), and parent of "/root" is record so goto_next_array_element
  // on "/root" fails with parent-not-array.
  attr_cursor = valid_root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&attr_cursor));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_record_field(&attr_cursor));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  coda_cursor root_field = valid_root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_record_field(&root_field));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&root_field));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
}

TEST(CodaCursorTest,
     BinaryCodadefNavigationOptionalHiddenUnionsArraysAndSpecial) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  const absl::Cleanup cleanup_state = [] {
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  TempFile codadef_file("coda_cursor_bin.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(MakeCursorCodadefBytes()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  TempFile bin_file("coda_cursor_bin.dat");
  ASSERT_TRUE(bin_file.WriteBytes(MakeCursorBinBytes(/*u_sel=*/0)));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(bin_file.c_str(), product.OutParam()));

  coda_cursor cursor{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  // 1. Root product, format, depth, type, bit/byte size, and offsets.
  coda_product* got_product = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_product_file(&cursor, &got_product));
  EXPECT_EQ(product.get(), got_product);

  coda_format format = coda_format_ascii;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_format(&cursor, &format));
  EXPECT_EQ(coda_format_binary, format);

  coda_type_class type_class = coda_integer_class;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &type_class));
  EXPECT_EQ(coda_record_class, type_class);

  coda_native_type read_type = coda_native_type_int8;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_read_type(&cursor, &read_type));
  EXPECT_EQ(coda_native_type_bytes, read_type);

  int depth = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_depth(&cursor, &depth));
  EXPECT_EQ(0, depth);

  long num_elements = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elements));
  EXPECT_EQ(20, num_elements);

  int64_t bit_size = 0;
  int64_t byte_size = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(103 * 8, bit_size);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_byte_size(&cursor, &byte_size));
  EXPECT_EQ(103, byte_size);

  int64_t bit_off = -1;
  int64_t byte_off = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_bit_offset(&cursor, &bit_off));
  EXPECT_EQ(0, bit_off);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_byte_offset(&cursor, &byte_off));
  EXPECT_EQ(0, byte_off);

  // 2. Attributes on binary record return empty record with bit_offset == -1.
  int has_attr = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&cursor, &has_attr));
  EXPECT_EQ(0, has_attr);
  {
    coda_cursor attr = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&attr));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_depth(&attr, &depth));
    EXPECT_EQ(1, depth);
    long attr_idx = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&attr, &attr_idx));
    EXPECT_EQ(-1, attr_idx);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&attr, &num_elements));
    EXPECT_EQ(0, num_elements);
    int64_t attr_bit_off = 0;
    int64_t attr_byte_off = 0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_file_bit_offset(&attr, &attr_bit_off));
    EXPECT_EQ(-1, attr_bit_off);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_file_byte_offset(&attr, &attr_byte_off));
    EXPECT_EQ(-1, attr_byte_off);
  }

  // 3. Hidden and optional fields (available vs no_data) and bit-size rounding.
  coda_type* root_type = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type(&cursor, &root_type));
  long hidden_idx = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_index_from_name(
                              &cursor, "hidden_flag", &hidden_idx));
  EXPECT_EQ(3, hidden_idx);
  int is_hidden = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_type_get_record_field_hidden_status(
                              root_type, hidden_idx, &is_hidden));
  EXPECT_EQ(1, is_hidden);

  int available = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_available_status(
                              &cursor, 4, &available));
  EXPECT_EQ(1, available);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_record_field_available_status(
                              &cursor, 5, &available));
  EXPECT_EQ(0, available);

  // Navigate first -> next through hidden & optional fields.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_record_field(&cursor));
  long field_idx = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&cursor, &field_idx));
  EXPECT_EQ(0, field_idx);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&cursor, &field_idx));
  EXPECT_EQ(3, field_idx);
  uint8_t hidden_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&cursor, &hidden_val));
  EXPECT_EQ(7, hidden_val);

  // Unavailable optional field (index 5 = opt_absent) has coda_special_no_data,
  // whose raw base type is binary (has_ascii == 0 -> get_string_length fails
  // with CODA_ERROR_INVALID_TYPE).
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &type_class));
  EXPECT_EQ(coda_special_class, type_class);
  coda_special_type sp_type = coda_special_time;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&cursor, &sp_type));
  EXPECT_EQ(coda_special_no_data, sp_type);
  int has_ascii = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_ascii_content(&cursor, &has_ascii));
  EXPECT_EQ(0, has_ascii);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_string_length(&cursor, &num_elements));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  // Non-byte-aligned bit sizes: 5 bits -> 1 byte, 9 bits -> 2 bytes.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "bits5"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(5, bit_size);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_byte_size(&cursor, &byte_size));
  EXPECT_EQ(1, byte_size);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "bits9"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(9, bit_size);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_byte_size(&cursor, &byte_size));
  EXPECT_EQ(2, byte_size);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_bit_offset(&cursor, &bit_off));
  EXPECT_EQ(14 * 8, bit_off);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_byte_offset(&cursor, &byte_off));
  EXPECT_EQ(14, byte_off);

  // 4. Fixed 2x3 array, variable-dim array, and 0-element array.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "fixed_arr"));
  int num_dims = 0;
  std::array<long, CODA_MAX_NUM_DIMS> dims{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&cursor, &num_dims, dims.data()));
  EXPECT_EQ(2, num_dims);
  EXPECT_EQ(2, dims[0]);
  EXPECT_EQ(3, dims[1]);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elements));
  EXPECT_EQ(6, num_elements);

  const std::array<long, 2> subs_ok = {1, 2};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element(&cursor, 2, subs_ok.data()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&cursor, &field_idx));
  EXPECT_EQ(5, field_idx);
  int16_t arr_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &arr_val));
  EXPECT_EQ(60, arr_val);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &arr_val));
  EXPECT_EQ(10, arr_val);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &arr_val));
  EXPECT_EQ(20, arr_val);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&cursor, 3));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &arr_val));
  EXPECT_EQ(40, arr_val);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));

  // Array rank mismatch and out-of-bounds checks.
  const std::array<long, 1> bad_rank = {0};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&cursor, 1, bad_rank.data()));
  EXPECT_EQ(CODA_ERROR_ARRAY_NUM_DIMS_MISMATCH, coda_errno);

  const std::array<long, 2> bad_subs = {2, 0};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&cursor, 2, bad_subs.data()));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&cursor, 6));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  // Variable-dim array (/count = 2) and empty array (/zero_count = 0).
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "var_arr"));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&cursor, &num_dims, dims.data()));
  EXPECT_EQ(1, num_dims);
  EXPECT_EQ(2, dims[0]);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&cursor, 1));
  uint8_t var_val = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&cursor, &var_val));
  EXPECT_EQ(22, var_val);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "empty_arr"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elements));
  EXPECT_EQ(0, num_elements);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(0, bit_size);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_byte_size(&cursor, &byte_size));
  EXPECT_EQ(0, byte_size);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_first_array_element(&cursor));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  // 5. Union navigation: get_available_union_field_index &
  //    goto_available_union_field.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&cursor, "u"));
  long union_idx = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_available_union_field_index(&cursor, &union_idx));
  EXPECT_EQ(0, union_idx);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_available_union_field(&cursor));
  int32_t u_i32 = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &u_i32));
  EXPECT_EQ(123456, u_i32);

  // 6. Binary string length and special types (complex, vsf_integer) +
  //    use_base_type_of_special_type.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "label"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_ascii_content(&cursor, &has_ascii));
  EXPECT_EQ(1, has_ascii);
  long str_len = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &str_len));
  EXPECT_EQ(5, str_len);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "bin_complex"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &type_class));
  EXPECT_EQ(coda_special_class, type_class);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&cursor, &sp_type));
  EXPECT_EQ(coda_special_complex, sp_type);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_ascii_content(&cursor, &has_ascii));
  EXPECT_EQ(0, has_ascii);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_use_base_type_of_special_type(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &type_class));
  EXPECT_EQ(coda_record_class, type_class);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "real"));
  double re = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &re));
  EXPECT_DOUBLE_EQ(1.5, re);
  // goto_parent goes from "real" to bin_complex base record, then to root.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_depth(&cursor, &depth));
  EXPECT_EQ(0, depth);

  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "bin_vsf"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&cursor, &sp_type));
  EXPECT_EQ(coda_special_vsf_integer, sp_type);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_use_base_type_of_special_type(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "value"));
  int16_t vsf_raw = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &vsf_raw));
  EXPECT_EQ(420, vsf_raw);

  // 7. Enable coda_option_bypass_special_types and verify automatic unwrapping
  //    across record-by-index, next-record-field, available-union-field, and
  //    array-element navigation.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_bypass_special_types(1));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));

  // Union u_special -> sp_time automatically unwrapped to record.
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "u_special"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_available_union_field(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &type_class));
  EXPECT_EQ(coda_record_class, type_class);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "days"));
  int32_t days = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &days));
  EXPECT_EQ(100, days);

  // goto_record_field_by_index on bin_complex (index 17) & next field bin_vsf
  // (index 18) both unwrap automatically.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_index(&cursor, 17));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &type_class));
  EXPECT_EQ(coda_record_class, type_class);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &type_class));
  EXPECT_EQ(coda_record_class, type_class);

  // complex_arr elements unwrap automatically across all array nav functions.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "complex_arr"));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element(&cursor, 1, bad_rank.data()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &type_class));
  EXPECT_EQ(coda_record_class, type_class);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &type_class));
  EXPECT_EQ(coda_record_class, type_class);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&cursor, 1));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &type_class));
  EXPECT_EQ(coda_record_class, type_class);

  // Binary out-of-bounds / end-of-sequence error paths.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&cursor));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_record_field_by_index(&cursor, 99));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_available_union_field(&cursor));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_index(&cursor, 19));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_record_field(&cursor));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);
}

TEST(CodaCursorTest, AsciiCodadefNavigationStringLengthAndSpecialBypass) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  const absl::Cleanup cleanup_state = [] {
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  TempFile codadef_file("coda_cursor_asc.codadef");
  ASSERT_TRUE(codadef_file.WriteBytes(MakeCursorCodadefBytes()));
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(codadef_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  TempFile asc_file("coda_cursor_asc.dat");
  // Include a trailing '\n' because get_bit_size_boundary() in
  // coda-ascii-cursor.c checks `current_bit_offset + read_bit_size >=
  // bit_size`.
  ASSERT_TRUE(asc_file.WriteText("CURSASC1  22026-01-01T12:00:00 1.25 2.50\n"));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(asc_file.c_str(), product.OutParam()));

  coda_cursor cursor{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  coda_format format = coda_format_binary;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_format(&cursor, &format));
  EXPECT_EQ(coda_format_ascii, format);

  int64_t bit_size = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(40 * 8, bit_size);

  long num_elements = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elements));
  EXPECT_EQ(4, num_elements);

  int has_ascii = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_ascii_content(&cursor, &has_ascii));
  EXPECT_EQ(1, has_ascii);

  // Navigate to asc_time (special time wrapping 19-byte ASCII text).
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "asc_time"));
  coda_special_type sp_type = coda_special_no_data;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&cursor, &sp_type));
  EXPECT_EQ(coda_special_time, sp_type);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_ascii_content(&cursor, &has_ascii));
  EXPECT_EQ(1, has_ascii);

  long len = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &len));
  EXPECT_EQ(19, len);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_use_base_type_of_special_type(&cursor));
  coda_type_class tc = coda_special_class;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &tc));
  EXPECT_EQ(coda_text_class, tc);

  int64_t bit_off = -1;
  int64_t byte_off = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_bit_offset(&cursor, &bit_off));
  EXPECT_EQ(11 * 8, bit_off);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_byte_offset(&cursor, &byte_off));
  EXPECT_EQ(11, byte_off);

  // Attributes on ascii node.
  {
    coda_cursor attr = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&attr));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_byte_offset(&attr, &byte_off));
    EXPECT_EQ(-1, byte_off);
  }

  // Array navigation in ascii backend.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "arr"));
  int num_dims = 0;
  std::array<long, CODA_MAX_NUM_DIMS> dims{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&cursor, &num_dims, dims.data()));
  EXPECT_EQ(1, num_dims);
  EXPECT_EQ(2, dims[0]);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&cursor));
  double val = 0.0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &val));
  EXPECT_DOUBLE_EQ(1.25, val);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &val));
  EXPECT_DOUBLE_EQ(2.50, val);
}

TEST(CodaCursorTest, XmlProductPathsAttributesCompareAndPrintPath) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  const absl::Cleanup cleanup_state = [] {
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  TempFile xml_file("coda_cursor_xml.xml");
  ASSERT_TRUE(
      xml_file.WriteText("<?xml version=\"1.0\"?>"
                         "<root tag=\"abc\">"
                         "<item idx=\"1\">hello</item>"
                         "<item idx=\"2\">world!</item>"
                         "<nested><val>42</val></nested>"
                         "</root>"));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(xml_file.c_str(), product.OutParam()));

  coda_cursor root{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
  EXPECT_EQ("/", PrintCursorPath(root));

  coda_format format = coda_format_binary;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_format(&root, &format));
  EXPECT_EQ(coda_format_xml, format);

  // Memory backend returns -1 for file bit/byte offset (byte_offset returns
  // -1).
  int64_t bit_off = 0;
  int64_t byte_off = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_file_bit_offset(&root, &bit_off));
  EXPECT_EQ(-1, bit_off);
  EXPECT_EQ(-1, coda_cursor_get_file_byte_offset(&root, &byte_off));
  EXPECT_EQ(-1, byte_off);

  // 1. Valid path navigation variants in coda_cursor_goto().
  coda_cursor c = root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, ""));
  EXPECT_EQ("/", PrintCursorPath(c));

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "//root"));
  EXPECT_EQ("/root", PrintCursorPath(c));

  int has_attr = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_has_attributes(&c, &has_attr));
  EXPECT_EQ(1, has_attr);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "@tag"));
  EXPECT_EQ("/root@tag", PrintCursorPath(c));
  long str_len = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&c, &str_len));
  EXPECT_EQ(3, str_len);

  // Attribute traversal with "@.", "@..", and bare "@".
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/root@"));
  EXPECT_EQ("/root@", PrintCursorPath(c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/root@."));
  EXPECT_EQ("/root@", PrintCursorPath(c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/root@.."));
  EXPECT_EQ("/root", PrintCursorPath(c));

  // Nested path with array index and attribute.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/root/item[1]"));
  EXPECT_EQ("/root/item[1]", PrintCursorPath(c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&c, &str_len));
  EXPECT_EQ(6, str_len);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "@idx"));
  EXPECT_EQ("/root/item[1]@idx", PrintCursorPath(c));

  // Relative ".", "..", and absolute "/" and "/@" paths.
  // Navigating to "@idx" pushed both "@" (attribute record) and "idx", so
  // one ".." moves to "/root/item[1]@" and a second ".." returns to
  // "/root/item[1]".
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, ".."));
  EXPECT_EQ("/root/item[1]@", PrintCursorPath(c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, ".."));
  EXPECT_EQ("/root/item[1]", PrintCursorPath(c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "."));
  EXPECT_EQ("/root/item[1]", PrintCursorPath(c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "../../nested/val"));
  EXPECT_EQ("/root/nested/val", PrintCursorPath(c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/@"));
  EXPECT_EQ("/@", PrintCursorPath(c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/"));
  EXPECT_EQ("/", PrintCursorPath(c));

  // 2. Invalid path syntax and target errors in coda_cursor_goto().
  struct BadPathCase {
    const char* path;
    int expected_errno;
  };
  constexpr BadPathCase kBadPaths[] = {
      {.path = "/root/item[0", .expected_errno = CODA_ERROR_INVALID_ARGUMENT},
      {.path = "/root/item[]", .expected_errno = CODA_ERROR_INVALID_ARGUMENT},
      {.path = "/root/item[abc]",
       .expected_errno = CODA_ERROR_INVALID_ARGUMENT},
      {.path = "/root/item[1x]", .expected_errno = CODA_ERROR_INVALID_ARGUMENT},
      {.path = "/root/item[0]foo",
       .expected_errno = CODA_ERROR_INVALID_ARGUMENT},
      {.path = "/root/missing", .expected_errno = CODA_ERROR_INVALID_NAME},
      {.path = "/root@missing", .expected_errno = CODA_ERROR_INVALID_NAME},
      {.path = "/root/item[9]",
       .expected_errno = CODA_ERROR_ARRAY_OUT_OF_BOUNDS},
      {.path = "/..", .expected_errno = CODA_ERROR_NO_PARENT},
  };
  for (const BadPathCase& tc : kBadPaths) {
    coda_set_error(CODA_SUCCESS, nullptr);
    c = root;
    EXPECT_EQ(-1, coda_cursor_goto(&c, tc.path)) << tc.path;
    EXPECT_EQ(tc.expected_errno, coda_errno) << tc.path;
  }

  // 3. coda_cursor_compare() across depth, field index, array index, and attr.
  coda_cursor c_item = root;
  coda_cursor c_item0 = root;
  coda_cursor c_item1 = root;
  coda_cursor c_nested = root;
  coda_cursor c_root_attr = root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c_item, "/root/item"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c_item0, "/root/item[0]"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c_item1, "/root/item[1]"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c_nested, "/root/nested"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c_root_attr, "/root@"));

  EXPECT_EQ(0, coda_cursor_compare(&root, &root));
  EXPECT_EQ(0, coda_cursor_compare(&c_item0, &c_item0));
  EXPECT_EQ(-1, coda_cursor_compare(&c_item0, &c_item1));
  EXPECT_EQ(1, coda_cursor_compare(&c_item1, &c_item0));
  EXPECT_EQ(-1, coda_cursor_compare(&c_item, &c_nested));
  EXPECT_EQ(1, coda_cursor_compare(&c_nested, &c_item));
  EXPECT_EQ(-1, coda_cursor_compare(&c_item, &c_item0));
  EXPECT_EQ(1, coda_cursor_compare(&c_item0, &c_item));
  EXPECT_EQ(-1, coda_cursor_compare(&c_root_attr, &c_item));
  EXPECT_EQ(1, coda_cursor_compare(&c_item, &c_root_attr));

  // 4. coda_cursor_print_path() error propagation when print callback fails.
  EXPECT_EQ(-1, PrintCursorPathFailingOnCall(root, 1));
  EXPECT_EQ(-1, PrintCursorPathFailingOnCall(c_root_attr, 3));
  EXPECT_EQ(-1, PrintCursorPathFailingOnCall(c_item0, 1));
  EXPECT_EQ(-1, PrintCursorPathFailingOnCall(c_item0, 2));
  EXPECT_EQ(-1, PrintCursorPathFailingOnCall(c_item0, 5));

  coda_cursor top_attr = root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&top_attr));
  EXPECT_EQ("/@", PrintCursorPath(top_attr));
  EXPECT_EQ(-1, PrintCursorPathFailingOnCall(top_attr, 1));

  // 5. Memory backend queries and record/array navigation error paths on XML.
  int64_t mem_bit_size = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&root, &mem_bit_size));
  EXPECT_EQ(-1, mem_bit_size);
  c = root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/root/nested/val"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&c, &mem_bit_size));
  EXPECT_EQ(16, mem_bit_size);

  long num_elems = 0;
  c = root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "/root"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&c, &num_elems));
  EXPECT_EQ(2, num_elems);

  int avail = 0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_record_field_available_status(&c, 0, &avail));
  EXPECT_EQ(1, avail);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_get_record_field_available_status(&c, 99, &avail));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_record_field_by_index(&c, 99));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_record_field(&c));
  EXPECT_EQ("/root/item", PrintCursorPath(c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&c));
  EXPECT_EQ("/root/nested", PrintCursorPath(c));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_record_field(&c));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_available_union_field(&top_attr));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);

  int num_dims = 0;
  std::array<long, CODA_MAX_NUM_DIMS> dims{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&c_item, &num_dims, dims.data()));
  EXPECT_EQ(1, num_dims);
  EXPECT_EQ(2, dims[0]);

  const long sub0 = 0;
  const long bad_sub = 9;
  c = c_item;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element(&c, 1, &sub0));
  EXPECT_EQ("/root/item[0]", PrintCursorPath(c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&c));
  EXPECT_EQ("/root/item[1]", PrintCursorPath(c));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&c));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  c = c_item;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&c, 1, &bad_sub));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
}

TEST(CodaCursorTest, MemoryBackendMaxDepthNestedSpecialUnionAndRootArrayPrint) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  const absl::Cleanup cleanup_state = [] {
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  std::vector<ScopedType> owned_types;

  // 1. Test CODA_CURSOR_MAXDEPTH (32) enforcement for record, array, and
  //    attributes navigation.
  {
    ScopedProduct product = MakeSyntheticMemProduct();
    ASSERT_NE(nullptr, product.get());

    // Build leaf array (with 1 int32 element) and leaf record (with 1 int32
    // field) to sit at depth 31 (stack size n == 32 == CODA_CURSOR_MAXDEPTH).
    coda_type_number* int_def =
        coda_type_number_new(coda_format_sp3, coda_integer_class);
    ASSERT_NE(nullptr, int_def);
    TrackType(int_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_read_type(reinterpret_cast<coda_type*>(int_def),
                                      coda_native_type_int32));
    ASSERT_EQ(CODA_SUCCESS, coda_type_set_bit_size(
                                reinterpret_cast<coda_type*>(int_def), 32));

    coda_type_array* arr_def = coda_type_array_new(coda_format_sp3);
    ASSERT_NE(nullptr, arr_def);
    TrackType(arr_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_array_set_base_type(
                  arr_def, reinterpret_cast<coda_type*>(int_def)));
    ASSERT_EQ(CODA_SUCCESS, coda_type_array_add_fixed_dimension(arr_def, 1));
    UniqueDynamicType<coda_mem_array> leaf_arr(
        coda_mem_array_new(arr_def, nullptr));
    ASSERT_NE(nullptr, leaf_arr);
    UniqueDynamicType<coda_mem_data> elem(
        coda_mem_int32_new(int_def, nullptr, product.get(), 5));
    ASSERT_NE(nullptr, elem);
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_array_set_element(
                  leaf_arr.get(), 0,
                  reinterpret_cast<coda_dynamic_type*>(elem.get())));
    elem.release();

    coda_type_record* leaf_rec_def = coda_type_record_new(coda_format_sp3);
    ASSERT_NE(nullptr, leaf_rec_def);
    TrackType(leaf_rec_def, owned_types);
    UniqueDynamicType<coda_mem_record> leaf_rec(
        coda_mem_record_new(leaf_rec_def, nullptr));
    ASSERT_NE(nullptr, leaf_rec);
    UniqueDynamicType<coda_mem_data> leaf_val(
        coda_mem_int32_new(int_def, nullptr, product.get(), 9));
    ASSERT_NE(nullptr, leaf_val);
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(
                  leaf_rec.get(), "v",
                  reinterpret_cast<coda_dynamic_type*>(leaf_val.get()), 1));
    leaf_val.release();

    // Parent at depth 30 (n == 31) holds both leaf_rec ("r") and leaf_arr
    // ("a").
    coda_type_record* fork_def = coda_type_record_new(coda_format_sp3);
    ASSERT_NE(nullptr, fork_def);
    TrackType(fork_def, owned_types);
    UniqueDynamicType<coda_mem_record> fork(
        coda_mem_record_new(fork_def, nullptr));
    ASSERT_NE(nullptr, fork);
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(
                  fork.get(), "r",
                  reinterpret_cast<coda_dynamic_type*>(leaf_rec.get()), 1));
    leaf_rec.release();
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(
                  fork.get(), "a",
                  reinterpret_cast<coda_dynamic_type*>(leaf_arr.get()), 1));
    leaf_arr.release();

    // Wrap 30 more parent record levels so fork is at depth 30 (n = 31) and
    // its children ("r" and "a") are at depth 31 (n = 32 =
    // CODA_CURSOR_MAXDEPTH).
    UniqueDynamicType<coda_mem_record> cur(fork.release());
    for (int i = 0; i < 30; ++i) {
      coda_type_record* parent_def = coda_type_record_new(coda_format_sp3);
      ASSERT_NE(nullptr, parent_def);
      TrackType(parent_def, owned_types);
      UniqueDynamicType<coda_mem_record> parent(
          coda_mem_record_new(parent_def, nullptr));
      ASSERT_NE(nullptr, parent);
      ASSERT_EQ(CODA_SUCCESS,
                coda_mem_record_add_field(
                    parent.get(), "c",
                    reinterpret_cast<coda_dynamic_type*>(cur.get()), 1));
      cur.release();
      cur = std::move(parent);
    }
    product.get()->root_type =
        reinterpret_cast<coda_dynamic_type*>(cur.release());

    coda_cursor cursor{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    for (int i = 0; i < 30; ++i) {
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_goto_record_field_by_index(&cursor, 0));
    }
    int depth = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_depth(&cursor, &depth));
    EXPECT_EQ(30, depth);

    // Descend to "r" -> depth 31 (n == 32 == CODA_CURSOR_MAXDEPTH).
    coda_cursor max_rec = cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&max_rec, "r"));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_depth(&max_rec, &depth));
    EXPECT_EQ(31, depth);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_record_field_by_index(&max_rec, 0));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto(&max_rec, "v"));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_attributes(&max_rec));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    // Descend to "a" -> depth 31 (n == 32 == CODA_CURSOR_MAXDEPTH).
    coda_cursor max_arr = cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&max_arr, "a"));
    const long sub0 = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element(&max_arr, 1, &sub0));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&max_arr, 0));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  }

  // 2. Root special type wrapping another special type: test goto_root and
  //    nested use_base_type_of_special_type with bypass_special_types = 0/1.
  {
    ScopedProduct product = MakeSyntheticMemProduct();
    ASSERT_NE(nullptr, product.get());

    coda_type_text* txt_def = coda_type_text_new(coda_format_sp3);
    ASSERT_NE(nullptr, txt_def);
    TrackType(txt_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS, coda_type_set_byte_size(
                                reinterpret_cast<coda_type*>(txt_def), 4));

    ScopedExpr time_expr1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("float(0)", time_expr1.OutParam()));
    coda_type_special* inner_time_def =
        coda_type_time_new(coda_format_sp3, time_expr1.get());
    ASSERT_NE(nullptr, inner_time_def);
    time_expr1.release();
    TrackType(inner_time_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_time_set_base_type(
                  inner_time_def, reinterpret_cast<coda_type*>(txt_def)));

    ScopedExpr time_expr2;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("float(0)", time_expr2.OutParam()));
    coda_type_special* outer_time_def =
        coda_type_time_new(coda_format_sp3, time_expr2.get());
    ASSERT_NE(nullptr, outer_time_def);
    time_expr2.release();
    TrackType(outer_time_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS, coda_type_time_set_base_type(
                                outer_time_def,
                                reinterpret_cast<coda_type*>(inner_time_def)));

    UniqueDynamicType<coda_mem_data> mem_str(
        coda_mem_string_new(txt_def, nullptr, product.get(), "2026"));
    ASSERT_NE(nullptr, mem_str);
    UniqueDynamicType<coda_mem_special> mem_inner(
        coda_mem_time_new(inner_time_def, nullptr,
                          reinterpret_cast<coda_dynamic_type*>(mem_str.get())));
    ASSERT_NE(nullptr, mem_inner);
    mem_str.release();
    UniqueDynamicType<coda_mem_special> mem_outer(coda_mem_time_new(
        outer_time_def, nullptr,
        reinterpret_cast<coda_dynamic_type*>(mem_inner.get())));
    ASSERT_NE(nullptr, mem_outer);
    mem_inner.release();
    product.get()->root_type =
        reinterpret_cast<coda_dynamic_type*>(mem_outer.release());

    ASSERT_EQ(CODA_SUCCESS, coda_set_option_bypass_special_types(1));
    coda_cursor cursor{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    coda_type_class tc = coda_record_class;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &tc));
    EXPECT_EQ(coda_special_class, tc);

    // One call unwraps both special layers because bypass_special_types == 1.
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_use_base_type_of_special_type(&cursor));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &tc));
    EXPECT_EQ(coda_text_class, tc);

    // goto_root also unwraps root special type when bypass_special_types == 1.
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &tc));
    EXPECT_EQ(coda_text_class, tc);
  }

  // 3. Memory union with special field + root array print_path ("/[0]").
  {
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_bypass_special_types(1));
    ScopedProduct product = MakeSyntheticMemProduct();
    ASSERT_NE(nullptr, product.get());

    coda_type_text* txt_def = coda_type_text_new(coda_format_sp3);
    ASSERT_NE(nullptr, txt_def);
    TrackType(txt_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS, coda_type_set_byte_size(
                                reinterpret_cast<coda_type*>(txt_def), 4));

    ScopedExpr time_expr;
    ASSERT_EQ(CODA_SUCCESS,
              coda_expression_from_string("float(0)", time_expr.OutParam()));
    coda_type_special* time_def =
        coda_type_time_new(coda_format_sp3, time_expr.get());
    ASSERT_NE(nullptr, time_def);
    time_expr.release();
    TrackType(time_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_time_set_base_type(
                  time_def, reinterpret_cast<coda_type*>(txt_def)));

    coda_type_record* union_def = coda_type_union_new(coda_format_sp3);
    ASSERT_NE(nullptr, union_def);
    TrackType(union_def, owned_types);

    ScopedRecordField f0(coda_type_record_field_new("unused"));
    ASSERT_NE(nullptr, f0.get());
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_optional(f0.get()));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_field_set_type(
                  f0.get(), reinterpret_cast<coda_type*>(txt_def)));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_add_field(union_def, f0.release()));

    ScopedRecordField f1(coda_type_record_field_new("ts"));
    ASSERT_NE(nullptr, f1.get());
    ASSERT_EQ(CODA_SUCCESS, coda_type_record_field_set_optional(f1.get()));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_field_set_type(
                  f1.get(), reinterpret_cast<coda_type*>(time_def)));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_record_add_field(union_def, f1.release()));

    coda_type_array* root_arr_def = coda_type_array_new(coda_format_sp3);
    ASSERT_NE(nullptr, root_arr_def);
    TrackType(root_arr_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_array_set_base_type(
                  root_arr_def, reinterpret_cast<coda_type*>(union_def)));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_array_add_fixed_dimension(root_arr_def, 1));

    UniqueDynamicType<coda_mem_record> mem_union(
        coda_mem_record_new(union_def, nullptr));
    ASSERT_NE(nullptr, mem_union);
    UniqueDynamicType<coda_mem_data> mem_txt(
        coda_mem_string_new(txt_def, nullptr, product.get(), "2026"));
    ASSERT_NE(nullptr, mem_txt);
    UniqueDynamicType<coda_mem_special> mem_time(
        coda_mem_time_new(time_def, nullptr,
                          reinterpret_cast<coda_dynamic_type*>(mem_txt.get())));
    ASSERT_NE(nullptr, mem_time);
    mem_txt.release();
    mem_union->field_type[1] =
        reinterpret_cast<coda_dynamic_type*>(mem_time.release());

    UniqueDynamicType<coda_mem_array> mem_arr(
        coda_mem_array_new(root_arr_def, nullptr));
    ASSERT_NE(nullptr, mem_arr);
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_array_set_element(
                  mem_arr.get(), 0,
                  reinterpret_cast<coda_dynamic_type*>(mem_union.get())));
    mem_union.release();
    product.get()->root_type =
        reinterpret_cast<coda_dynamic_type*>(mem_arr.release());

    coda_cursor cursor{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&cursor));
    EXPECT_EQ("/[0]", PrintCursorPath(cursor));
    EXPECT_EQ(-1, PrintCursorPathFailingOnCall(cursor, 1));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/[0]"));
    EXPECT_EQ("/[0]", PrintCursorPath(cursor));

    long avail_idx = -1;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_available_union_field_index(&cursor, &avail_idx));
    EXPECT_EQ(1, avail_idx);

    coda_cursor union_cursor = cursor;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_available_union_field(&union_cursor));
    coda_type_class tc = coda_special_class;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&union_cursor, &tc));
    EXPECT_EQ(coda_text_class, tc);
  }
}

TEST(CodaCursorTest, Hdf5BackendNavigationStringsAndAttributes) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  const absl::Cleanup cleanup_state = [] {
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  TempFile h5_file("coda_cursor_hdf5.h5");
  ASSERT_TRUE(WriteMinimalHdf5File(h5_file.c_str()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));

  coda_cursor cursor{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  coda_format format = coda_format_ascii;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_format(&cursor, &format));
  EXPECT_EQ(coda_format_hdf5, format);

  int64_t bit_size = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(-1, bit_size);
  int64_t byte_size = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_byte_size(&cursor, &byte_size));
  EXPECT_EQ(-1, byte_size);

  long num_elements = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elements));
  EXPECT_EQ(1, num_elements);

  int avail = 0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_record_field_available_status(&cursor, 0, &avail));
  EXPECT_EQ(1, avail);

  // Root attribute.
  {
    coda_cursor attr = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&attr));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&attr, "root_attr"));
    int32_t v = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&attr, &v));
    EXPECT_EQ(99, v);
  }

  // Navigate "/grp" -> first field -> next field -> past end.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_record_field_by_index(&cursor, 99));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_index(&cursor, 0));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elements));
  EXPECT_EQ(2, num_elements);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_record_field(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_record_field(&cursor));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_record_field(&cursor));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  // Fixed-length string dataset "/grp/d_labels" -> string length == 6.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "d_labels"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_first_array_element(&cursor));
  long str_len = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &str_len));
  EXPECT_EQ(6, str_len);

  // 2D dataset "/grp/d_mat" (2x3): array dim, subscripts, index, next, attrs.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/grp/d_mat"));
  int num_dims = 0;
  std::array<long, CODA_MAX_NUM_DIMS> dims{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&cursor, &num_dims, dims.data()));
  EXPECT_EQ(2, num_dims);
  EXPECT_EQ(2, dims[0]);
  EXPECT_EQ(3, dims[1]);

  {
    coda_cursor attr = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&attr));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&attr, "units"));
    std::array<char, 8> units{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_string(&attr, units.data(),
                                      static_cast<long>(units.size())));
    EXPECT_STREQ("kelvn", units.data());
  }

  const std::array<long, 2> subs = {1, 1};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element(&cursor, 2, subs.data()));
  int32_t elem = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &elem));
  EXPECT_EQ(5, elem);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &elem));
  EXPECT_EQ(6, elem);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&cursor, 2));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &elem));
  EXPECT_EQ(3, elem);

  // Out-of-bounds checks on HDF5 dataset array navigation.
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  const std::array<long, 2> bad_subs = {2, 0};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&cursor, 2, bad_subs.data()));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&cursor, 99));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&cursor, 5));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&cursor));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
}

TEST(CodaCursorTest, NetcdfBackendNavigationStringsAndAttributes) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  const absl::Cleanup cleanup_state = [] {
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  NetcdfBuilder nc(1);
  nc.AddDim({.name = "row", .size = 2});
  nc.AddDim({.name = "col", .size = 3});
  nc.AddDim({.name = "strlen", .size = 5});
  nc.AddGlobalAttr({.name = "title",
                    .type = NetcdfType::kChar,
                    .count = 4,
                    .raw_values = {'t', 'e', 's', 't'}});

  std::vector<uint8_t> grid_bytes;
  for (uint32_t v = 10; v <= 60; v += 10) {
    AppendBe32(grid_bytes, v);
  }
  nc.AddVar({.name = "grid",
             .dim_ids = {0, 1},
             .attrs = {{.name = "scale",
                        .type = NetcdfType::kInt,
                        .count = 1,
                        .raw_values = {0, 0, 0, 2}}},
             .type = NetcdfType::kInt,
             .vsize = 24,
             .data = grid_bytes});
  nc.AddVar({.name = "label",
             .dim_ids = {2},
             .attrs = {},
             .type = NetcdfType::kChar,
             .vsize = 8,
             .data = {'h', 'e', 'l', 'l', 'o'}});

  TempFile nc_file("coda_cursor_netcdf.nc");
  ASSERT_TRUE(nc_file.WriteBytes(nc.Build()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(nc_file.c_str(), product.OutParam()));

  coda_cursor cursor{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  coda_format format = coda_format_ascii;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_format(&cursor, &format));
  EXPECT_EQ(coda_format_netcdf, format);

  int avail = 0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_record_field_available_status(&cursor, 0, &avail));
  EXPECT_EQ(1, avail);

  // Navigate to 2D netCDF variable "grid".
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "grid"));
  int64_t bit_size = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(-1, bit_size);

  long num_elements = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elements));
  EXPECT_EQ(6, num_elements);

  int num_dims = 0;
  std::array<long, CODA_MAX_NUM_DIMS> dims{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&cursor, &num_dims, dims.data()));
  EXPECT_EQ(2, num_dims);
  EXPECT_EQ(2, dims[0]);
  EXPECT_EQ(3, dims[1]);

  {
    coda_cursor attr = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&attr));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&attr, "scale"));
    int32_t scale = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&attr, &scale));
    EXPECT_EQ(2, scale);
  }

  const std::array<long, 2> subs = {1, 0};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_array_element(&cursor, 2, subs.data()));
  int32_t v = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &v));
  EXPECT_EQ(40, v);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &v));
  EXPECT_EQ(50, v);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&cursor, 5));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &v));
  EXPECT_EQ(60, v);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&cursor));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  const std::array<long, 2> bad_subs = {2, 0};
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&cursor, 2, bad_subs.data()));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&cursor, 99));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  // Navigate to 1D char variable "label" (coda_netcdf_basic_type text).
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "label"));
  long str_len = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &str_len));
  EXPECT_EQ(5, str_len);
}

TEST(CodaCursorTest, CdfBackendNavigationTimeStringsAndArrays) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  const absl::Cleanup cleanup_state = [] {
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  TempFile cdf_file("coda_cursor_cdf.cdf");
  ASSERT_TRUE(cdf_file.WriteBytes(MakeMinimalCdfV3Bytes()));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(cdf_file.c_str(), product.OutParam()));

  coda_cursor cursor{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));

  coda_format format = coda_format_ascii;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_format(&cursor, &format));
  EXPECT_EQ(coda_format_cdf, format);

  int avail = 0;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_record_field_available_status(&cursor, 0, &avail));
  EXPECT_EQ(1, avail);

  // 1. zVariable "arr" (3-element 1D array).
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "arr"));
  int64_t bit_size = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
  EXPECT_EQ(-1, bit_size);

  long num_elements = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&cursor, &num_elements));
  EXPECT_EQ(3, num_elements);

  int num_dims = 0;
  std::array<long, CODA_MAX_NUM_DIMS> dims{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_get_array_dim(&cursor, &num_dims, dims.data()));
  EXPECT_EQ(1, num_dims);
  EXPECT_EQ(3, dims[0]);

  {
    coda_cursor attr = cursor;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&attr));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_num_elements(&attr, &num_elements));
    EXPECT_EQ(0, num_elements);
  }

  const long sub0 = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element(&cursor, 1, &sub0));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&cursor));
  long elem_idx = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&cursor, &elem_idx));
  EXPECT_EQ(1, elem_idx);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&cursor, 2));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&cursor, &elem_idx));
  EXPECT_EQ(2, elem_idx);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&cursor));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
  const long bad_sub = 9;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element(&cursor, 1, &bad_sub));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&cursor, 99));
  EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

  // 2. zVariable "txt" (scalar 5-char string).
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "txt"));
  long str_len = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &str_len));
  EXPECT_EQ(5, str_len);

  // 3. zVariable "epoch" (CDF_EPOCH special time -> base double).
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_root(&cursor));
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_goto_record_field_by_name(&cursor, "epoch"));
  coda_special_type sp_type = coda_special_no_data;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_special_type(&cursor, &sp_type));
  EXPECT_EQ(coda_special_time, sp_type);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_use_base_type_of_special_type(&cursor));
  coda_type_class tc = coda_special_class;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_type_class(&cursor, &tc));
  EXPECT_EQ(coda_real_class, tc);
}

// Helpers for testing CODA_ERROR_NO_HDF4_SUPPORT cursor branches: HDF4 is
// compiled out in google3 (HAVE_HDF4 undefined), so these branches are
// unreachable via coda_open() and require constructing a synthetic cursor
// using unmangled coda_cursor fields enabled by CODA_INTERNAL.
coda_cursor MakeSyntheticHdf4RootCursor(coda_product* product,
                                        coda_dynamic_type* dyn) {
  coda_cursor c{};
  c.product = product;
  c.n = 1;
  c.stack[0].type = dyn;
  c.stack[0].index = -1;
  c.stack[0].bit_offset = -1;
  return c;
}

coda_cursor PushSyntheticHdf4Child(const coda_cursor& parent,
                                   coda_dynamic_type* child_dyn, long index) {
  coda_cursor c = parent;
  c.n = parent.n + 1;
  c.stack[c.n - 1].type = child_dyn;
  c.stack[c.n - 1].index = index;
  c.stack[c.n - 1].bit_offset = -1;
  return c;
}

TEST(CodaCursorTest, GribRinexSp3AndHdf4UnsupportedBranches) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();

  const absl::Cleanup cleanup_state = [] {
    coda_set_definition_path(nullptr);
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. GRIB1 backend dispatch in coda-cursor.c.
  {
    TempFile grib_file("coda_cursor_grib1.grb");
    ASSERT_TRUE(grib_file.WriteBytes(MakeMinimalGrib1Bytes()));

    ScopedProduct product;
    ASSERT_EQ(CODA_SUCCESS, coda_open(grib_file.c_str(), product.OutParam()));

    coda_cursor cursor{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
    coda_format format = coda_format_ascii;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_format(&cursor, &format));
    EXPECT_EQ(coda_format_grib, format);

    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto(&cursor, "/[0]/grib1/data/values"));
    int64_t bit_size = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&cursor, &bit_size));
    EXPECT_EQ(-1, bit_size);

    long num_elements = 0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_num_elements(&cursor, &num_elements));
    EXPECT_EQ(4, num_elements);

    int num_dims = 0;
    std::array<long, CODA_MAX_NUM_DIMS> dims{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_array_dim(&cursor, &num_dims, dims.data()));
    EXPECT_EQ(1, num_dims);
    EXPECT_EQ(4, dims[0]);

    {
      coda_cursor attr = cursor;
      ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_attributes(&attr));
      ASSERT_EQ(CODA_SUCCESS,
                coda_cursor_get_num_elements(&attr, &num_elements));
      EXPECT_EQ(0, num_elements);
    }

    const long sub0 = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element(&cursor, 1, &sub0));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_next_array_element(&cursor));
    long idx = -1;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&cursor, &idx));
    EXPECT_EQ(1, idx);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_array_element_by_index(&cursor, 3));
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_index(&cursor, &idx));
    EXPECT_EQ(3, idx);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&cursor));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_parent(&cursor));
    const long bad_sub = 9;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element(&cursor, 1, &bad_sub));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&cursor, 99));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
  }

  // 2. SP3 and RINEX format dispatch in coda_cursor_set_product().
  {
    TempFile sp3_file("coda_cursor_sp3.sp3");
    ASSERT_TRUE(sp3_file.WriteText(MakeMinimalSp3Text()));

    ScopedProduct sp3_prod;
    ASSERT_EQ(CODA_SUCCESS, coda_open(sp3_file.c_str(), sp3_prod.OutParam()));

    coda_cursor sp3_cursor{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_set_product(&sp3_cursor, sp3_prod.get()));
    coda_format format = coda_format_ascii;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_format(&sp3_cursor, &format));
    EXPECT_EQ(coda_format_sp3, format);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&sp3_cursor, "/header"));
  }

  {
    TempFile rnx_file("coda_cursor_rinex.clk");
    ASSERT_TRUE(rnx_file.WriteText(MakeMinimalRinexClockText()));

    ScopedProduct rnx_prod;
    ASSERT_EQ(CODA_SUCCESS, coda_open(rnx_file.c_str(), rnx_prod.OutParam()));
    const absl::Cleanup free_rinex_mem_ptr = [&rnx_prod] {
      // Upstream coda_rinex_close() omits free(product->mem_ptr), leaking the
      // DATA_BLOCK_SIZE (4096-byte) buffer allocated by coda_mem_data_new().
      if (rnx_prod && rnx_prod.get()->mem_ptr != nullptr) {
        std::free(rnx_prod.get()->mem_ptr);
        rnx_prod.get()->mem_ptr = nullptr;
      }
    };

    coda_cursor rnx_cursor{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_set_product(&rnx_cursor, rnx_prod.get()));
    coda_format format = coda_format_ascii;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_format(&rnx_cursor, &format));
    EXPECT_EQ(coda_format_rinex, format);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&rnx_cursor, "/header"));
  }

  // 3. Disabled HDF4 backend branches in coda-cursor.c and
  //    coda_dynamic_type_delete(nullptr / ascii / binary / hdf4).
  {
    coda_dynamic_type_delete(nullptr);

    coda_type_number* asc_type =
        coda_type_number_new(coda_format_ascii, coda_integer_class);
    ASSERT_NE(nullptr, asc_type);
    coda_dynamic_type_delete(reinterpret_cast<coda_dynamic_type*>(asc_type));

    coda_type_number* bin_type =
        coda_type_number_new(coda_format_binary, coda_integer_class);
    ASSERT_NE(nullptr, bin_type);
    coda_dynamic_type_delete(reinterpret_cast<coda_dynamic_type*>(bin_type));

    std::vector<ScopedType> owned_types;
    coda_type_record* rec_def = coda_type_record_new(coda_format_hdf4);
    ASSERT_NE(nullptr, rec_def);
    TrackType(rec_def, owned_types);

    coda_type_array* arr_def = coda_type_array_new(coda_format_hdf4);
    ASSERT_NE(nullptr, arr_def);
    TrackType(arr_def, owned_types);

    coda_type_text* txt_def = coda_type_text_new(coda_format_hdf4);
    ASSERT_NE(nullptr, txt_def);
    TrackType(txt_def, owned_types);

    coda_product hdf4_prod{};
    hdf4_prod.format = coda_format_hdf4;

    coda_cursor cursor{};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_set_product(&cursor, &hdf4_prod));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);

    coda_dynamic_type hdf4_rec_dyn{coda_backend_hdf4,
                                   reinterpret_cast<coda_type*>(rec_def)};
    coda_dynamic_type hdf4_arr_dyn{coda_backend_hdf4,
                                   reinterpret_cast<coda_type*>(arr_def)};
    coda_dynamic_type hdf4_txt_dyn{coda_backend_hdf4,
                                   reinterpret_cast<coda_type*>(txt_def)};

    coda_cursor rec_c = MakeSyntheticHdf4RootCursor(&hdf4_prod, &hdf4_rec_dyn);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_record_field_by_index(&rec_c, 0));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);

    coda_cursor rec_child = PushSyntheticHdf4Child(rec_c, &hdf4_txt_dyn, 0);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_next_record_field(&rec_child));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);

    int avail = 0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_get_record_field_available_status(&rec_c, 0, &avail));
    EXPECT_EQ(1, avail);

    coda_cursor arr_c = MakeSyntheticHdf4RootCursor(&hdf4_prod, &hdf4_arr_dyn);
    const long sub0 = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element(&arr_c, 1, &sub0));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_array_element_by_index(&arr_c, 0));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);

    coda_cursor arr_child = PushSyntheticHdf4Child(arr_c, &hdf4_txt_dyn, 0);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_next_array_element(&arr_child));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);

    int num_dims = 0;
    std::array<long, CODA_MAX_NUM_DIMS> dims{};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_array_dim(&arr_c, &num_dims, dims.data()));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);

    coda_cursor txt_c = MakeSyntheticHdf4RootCursor(&hdf4_prod, &hdf4_txt_dyn);
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_goto_attributes(&txt_c));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);

    long len = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_string_length(&txt_c, &len));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);

    int64_t bits = 0;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_bit_size(&txt_c, &bits));
    EXPECT_EQ(-1, bits);

    long n = 0;
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_get_num_elements(&txt_c, &n));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);

    coda_dynamic_type_delete(&hdf4_rec_dyn);
  }
}

// TODO(schwehr): 94.8% line coverage in libcoda/coda-cursor.c (740/781);
// remaining 41 missed lines are OOM branches, recursive path error propagation,
// and disabled HDF4 backend switch cases.

}  // namespace
}  // namespace stcorp_coda
