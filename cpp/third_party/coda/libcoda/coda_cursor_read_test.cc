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
// Tests CODA cursor scalar, array, partial-array, raw bit/byte, conversion,
// special-type, and complex read APIs across backends
// (libcoda/coda-cursor-read.c).
//
// google3 build caveats:
//   - HAVE_HDF4 is undefined, so coda_backend_hdf4 dispatches return -1 with
//     CODA_ERROR_NO_HDF4_SUPPORT.
//   - HAVE_HDF5 is enabled (1), so HDF5 scalar/array/partial-array reads are
//     exercised directly against synthesized .h5 files.
//   - HAVE_NETCDF_H is undefined, so NetCDF uses CODA's built-in classic
//     parser (coda-netcdf.c).
//   - Upstream coda-netcdf-cursor.c read_partial_array() loops over
//     num_elements rather than length during endian swapping, and
//     coda-cdf-cursor.c read_partial_array() computes
//     record_to_id = (offset + length) / num_values_per_record; tests size
//     buffers and partial ranges accordingly.
//   - Upstream coda-ascii-cursor.c get_bit_size_boundary() checks
//     current_bit_offset + read_bit_size >= bit_size, so synthetic ASCII files
//     include a trailing newline.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-cursor-read.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda.h

#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-mem-internal.h"
#include "third_party/stcorp_coda/libcoda/coda-type.h"
}  // extern "C"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // NOLINT
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
#include "third_party/stcorp_coda/google/util/hdf5_test_util.h"
#include "third_party/stcorp_coda/google/util/netcdf_builder.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"

namespace stcorp_coda {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

constexpr coda_endianness kHostEndianness =
    (std::endian::native == std::endian::big) ? coda_big_endian
                                              : coda_little_endian;

struct TypeDeleter {
  void operator()(coda_type* type) const { coda_type_release(type); }
};
using ScopedType = std::unique_ptr<coda_type, TypeDeleter>;

struct DynamicTypeDeleter {
  void operator()(coda_dynamic_type* type) const {
    coda_dynamic_type_delete(type);
  }
};
using ScopedDynamicType =
    std::unique_ptr<coda_dynamic_type, DynamicTypeDeleter>;

template <typename T>
coda_type* TrackType(T* raw_type, std::vector<ScopedType>& owned_types) {
  auto* base = reinterpret_cast<coda_type*>(raw_type);
  owned_types.emplace_back(base);
  return base;
}

// Allocates a synthetic coda_product using coda_format_sp3 so coda_close()
// invokes coda_sp3_close() (freeing root_type, mem_ptr, and the product
// without requiring an OS file descriptor).
ScopedProduct MakeSyntheticMemProduct() {
  auto* product =
      static_cast<coda_product*>(std::calloc(1, sizeof(coda_product)));
  if (product != nullptr) {
    product->format = coda_format_sp3;
  }
  return ScopedProduct(product);
}

coda_type_number* MakeNumberType(coda_format format, coda_type_class type_class,
                                 coda_native_type read_type, int64_t bit_size,
                                 std::vector<ScopedType>& owned_types) {
  coda_type_number* num = coda_type_number_new(format, type_class);
  if (num == nullptr) {
    return nullptr;
  }
  TrackType(num, owned_types);
  if (coda_type_number_set_endianness(num, kHostEndianness) != CODA_SUCCESS ||
      coda_type_set_read_type(reinterpret_cast<coda_type*>(num), read_type) !=
          CODA_SUCCESS) {
    return nullptr;
  }
  if (bit_size >= 0 && coda_type_set_bit_size(reinterpret_cast<coda_type*>(num),
                                              bit_size) != CODA_SUCCESS) {
    return nullptr;
  }
  return num;
}

coda_type_text* MakeCharType(coda_format format,
                             std::vector<ScopedType>& owned_types) {
  coda_type_text* text = coda_type_text_new(format);
  if (text == nullptr) {
    return nullptr;
  }
  TrackType(text, owned_types);
  if (coda_type_set_read_type(reinterpret_cast<coda_type*>(text),
                              coda_native_type_char) != CODA_SUCCESS ||
      coda_type_set_bit_size(reinterpret_cast<coda_type*>(text), 8) !=
          CODA_SUCCESS) {
    return nullptr;
  }
  return text;
}

coda_type_text* MakeStringType(coda_format format, int64_t byte_size,
                               std::vector<ScopedType>& owned_types) {
  coda_type_text* text = coda_type_text_new(format);
  if (text == nullptr) {
    return nullptr;
  }
  TrackType(text, owned_types);
  if (byte_size >= 0 &&
      coda_type_set_byte_size(reinterpret_cast<coda_type*>(text), byte_size) !=
          CODA_SUCCESS) {
    return nullptr;
  }
  return text;
}

// Byte encoding helpers for synthesizing binary, CDF, and GRIB streams.
void AppendLe16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t v) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
  }
}

void AppendLe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
  }
}

void AppendBe16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) {
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

void AppendBeFloat(std::vector<uint8_t>& out, float v) {
  AppendBe32(out, std::bit_cast<uint32_t>(v));
}

void AppendLeDouble(std::vector<uint8_t>& out, double v) {
  AppendLe64(out, std::bit_cast<uint64_t>(v));
}

void AppendBeDouble(std::vector<uint8_t>& out, double v) {
  AppendBe64(out, std::bit_cast<uint64_t>(v));
}

void AppendString(std::vector<uint8_t>& out, std::string_view s) {
  out.insert(out.end(), s.begin(), s.end());
}

// Helper to build a single-product .codadef archive and initialize CODA with
// it.
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

  bool InitWithXml(std::string_view magic, std::string_view format,
                   std::string_view prod_id, std::string_view product_xml) {
    const std::string index_xml = absl::StrFormat(
        R"(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="CursorReadClass">
  <cd:ProductType name="CursorReadType">
    <cd:ProductDefinition id="%s" format="%s" version="1">
      <cd:DetectionRule>
        <cd:MatchData offset="0">%s</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)",
        prod_id, format, magic);
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

coda_cursor GotoField(const coda_cursor& root, const char* name) {
  coda_cursor c = root;
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_goto_record_field_by_name(&c, name))
      << "field=" << name;
  return c;
}

// Synthesizes a minimal CDF v3.0 uncompressed file (IBMPC little-endian,
// row-major ordering) containing zVariables for every CDF type supported by
// CODA.
struct CdfVarSpec {
  std::string_view name;
  int32_t data_type;
  int32_t num_elems;
  std::vector<int32_t> dims;
  std::vector<uint8_t> data;
  bool missing_record = false;
};

std::vector<uint8_t> BuildCdfBytes(std::span<const CdfVarSpec> vars) {
  std::vector<uint8_t> out;
  // 8-byte CDF3 magic: 0xCDF30001 0x0000FFFF (big-endian).
  AppendBe32(out, 0xCDF30001U);
  AppendBe32(out, 0x0000FFFFU);

  // CDR at offset 8 (56 bytes -> ends at offset 64).
  constexpr int64_t kCdrSize = 56;
  constexpr int64_t kGdrOffset = 8 + kCdrSize;
  AppendBe64(out, kCdrSize);
  AppendBe32(out, 1);           // CDR record_type
  AppendBe64(out, kGdrOffset);  // gdr_offset
  AppendBe32(out, 3);           // cdf_version
  AppendBe32(out, 0);           // cdf_release
  AppendBe32(out, 6);           // encoding = IBMPC (coda_little_endian)
  AppendBe32(out, 3);           // flags = row-major (1) | single-file (2)
  AppendBe64(out, 0);           // rfuA, rfuB
  AppendBe32(out, 0);           // cdf_increment
  AppendBe64(out, 0);           // rfuD, rfuE

  // GDR at offset 64 (64 bytes -> ends at offset 128).
  constexpr int64_t kGdrSize = 64;
  constexpr int64_t kZvdrHead = kGdrOffset + kGdrSize;
  AppendBe64(out, kGdrSize);
  AppendBe32(out, 2);  // GDR record_type
  AppendBe64(out, 0);  // rvdr_head
  AppendBe64(out, vars.empty() ? 0 : kZvdrHead);
  AppendBe64(out, 0);  // adr_head
  const size_t eof_patch_pos = out.size();
  AppendBe64(out, 0);                                   // eof (patched below)
  AppendBe32(out, 0);                                   // nr_vars
  AppendBe32(out, 0);                                   // num_attr
  AppendBe32(out, 0);                                   // r_max_rec
  AppendBe32(out, 0);                                   // rnum_dims
  AppendBe32(out, static_cast<uint32_t>(vars.size()));  // nz_vars

  struct PatchOffsets {
    size_t vxr_head_pos;
    size_t vxr_tail_pos;
  };
  std::vector<PatchOffsets> patches(vars.size());

  // Emit zVDR records sequentially starting at kZvdrHead.
  for (size_t i = 0; i < vars.size(); ++i) {
    const CdfVarSpec& v = vars[i];
    const int64_t vdr_size =
        344 + static_cast<int64_t>(v.dims.size()) * 8;  // sizes + varys
    const int64_t next_vdr =
        (i + 1 < vars.size()) ? static_cast<int64_t>(out.size()) + vdr_size : 0;

    AppendBe64(out, vdr_size);
    AppendBe32(out, 8);  // zVDR record_type
    AppendBe64(out, static_cast<uint64_t>(next_vdr));
    AppendBe32(out, static_cast<uint32_t>(v.data_type));
    AppendBe32(out, 0);  // max_rec = 0 (1 record)
    patches[i].vxr_head_pos = out.size();
    AppendBe64(out, 0);  // vxr_head patched below
    patches[i].vxr_tail_pos = out.size();
    AppendBe64(out, 0);  // vxr_tail patched below
    AppendBe32(out, 0);  // flags = 0 (rec_varys=0, uncompressed)
    AppendBe32(out, 0);  // srecords
    AppendBe32(out, 0);  // rfuB
    AppendBe32(out, 0);  // rfuC
    AppendBe32(out, 0);  // rfuF
    AppendBe32(out, static_cast<uint32_t>(v.num_elems));
    AppendBe32(out, static_cast<uint32_t>(i));            // num
    AppendBe64(out, static_cast<uint64_t>(int64_t{-1}));  // cpr_spr_offset
    AppendBe32(out, 1);                                   // blocking_factor

    std::array<uint8_t, 256> name_buf{};
    const size_t copy_len = std::min(v.name.size(), size_t{255});
    std::memcpy(name_buf.data(), v.name.data(), copy_len);
    out.insert(out.end(), name_buf.begin(), name_buf.end());

    AppendBe32(out, static_cast<uint32_t>(v.dims.size()));
    for (const int32_t d : v.dims) {
      AppendBe32(out, static_cast<uint32_t>(d));
    }
    for (size_t d = 0; d < v.dims.size(); ++d) {
      AppendBe32(out, 1);  // dim_varys = 1
    }
  }

  auto patch_be64 = [&out](size_t pos, uint64_t val) {
    for (int b = 0; b < 8; ++b) {
      out[pos + b] = static_cast<uint8_t>((val >> (56 - 8 * b)) & 0xFF);
    }
  };

  // Emit VXR + VVR for each non-missing variable.
  for (size_t i = 0; i < vars.size(); ++i) {
    const CdfVarSpec& v = vars[i];
    if (v.missing_record) {
      continue;
    }
    const uint64_t vxr_offset = out.size();
    patch_be64(patches[i].vxr_head_pos, vxr_offset);
    patch_be64(patches[i].vxr_tail_pos, vxr_offset);

    constexpr int64_t kVxrSize = 44;
    AppendBe64(out, kVxrSize);
    AppendBe32(out, 6);  // VXR record_type
    AppendBe64(out, 0);  // vxr_next
    AppendBe32(out, 1);  // n_entries
    AppendBe32(out, 1);  // nused_entries
    AppendBe32(out, 0);  // vr_first
    AppendBe32(out, 0);  // vr_last
    const size_t vr_patch_pos = out.size();
    AppendBe64(out, 0);  // vr_offset patched below

    const uint64_t vvr_offset = out.size();
    patch_be64(vr_patch_pos, vvr_offset);
    const int64_t vvr_size = 12 + static_cast<int64_t>(v.data.size());
    AppendBe64(out, static_cast<uint64_t>(vvr_size));
    AppendBe32(out, 7);  // VVR record_type
    out.insert(out.end(), v.data.begin(), v.data.end());
  }

  patch_be64(eof_patch_pos, out.size());
  return out;
}

// Synthesizes a minimal 87-byte GRIB Edition 1 message with a 2x2 grid (Ni=2,
// Nj=2) and 4 simple-packed 8-bit values {1, 2, 3, 4} (scale=0, ref=0.0f).
std::vector<uint8_t> BuildGrib1Bytes(std::array<uint8_t, 4> packed_values) {
  std::vector<uint8_t> out;
  out.reserve(87);

  // Section 0: Indicator Section (8 bytes). Total length = 87.
  AppendString(out, "GRIB");
  out.push_back(0);
  out.push_back(0);
  out.push_back(87);
  out.push_back(1);  // GRIB Edition 1

  // Section 1: Product Definition Section (28 bytes).
  out.push_back(0);
  out.push_back(0);
  out.push_back(28);  // section_size = 28
  for (int i = 3; i < 28; ++i) {
    if (i == 6) {
      out.push_back(255);  // gridDefinition = 255 (defined in GDS)
    } else if (i == 7) {
      out.push_back(0x80);  // flags: has_gds=1, has_bms=0
    } else {
      out.push_back(0);
    }
  }

  // Section 2: Grid Description Section (32 bytes).
  out.push_back(0);
  out.push_back(0);
  out.push_back(32);   // section_size = 32
  out.push_back(0);    // NV = 0
  out.push_back(255);  // PVL = 255 (none)
  out.push_back(0);    // dataRepresentationType = 0 (lat/lon)
  AppendBe16(out, 2);  // Ni = 2
  AppendBe16(out, 2);  // Nj = 2
  for (int i = 10; i < 32; ++i) {
    out.push_back(0);
  }

  // Section 4: Binary Data Section (11 + 4 = 15 bytes).
  out.push_back(0);
  out.push_back(0);
  out.push_back(15);   // section_size = 15
  out.push_back(0);    // flags
  AppendBe16(out, 0);  // binaryScaleFactor = 0
  AppendBe32(out, 0);  // referenceValue = 0.0f (IBM float zero)
  out.push_back(8);    // bitsPerValue = 8
  out.insert(out.end(), packed_values.begin(), packed_values.end());

  // Section 5: End Section (4 bytes).
  AppendString(out, "7777");
  return out;
}

TEST(CodaCursorReadTest, InvalidArgumentsAndNonArrayErrors) {
  CodadefEnv env("coda_cursor_read_invalid_args.codadef");
  constexpr std::string_view kXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="InvalidArgs" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>8</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="val">
      <cd:Integer>
        <cd:BitSize>32</cd:BitSize>
        <cd:NativeType>int32</cd:NativeType>
        <cd:LittleEndian/>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="arr">
      <cd:Array>
        <cd:Dimension>2</cd:Dimension>
        <cd:Integer>
          <cd:BitSize>32</cd:BitSize>
          <cd:NativeType>int32</cd:NativeType>
          <cd:LittleEndian/>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="tm">
      <cd:Time timeformat="binary_envisat_datetime"/>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";
  ASSERT_TRUE(env.InitWithXml("CRINVALI", "binary", "InvalidArgs", kXml));
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  std::vector<uint8_t> bin;
  AppendString(bin, "CRINVALI");
  AppendLe32(bin, 42);
  AppendLe32(bin, 10);
  AppendLe32(bin, 20);
  AppendBe32(bin, 0);
  AppendBe32(bin, 0);
  AppendBe32(bin, 0);

  TempFile dat_file("coda_cursor_read_invalid_args.dat");
  ASSERT_TRUE(dat_file.WriteBytes(bin));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(dat_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
  const coda_cursor int_cur = GotoField(root, "val");
  const coda_cursor arr_cur = GotoField(root, "arr");
  const coda_cursor time_cur = GotoField(root, "tm");
  const coda_cursor empty_cur{};

  // Helper to verify nullptr/uninitialized cursor and nullptr dst for all
  // scalar, array, partial-array, and complex functions.
  auto check_invalid_arg = [](auto fn, auto* dummy_dst) {
    coda_cursor uninit{};
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, fn(nullptr, dummy_dst));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, fn(&uninit, dummy_dst));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
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
  char ch = '\0';
  char str[16]{};
  double pair[2]{};

  // Scalar null-cursor, uninitialized-cursor, and null-dst checks.
  check_invalid_arg(coda_cursor_read_int8, &i8);
  check_invalid_arg(coda_cursor_read_uint8, &u8);
  check_invalid_arg(coda_cursor_read_int16, &i16);
  check_invalid_arg(coda_cursor_read_uint16, &u16);
  check_invalid_arg(coda_cursor_read_int32, &i32);
  check_invalid_arg(coda_cursor_read_uint32, &u32);
  check_invalid_arg(coda_cursor_read_int64, &i64);
  check_invalid_arg(coda_cursor_read_uint64, &u64);
  check_invalid_arg(coda_cursor_read_float, &f32);
  check_invalid_arg(coda_cursor_read_double, &f64);
  check_invalid_arg(coda_cursor_read_char, &ch);
  check_invalid_arg(coda_cursor_read_complex_double_pair, pair);

  EXPECT_EQ(-1, coda_cursor_read_int8(&int_cur, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_uint8(&int_cur, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_int16(&int_cur, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_uint16(&int_cur, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_int32(&int_cur, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_uint32(&int_cur, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_int64(&int_cur, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_uint64(&int_cur, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_float(&int_cur, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_double(&int_cur, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_char(&int_cur, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // read_string invalid arguments and non-text cursor.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_cursor_read_string(&int_cur, nullptr, 8));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_string(&int_cur, str, 0));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_string(&int_cur, str, -1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_string(nullptr, str, sizeof(str)));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_string(&empty_cur, str, sizeof(str)));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_string(&int_cur, str, sizeof(str)));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  // read_bits and read_bytes argument validation and zero-length no-op.
  uint8_t byte_buf[4]{0x5A, 0x5A, 0x5A, 0x5A};
  EXPECT_EQ(-1, coda_cursor_read_bits(nullptr, byte_buf, 0, 8));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_bits(&empty_cur, byte_buf, 0, 8));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_bits(&int_cur, nullptr, 0, 8));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_bits(&int_cur, byte_buf, 0, -1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_bits(&int_cur, byte_buf, 0, 0));
  EXPECT_EQ(0x5A, byte_buf[0]);

  EXPECT_EQ(-1, coda_cursor_read_bytes(nullptr, byte_buf, 0, 1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_bytes(&empty_cur, byte_buf, 0, 1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_bytes(&int_cur, nullptr, 0, 1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_bytes(&int_cur, byte_buf, -1, 1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_bytes(&int_cur, byte_buf, 0, -1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_bytes(&int_cur, byte_buf, 0, 0));
  EXPECT_EQ(0x5A, byte_buf[0]);

  // Array and partial-array invalid cursor, null dst, and non-array cursor.
  auto check_array_errors = [&](auto arr_fn, auto partial_fn, auto* dummy_dst) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, arr_fn(nullptr, dummy_dst, coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
    EXPECT_EQ(-1, arr_fn(&empty_cur, dummy_dst, coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
    EXPECT_EQ(-1, arr_fn(&arr_cur, nullptr, coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
    EXPECT_EQ(-1, arr_fn(&int_cur, dummy_dst, coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, partial_fn(nullptr, 0, 1, dummy_dst));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
    EXPECT_EQ(-1, partial_fn(&empty_cur, 0, 1, dummy_dst));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
    EXPECT_EQ(-1, partial_fn(&arr_cur, 0, 1, nullptr));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
    EXPECT_EQ(-1, partial_fn(&int_cur, 0, 1, dummy_dst));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  };

  int8_t i8_buf[2]{};
  uint8_t u8_buf[2]{};
  int16_t i16_buf[2]{};
  uint16_t u16_buf[2]{};
  int32_t i32_buf[2]{};
  uint32_t u32_buf[2]{};
  int64_t i64_buf[2]{};
  uint64_t u64_buf[2]{};
  float f32_buf[2]{};
  double f64_buf[2]{};
  char ch_buf[2]{};

  check_array_errors(coda_cursor_read_int8_array,
                     coda_cursor_read_int8_partial_array, i8_buf);
  check_array_errors(coda_cursor_read_uint8_array,
                     coda_cursor_read_uint8_partial_array, u8_buf);
  check_array_errors(coda_cursor_read_int16_array,
                     coda_cursor_read_int16_partial_array, i16_buf);
  check_array_errors(coda_cursor_read_uint16_array,
                     coda_cursor_read_uint16_partial_array, u16_buf);
  check_array_errors(coda_cursor_read_int32_array,
                     coda_cursor_read_int32_partial_array, i32_buf);
  check_array_errors(coda_cursor_read_uint32_array,
                     coda_cursor_read_uint32_partial_array, u32_buf);
  check_array_errors(coda_cursor_read_int64_array,
                     coda_cursor_read_int64_partial_array, i64_buf);
  check_array_errors(coda_cursor_read_uint64_array,
                     coda_cursor_read_uint64_partial_array, u64_buf);
  check_array_errors(coda_cursor_read_float_array,
                     coda_cursor_read_float_partial_array, f32_buf);
  check_array_errors(coda_cursor_read_double_array,
                     coda_cursor_read_double_partial_array, f64_buf);
  check_array_errors(coda_cursor_read_char_array,
                     coda_cursor_read_char_partial_array, ch_buf);

  // Complex invalid argument & non-complex / non-array errors.
  EXPECT_EQ(-1, coda_cursor_read_complex_double_pair(&int_cur, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_complex_double_pair(&int_cur, pair));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_complex_double_pair(&time_cur, pair));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  double re = 0.0;
  double im = 0.0;
  EXPECT_EQ(-1, coda_cursor_read_complex_double_split(&int_cur, &re, &im));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  double cplx_pairs[4]{};
  double cplx_re[2]{};
  double cplx_im[2]{};
  EXPECT_EQ(-1, coda_cursor_read_complex_double_pairs_array(
                    nullptr, cplx_pairs, coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_complex_double_pairs_array(
                    &empty_cur, cplx_pairs, coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_complex_double_pairs_array(
                    &arr_cur, nullptr, coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_complex_double_pairs_array(
                    &int_cur, cplx_pairs, coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_complex_double_pairs_array(
                    &arr_cur, cplx_pairs, coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  EXPECT_EQ(-1, coda_cursor_read_complex_double_split_array(
                    nullptr, cplx_re, cplx_im, coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_complex_double_split_array(
                    &empty_cur, cplx_re, cplx_im, coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_complex_double_split_array(
                    &arr_cur, nullptr, cplx_im, coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_complex_double_split_array(
                    &arr_cur, cplx_re, nullptr, coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_complex_double_split_array(
                    &int_cur, cplx_re, cplx_im, coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  // Upstream quirk: read_split_array() in coda-cursor-read.c does not check
  // the return value of read_double_pair(), so on a non-complex array it sets
  // coda_errno to CODA_ERROR_INVALID_TYPE yet still returns 0 (CODA_SUCCESS).
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(0, coda_cursor_read_complex_double_split_array(
                   &arr_cur, cplx_re, cplx_im, coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
}

TEST(CodaCursorReadTest, BinaryScalarsWideningIncompatibleTypesAndTruncated) {
  CodadefEnv env("coda_cursor_read_bin_scalars.codadef");
  constexpr std::string_view kXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinScalars" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text><cd:ByteSize>8</cd:ByteSize></cd:Text>
    </cd:Field>
    <cd:Field name="i8">
      <cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>int8</cd:NativeType></cd:Integer>
    </cd:Field>
    <cd:Field name="u8">
      <cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>uint8</cd:NativeType></cd:Integer>
    </cd:Field>
    <cd:Field name="i16">
      <cd:Integer><cd:BitSize>16</cd:BitSize><cd:NativeType>int16</cd:NativeType><cd:LittleEndian/></cd:Integer>
    </cd:Field>
    <cd:Field name="u16">
      <cd:Integer><cd:BitSize>16</cd:BitSize><cd:NativeType>uint16</cd:NativeType></cd:Integer>
    </cd:Field>
    <cd:Field name="i32">
      <cd:Integer><cd:BitSize>32</cd:BitSize><cd:NativeType>int32</cd:NativeType><cd:LittleEndian/></cd:Integer>
    </cd:Field>
    <cd:Field name="u32">
      <cd:Integer><cd:BitSize>32</cd:BitSize><cd:NativeType>uint32</cd:NativeType></cd:Integer>
    </cd:Field>
    <cd:Field name="i64">
      <cd:Integer><cd:BitSize>64</cd:BitSize><cd:NativeType>int64</cd:NativeType><cd:LittleEndian/></cd:Integer>
    </cd:Field>
    <cd:Field name="u64">
      <cd:Integer><cd:BitSize>64</cd:BitSize><cd:NativeType>uint64</cd:NativeType></cd:Integer>
    </cd:Field>
    <cd:Field name="f32">
      <cd:Float><cd:BitSize>32</cd:BitSize><cd:NativeType>float</cd:NativeType><cd:LittleEndian/></cd:Float>
    </cd:Field>
    <cd:Field name="f64">
      <cd:Float><cd:BitSize>64</cd:BitSize><cd:NativeType>double</cd:NativeType></cd:Float>
    </cd:Field>
    <cd:Field name="ch">
      <cd:Text><cd:ByteSize>1</cd:ByteSize><cd:NativeType>char</cd:NativeType></cd:Text>
    </cd:Field>
    <cd:Field name="str">
      <cd:Text><cd:ByteSize>6</cd:ByteSize></cd:Text>
    </cd:Field>
    <cd:Field name="raw">
      <cd:Raw><cd:BitSize>32</cd:BitSize></cd:Raw>
    </cd:Field>
    <cd:Field name="missing">
      <cd:Available>1 == 0</cd:Available>
      <cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>int8</cd:NativeType></cd:Integer>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";
  ASSERT_TRUE(env.InitWithXml("BINSCALR", "binary", "BinScalars", kXml));
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  std::vector<uint8_t> bin;
  AppendString(bin, "BINSCALR");
  bin.push_back(static_cast<uint8_t>(int8_t{-42}));  // i8
  bin.push_back(uint8_t{200});                       // u8
  AppendLe16(bin, static_cast<uint16_t>(int16_t{-1234}));
  AppendBe16(bin, uint16_t{50000});  // big-endian u16
  AppendLe32(bin, static_cast<uint32_t>(int32_t{-100000}));
  AppendBe32(bin, uint32_t{3000000000U});  // big-endian u32
  AppendLe64(bin, static_cast<uint64_t>(int64_t{-9000000000LL}));
  AppendBe64(bin, uint64_t{10000000000ULL});  // big-endian u64
  AppendLeFloat(bin, 3.25f);
  AppendBeDouble(bin, -12.5);  // big-endian f64
  bin.push_back('Z');          // ch
  AppendString(bin, "coda!");  // 5 chars + NUL = 6 bytes
  bin.push_back('\0');
  bin.insert(bin.end(), {0xAB, 0xCD, 0xEF, 0x12});  // raw

  TempFile dat_file("coda_cursor_read_bin_scalars.dat");
  ASSERT_TRUE(dat_file.WriteBytes(bin));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(dat_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  const coda_cursor c_i8 = GotoField(root, "i8");
  const coda_cursor c_u8 = GotoField(root, "u8");
  const coda_cursor c_i16 = GotoField(root, "i16");
  const coda_cursor c_u16 = GotoField(root, "u16");
  const coda_cursor c_i32 = GotoField(root, "i32");
  const coda_cursor c_u32 = GotoField(root, "u32");
  const coda_cursor c_i64 = GotoField(root, "i64");
  const coda_cursor c_u64 = GotoField(root, "u64");
  const coda_cursor c_f32 = GotoField(root, "f32");
  const coda_cursor c_f64 = GotoField(root, "f64");
  const coda_cursor c_ch = GotoField(root, "ch");
  const coda_cursor c_str = GotoField(root, "str");
  const coda_cursor c_raw = GotoField(root, "raw");
  const coda_cursor c_miss = GotoField(root, "missing");

  // Exact reads and all widening paths.
  int8_t v_i8 = 0;
  uint8_t v_u8 = 0;
  int16_t v_i16 = 0;
  uint16_t v_u16 = 0;
  int32_t v_i32 = 0;
  uint32_t v_u32 = 0;
  int64_t v_i64 = 0;
  uint64_t v_u64 = 0;
  float v_f32 = 0.0f;
  double v_f64 = 0.0;
  char v_ch = '\0';

  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&c_i8, &v_i8));
  EXPECT_EQ(-42, v_i8);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&c_u8, &v_u8));
  EXPECT_EQ(200, v_u8);

  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&c_i8, &v_i16));
  EXPECT_EQ(-42, v_i16);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&c_u8, &v_i16));
  EXPECT_EQ(200, v_i16);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&c_i16, &v_i16));
  EXPECT_EQ(-1234, v_i16);

  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&c_u8, &v_u16));
  EXPECT_EQ(200, v_u16);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&c_u16, &v_u16));
  EXPECT_EQ(50000, v_u16);

  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c_i8, &v_i32));
  EXPECT_EQ(-42, v_i32);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c_u8, &v_i32));
  EXPECT_EQ(200, v_i32);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c_i16, &v_i32));
  EXPECT_EQ(-1234, v_i32);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c_u16, &v_i32));
  EXPECT_EQ(50000, v_i32);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&c_i32, &v_i32));
  EXPECT_EQ(-100000, v_i32);

  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_uint32(&c_u8, &v_u32));
  EXPECT_EQ(200U, v_u32);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_uint32(&c_u16, &v_u32));
  EXPECT_EQ(50000U, v_u32);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_uint32(&c_u32, &v_u32));
  EXPECT_EQ(3000000000U, v_u32);

  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c_i8, &v_i64));
  EXPECT_EQ(-42, v_i64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c_u8, &v_i64));
  EXPECT_EQ(200, v_i64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c_i16, &v_i64));
  EXPECT_EQ(-1234, v_i64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c_u16, &v_i64));
  EXPECT_EQ(50000, v_i64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c_i32, &v_i64));
  EXPECT_EQ(-100000, v_i64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c_u32, &v_i64));
  EXPECT_EQ(3000000000LL, v_i64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&c_i64, &v_i64));
  EXPECT_EQ(-9000000000LL, v_i64);

  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_uint64(&c_u8, &v_u64));
  EXPECT_EQ(200ULL, v_u64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_uint64(&c_u16, &v_u64));
  EXPECT_EQ(50000ULL, v_u64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_uint64(&c_u32, &v_u64));
  EXPECT_EQ(3000000000ULL, v_u64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_uint64(&c_u64, &v_u64));
  EXPECT_EQ(10000000000ULL, v_u64);

  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c_i8, &v_f32));
  EXPECT_FLOAT_EQ(-42.0f, v_f32);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c_u8, &v_f32));
  EXPECT_FLOAT_EQ(200.0f, v_f32);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c_i16, &v_f32));
  EXPECT_FLOAT_EQ(-1234.0f, v_f32);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c_u16, &v_f32));
  EXPECT_FLOAT_EQ(50000.0f, v_f32);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c_i32, &v_f32));
  EXPECT_FLOAT_EQ(-100000.0f, v_f32);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c_u32, &v_f32));
  EXPECT_FLOAT_EQ(3000000000.0f, v_f32);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c_i64, &v_f32));
  EXPECT_FLOAT_EQ(-9000000000.0f, v_f32);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c_u64, &v_f32));
  EXPECT_FLOAT_EQ(10000000000.0f, v_f32);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c_f32, &v_f32));
  EXPECT_FLOAT_EQ(3.25f, v_f32);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c_f64, &v_f32));
  EXPECT_FLOAT_EQ(-12.5f, v_f32);

  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_i8, &v_f64));
  EXPECT_DOUBLE_EQ(-42.0, v_f64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_u8, &v_f64));
  EXPECT_DOUBLE_EQ(200.0, v_f64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_i16, &v_f64));
  EXPECT_DOUBLE_EQ(-1234.0, v_f64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_u16, &v_f64));
  EXPECT_DOUBLE_EQ(50000.0, v_f64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_i32, &v_f64));
  EXPECT_DOUBLE_EQ(-100000.0, v_f64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_u32, &v_f64));
  EXPECT_DOUBLE_EQ(3000000000.0, v_f64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_i64, &v_f64));
  EXPECT_DOUBLE_EQ(-9000000000.0, v_f64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_u64, &v_f64));
  EXPECT_DOUBLE_EQ(10000000000.0, v_f64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_f32, &v_f64));
  EXPECT_DOUBLE_EQ(3.25, v_f64);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_f64, &v_f64));
  EXPECT_DOUBLE_EQ(-12.5, v_f64);

  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_char(&c_ch, &v_ch));
  EXPECT_EQ('Z', v_ch);

  char str_buf[8]{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&c_str, str_buf, sizeof(str_buf)));
  EXPECT_STREQ("coda!", str_buf);

  std::array<uint8_t, 2> raw_bits{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_bits(&c_raw, raw_bits.data(), 4, 12));
  EXPECT_THAT(raw_bits, ElementsAre(0x0B, 0xCD));

  std::array<uint8_t, 3> raw_bytes{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_bytes(&c_raw, raw_bytes.data(), 1, 3));
  EXPECT_THAT(raw_bytes, ElementsAre(0xCD, 0xEF, 0x12));

  // Incompatible read_type (including coda_special_no_data) for every scalar.
  for (const coda_cursor* bad : {&c_str, &c_miss}) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_cursor_read_int8(bad, &v_i8));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
    EXPECT_EQ(-1, coda_cursor_read_uint8(bad, &v_u8));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
    EXPECT_EQ(-1, coda_cursor_read_int16(bad, &v_i16));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
    EXPECT_EQ(-1, coda_cursor_read_uint16(bad, &v_u16));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
    EXPECT_EQ(-1, coda_cursor_read_int32(bad, &v_i32));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
    EXPECT_EQ(-1, coda_cursor_read_uint32(bad, &v_u32));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
    EXPECT_EQ(-1, coda_cursor_read_int64(bad, &v_i64));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
    EXPECT_EQ(-1, coda_cursor_read_uint64(bad, &v_u64));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
    EXPECT_EQ(-1, coda_cursor_read_float(bad, &v_f32));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
    EXPECT_EQ(-1, coda_cursor_read_double(bad, &v_f64));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
    EXPECT_EQ(-1, coda_cursor_read_char(bad, &v_ch));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  }

  // Truncated binary file: only 8-byte magic, so every field read hits EOF.
  TempFile trunc_file("coda_cursor_read_bin_scalars_trunc.dat");
  const std::vector<uint8_t> trunc_bytes(bin.begin(), bin.begin() + 8);
  ASSERT_TRUE(trunc_file.WriteBytes(trunc_bytes));
  ScopedProduct trunc_prod;
  ASSERT_EQ(CODA_SUCCESS, coda_open(trunc_file.c_str(), trunc_prod.OutParam()));
  coda_cursor trunc_root;
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_set_product(&trunc_root, trunc_prod.get()));

  const coda_cursor t_i8 = GotoField(trunc_root, "i8");
  const coda_cursor t_u8 = GotoField(trunc_root, "u8");
  const coda_cursor t_i16 = GotoField(trunc_root, "i16");
  const coda_cursor t_u16 = GotoField(trunc_root, "u16");
  const coda_cursor t_i32 = GotoField(trunc_root, "i32");
  const coda_cursor t_u32 = GotoField(trunc_root, "u32");
  const coda_cursor t_i64 = GotoField(trunc_root, "i64");
  const coda_cursor t_u64 = GotoField(trunc_root, "u64");
  const coda_cursor t_f32 = GotoField(trunc_root, "f32");
  const coda_cursor t_f64 = GotoField(trunc_root, "f64");
  const coda_cursor t_ch = GotoField(trunc_root, "ch");

  EXPECT_EQ(-1, coda_cursor_read_int8(&t_i8, &v_i8));
  EXPECT_EQ(CODA_ERROR_OUT_OF_BOUNDS_READ, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_uint8(&t_u8, &v_u8));
  EXPECT_EQ(-1, coda_cursor_read_int16(&t_i8, &v_i16));
  EXPECT_EQ(-1, coda_cursor_read_int16(&t_u8, &v_i16));
  EXPECT_EQ(-1, coda_cursor_read_int16(&t_i16, &v_i16));
  EXPECT_EQ(-1, coda_cursor_read_uint16(&t_u8, &v_u16));
  EXPECT_EQ(-1, coda_cursor_read_uint16(&t_u16, &v_u16));
  EXPECT_EQ(-1, coda_cursor_read_int32(&t_i8, &v_i32));
  EXPECT_EQ(-1, coda_cursor_read_int32(&t_u8, &v_i32));
  EXPECT_EQ(-1, coda_cursor_read_int32(&t_i16, &v_i32));
  EXPECT_EQ(-1, coda_cursor_read_int32(&t_u16, &v_i32));
  EXPECT_EQ(-1, coda_cursor_read_int32(&t_i32, &v_i32));
  EXPECT_EQ(-1, coda_cursor_read_uint32(&t_u8, &v_u32));
  EXPECT_EQ(-1, coda_cursor_read_uint32(&t_u16, &v_u32));
  EXPECT_EQ(-1, coda_cursor_read_uint32(&t_u32, &v_u32));
  EXPECT_EQ(-1, coda_cursor_read_int64(&t_i8, &v_i64));
  EXPECT_EQ(-1, coda_cursor_read_int64(&t_u8, &v_i64));
  EXPECT_EQ(-1, coda_cursor_read_int64(&t_i16, &v_i64));
  EXPECT_EQ(-1, coda_cursor_read_int64(&t_u16, &v_i64));
  EXPECT_EQ(-1, coda_cursor_read_int64(&t_i32, &v_i64));
  EXPECT_EQ(-1, coda_cursor_read_int64(&t_u32, &v_i64));
  EXPECT_EQ(-1, coda_cursor_read_int64(&t_i64, &v_i64));
  EXPECT_EQ(-1, coda_cursor_read_uint64(&t_u8, &v_u64));
  EXPECT_EQ(-1, coda_cursor_read_uint64(&t_u16, &v_u64));
  EXPECT_EQ(-1, coda_cursor_read_uint64(&t_u32, &v_u64));
  EXPECT_EQ(-1, coda_cursor_read_uint64(&t_u64, &v_u64));
  EXPECT_EQ(-1, coda_cursor_read_float(&t_i8, &v_f32));
  EXPECT_EQ(-1, coda_cursor_read_float(&t_u8, &v_f32));
  EXPECT_EQ(-1, coda_cursor_read_float(&t_i16, &v_f32));
  EXPECT_EQ(-1, coda_cursor_read_float(&t_u16, &v_f32));
  EXPECT_EQ(-1, coda_cursor_read_float(&t_i32, &v_f32));
  EXPECT_EQ(-1, coda_cursor_read_float(&t_u32, &v_f32));
  EXPECT_EQ(-1, coda_cursor_read_float(&t_i64, &v_f32));
  EXPECT_EQ(-1, coda_cursor_read_float(&t_u64, &v_f32));
  EXPECT_EQ(-1, coda_cursor_read_float(&t_f32, &v_f32));
  EXPECT_EQ(-1, coda_cursor_read_float(&t_f64, &v_f32));
  EXPECT_EQ(-1, coda_cursor_read_double(&t_i8, &v_f64));
  EXPECT_EQ(-1, coda_cursor_read_double(&t_u8, &v_f64));
  EXPECT_EQ(-1, coda_cursor_read_double(&t_i16, &v_f64));
  EXPECT_EQ(-1, coda_cursor_read_double(&t_u16, &v_f64));
  EXPECT_EQ(-1, coda_cursor_read_double(&t_i32, &v_f64));
  EXPECT_EQ(-1, coda_cursor_read_double(&t_u32, &v_f64));
  EXPECT_EQ(-1, coda_cursor_read_double(&t_i64, &v_f64));
  EXPECT_EQ(-1, coda_cursor_read_double(&t_u64, &v_f64));
  EXPECT_EQ(-1, coda_cursor_read_double(&t_f32, &v_f64));
  EXPECT_EQ(-1, coda_cursor_read_double(&t_f64, &v_f64));
  EXPECT_EQ(-1, coda_cursor_read_char(&t_ch, &v_ch));
}

TEST(CodaCursorReadTest, BinaryArraysAndPartialArraysAllTypesAndBounds) {
  CodadefEnv env("coda_cursor_read_bin_arrays.codadef");
  constexpr std::string_view kXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="BinArrays" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text><cd:ByteSize>8</cd:ByteSize></cd:Text>
    </cd:Field>
    <cd:Field name="i8">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension>
        <cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>int8</cd:NativeType></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="u8">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension>
        <cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>uint8</cd:NativeType></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="i16">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension>
        <cd:Integer><cd:BitSize>16</cd:BitSize><cd:NativeType>int16</cd:NativeType><cd:LittleEndian/></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="u16">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension>
        <cd:Integer><cd:BitSize>16</cd:BitSize><cd:NativeType>uint16</cd:NativeType><cd:LittleEndian/></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="i32">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension>
        <cd:Integer><cd:BitSize>32</cd:BitSize><cd:NativeType>int32</cd:NativeType><cd:LittleEndian/></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="u32">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension>
        <cd:Integer><cd:BitSize>32</cd:BitSize><cd:NativeType>uint32</cd:NativeType><cd:LittleEndian/></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="i64">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension>
        <cd:Integer><cd:BitSize>64</cd:BitSize><cd:NativeType>int64</cd:NativeType><cd:LittleEndian/></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="u64">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension>
        <cd:Integer><cd:BitSize>64</cd:BitSize><cd:NativeType>uint64</cd:NativeType><cd:LittleEndian/></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="f32">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension>
        <cd:Float><cd:BitSize>32</cd:BitSize><cd:NativeType>float</cd:NativeType><cd:LittleEndian/></cd:Float>
      </cd:Array>
    </cd:Field>
    <cd:Field name="f64">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension>
        <cd:Float><cd:BitSize>64</cd:BitSize><cd:NativeType>double</cd:NativeType><cd:LittleEndian/></cd:Float>
      </cd:Array>
    </cd:Field>
    <cd:Field name="ch">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>3</cd:Dimension>
        <cd:Text><cd:ByteSize>1</cd:ByteSize><cd:NativeType>char</cd:NativeType></cd:Text>
      </cd:Array>
    </cd:Field>
    <cd:Field name="str_arr">
      <cd:Array><cd:Dimension>2</cd:Dimension>
        <cd:Text><cd:ByteSize>4</cd:ByteSize></cd:Text>
      </cd:Array>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";
  ASSERT_TRUE(env.InitWithXml("BINARRAY", "binary", "BinArrays", kXml));
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  constexpr std::array<int8_t, 6> kI8 = {-1, 2, -3, 4, -5, 6};
  constexpr std::array<uint8_t, 6> kU8 = {10, 20, 30, 40, 50, 60};
  constexpr std::array<int16_t, 6> kI16 = {-100, 200, -300, 400, -500, 600};
  constexpr std::array<uint16_t, 6> kU16 = {1000, 2000, 3000, 4000, 5000, 6000};
  constexpr std::array<int32_t, 6> kI32 = {-10, 20, -30, 40, -50, 60};
  constexpr std::array<uint32_t, 6> kU32 = {100, 200, 300, 400, 500, 600};
  constexpr std::array<int64_t, 6> kI64 = {-1000, 2000,  -3000,
                                           4000,  -5000, 6000};
  constexpr std::array<uint64_t, 6> kU64 = {11, 22, 33, 44, 55, 66};
  constexpr std::array<float, 6> kF32 = {1.5f, -2.5f, 3.5f, -4.5f, 5.5f, -6.5f};
  constexpr std::array<double, 6> kF64 = {10.25,  -20.25, 30.25,
                                          -40.25, 50.25,  -60.25};
  constexpr std::array<char, 6> kCh = {'A', 'B', 'C', 'D', 'E', 'F'};

  std::vector<uint8_t> bin;
  AppendString(bin, "BINARRAY");
  for (int8_t v : kI8) bin.push_back(static_cast<uint8_t>(v));
  for (uint8_t v : kU8) bin.push_back(v);
  for (int16_t v : kI16) AppendLe16(bin, static_cast<uint16_t>(v));
  for (uint16_t v : kU16) AppendLe16(bin, v);
  for (int32_t v : kI32) AppendLe32(bin, static_cast<uint32_t>(v));
  for (uint32_t v : kU32) AppendLe32(bin, v);
  for (int64_t v : kI64) AppendLe64(bin, static_cast<uint64_t>(v));
  for (uint64_t v : kU64) AppendLe64(bin, v);
  for (float v : kF32) AppendLeFloat(bin, v);
  for (double v : kF64) AppendLeDouble(bin, v);
  for (char v : kCh) bin.push_back(static_cast<uint8_t>(v));
  AppendString(bin, "abcd");
  AppendString(bin, "efgh");

  TempFile dat_file("coda_cursor_read_bin_arrays.dat");
  ASSERT_TRUE(dat_file.WriteBytes(bin));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(dat_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  const coda_cursor c_i8 = GotoField(root, "i8");
  const coda_cursor c_u8 = GotoField(root, "u8");
  const coda_cursor c_i16 = GotoField(root, "i16");
  const coda_cursor c_u16 = GotoField(root, "u16");
  const coda_cursor c_i32 = GotoField(root, "i32");
  const coda_cursor c_u32 = GotoField(root, "u32");
  const coda_cursor c_i64 = GotoField(root, "i64");
  const coda_cursor c_u64 = GotoField(root, "u64");
  const coda_cursor c_f32 = GotoField(root, "f32");
  const coda_cursor c_f64 = GotoField(root, "f64");
  const coda_cursor c_ch = GotoField(root, "ch");
  const coda_cursor c_str_arr = GotoField(root, "str_arr");

  // Fortran ordering of 2x3 row-major {a,b,c, d,e,f} -> {a,d, b,e, c,f}.
  auto to_fortran_2x3 = [](const auto& src) {
    using T = typename std::decay_t<decltype(src)>::value_type;
    return std::array<T, 6>{src[0], src[3], src[1], src[4], src[2], src[5]};
  };

  auto check_exact_array = [&](const coda_cursor& c, auto arr_fn,
                               auto partial_fn, const auto& expected,
                               auto sentinel) {
    using T = typename std::decay_t<decltype(expected)>::value_type;
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

    // Boundary checks enabled: negative offset, offset >= N, offset+len > N.
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, partial_fn(&c, -1, 1, pbuf.data()));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
    EXPECT_EQ(-1, partial_fn(&c, 6, 1, pbuf.data()));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);
    EXPECT_EQ(-1, partial_fn(&c, 4, 3, pbuf.data()));
    EXPECT_EQ(CODA_ERROR_ARRAY_OUT_OF_BOUNDS, coda_errno);

    // Incompatible base type (string array).
    EXPECT_EQ(-1, arr_fn(&c_str_arr, buf.data(), coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
    EXPECT_EQ(-1, partial_fn(&c_str_arr, 0, 1, pbuf.data()));
    EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  };

  check_exact_array(c_i8, coda_cursor_read_int8_array,
                    coda_cursor_read_int8_partial_array, kI8, int8_t{-99});
  check_exact_array(c_u8, coda_cursor_read_uint8_array,
                    coda_cursor_read_uint8_partial_array, kU8, uint8_t{99});
  check_exact_array(c_i16, coda_cursor_read_int16_array,
                    coda_cursor_read_int16_partial_array, kI16, int16_t{-999});
  check_exact_array(c_u16, coda_cursor_read_uint16_array,
                    coda_cursor_read_uint16_partial_array, kU16, uint16_t{999});
  check_exact_array(c_i32, coda_cursor_read_int32_array,
                    coda_cursor_read_int32_partial_array, kI32, int32_t{-999});
  check_exact_array(c_u32, coda_cursor_read_uint32_array,
                    coda_cursor_read_uint32_partial_array, kU32, uint32_t{999});
  check_exact_array(c_i64, coda_cursor_read_int64_array,
                    coda_cursor_read_int64_partial_array, kI64, int64_t{-999});
  check_exact_array(c_u64, coda_cursor_read_uint64_array,
                    coda_cursor_read_uint64_partial_array, kU64, uint64_t{999});
  check_exact_array(c_f32, coda_cursor_read_float_array,
                    coda_cursor_read_float_partial_array, kF32, -999.0f);
  check_exact_array(c_f64, coda_cursor_read_double_array,
                    coda_cursor_read_double_partial_array, kF64, -999.0);
  check_exact_array(c_ch, coda_cursor_read_char_array,
                    coda_cursor_read_char_partial_array, kCh, '?');

  // Exercise all widening paths across array and partial_array readers.
  auto expect_widen_i16 = [&](const coda_cursor& c, const auto& src) {
    std::vector<int16_t> full(6, -999);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16_array(
                                &c, full.data(), coda_array_ordering_c));
    for (size_t i = 0; i < 6; ++i)
      EXPECT_EQ(static_cast<int16_t>(src[i]), full[i]);
    std::vector<int16_t> part(3, -999);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int16_partial_array(&c, 2, 3, part.data()));
    for (size_t i = 0; i < 3; ++i)
      EXPECT_EQ(static_cast<int16_t>(src[i + 2]), part[i]);
  };
  expect_widen_i16(c_i8, kI8);
  expect_widen_i16(c_u8, kU8);

  {
    std::vector<uint16_t> full(6, 999);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16_array(
                                &c_u8, full.data(), coda_array_ordering_c));
    EXPECT_THAT(full, ElementsAre(10, 20, 30, 40, 50, 60));
    std::vector<uint16_t> part(3, 999);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_uint16_partial_array(&c_u8, 2, 3, part.data()));
    EXPECT_THAT(part, ElementsAre(30, 40, 50));
  }

  auto expect_widen_i32 = [&](const coda_cursor& c, const auto& src) {
    std::vector<int32_t> full(6, -999);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32_array(
                                &c, full.data(), coda_array_ordering_c));
    for (size_t i = 0; i < 6; ++i)
      EXPECT_EQ(static_cast<int32_t>(src[i]), full[i]);
    std::vector<int32_t> part(3, -999);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int32_partial_array(&c, 2, 3, part.data()));
    for (size_t i = 0; i < 3; ++i)
      EXPECT_EQ(static_cast<int32_t>(src[i + 2]), part[i]);
  };
  expect_widen_i32(c_i8, kI8);
  expect_widen_i32(c_u8, kU8);
  expect_widen_i32(c_i16, kI16);
  expect_widen_i32(c_u16, kU16);

  auto expect_widen_u32 = [&](const coda_cursor& c, const auto& src) {
    std::vector<uint32_t> full(6, 999);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint32_array(
                                &c, full.data(), coda_array_ordering_c));
    for (size_t i = 0; i < 6; ++i)
      EXPECT_EQ(static_cast<uint32_t>(src[i]), full[i]);
    std::vector<uint32_t> part(3, 999);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_uint32_partial_array(&c, 2, 3, part.data()));
    for (size_t i = 0; i < 3; ++i)
      EXPECT_EQ(static_cast<uint32_t>(src[i + 2]), part[i]);
  };
  expect_widen_u32(c_u8, kU8);
  expect_widen_u32(c_u16, kU16);

  auto expect_widen_i64 = [&](const coda_cursor& c, const auto& src) {
    std::vector<int64_t> full(6, -999);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64_array(
                                &c, full.data(), coda_array_ordering_c));
    for (size_t i = 0; i < 6; ++i)
      EXPECT_EQ(static_cast<int64_t>(src[i]), full[i]);
    std::vector<int64_t> part(3, -999);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_int64_partial_array(&c, 2, 3, part.data()));
    for (size_t i = 0; i < 3; ++i)
      EXPECT_EQ(static_cast<int64_t>(src[i + 2]), part[i]);
  };
  expect_widen_i64(c_i8, kI8);
  expect_widen_i64(c_u8, kU8);
  expect_widen_i64(c_i16, kI16);
  expect_widen_i64(c_u16, kU16);
  expect_widen_i64(c_i32, kI32);
  expect_widen_i64(c_u32, kU32);

  auto expect_widen_u64 = [&](const coda_cursor& c, const auto& src) {
    std::vector<uint64_t> full(6, 999);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint64_array(
                                &c, full.data(), coda_array_ordering_c));
    for (size_t i = 0; i < 6; ++i)
      EXPECT_EQ(static_cast<uint64_t>(src[i]), full[i]);
    std::vector<uint64_t> part(3, 999);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_uint64_partial_array(&c, 2, 3, part.data()));
    for (size_t i = 0; i < 3; ++i)
      EXPECT_EQ(static_cast<uint64_t>(src[i + 2]), part[i]);
  };
  expect_widen_u64(c_u8, kU8);
  expect_widen_u64(c_u16, kU16);
  expect_widen_u64(c_u32, kU32);

  auto expect_widen_f32 = [&](const coda_cursor& c, const auto& src) {
    std::vector<float> full(6, -999.0f);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(
                                &c, full.data(), coda_array_ordering_c));
    for (size_t i = 0; i < 6; ++i)
      EXPECT_FLOAT_EQ(static_cast<float>(src[i]), full[i]);
    std::vector<float> part(3, -999.0f);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_float_partial_array(&c, 2, 3, part.data()));
    for (size_t i = 0; i < 3; ++i)
      EXPECT_FLOAT_EQ(static_cast<float>(src[i + 2]), part[i]);
  };
  expect_widen_f32(c_i8, kI8);
  expect_widen_f32(c_u8, kU8);
  expect_widen_f32(c_i16, kI16);
  expect_widen_f32(c_u16, kU16);
  expect_widen_f32(c_i32, kI32);
  expect_widen_f32(c_u32, kU32);
  expect_widen_f32(c_i64, kI64);
  expect_widen_f32(c_u64, kU64);
  expect_widen_f32(c_f64, kF64);

  auto expect_widen_f64 = [&](const coda_cursor& c, const auto& src) {
    std::vector<double> full(6, -999.0);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(
                                &c, full.data(), coda_array_ordering_c));
    for (size_t i = 0; i < 6; ++i)
      EXPECT_DOUBLE_EQ(static_cast<double>(src[i]), full[i]);
    std::vector<double> part(3, -999.0);
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_double_partial_array(&c, 2, 3, part.data()));
    for (size_t i = 0; i < 3; ++i)
      EXPECT_DOUBLE_EQ(static_cast<double>(src[i + 2]), part[i]);
  };
  expect_widen_f64(c_i8, kI8);
  expect_widen_f64(c_u8, kU8);
  expect_widen_f64(c_i16, kI16);
  expect_widen_f64(c_u16, kU16);
  expect_widen_f64(c_i32, kI32);
  expect_widen_f64(c_u32, kU32);
  expect_widen_f64(c_i64, kI64);
  expect_widen_f64(c_u64, kU64);
  expect_widen_f64(c_f32, kF32);

  // Verify disabling boundary checks skips the partial-array range check.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));
  std::vector<int32_t> zero_len_buf(1, -999);
  EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_int32_partial_array(
                              &c_i32, 0, 0, zero_len_buf.data()));
  EXPECT_EQ(-999, zero_len_buf[0]);
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(1));
}

TEST(CodaCursorReadTest, ConversionsAndPerformConversionsOption) {
  CodadefEnv env("coda_cursor_read_conversions.codadef");
  constexpr std::string_view kXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="ConvProd" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text><cd:ByteSize>8</cd:ByteSize></cd:Text>
    </cd:Field>
    <cd:Field name="conv_valid">
      <cd:Integer>
        <cd:BitSize>16</cd:BitSize>
        <cd:NativeType>int16</cd:NativeType>
        <cd:LittleEndian/>
        <cd:Conversion numerator="2" denominator="4" offset="10" invalid="-99">
          <cd:Unit>m</cd:Unit>
        </cd:Conversion>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="conv_invalid">
      <cd:Integer>
        <cd:BitSize>16</cd:BitSize>
        <cd:NativeType>int16</cd:NativeType>
        <cd:LittleEndian/>
        <cd:Conversion numerator="2" denominator="4" offset="10" invalid="-99">
          <cd:Unit>m</cd:Unit>
        </cd:Conversion>
      </cd:Integer>
    </cd:Field>
    <cd:Field name="conv_arr">
      <cd:Array><cd:Dimension>4</cd:Dimension>
        <cd:Integer>
          <cd:BitSize>16</cd:BitSize>
          <cd:NativeType>int16</cd:NativeType>
          <cd:LittleEndian/>
          <cd:Conversion numerator="2" denominator="4" offset="10" invalid="-99">
            <cd:Unit>m</cd:Unit>
          </cd:Conversion>
        </cd:Integer>
      </cd:Array>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";
  ASSERT_TRUE(env.InitWithXml("CONVPROD", "binary", "ConvProd", kXml));
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  std::vector<uint8_t> bin;
  AppendString(bin, "CONVPROD");
  AppendLe16(bin, static_cast<uint16_t>(int16_t{10}));   // 10 * 2/4 + 10 = 15
  AppendLe16(bin, static_cast<uint16_t>(int16_t{-99}));  // invalid -> NaN
  for (int16_t v : {int16_t{10}, int16_t{-99}, int16_t{20}, int16_t{-4}}) {
    AppendLe16(bin, static_cast<uint16_t>(v));
  }

  TempFile dat_file("coda_cursor_read_conversions.dat");
  ASSERT_TRUE(dat_file.WriteBytes(bin));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(dat_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));
  const coda_cursor c_val = GotoField(root, "conv_valid");
  const coda_cursor c_inv = GotoField(root, "conv_invalid");
  const coda_cursor c_arr = GotoField(root, "conv_arr");

  // 1. With perform_conversions == 1, integer reads fail with INVALID_TYPE,
  //    while double/float reads apply scale, offset, and invalid->NaN.
  ASSERT_EQ(1, coda_get_option_perform_conversions());
  int16_t raw_i16 = 0;
  EXPECT_EQ(-1, coda_cursor_read_int16(&c_val, &raw_i16));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  std::array<int16_t, 4> raw_arr{};
  EXPECT_EQ(-1, coda_cursor_read_int16_array(&c_arr, raw_arr.data(),
                                             coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  EXPECT_EQ(-1,
            coda_cursor_read_int16_partial_array(&c_arr, 0, 2, raw_arr.data()));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);

  double d = 0.0;
  float f = 0.0f;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_val, &d));
  EXPECT_DOUBLE_EQ(15.0, d);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c_val, &f));
  EXPECT_FLOAT_EQ(15.0f, f);

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_inv, &d));
  EXPECT_TRUE(std::isnan(d));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c_inv, &f));
  EXPECT_TRUE(std::isnan(f));

  std::array<double, 4> d_arr{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(&c_arr, d_arr.data(),
                                                        coda_array_ordering_c));
  EXPECT_DOUBLE_EQ(15.0, d_arr[0]);
  EXPECT_TRUE(std::isnan(d_arr[1]));
  EXPECT_DOUBLE_EQ(20.0, d_arr[2]);
  EXPECT_DOUBLE_EQ(8.0, d_arr[3]);

  std::array<float, 4> f_arr{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(&c_arr, f_arr.data(),
                                                       coda_array_ordering_c));
  EXPECT_FLOAT_EQ(15.0f, f_arr[0]);
  EXPECT_TRUE(std::isnan(f_arr[1]));
  EXPECT_FLOAT_EQ(20.0f, f_arr[2]);
  EXPECT_FLOAT_EQ(8.0f, f_arr[3]);

  std::array<double, 2> d_part{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_double_partial_array(&c_arr, 1, 2, d_part.data()));
  EXPECT_TRUE(std::isnan(d_part[0]));
  EXPECT_DOUBLE_EQ(20.0, d_part[1]);

  std::array<float, 2> f_part{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_float_partial_array(&c_arr, 1, 2, f_part.data()));
  EXPECT_TRUE(std::isnan(f_part[0]));
  EXPECT_FLOAT_EQ(20.0f, f_part[1]);

  // 2. With perform_conversions == 0, raw values are returned unconverted.
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_conversions(0));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&c_val, &raw_i16));
  EXPECT_EQ(10, raw_i16);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_inv, &d));
  EXPECT_DOUBLE_EQ(-99.0, d);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16_array(&c_arr, raw_arr.data(),
                                                       coda_array_ordering_c));
  EXPECT_THAT(raw_arr, ElementsAre(10, -99, 20, -4));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(&c_arr, d_arr.data(),
                                                        coda_array_ordering_c));
  EXPECT_THAT(d_arr, ElementsAre(10.0, -99.0, 20.0, -4.0));
}

TEST(CodaCursorReadTest, SpecialTypesTimeVsfIntegerAndComplex) {
  CodadefEnv env("coda_cursor_read_special.codadef");
  constexpr std::string_view kXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="SpecialProd" format="binary">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text><cd:ByteSize>8</cd:ByteSize></cd:Text>
    </cd:Field>
    <cd:Field name="t_scalar">
      <cd:Time timeformat="binary_envisat_datetime"/>
    </cd:Field>
    <cd:Field name="t_arr">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>2</cd:Dimension>
        <cd:Time timeformat="binary_envisat_datetime"/>
      </cd:Array>
    </cd:Field>
    <cd:Field name="vsf_pos">
      <cd:VSFInteger>
        <cd:ScaleFactor>
          <cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>int8</cd:NativeType></cd:Integer>
        </cd:ScaleFactor>
        <cd:Integer><cd:BitSize>16</cd:BitSize><cd:NativeType>int16</cd:NativeType><cd:LittleEndian/></cd:Integer>
      </cd:VSFInteger>
    </cd:Field>
    <cd:Field name="vsf_neg">
      <cd:VSFInteger>
        <cd:ScaleFactor>
          <cd:Integer><cd:BitSize>8</cd:BitSize><cd:NativeType>int8</cd:NativeType></cd:Integer>
        </cd:ScaleFactor>
        <cd:Integer><cd:BitSize>16</cd:BitSize><cd:NativeType>int16</cd:NativeType><cd:LittleEndian/></cd:Integer>
      </cd:VSFInteger>
    </cd:Field>
    <cd:Field name="cplx_f64">
      <cd:Complex>
        <cd:Float><cd:BitSize>64</cd:BitSize><cd:NativeType>double</cd:NativeType><cd:LittleEndian/></cd:Float>
      </cd:Complex>
    </cd:Field>
    <cd:Field name="cplx_i32">
      <cd:Complex>
        <cd:Integer><cd:BitSize>32</cd:BitSize><cd:NativeType>int32</cd:NativeType><cd:LittleEndian/></cd:Integer>
      </cd:Complex>
    </cd:Field>
    <cd:Field name="cplx_2d">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>2</cd:Dimension>
        <cd:Complex>
          <cd:Float><cd:BitSize>64</cd:BitSize><cd:NativeType>double</cd:NativeType><cd:LittleEndian/></cd:Float>
        </cd:Complex>
      </cd:Array>
    </cd:Field>
    <cd:Field name="cplx_3d">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>2</cd:Dimension><cd:Dimension>2</cd:Dimension>
        <cd:Complex>
          <cd:Float><cd:BitSize>64</cd:BitSize><cd:NativeType>double</cd:NativeType><cd:LittleEndian/></cd:Float>
        </cd:Complex>
      </cd:Array>
    </cd:Field>
    <cd:Field name="empty_dim">
      <cd:Integer><cd:BitSize>32</cd:BitSize><cd:NativeType>int32</cd:NativeType><cd:LittleEndian/></cd:Integer>
    </cd:Field>
    <cd:Field name="cplx_empty_1d">
      <cd:Array><cd:Dimension>int(/empty_dim)</cd:Dimension>
        <cd:Complex>
          <cd:Float><cd:BitSize>64</cd:BitSize><cd:NativeType>double</cd:NativeType><cd:LittleEndian/></cd:Float>
        </cd:Complex>
      </cd:Array>
    </cd:Field>
    <cd:Field name="cplx_empty_2d">
      <cd:Array><cd:Dimension>int(/empty_dim)</cd:Dimension><cd:Dimension>2</cd:Dimension>
        <cd:Complex>
          <cd:Float><cd:BitSize>64</cd:BitSize><cd:NativeType>double</cd:NativeType><cd:LittleEndian/></cd:Float>
        </cd:Complex>
      </cd:Array>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";
  ASSERT_TRUE(env.InitWithXml("SPECIALP", "binary", "SpecialProd", kXml));
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  auto append_envisat_time = [](std::vector<uint8_t>& out, int32_t days,
                                uint32_t secs, uint32_t usecs) {
    AppendBe32(out, static_cast<uint32_t>(days));
    AppendBe32(out, secs);
    AppendBe32(out, usecs);
  };

  std::vector<uint8_t> bin;
  AppendString(bin, "SPECIALP");
  append_envisat_time(bin, 1, 3600, 500000);  // 86400 + 3600 + 0.5 = 90000.5
  for (int i = 0; i < 4; ++i) {
    append_envisat_time(bin, 0, static_cast<uint32_t>(i * 10), 0);
  }
  // vsf_pos: scale=2 (int8), val=1234 (int16 LE) -> 12.34
  bin.push_back(2);
  AppendLe16(bin, 1234);
  // vsf_neg: scale=-2 (int8), val=5 (int16 LE) -> 500.0
  bin.push_back(static_cast<uint8_t>(int8_t{-2}));
  AppendLe16(bin, 5);
  // cplx_f64: (1.5, -2.5)
  AppendLeDouble(bin, 1.5);
  AppendLeDouble(bin, -2.5);
  // cplx_i32: (7, -9)
  AppendLe32(bin, 7);
  AppendLe32(bin, static_cast<uint32_t>(int32_t{-9}));
  // cplx_2d: (1,10), (2,20), (3,30), (4,40)
  for (int i = 1; i <= 4; ++i) {
    AppendLeDouble(bin, static_cast<double>(i));
    AppendLeDouble(bin, static_cast<double>(i * 10));
  }
  // cplx_3d: 8 elements (i, -i) for i=0..7
  for (int i = 0; i < 8; ++i) {
    AppendLeDouble(bin, static_cast<double>(i));
    AppendLeDouble(bin, static_cast<double>(-i));
  }
  // empty_dim = 0
  AppendLe32(bin, 0);

  TempFile dat_file("coda_cursor_read_special.dat");
  ASSERT_TRUE(dat_file.WriteBytes(bin));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(dat_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  // 1. Binary time scalar and 2D time array (special-type array paths).
  {
    const coda_cursor c = GotoField(root, "t_scalar");
    double t = 0.0;
    float tf = 0.0f;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c, &t));
    EXPECT_DOUBLE_EQ(90000.5, t);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c, &tf));
    EXPECT_FLOAT_EQ(90000.5f, tf);
  }
  {
    const coda_cursor c = GotoField(root, "t_arr");
    std::array<double, 4> t_c{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(
                                &c, t_c.data(), coda_array_ordering_c));
    EXPECT_THAT(t_c, ElementsAre(0.0, 10.0, 20.0, 30.0));

    std::array<double, 4> t_f{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double_array(
                                &c, t_f.data(), coda_array_ordering_fortran));
    EXPECT_THAT(t_f, ElementsAre(0.0, 20.0, 10.0, 30.0));

    std::array<double, 2> t_part{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_double_partial_array(&c, 1, 2, t_part.data()));
    EXPECT_THAT(t_part, ElementsAre(10.0, 20.0));

    std::array<float, 4> tf_c{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(
                                &c, tf_c.data(), coda_array_ordering_c));
    EXPECT_THAT(tf_c, ElementsAre(0.0f, 10.0f, 20.0f, 30.0f));

    std::array<float, 2> tf_part{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_float_partial_array(&c, 1, 2, tf_part.data()));
    EXPECT_THAT(tf_part, ElementsAre(10.0f, 20.0f));
  }

  // 2. VSFInteger (positive and negative scale factors).
  {
    const coda_cursor c_pos = GotoField(root, "vsf_pos");
    const coda_cursor c_neg = GotoField(root, "vsf_neg");
    double val = 0.0;
    float fval = 0.0f;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_pos, &val));
    EXPECT_DOUBLE_EQ(12.34, val);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&c_pos, &fval));
    EXPECT_FLOAT_EQ(12.34f, fval);
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&c_neg, &val));
    EXPECT_DOUBLE_EQ(500.0, val);
  }

  // 3. Complex scalars (float64 and int32 base types).
  {
    const coda_cursor c = GotoField(root, "cplx_f64");
    std::array<double, 2> pair{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_complex_double_pair(&c, pair.data()));
    EXPECT_THAT(pair, ElementsAre(1.5, -2.5));
    double re = 0.0;
    double im = 0.0;
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_complex_double_split(&c, &re, &im));
    EXPECT_DOUBLE_EQ(1.5, re);
    EXPECT_DOUBLE_EQ(-2.5, im);
  }
  {
    const coda_cursor c = GotoField(root, "cplx_i32");
    std::array<double, 2> pair{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_complex_double_pair(&c, pair.data()));
    EXPECT_THAT(pair, ElementsAre(7.0, -9.0));
  }

  // 4. Complex 2D and 3D arrays in C and Fortran orderings.
  {
    const coda_cursor c = GotoField(root, "cplx_2d");
    std::array<double, 8> pairs{};
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_complex_double_pairs_array(
                                &c, pairs.data(), coda_array_ordering_c));
    EXPECT_THAT(pairs, ElementsAre(1, 10, 2, 20, 3, 30, 4, 40));

    ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_complex_double_pairs_array(
                                &c, pairs.data(), coda_array_ordering_fortran));
    EXPECT_THAT(pairs, ElementsAre(1, 10, 3, 30, 2, 20, 4, 40));

    std::array<double, 4> re{};
    std::array<double, 4> im{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_complex_double_split_array(
                  &c, re.data(), im.data(), coda_array_ordering_c));
    EXPECT_THAT(re, ElementsAre(1, 2, 3, 4));
    EXPECT_THAT(im, ElementsAre(10, 20, 30, 40));

    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_complex_double_split_array(
                  &c, re.data(), im.data(), coda_array_ordering_fortran));
    EXPECT_THAT(re, ElementsAre(1, 3, 2, 4));
    EXPECT_THAT(im, ElementsAre(10, 30, 20, 40));
  }
  {
    const coda_cursor c = GotoField(root, "cplx_3d");
    std::array<double, 8> re{};
    std::array<double, 8> im{};
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_read_complex_double_split_array(
                  &c, re.data(), im.data(), coda_array_ordering_fortran));
    // C index i = 4*k0 + 2*k1 + k2 -> Fortran index k0 + 2*k1 + 4*k2.
    EXPECT_THAT(re, ElementsAre(0, 4, 2, 6, 1, 5, 3, 7));
    EXPECT_THAT(im, ElementsAre(0, -4, -2, -6, -1, -5, -3, -7));
  }

  // 5. Empty 1D and 2D complex arrays (num_elements == 0 branches).
  {
    std::array<double, 1> dummy_re{-999.0};
    std::array<double, 1> dummy_im{-999.0};
    const coda_cursor c1 = GotoField(root, "cplx_empty_1d");
    EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_complex_double_split_array(
                                &c1, dummy_re.data(), dummy_im.data(),
                                coda_array_ordering_c));
    const coda_cursor c2 = GotoField(root, "cplx_empty_2d");
    EXPECT_EQ(CODA_SUCCESS, coda_cursor_read_complex_double_split_array(
                                &c2, dummy_re.data(), dummy_im.data(),
                                coda_array_ordering_fortran));
    EXPECT_DOUBLE_EQ(-999.0, dummy_re[0]);
  }
}

TEST(CodaCursorReadTest, AsciiBackendScalarsArraysAndPartialArrays) {
  CodadefEnv env("coda_cursor_read_ascii.codadef");
  constexpr std::string_view kXml = R"(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="AsciiProd" format="ascii">
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text><cd:ByteSize>6</cd:ByteSize></cd:Text>
    </cd:Field>
    <cd:Field name="i8">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>2</cd:Dimension>
        <cd:Integer><cd:ByteSize>4</cd:ByteSize><cd:NativeType>int8</cd:NativeType></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="u8">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>2</cd:Dimension>
        <cd:Integer><cd:ByteSize>4</cd:ByteSize><cd:NativeType>uint8</cd:NativeType></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="i16">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>2</cd:Dimension>
        <cd:Integer><cd:ByteSize>6</cd:ByteSize><cd:NativeType>int16</cd:NativeType></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="u16">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>2</cd:Dimension>
        <cd:Integer><cd:ByteSize>6</cd:ByteSize><cd:NativeType>uint16</cd:NativeType></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="i32">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>2</cd:Dimension>
        <cd:Integer><cd:ByteSize>6</cd:ByteSize><cd:NativeType>int32</cd:NativeType></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="u32">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>2</cd:Dimension>
        <cd:Integer><cd:ByteSize>6</cd:ByteSize><cd:NativeType>uint32</cd:NativeType></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="i64">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>2</cd:Dimension>
        <cd:Integer><cd:ByteSize>6</cd:ByteSize><cd:NativeType>int64</cd:NativeType></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="u64">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>2</cd:Dimension>
        <cd:Integer><cd:ByteSize>6</cd:ByteSize><cd:NativeType>uint64</cd:NativeType></cd:Integer>
      </cd:Array>
    </cd:Field>
    <cd:Field name="f32">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>2</cd:Dimension>
        <cd:Float><cd:ByteSize>6</cd:ByteSize><cd:NativeType>float</cd:NativeType></cd:Float>
      </cd:Array>
    </cd:Field>
    <cd:Field name="f64">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>2</cd:Dimension>
        <cd:Float><cd:ByteSize>6</cd:ByteSize><cd:NativeType>double</cd:NativeType></cd:Float>
      </cd:Array>
    </cd:Field>
    <cd:Field name="ch">
      <cd:Array><cd:Dimension>2</cd:Dimension><cd:Dimension>2</cd:Dimension>
        <cd:Text><cd:ByteSize>1</cd:ByteSize><cd:NativeType>char</cd:NativeType></cd:Text>
      </cd:Array>
    </cd:Field>
    <cd:Field name="str">
      <cd:Text><cd:ByteSize>5</cd:ByteSize></cd:Text>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)";
  ASSERT_TRUE(env.InitWithXml("ASCII:", "ascii", "AsciiProd", kXml));
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  // Trailing '\n' avoids coda-ascii-cursor.c >= boundary check on final field.
  constexpr std::string_view kAsciiData =
      "ASCII:"
      " -10  20 -30  40"          // i8
      "  10  20  30  40"          // u8
      "  -100   200  -300   400"  // i16
      "  1000  2000  3000  4000"  // u16
      "   -10    20   -30    40"  // i32
      "   100   200   300   400"  // u32
      " -1000  2000 -3000  4000"  // i64
      "  1111  2222  3333  4444"  // u64
      "   1.5  -2.5   3.5  -4.5"  // f32
      "  10.5 -20.5  30.5 -40.5"  // f64
      "WXYZ"                      // ch
      "hello\n";                  // str + newline

  TempFile dat_file("coda_cursor_read_ascii.txt");
  ASSERT_TRUE(dat_file.WriteText(kAsciiData));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(dat_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  auto check_ascii_field = [&](const char* name, auto read_fn, auto arr_fn,
                               auto partial_fn, const auto& expected,
                               auto sentinel) {
    using T = typename std::decay_t<decltype(expected)>::value_type;
    const coda_cursor c = GotoField(root, name);

    // Scalar element read.
    coda_cursor elem = c;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 1));
    T scalar = sentinel;
    ASSERT_EQ(CODA_SUCCESS, read_fn(&elem, &scalar));
    EXPECT_EQ(expected[1], scalar);

    // Full C-ordered array read.
    std::vector<T> full(4, sentinel);
    ASSERT_EQ(CODA_SUCCESS, arr_fn(&c, full.data(), coda_array_ordering_c));
    EXPECT_THAT(full, ElementsAreArray(expected));

    // Partial array read.
    std::vector<T> part(2, sentinel);
    ASSERT_EQ(CODA_SUCCESS, partial_fn(&c, 1, 2, part.data()));
    EXPECT_THAT(part, ElementsAre(expected[1], expected[2]));
  };

  check_ascii_field("i8", coda_cursor_read_int8, coda_cursor_read_int8_array,
                    coda_cursor_read_int8_partial_array,
                    std::array<int8_t, 4>{-10, 20, -30, 40}, int8_t{-99});
  check_ascii_field("u8", coda_cursor_read_uint8, coda_cursor_read_uint8_array,
                    coda_cursor_read_uint8_partial_array,
                    std::array<uint8_t, 4>{10, 20, 30, 40}, uint8_t{99});
  check_ascii_field("i16", coda_cursor_read_int16, coda_cursor_read_int16_array,
                    coda_cursor_read_int16_partial_array,
                    std::array<int16_t, 4>{-100, 200, -300, 400},
                    int16_t{-999});
  check_ascii_field(
      "u16", coda_cursor_read_uint16, coda_cursor_read_uint16_array,
      coda_cursor_read_uint16_partial_array,
      std::array<uint16_t, 4>{1000, 2000, 3000, 4000}, uint16_t{999});
  check_ascii_field("i32", coda_cursor_read_int32, coda_cursor_read_int32_array,
                    coda_cursor_read_int32_partial_array,
                    std::array<int32_t, 4>{-10, 20, -30, 40}, int32_t{-999});
  check_ascii_field("u32", coda_cursor_read_uint32,
                    coda_cursor_read_uint32_array,
                    coda_cursor_read_uint32_partial_array,
                    std::array<uint32_t, 4>{100, 200, 300, 400}, uint32_t{999});
  check_ascii_field("i64", coda_cursor_read_int64, coda_cursor_read_int64_array,
                    coda_cursor_read_int64_partial_array,
                    std::array<int64_t, 4>{-1000, 2000, -3000, 4000},
                    int64_t{-999});
  check_ascii_field(
      "u64", coda_cursor_read_uint64, coda_cursor_read_uint64_array,
      coda_cursor_read_uint64_partial_array,
      std::array<uint64_t, 4>{1111, 2222, 3333, 4444}, uint64_t{999});
  check_ascii_field("f32", coda_cursor_read_float, coda_cursor_read_float_array,
                    coda_cursor_read_float_partial_array,
                    std::array<float, 4>{1.5f, -2.5f, 3.5f, -4.5f}, -999.0f);
  check_ascii_field("f64", coda_cursor_read_double,
                    coda_cursor_read_double_array,
                    coda_cursor_read_double_partial_array,
                    std::array<double, 4>{10.5, -20.5, 30.5, -40.5}, -999.0);
  check_ascii_field("ch", coda_cursor_read_char, coda_cursor_read_char_array,
                    coda_cursor_read_char_partial_array,
                    std::array<char, 4>{'W', 'X', 'Y', 'Z'}, '?');

  const coda_cursor c_str = GotoField(root, "str");
  char sbuf[8]{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c_str, sbuf, sizeof(sbuf)));
  EXPECT_STREQ("hello", sbuf);

  std::array<uint8_t, 2> bits_out{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_bits(&c_str, bits_out.data(), 8, 16));
  EXPECT_THAT(bits_out, ElementsAre('e', 'l'));

  std::array<uint8_t, 3> bytes_out{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_bytes(&c_str, bytes_out.data(), 2, 3));
  EXPECT_THAT(bytes_out, ElementsAre('l', 'l', 'o'));
}

TEST(CodaCursorReadTest, MemoryBackendScalarsArraysAndPartialArrays) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  std::vector<ScopedType> owned_types;
  ScopedProduct product = MakeSyntheticMemProduct();
  ASSERT_NE(nullptr, product.get());

  auto* root_def = coda_type_record_new(coda_format_binary);
  ASSERT_NE(nullptr, root_def);
  TrackType(root_def, owned_types);
  ScopedDynamicType root_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_record_new(root_def, nullptr)));
  ASSERT_NE(nullptr, root_dyn.get());
  auto* root_rec = reinterpret_cast<coda_mem_record*>(root_dyn.get());

  auto add_mem_array = [&](const char* name, coda_type* elem_def,
                           const auto& vals, auto new_fn) {
    auto* arr_def = coda_type_array_new(coda_format_binary);
    ASSERT_NE(nullptr, arr_def);
    TrackType(arr_def, owned_types);
    ASSERT_EQ(CODA_SUCCESS, coda_type_array_set_base_type(arr_def, elem_def));
    ASSERT_EQ(CODA_SUCCESS, coda_type_array_add_fixed_dimension(arr_def, 2));
    ASSERT_EQ(CODA_SUCCESS, coda_type_array_add_fixed_dimension(arr_def, 2));
    ScopedDynamicType arr_dyn(reinterpret_cast<coda_dynamic_type*>(
        coda_mem_array_new(arr_def, nullptr)));
    ASSERT_NE(nullptr, arr_dyn.get());
    for (size_t i = 0; i < vals.size(); ++i) {
      ScopedDynamicType elem(
          reinterpret_cast<coda_dynamic_type*>(new_fn(vals[i])));
      ASSERT_NE(nullptr, elem.get());
      ASSERT_EQ(CODA_SUCCESS,
                coda_mem_array_set_element(
                    reinterpret_cast<coda_mem_array*>(arr_dyn.get()),
                    static_cast<long>(i), elem.get()));
      elem.release();
    }
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(root_rec, name, arr_dyn.get(), 1));
    arr_dyn.release();
  };

  auto add_num_arr = [&](const char* name, coda_type_class tc,
                         coda_native_type nt, int64_t bits, const auto& vals,
                         auto mem_new_fn) {
    coda_type_number* def =
        MakeNumberType(coda_format_binary, tc, nt, bits, owned_types);
    ASSERT_NE(nullptr, def);
    add_mem_array(name, reinterpret_cast<coda_type*>(def), vals, [&](auto v) {
      return mem_new_fn(def, nullptr, product.get(), v);
    });
  };

  add_num_arr("i8", coda_integer_class, coda_native_type_int8, 8,
              std::array<int8_t, 4>{-1, 2, -3, 4}, coda_mem_int8_new);
  add_num_arr("u8", coda_integer_class, coda_native_type_uint8, 8,
              std::array<uint8_t, 4>{1, 2, 3, 4}, coda_mem_uint8_new);
  add_num_arr("i16", coda_integer_class, coda_native_type_int16, 16,
              std::array<int16_t, 4>{-10, 20, -30, 40}, coda_mem_int16_new);
  add_num_arr("u16", coda_integer_class, coda_native_type_uint16, 16,
              std::array<uint16_t, 4>{10, 20, 30, 40}, coda_mem_uint16_new);
  add_num_arr("i32", coda_integer_class, coda_native_type_int32, 32,
              std::array<int32_t, 4>{-100, 200, -300, 400}, coda_mem_int32_new);
  add_num_arr("u32", coda_integer_class, coda_native_type_uint32, 32,
              std::array<uint32_t, 4>{100, 200, 300, 400}, coda_mem_uint32_new);
  add_num_arr("i64", coda_integer_class, coda_native_type_int64, 64,
              std::array<int64_t, 4>{-1000, 2000, -3000, 4000},
              coda_mem_int64_new);
  add_num_arr("u64", coda_integer_class, coda_native_type_uint64, 64,
              std::array<uint64_t, 4>{1000, 2000, 3000, 4000},
              coda_mem_uint64_new);
  add_num_arr("f32", coda_real_class, coda_native_type_float, 32,
              std::array<float, 4>{1.5f, -2.5f, 3.5f, -4.5f},
              coda_mem_float_new);
  add_num_arr("f64", coda_real_class, coda_native_type_double, 64,
              std::array<double, 4>{1.25, -2.25, 3.25, -4.25},
              coda_mem_double_new);

  coda_type_text* ch_def = MakeCharType(coda_format_binary, owned_types);
  ASSERT_NE(nullptr, ch_def);
  add_mem_array("ch", reinterpret_cast<coda_type*>(ch_def),
                std::array<char, 4>{'M', 'E', 'M', 'O'}, [&](char v) {
                  return coda_mem_char_new(ch_def, nullptr, product.get(), v);
                });

  coda_type_text* str_def = MakeStringType(coda_format_ascii, -1, owned_types);
  ASSERT_NE(nullptr, str_def);
  ScopedDynamicType str_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_string_new(str_def, nullptr, product.get(), "memstr")));
  ASSERT_NE(nullptr, str_dyn.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(root_rec, "str", str_dyn.get(), 1));
  str_dyn.release();

  auto* raw_def = coda_type_raw_new(coda_format_binary);
  ASSERT_NE(nullptr, raw_def);
  TrackType(raw_def, owned_types);
  constexpr std::array<uint8_t, 4> kRaw = {0x12, 0x34, 0x56, 0x78};
  ScopedDynamicType raw_dyn(reinterpret_cast<coda_dynamic_type*>(
      coda_mem_raw_new(raw_def, nullptr, product.get(), 4, kRaw.data())));
  ASSERT_NE(nullptr, raw_dyn.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(root_rec, "raw", raw_dyn.get(), 1));
  raw_dyn.release();

  product.get()->root_type = root_dyn.release();
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  auto check_mem = [&](const char* name, auto read_fn, auto arr_fn,
                       auto partial_fn, const auto& expected, auto sentinel) {
    using T = typename std::decay_t<decltype(expected)>::value_type;
    const coda_cursor c = GotoField(root, name);
    coda_cursor elem = c;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 2));
    T v = sentinel;
    ASSERT_EQ(CODA_SUCCESS, read_fn(&elem, &v));
    EXPECT_EQ(expected[2], v);

    std::vector<T> full(4, sentinel);
    ASSERT_EQ(CODA_SUCCESS, arr_fn(&c, full.data(), coda_array_ordering_c));
    EXPECT_THAT(full, ElementsAreArray(expected));

    std::vector<T> part(2, sentinel);
    ASSERT_EQ(CODA_SUCCESS, partial_fn(&c, 1, 2, part.data()));
    EXPECT_THAT(part, ElementsAre(expected[1], expected[2]));
  };

  check_mem("i8", coda_cursor_read_int8, coda_cursor_read_int8_array,
            coda_cursor_read_int8_partial_array,
            std::array<int8_t, 4>{-1, 2, -3, 4}, int8_t{-99});
  check_mem("u8", coda_cursor_read_uint8, coda_cursor_read_uint8_array,
            coda_cursor_read_uint8_partial_array,
            std::array<uint8_t, 4>{1, 2, 3, 4}, uint8_t{99});
  check_mem("i16", coda_cursor_read_int16, coda_cursor_read_int16_array,
            coda_cursor_read_int16_partial_array,
            std::array<int16_t, 4>{-10, 20, -30, 40}, int16_t{-999});
  check_mem("u16", coda_cursor_read_uint16, coda_cursor_read_uint16_array,
            coda_cursor_read_uint16_partial_array,
            std::array<uint16_t, 4>{10, 20, 30, 40}, uint16_t{999});
  check_mem("i32", coda_cursor_read_int32, coda_cursor_read_int32_array,
            coda_cursor_read_int32_partial_array,
            std::array<int32_t, 4>{-100, 200, -300, 400}, int32_t{-999});
  check_mem("u32", coda_cursor_read_uint32, coda_cursor_read_uint32_array,
            coda_cursor_read_uint32_partial_array,
            std::array<uint32_t, 4>{100, 200, 300, 400}, uint32_t{999});
  check_mem("i64", coda_cursor_read_int64, coda_cursor_read_int64_array,
            coda_cursor_read_int64_partial_array,
            std::array<int64_t, 4>{-1000, 2000, -3000, 4000}, int64_t{-999});
  check_mem("u64", coda_cursor_read_uint64, coda_cursor_read_uint64_array,
            coda_cursor_read_uint64_partial_array,
            std::array<uint64_t, 4>{1000, 2000, 3000, 4000}, uint64_t{999});
  check_mem("f32", coda_cursor_read_float, coda_cursor_read_float_array,
            coda_cursor_read_float_partial_array,
            std::array<float, 4>{1.5f, -2.5f, 3.5f, -4.5f}, -999.0f);
  check_mem("f64", coda_cursor_read_double, coda_cursor_read_double_array,
            coda_cursor_read_double_partial_array,
            std::array<double, 4>{1.25, -2.25, 3.25, -4.25}, -999.0);
  check_mem("ch", coda_cursor_read_char, coda_cursor_read_char_array,
            coda_cursor_read_char_partial_array,
            std::array<char, 4>{'M', 'E', 'M', 'O'}, '?');

  const coda_cursor c_str = GotoField(root, "str");
  char sbuf[16]{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_string(&c_str, sbuf, sizeof(sbuf)));
  EXPECT_STREQ("memstr", sbuf);

  const coda_cursor c_raw = GotoField(root, "raw");
  uint8_t b = 0;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_bits(&c_raw, &b, 4, 8));
  EXPECT_EQ(0x23, b);
  std::array<uint8_t, 2> bytes_out{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_bytes(&c_raw, bytes_out.data(), 1, 2));
  EXPECT_THAT(bytes_out, ElementsAre(0x34, 0x56));
}

TEST(CodaCursorReadTest, Hdf5BackendScalarsArraysTransposesAndPartialArrays) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  TempFile h5_file("coda_cursor_read_hdf5.h5");
  {
    const ScopedH5File file(
        H5Fcreate(h5_file.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    constexpr hsize_t kDims2x2[2] = {2, 2};
    const ScopedH5Space space2x2(H5Screate_simple(2, kDims2x2, nullptr));
    ASSERT_TRUE(space2x2.valid());

    auto write_dset = [&](const char* name, hid_t dtype, const void* data) {
      const ScopedH5Dataset dset(H5Dcreate2(file.get(), name, dtype,
                                            space2x2.get(), H5P_DEFAULT,
                                            H5P_DEFAULT, H5P_DEFAULT));
      ASSERT_TRUE(dset.valid());
      ASSERT_GE(
          H5Dwrite(dset.get(), dtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), 0);
    };

    const std::array<int8_t, 4> d_i8 = {-1, 2, -3, 4};
    const std::array<uint8_t, 4> d_u8 = {1, 2, 3, 4};
    const std::array<int16_t, 4> d_i16 = {-10, 20, -30, 40};
    const std::array<uint16_t, 4> d_u16 = {10, 20, 30, 40};
    const std::array<int32_t, 4> d_i32 = {-100, 200, -300, 400};
    const std::array<uint32_t, 4> d_u32 = {100, 200, 300, 400};
    const std::array<int64_t, 4> d_i64 = {-1000, 2000, -3000, 4000};
    const std::array<uint64_t, 4> d_u64 = {1000, 2000, 3000, 4000};
    const std::array<float, 4> d_f32 = {1.5f, -2.5f, 3.5f, -4.5f};
    const std::array<double, 4> d_f64 = {1.25, -2.25, 3.25, -4.25};

    write_dset("i8", H5T_NATIVE_INT8, d_i8.data());
    write_dset("u8", H5T_NATIVE_UINT8, d_u8.data());
    write_dset("i16", H5T_NATIVE_INT16, d_i16.data());
    write_dset("u16", H5T_NATIVE_UINT16, d_u16.data());
    write_dset("i32", H5T_NATIVE_INT32, d_i32.data());
    write_dset("u32", H5T_NATIVE_UINT32, d_u32.data());
    write_dset("i64", H5T_NATIVE_INT64, d_i64.data());
    write_dset("u64", H5T_NATIVE_UINT64, d_u64.data());
    write_dset("f32", H5T_NATIVE_FLOAT, d_f32.data());
    write_dset("f64", H5T_NATIVE_DOUBLE, d_f64.data());

    const ScopedH5Type str_type(H5Tcopy(H5T_C_S1));
    ASSERT_TRUE(str_type.valid());
    ASSERT_GE(H5Tset_size(str_type.get(), 8), 0);
    ASSERT_GE(H5Tset_strpad(str_type.get(), H5T_STR_NULLTERM), 0);
    constexpr hsize_t kDims2[1] = {2};
    const ScopedH5Space space2(H5Screate_simple(1, kDims2, nullptr));
    ASSERT_TRUE(space2.valid());
    const ScopedH5Dataset str_dset(H5Dcreate2(file.get(), "str", str_type.get(),
                                              space2.get(), H5P_DEFAULT,
                                              H5P_DEFAULT, H5P_DEFAULT));
    ASSERT_TRUE(str_dset.valid());
    const char strs[2][8] = {"alpha\0\0", "beta\0\0\0"};
    ASSERT_GE(H5Dwrite(str_dset.get(), str_type.get(), H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, strs),
              0);
  }

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(h5_file.c_str(), product.OutParam()));
  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  auto check_h5 = [&](const char* name, auto read_fn, auto arr_fn,
                      auto partial_fn, const auto& expected, auto sentinel) {
    using T = typename std::decay_t<decltype(expected)>::value_type;
    const coda_cursor c = GotoField(root, name);
    coda_cursor elem = c;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 1));
    T scalar = sentinel;
    ASSERT_EQ(CODA_SUCCESS, read_fn(&elem, &scalar));
    EXPECT_EQ(expected[1], scalar);

    std::vector<T> full(4, sentinel);
    ASSERT_EQ(CODA_SUCCESS, arr_fn(&c, full.data(), coda_array_ordering_c));
    EXPECT_THAT(full, ElementsAreArray(expected));

    // Fortran ordering invokes transpose_array() in coda-cursor-read.c.
    std::fill(full.begin(), full.end(), sentinel);
    ASSERT_EQ(CODA_SUCCESS,
              arr_fn(&c, full.data(), coda_array_ordering_fortran));
    EXPECT_THAT(
        full, ElementsAre(expected[0], expected[2], expected[1], expected[3]));

    // Valid hyperslab (row 0: offset=0, length=2) and non-hyperslab failure
    // (offset=1, length=2 on 2x2) to cover both success and error returns.
    std::vector<T> part(2, sentinel);
    ASSERT_EQ(CODA_SUCCESS, partial_fn(&c, 0, 2, part.data()));
    EXPECT_THAT(part, ElementsAre(expected[0], expected[1]));
    EXPECT_EQ(-1, partial_fn(&c, 1, 2, part.data()));
    EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  };

  check_h5("i8", coda_cursor_read_int8, coda_cursor_read_int8_array,
           coda_cursor_read_int8_partial_array,
           std::array<int8_t, 4>{-1, 2, -3, 4}, int8_t{-99});
  check_h5("u8", coda_cursor_read_uint8, coda_cursor_read_uint8_array,
           coda_cursor_read_uint8_partial_array,
           std::array<uint8_t, 4>{1, 2, 3, 4}, uint8_t{99});
  check_h5("i16", coda_cursor_read_int16, coda_cursor_read_int16_array,
           coda_cursor_read_int16_partial_array,
           std::array<int16_t, 4>{-10, 20, -30, 40}, int16_t{-999});
  check_h5("u16", coda_cursor_read_uint16, coda_cursor_read_uint16_array,
           coda_cursor_read_uint16_partial_array,
           std::array<uint16_t, 4>{10, 20, 30, 40}, uint16_t{999});
  check_h5("i32", coda_cursor_read_int32, coda_cursor_read_int32_array,
           coda_cursor_read_int32_partial_array,
           std::array<int32_t, 4>{-100, 200, -300, 400}, int32_t{-999});
  check_h5("u32", coda_cursor_read_uint32, coda_cursor_read_uint32_array,
           coda_cursor_read_uint32_partial_array,
           std::array<uint32_t, 4>{100, 200, 300, 400}, uint32_t{999});
  check_h5("i64", coda_cursor_read_int64, coda_cursor_read_int64_array,
           coda_cursor_read_int64_partial_array,
           std::array<int64_t, 4>{-1000, 2000, -3000, 4000}, int64_t{-999});
  check_h5("u64", coda_cursor_read_uint64, coda_cursor_read_uint64_array,
           coda_cursor_read_uint64_partial_array,
           std::array<uint64_t, 4>{1000, 2000, 3000, 4000}, uint64_t{999});
  check_h5("f32", coda_cursor_read_float, coda_cursor_read_float_array,
           coda_cursor_read_float_partial_array,
           std::array<float, 4>{1.5f, -2.5f, 3.5f, -4.5f}, -999.0f);
  check_h5("f64", coda_cursor_read_double, coda_cursor_read_double_array,
           coda_cursor_read_double_partial_array,
           std::array<double, 4>{1.25, -2.25, 3.25, -4.25}, -999.0);

  coda_cursor str_cur = GotoField(root, "str");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&str_cur, 1));
  char sbuf[16]{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&str_cur, sbuf, sizeof(sbuf)));
  EXPECT_STREQ("beta", sbuf);

  // read_bits and read_bytes are unsupported on HDF5.
  uint8_t dummy = 0;
  const coda_cursor i8_cur = GotoField(root, "i8");
  EXPECT_EQ(-1, coda_cursor_read_bits(&i8_cur, &dummy, 0, 8));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_bytes(&i8_cur, &dummy, 0, 1));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
}

TEST(CodaCursorReadTest, NetcdfBackendScalarsArraysAndPartialArrays) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  NetcdfBuilder nc(1);
  nc.AddDim({.name = "d2", .size = 2});  // dim 0
  nc.AddDim({.name = "d1", .size = 1});  // dim 1
  nc.AddDim({.name = "d5", .size = 5});  // dim 2

  std::vector<uint8_t> d_i8;
  for (int8_t v : {int8_t{-1}, int8_t{2}, int8_t{-3}, int8_t{4}}) {
    d_i8.push_back(static_cast<uint8_t>(v));
  }
  nc.AddVar({.name = "i8",
             .dim_ids = {0, 0},
             .type = NetcdfType::kByte,
             .vsize = 4,
             .data = std::move(d_i8)});

  std::vector<uint8_t> d_i16;
  for (int16_t v : {int16_t{-10}, int16_t{20}, int16_t{-30}, int16_t{40}}) {
    AppendBe16(d_i16, static_cast<uint16_t>(v));
  }
  nc.AddVar({.name = "i16",
             .dim_ids = {0, 0},
             .type = NetcdfType::kShort,
             .vsize = 8,
             .data = std::move(d_i16)});

  std::vector<uint8_t> d_i32;
  for (int32_t v : {-100, 200, -300, 400}) {
    AppendBe32(d_i32, static_cast<uint32_t>(v));
  }
  nc.AddVar({.name = "i32",
             .dim_ids = {0, 0},
             .type = NetcdfType::kInt,
             .vsize = 16,
             .data = std::move(d_i32)});

  std::vector<uint8_t> d_f32;
  for (float v : {1.5f, -2.5f, 3.5f, -4.5f}) {
    AppendBeFloat(d_f32, v);
  }
  nc.AddVar({.name = "f32",
             .dim_ids = {0, 0},
             .type = NetcdfType::kFloat,
             .vsize = 16,
             .data = std::move(d_f32)});

  std::vector<uint8_t> d_f64;
  for (double v : {1.25, -2.25, 3.25, -4.25}) {
    AppendBeDouble(d_f64, v);
  }
  nc.AddVar({.name = "f64",
             .dim_ids = {0, 0},
             .type = NetcdfType::kDouble,
             .vsize = 32,
             .data = std::move(d_f64)});

  // Last dim = 1 makes coda-netcdf.c produce a 2x2 array of char.
  nc.AddVar({.name = "ch_arr",
             .dim_ids = {0, 0, 1},
             .type = NetcdfType::kChar,
             .vsize = 4,
             .data = {'N', 'C', 'D', 'F'}});
  nc.AddVar({.name = "ch_scalar",
             .dim_ids = {},
             .type = NetcdfType::kChar,
             .vsize = 4,
             .data = {'Q'}});
  nc.AddVar({.name = "str_scalar",
             .dim_ids = {2},
             .type = NetcdfType::kChar,
             .vsize = 8,
             .data = {'h', 'e', 'l', 'l', 'o'}});

  TempFile nc_file("coda_cursor_read_netcdf.nc");
  ASSERT_TRUE(nc_file.WriteBytes(nc.Build()));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(nc_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  auto check_nc = [&](const char* name, auto read_fn, auto arr_fn,
                      auto partial_fn, const auto& expected, auto sentinel) {
    using T = typename std::decay_t<decltype(expected)>::value_type;
    const coda_cursor c = GotoField(root, name);
    coda_cursor elem = c;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 2));
    T scalar = sentinel;
    ASSERT_EQ(CODA_SUCCESS, read_fn(&elem, &scalar));
    EXPECT_EQ(expected[2], scalar);

    std::vector<T> full(4, sentinel);
    ASSERT_EQ(CODA_SUCCESS, arr_fn(&c, full.data(), coda_array_ordering_c));
    EXPECT_THAT(full, ElementsAreArray(expected));

    std::fill(full.begin(), full.end(), sentinel);
    ASSERT_EQ(CODA_SUCCESS,
              arr_fn(&c, full.data(), coda_array_ordering_fortran));
    EXPECT_THAT(
        full, ElementsAre(expected[0], expected[2], expected[1], expected[3]));

    // Allocate num_elements=4 (pre-initialized) due to upstream
    // coda-netcdf-cursor.c read_partial_array swap loop bound.
    std::vector<T> part(4, sentinel);
    ASSERT_EQ(CODA_SUCCESS, partial_fn(&c, 1, 2, part.data()));
    EXPECT_EQ(expected[1], part[0]);
    EXPECT_EQ(expected[2], part[1]);
  };

  check_nc("i8", coda_cursor_read_int8, coda_cursor_read_int8_array,
           coda_cursor_read_int8_partial_array,
           std::array<int8_t, 4>{-1, 2, -3, 4}, int8_t{-99});
  check_nc("i16", coda_cursor_read_int16, coda_cursor_read_int16_array,
           coda_cursor_read_int16_partial_array,
           std::array<int16_t, 4>{-10, 20, -30, 40}, int16_t{-999});
  check_nc("i32", coda_cursor_read_int32, coda_cursor_read_int32_array,
           coda_cursor_read_int32_partial_array,
           std::array<int32_t, 4>{-100, 200, -300, 400}, int32_t{-999});
  check_nc("f32", coda_cursor_read_float, coda_cursor_read_float_array,
           coda_cursor_read_float_partial_array,
           std::array<float, 4>{1.5f, -2.5f, 3.5f, -4.5f}, -999.0f);
  check_nc("f64", coda_cursor_read_double, coda_cursor_read_double_array,
           coda_cursor_read_double_partial_array,
           std::array<double, 4>{1.25, -2.25, 3.25, -4.25}, -999.0);
  check_nc("ch_arr", coda_cursor_read_char, coda_cursor_read_char_array,
           coda_cursor_read_char_partial_array,
           std::array<char, 4>{'N', 'C', 'D', 'F'}, '?');

  char ch = '\0';
  const coda_cursor ch_cur = GotoField(root, "ch_scalar");
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&ch_cur, &ch));
  EXPECT_EQ('Q', ch);

  char sbuf[8]{};
  const coda_cursor str_cur = GotoField(root, "str_scalar");
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&str_cur, sbuf, sizeof(sbuf)));
  EXPECT_STREQ("hello", sbuf);

  uint8_t dummy = 0;
  EXPECT_EQ(-1, coda_cursor_read_bits(&ch_cur, &dummy, 0, 8));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_bytes(&ch_cur, &dummy, 0, 1));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
}

TEST(CodaCursorReadTest, CdfBackendScalarsArraysAndPartialArrays) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  std::vector<CdfVarSpec> vars;
  {
    std::vector<uint8_t> d;
    for (int8_t v : {int8_t{-1}, int8_t{2}, int8_t{-3}, int8_t{4}}) {
      d.push_back(static_cast<uint8_t>(v));
    }
    vars.push_back({.name = "i8",
                    .data_type = 1,
                    .num_elems = 1,
                    .dims = {2, 2},
                    .data = std::move(d)});
  }
  {
    vars.push_back({.name = "u8",
                    .data_type = 11,
                    .num_elems = 1,
                    .dims = {2, 2},
                    .data = {1, 2, 3, 4}});
  }
  {
    std::vector<uint8_t> d;
    for (int16_t v : {int16_t{-10}, int16_t{20}, int16_t{-30}, int16_t{40}}) {
      AppendLe16(d, static_cast<uint16_t>(v));
    }
    vars.push_back({.name = "i16",
                    .data_type = 2,
                    .num_elems = 1,
                    .dims = {2, 2},
                    .data = std::move(d)});
  }
  {
    std::vector<uint8_t> d;
    for (uint16_t v :
         {uint16_t{10}, uint16_t{20}, uint16_t{30}, uint16_t{40}}) {
      AppendLe16(d, v);
    }
    vars.push_back({.name = "u16",
                    .data_type = 12,
                    .num_elems = 1,
                    .dims = {2, 2},
                    .data = std::move(d)});
  }
  {
    std::vector<uint8_t> d;
    for (int32_t v : {-100, 200, -300, 400}) {
      AppendLe32(d, static_cast<uint32_t>(v));
    }
    vars.push_back({.name = "i32",
                    .data_type = 4,
                    .num_elems = 1,
                    .dims = {2, 2},
                    .data = std::move(d)});
  }
  {
    std::vector<uint8_t> d;
    for (uint32_t v : {100U, 200U, 300U, 400U}) {
      AppendLe32(d, v);
    }
    vars.push_back({.name = "u32",
                    .data_type = 14,
                    .num_elems = 1,
                    .dims = {2, 2},
                    .data = std::move(d)});
  }
  {
    std::vector<uint8_t> d;
    for (int64_t v :
         {int64_t{-1000}, int64_t{2000}, int64_t{-3000}, int64_t{4000}}) {
      AppendLe64(d, static_cast<uint64_t>(v));
    }
    vars.push_back({.name = "i64",
                    .data_type = 8,
                    .num_elems = 1,
                    .dims = {2, 2},
                    .data = std::move(d)});
  }
  {
    std::vector<uint8_t> d;
    for (float v : {1.5f, -2.5f, 3.5f, -4.5f}) {
      AppendLeFloat(d, v);
    }
    vars.push_back({.name = "f32",
                    .data_type = 21,
                    .num_elems = 1,
                    .dims = {2, 2},
                    .data = std::move(d)});
  }
  {
    std::vector<uint8_t> d;
    for (double v : {1.25, -2.25, 3.25, -4.25}) {
      AppendLeDouble(d, v);
    }
    vars.push_back({.name = "f64",
                    .data_type = 22,
                    .num_elems = 1,
                    .dims = {2, 2},
                    .data = std::move(d)});
  }
  {
    vars.push_back({.name = "ch",
                    .data_type = 51,
                    .num_elems = 1,
                    .dims = {2, 2},
                    .data = {'C', 'D', 'F', '3'}});
  }
  {
    vars.push_back({.name = "str",
                    .data_type = 51,
                    .num_elems = 5,
                    .dims = {},
                    .data = {'c', 'd', 'f', 'v', '3'}});
  }
  {
    vars.push_back({.name = "miss_i32",
                    .data_type = 4,
                    .num_elems = 1,
                    .dims = {2, 2},
                    .data = {},
                    .missing_record = true});
  }

  TempFile cdf_file("coda_cursor_read_cdf.cdf");
  ASSERT_TRUE(cdf_file.WriteBytes(BuildCdfBytes(vars)));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(cdf_file.c_str(), product.OutParam()));

  coda_cursor root;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&root, product.get()));

  auto check_cdf = [&](const char* name, auto read_fn, auto arr_fn,
                       auto partial_fn, const auto& expected, auto sentinel) {
    using T = typename std::decay_t<decltype(expected)>::value_type;
    const coda_cursor c = GotoField(root, name);
    coda_cursor elem = c;
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 2));
    T scalar = sentinel;
    ASSERT_EQ(CODA_SUCCESS, read_fn(&elem, &scalar));
    EXPECT_EQ(expected[2], scalar);

    std::vector<T> full(4, sentinel);
    ASSERT_EQ(CODA_SUCCESS, arr_fn(&c, full.data(), coda_array_ordering_c));
    EXPECT_THAT(full, ElementsAreArray(expected));

    std::fill(full.begin(), full.end(), sentinel);
    ASSERT_EQ(CODA_SUCCESS,
              arr_fn(&c, full.data(), coda_array_ordering_fortran));
    EXPECT_THAT(
        full, ElementsAre(expected[0], expected[2], expected[1], expected[3]));

    // Use offset=0, length=2 (< num_values_per_record=4) so
    // record_to_id = (0 + 2) / 4 = 0 stays within offset[0].
    std::vector<T> part(2, sentinel);
    ASSERT_EQ(CODA_SUCCESS, partial_fn(&c, 0, 2, part.data()));
    EXPECT_THAT(part, ElementsAre(expected[0], expected[1]));
  };

  check_cdf("i8", coda_cursor_read_int8, coda_cursor_read_int8_array,
            coda_cursor_read_int8_partial_array,
            std::array<int8_t, 4>{-1, 2, -3, 4}, int8_t{-99});
  check_cdf("u8", coda_cursor_read_uint8, coda_cursor_read_uint8_array,
            coda_cursor_read_uint8_partial_array,
            std::array<uint8_t, 4>{1, 2, 3, 4}, uint8_t{99});
  check_cdf("i16", coda_cursor_read_int16, coda_cursor_read_int16_array,
            coda_cursor_read_int16_partial_array,
            std::array<int16_t, 4>{-10, 20, -30, 40}, int16_t{-999});
  check_cdf("u16", coda_cursor_read_uint16, coda_cursor_read_uint16_array,
            coda_cursor_read_uint16_partial_array,
            std::array<uint16_t, 4>{10, 20, 30, 40}, uint16_t{999});
  check_cdf("i32", coda_cursor_read_int32, coda_cursor_read_int32_array,
            coda_cursor_read_int32_partial_array,
            std::array<int32_t, 4>{-100, 200, -300, 400}, int32_t{-999});
  check_cdf("u32", coda_cursor_read_uint32, coda_cursor_read_uint32_array,
            coda_cursor_read_uint32_partial_array,
            std::array<uint32_t, 4>{100, 200, 300, 400}, uint32_t{999});
  check_cdf("i64", coda_cursor_read_int64, coda_cursor_read_int64_array,
            coda_cursor_read_int64_partial_array,
            std::array<int64_t, 4>{-1000, 2000, -3000, 4000}, int64_t{-999});
  check_cdf("f32", coda_cursor_read_float, coda_cursor_read_float_array,
            coda_cursor_read_float_partial_array,
            std::array<float, 4>{1.5f, -2.5f, 3.5f, -4.5f}, -999.0f);
  check_cdf("f64", coda_cursor_read_double, coda_cursor_read_double_array,
            coda_cursor_read_double_partial_array,
            std::array<double, 4>{1.25, -2.25, 3.25, -4.25}, -999.0);
  check_cdf("ch", coda_cursor_read_char, coda_cursor_read_char_array,
            coda_cursor_read_char_partial_array,
            std::array<char, 4>{'C', 'D', 'F', '3'}, '?');

  char sbuf[8]{};
  const coda_cursor str_cur = GotoField(root, "str");
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&str_cur, sbuf, sizeof(sbuf)));
  EXPECT_STREQ("cdfv3", sbuf);

  // Missing-record CDF variable returns CODA_ERROR_UNSUPPORTED_PRODUCT.
  const coda_cursor miss_cur = GotoField(root, "miss_i32");
  std::array<int32_t, 4> tmp{};
  EXPECT_EQ(-1, coda_cursor_read_int32_array(&miss_cur, tmp.data(),
                                             coda_array_ordering_c));
  EXPECT_EQ(CODA_ERROR_UNSUPPORTED_PRODUCT, coda_errno);

  // read_bits and read_bytes are unsupported on CDF.
  uint8_t dummy = 0;
  EXPECT_EQ(-1, coda_cursor_read_bits(&str_cur, &dummy, 0, 8));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_bytes(&str_cur, &dummy, 0, 1));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
}

TEST(CodaCursorReadTest, GribBackendFloatScalarArrayAndPartialArray) {
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  TempFile grib_file("coda_cursor_read_grib1.grb");
  ASSERT_TRUE(grib_file.WriteBytes(BuildGrib1Bytes({1, 2, 3, 4})));
  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(grib_file.c_str(), product.OutParam()));

  coda_cursor c;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&c, product.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&c, 0));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_available_union_field(&c));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&c, "data/values"));

  coda_cursor elem = c;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto_array_element_by_index(&elem, 2));
  float scalar = -999.0f;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&elem, &scalar));
  EXPECT_FLOAT_EQ(3.0f, scalar);

  std::array<float, 4> full{};
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(&c, full.data(),
                                                       coda_array_ordering_c));
  EXPECT_THAT(full, ElementsAre(1.0f, 2.0f, 3.0f, 4.0f));

  std::fill(full.begin(), full.end(), -999.0f);
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float_array(
                              &c, full.data(), coda_array_ordering_fortran));
  EXPECT_THAT(full, ElementsAre(1.0f, 2.0f, 3.0f, 4.0f));

  std::array<float, 2> part{};
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_float_partial_array(&c, 1, 2, part.data()));
  EXPECT_THAT(part, ElementsAre(2.0f, 3.0f));

  // read_bits and read_bytes are unsupported on GRIB.
  uint8_t dummy = 0;
  EXPECT_EQ(-1, coda_cursor_read_bits(&elem, &dummy, 0, 8));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
  EXPECT_EQ(-1, coda_cursor_read_bytes(&elem, &dummy, 0, 1));
  EXPECT_EQ(CODA_ERROR_INVALID_TYPE, coda_errno);
}

TEST(CodaCursorReadTest, Hdf4BackendReturnsNoHdf4SupportAcrossAllDispatchers) {
  const ScopedCodaOptions restore_options;
  const ScopedEnvVar restore_def("CODA_DEFINITION");
  const ScopedEnvVar restore_leap("CODA_LEAP_SECOND_TABLE");
  restore_def.Unset();
  restore_leap.Unset();
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());
  const absl::Cleanup clear_err = [] { coda_set_error(CODA_SUCCESS, nullptr); };

  std::vector<ScopedType> owned_types;
  ScopedProduct product = MakeSyntheticMemProduct();
  ASSERT_NE(nullptr, product.get());
  const absl::Cleanup detach_root = [&] { product.get()->root_type = nullptr; };

  coda_dynamic_type fake_hdf4_type{};
  fake_hdf4_type.backend = coda_backend_hdf4;

  auto setup_scalar = [&](coda_type* def) -> coda_cursor {
    fake_hdf4_type.definition = def;
    product.get()->root_type = &fake_hdf4_type;
    coda_cursor c;
    EXPECT_EQ(CODA_SUCCESS, coda_cursor_set_product(&c, product.get()));
    return c;
  };

  auto setup_array = [&](coda_type* elem_def) -> coda_cursor {
    auto* arr_def = coda_type_array_new(coda_format_binary);
    EXPECT_NE(nullptr, arr_def);
    TrackType(arr_def, owned_types);
    EXPECT_EQ(CODA_SUCCESS, coda_type_array_set_base_type(arr_def, elem_def));
    EXPECT_EQ(CODA_SUCCESS, coda_type_array_add_fixed_dimension(arr_def, 2));
    return setup_scalar(reinterpret_cast<coda_type*>(arr_def));
  };

  coda_type* t_i8 = reinterpret_cast<coda_type*>(
      MakeNumberType(coda_format_binary, coda_integer_class,
                     coda_native_type_int8, 8, owned_types));
  coda_type* t_u8 = reinterpret_cast<coda_type*>(
      MakeNumberType(coda_format_binary, coda_integer_class,
                     coda_native_type_uint8, 8, owned_types));
  coda_type* t_i16 = reinterpret_cast<coda_type*>(
      MakeNumberType(coda_format_binary, coda_integer_class,
                     coda_native_type_int16, 16, owned_types));
  coda_type* t_u16 = reinterpret_cast<coda_type*>(
      MakeNumberType(coda_format_binary, coda_integer_class,
                     coda_native_type_uint16, 16, owned_types));
  coda_type* t_i32 = reinterpret_cast<coda_type*>(
      MakeNumberType(coda_format_binary, coda_integer_class,
                     coda_native_type_int32, 32, owned_types));
  coda_type* t_u32 = reinterpret_cast<coda_type*>(
      MakeNumberType(coda_format_binary, coda_integer_class,
                     coda_native_type_uint32, 32, owned_types));
  coda_type* t_i64 = reinterpret_cast<coda_type*>(
      MakeNumberType(coda_format_binary, coda_integer_class,
                     coda_native_type_int64, 64, owned_types));
  coda_type* t_u64 = reinterpret_cast<coda_type*>(
      MakeNumberType(coda_format_binary, coda_integer_class,
                     coda_native_type_uint64, 64, owned_types));
  coda_type* t_f32 = reinterpret_cast<coda_type*>(
      MakeNumberType(coda_format_binary, coda_real_class,
                     coda_native_type_float, 32, owned_types));
  coda_type* t_f64 = reinterpret_cast<coda_type*>(
      MakeNumberType(coda_format_binary, coda_real_class,
                     coda_native_type_double, 64, owned_types));
  coda_type* t_ch = reinterpret_cast<coda_type*>(
      MakeCharType(coda_format_binary, owned_types));
  coda_type* t_str = reinterpret_cast<coda_type*>(
      MakeStringType(coda_format_binary, 4, owned_types));

  // Converted int16 type to exercise conversion failure propagation on HDF4.
  coda_type_number* t_conv_num =
      MakeNumberType(coda_format_binary, coda_integer_class,
                     coda_native_type_int16, 16, owned_types);
  ASSERT_NE(nullptr, t_conv_num);
  coda_conversion* conv = coda_conversion_new(2.0, 1.0, 0.0, -99.0);
  ASSERT_NE(nullptr, conv);
  ASSERT_EQ(CODA_SUCCESS, coda_type_number_set_conversion(t_conv_num, conv));

  // Disable boundary checks so partial-array functions reach
  // read_*_partial_array instead of stopping at coda_cursor_get_num_elements().
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));

  int8_t v_i8 = 0;
  uint8_t v_u8 = 0;
  int16_t v_i16 = 0;
  uint16_t v_u16 = 0;
  int32_t v_i32 = 0;
  uint32_t v_u32 = 0;
  int64_t v_i64 = 0;
  uint64_t v_u64 = 0;
  float v_f32 = 0.0f;
  double v_f64 = 0.0;
  char v_ch = '\0';
  char v_str[8]{};

  // String scalar HDF4 check.
  {
    const coda_cursor c = setup_scalar(t_str);
    EXPECT_EQ(-1, coda_cursor_read_string(&c, v_str, sizeof(v_str)));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);
  }
  // Converted float scalar & array/partial-array HDF4 checks.
  {
    const coda_cursor c =
        setup_scalar(reinterpret_cast<coda_type*>(t_conv_num));
    EXPECT_EQ(-1, coda_cursor_read_float(&c, &v_f32));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);

    const coda_cursor ca =
        setup_array(reinterpret_cast<coda_type*>(t_conv_num));
    // Re-enable boundary checks once to cover get_num_elements failure in
    // coda_cursor_read_float_array() conversion branch.
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(1));
    std::array<float, 2> fa{};
    EXPECT_EQ(-1, coda_cursor_read_float_array(&ca, fa.data(),
                                               coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);
    ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(0));

    EXPECT_EQ(-1, coda_cursor_read_float_partial_array(&ca, 0, 2, fa.data()));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);

    std::array<double, 2> da{};
    EXPECT_EQ(-1, coda_cursor_read_double_array(&ca, da.data(),
                                                coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);
    EXPECT_EQ(-1, coda_cursor_read_double_partial_array(&ca, 0, 2, da.data()));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);
  }

  // Test all scalar/array/partial-array dispatchers for HDF4 error returns,
  // including every widening failure path.
  auto test_all_hdf4_signed = [&](coda_type* def) {
    const coda_cursor cs = setup_scalar(def);
    if (def == t_i8) {
      EXPECT_EQ(-1, coda_cursor_read_int8(&cs, &v_i8));
      EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);
    }
    if (def == t_i8 || def == t_i16) {
      EXPECT_EQ(-1, coda_cursor_read_int16(&cs, &v_i16));
      EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);
    }
    if (def == t_i8 || def == t_i16 || def == t_i32) {
      EXPECT_EQ(-1, coda_cursor_read_int32(&cs, &v_i32));
      EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);
    }
    if (def == t_i8 || def == t_i16 || def == t_i32 || def == t_i64) {
      EXPECT_EQ(-1, coda_cursor_read_int64(&cs, &v_i64));
      EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);
    }
    EXPECT_EQ(-1, coda_cursor_read_float(&cs, &v_f32));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);
    EXPECT_EQ(-1, coda_cursor_read_double(&cs, &v_f64));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);

    const coda_cursor ca = setup_array(def);
    std::array<double, 2> buf{};
    auto* p8 = reinterpret_cast<int8_t*>(buf.data());
    auto* p16 = reinterpret_cast<int16_t*>(buf.data());
    auto* p32 = reinterpret_cast<int32_t*>(buf.data());
    auto* p64 = reinterpret_cast<int64_t*>(buf.data());
    auto* pf = reinterpret_cast<float*>(buf.data());

    if (def == t_i8) {
      EXPECT_EQ(-1,
                coda_cursor_read_int8_array(&ca, p8, coda_array_ordering_c));
      EXPECT_EQ(-1, coda_cursor_read_int8_partial_array(&ca, 0, 2, p8));
    }
    if (def == t_i8 || def == t_i16) {
      EXPECT_EQ(-1,
                coda_cursor_read_int16_array(&ca, p16, coda_array_ordering_c));
      EXPECT_EQ(-1, coda_cursor_read_int16_partial_array(&ca, 0, 2, p16));
    }
    if (def == t_i8 || def == t_i16 || def == t_i32) {
      EXPECT_EQ(-1,
                coda_cursor_read_int32_array(&ca, p32, coda_array_ordering_c));
      EXPECT_EQ(-1, coda_cursor_read_int32_partial_array(&ca, 0, 2, p32));
    }
    if (def == t_i8 || def == t_i16 || def == t_i32 || def == t_i64) {
      EXPECT_EQ(-1,
                coda_cursor_read_int64_array(&ca, p64, coda_array_ordering_c));
      EXPECT_EQ(-1, coda_cursor_read_int64_partial_array(&ca, 0, 2, p64));
    }
    EXPECT_EQ(-1, coda_cursor_read_float_array(&ca, pf, coda_array_ordering_c));
    EXPECT_EQ(-1, coda_cursor_read_float_partial_array(&ca, 0, 2, pf));
    EXPECT_EQ(-1, coda_cursor_read_double_array(&ca, buf.data(),
                                                coda_array_ordering_c));
    EXPECT_EQ(-1, coda_cursor_read_double_partial_array(&ca, 0, 2, buf.data()));
  };

  for (coda_type* def : {t_i8, t_i16, t_i32, t_i64, t_f32, t_f64}) {
    test_all_hdf4_signed(def);
  }

  auto test_all_hdf4_unsigned = [&](coda_type* def) {
    const coda_cursor cs = setup_scalar(def);
    if (def == t_u8) {
      EXPECT_EQ(-1, coda_cursor_read_uint8(&cs, &v_u8));
      EXPECT_EQ(-1, coda_cursor_read_int16(&cs, &v_i16));
    }
    if (def == t_u8 || def == t_u16) {
      EXPECT_EQ(-1, coda_cursor_read_uint16(&cs, &v_u16));
      EXPECT_EQ(-1, coda_cursor_read_int32(&cs, &v_i32));
    }
    if (def == t_u8 || def == t_u16 || def == t_u32) {
      EXPECT_EQ(-1, coda_cursor_read_uint32(&cs, &v_u32));
      EXPECT_EQ(-1, coda_cursor_read_int64(&cs, &v_i64));
    }
    EXPECT_EQ(-1, coda_cursor_read_uint64(&cs, &v_u64));
    EXPECT_EQ(-1, coda_cursor_read_float(&cs, &v_f32));
    EXPECT_EQ(-1, coda_cursor_read_double(&cs, &v_f64));

    const coda_cursor ca = setup_array(def);
    std::array<uint64_t, 2> buf{};
    auto* p8 = reinterpret_cast<uint8_t*>(buf.data());
    auto* pi16 = reinterpret_cast<int16_t*>(buf.data());
    auto* p16 = reinterpret_cast<uint16_t*>(buf.data());
    auto* pi32 = reinterpret_cast<int32_t*>(buf.data());
    auto* p32 = reinterpret_cast<uint32_t*>(buf.data());
    auto* pi64 = reinterpret_cast<int64_t*>(buf.data());
    auto* pf = reinterpret_cast<float*>(buf.data());
    auto* pd = reinterpret_cast<double*>(buf.data());

    if (def == t_u8) {
      EXPECT_EQ(-1,
                coda_cursor_read_uint8_array(&ca, p8, coda_array_ordering_c));
      EXPECT_EQ(-1, coda_cursor_read_uint8_partial_array(&ca, 0, 2, p8));
      EXPECT_EQ(-1,
                coda_cursor_read_int16_array(&ca, pi16, coda_array_ordering_c));
      EXPECT_EQ(-1, coda_cursor_read_int16_partial_array(&ca, 0, 2, pi16));
    }
    if (def == t_u8 || def == t_u16) {
      EXPECT_EQ(-1,
                coda_cursor_read_uint16_array(&ca, p16, coda_array_ordering_c));
      EXPECT_EQ(-1, coda_cursor_read_uint16_partial_array(&ca, 0, 2, p16));
      EXPECT_EQ(-1,
                coda_cursor_read_int32_array(&ca, pi32, coda_array_ordering_c));
      EXPECT_EQ(-1, coda_cursor_read_int32_partial_array(&ca, 0, 2, pi32));
    }
    if (def == t_u8 || def == t_u16 || def == t_u32) {
      EXPECT_EQ(-1,
                coda_cursor_read_uint32_array(&ca, p32, coda_array_ordering_c));
      EXPECT_EQ(-1, coda_cursor_read_uint32_partial_array(&ca, 0, 2, p32));
      EXPECT_EQ(-1,
                coda_cursor_read_int64_array(&ca, pi64, coda_array_ordering_c));
      EXPECT_EQ(-1, coda_cursor_read_int64_partial_array(&ca, 0, 2, pi64));
    }
    EXPECT_EQ(-1, coda_cursor_read_uint64_array(&ca, buf.data(),
                                                coda_array_ordering_c));
    EXPECT_EQ(-1, coda_cursor_read_uint64_partial_array(&ca, 0, 2, buf.data()));
    EXPECT_EQ(-1, coda_cursor_read_float_array(&ca, pf, coda_array_ordering_c));
    EXPECT_EQ(-1, coda_cursor_read_float_partial_array(&ca, 0, 2, pf));
    EXPECT_EQ(-1,
              coda_cursor_read_double_array(&ca, pd, coda_array_ordering_c));
    EXPECT_EQ(-1, coda_cursor_read_double_partial_array(&ca, 0, 2, pd));
  };

  for (coda_type* def : {t_u8, t_u16, t_u32, t_u64}) {
    test_all_hdf4_unsigned(def);
  }

  {
    const coda_cursor cs = setup_scalar(t_ch);
    EXPECT_EQ(-1, coda_cursor_read_char(&cs, &v_ch));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);

    const coda_cursor ca = setup_array(t_ch);
    std::array<char, 2> ch_buf{};
    EXPECT_EQ(-1, coda_cursor_read_char_array(&ca, ch_buf.data(),
                                              coda_array_ordering_c));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);
    EXPECT_EQ(-1,
              coda_cursor_read_char_partial_array(&ca, 0, 2, ch_buf.data()));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);
  }

  // With boundary checks enabled, every partial-array function fails in
  // coda_cursor_get_num_elements() before reaching read_*_partial_array().
  ASSERT_EQ(CODA_SUCCESS, coda_set_option_perform_boundary_checks(1));
  std::array<uint64_t, 2> dummy_buf{};
  auto check_bounds_fail_hdf4 = [&](coda_type* elem_def, auto partial_fn,
                                    auto* ptr) {
    const coda_cursor ca = setup_array(elem_def);
    EXPECT_EQ(-1, partial_fn(&ca, 0, 2, ptr));
    EXPECT_EQ(CODA_ERROR_NO_HDF4_SUPPORT, coda_errno);
  };
  check_bounds_fail_hdf4(t_i8, coda_cursor_read_int8_partial_array,
                         reinterpret_cast<int8_t*>(dummy_buf.data()));
  check_bounds_fail_hdf4(t_u8, coda_cursor_read_uint8_partial_array,
                         reinterpret_cast<uint8_t*>(dummy_buf.data()));
  check_bounds_fail_hdf4(t_i16, coda_cursor_read_int16_partial_array,
                         reinterpret_cast<int16_t*>(dummy_buf.data()));
  check_bounds_fail_hdf4(t_u16, coda_cursor_read_uint16_partial_array,
                         reinterpret_cast<uint16_t*>(dummy_buf.data()));
  check_bounds_fail_hdf4(t_i32, coda_cursor_read_int32_partial_array,
                         reinterpret_cast<int32_t*>(dummy_buf.data()));
  check_bounds_fail_hdf4(t_u32, coda_cursor_read_uint32_partial_array,
                         reinterpret_cast<uint32_t*>(dummy_buf.data()));
  check_bounds_fail_hdf4(t_i64, coda_cursor_read_int64_partial_array,
                         reinterpret_cast<int64_t*>(dummy_buf.data()));
  check_bounds_fail_hdf4(t_u64, coda_cursor_read_uint64_partial_array,
                         dummy_buf.data());
  check_bounds_fail_hdf4(t_f32, coda_cursor_read_float_partial_array,
                         reinterpret_cast<float*>(dummy_buf.data()));
  check_bounds_fail_hdf4(t_f64, coda_cursor_read_double_partial_array,
                         reinterpret_cast<double*>(dummy_buf.data()));
  check_bounds_fail_hdf4(t_ch, coda_cursor_read_char_partial_array,
                         reinterpret_cast<char*>(dummy_buf.data()));
}

// TODO(schwehr): 92.5% line coverage in libcoda/coda-cursor-read.c (2346/2535);
// remaining missed lines are disabled HDF4 dispatch arms and redundant
// defensive cursor validation checks.

}  // namespace
}  // namespace stcorp_coda
