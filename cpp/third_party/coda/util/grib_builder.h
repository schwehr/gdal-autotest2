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
// Minimal in-memory GRIB Edition 1 and Edition 2 binary builder for
// synthesizing valid and malformed GRIB files (including multi-message and
// mixed-edition streams) without checked-in data.
//
// Byte offsets, sign-magnitude fields, IEEE/IBM float encodings, template
// numbers, and section rules match third_party/stcorp_coda/libcoda/coda-grib.c.

#ifndef THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_GRIB_BUILDER_H_
#define THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_GRIB_BUILDER_H_

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace stcorp_coda {

// Big-endian bit-stream writer matching CODA's MSB-first read_bits().
class GribBitWriter {
 public:
  void WriteBit(bool bit) {
    if (bit_pos_ == 0) {
      bytes_.push_back(0);
    }
    if (bit) {
      bytes_.back() |= static_cast<uint8_t>(1u << (7 - bit_pos_));
    }
    bit_pos_ = (bit_pos_ + 1) & 7;
  }

  void WriteBits(uint64_t value, int num_bits) {
    const int n = std::clamp(num_bits, 0, 64);
    for (int i = n - 1; i >= 0; --i) {
      WriteBit(((value >> i) & 1ULL) != 0);
    }
  }

  void AlignToByte() { bit_pos_ = 0; }

  uint8_t UnusedBitsInLastByte() const {
    return bit_pos_ == 0 ? 0 : static_cast<uint8_t>(8 - bit_pos_);
  }

  const std::vector<uint8_t>& bytes() const { return bytes_; }
  std::vector<uint8_t> TakeBytes() {
    bit_pos_ = 0;
    return std::move(bytes_);
  }

 private:
  std::vector<uint8_t> bytes_;
  int bit_pos_ = 0;
};

namespace grib_detail {

inline void AppendBe16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

inline void AppendBe24(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

inline void AppendBe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

inline void AppendBe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
  }
}

inline void PatchBe24(std::vector<uint8_t>& out, size_t offset, uint32_t v) {
  out[offset + 0] = static_cast<uint8_t>((v >> 16) & 0xFF);
  out[offset + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  out[offset + 2] = static_cast<uint8_t>(v & 0xFF);
}

inline void PatchBe32(std::vector<uint8_t>& out, size_t offset, uint32_t v) {
  out[offset + 0] = static_cast<uint8_t>((v >> 24) & 0xFF);
  out[offset + 1] = static_cast<uint8_t>((v >> 16) & 0xFF);
  out[offset + 2] = static_cast<uint8_t>((v >> 8) & 0xFF);
  out[offset + 3] = static_cast<uint8_t>(v & 0xFF);
}

inline void PatchBe64(std::vector<uint8_t>& out, size_t offset, uint64_t v) {
  for (int b = 0; b < 8; ++b) {
    out[offset + static_cast<size_t>(b)] =
        static_cast<uint8_t>((v >> (56 - 8 * b)) & 0xFF);
  }
}

inline void AppendSignMag16(std::vector<uint8_t>& out, int16_t v) {
  const bool neg = v < 0;
  const int32_t mag = neg ? -static_cast<int32_t>(v) : static_cast<int32_t>(v);
  const uint16_t encoded = static_cast<uint16_t>(
      (neg ? 0x8000u : 0u) | (static_cast<uint32_t>(mag) & 0x7FFFu));
  AppendBe16(out, encoded);
}

inline void AppendSignMag24(std::vector<uint8_t>& out, int32_t v) {
  const bool neg = v < 0;
  const int64_t mag = neg ? -static_cast<int64_t>(v) : static_cast<int64_t>(v);
  const uint32_t encoded =
      (neg ? 0x800000u : 0u) | (static_cast<uint32_t>(mag) & 0x7FFFFFu);
  AppendBe24(out, encoded);
}

inline void AppendSignMag32(std::vector<uint8_t>& out, int32_t v) {
  const bool neg = v < 0;
  const int64_t mag = neg ? -static_cast<int64_t>(v) : static_cast<int64_t>(v);
  const uint32_t encoded =
      (neg ? 0x80000000u : 0u) | (static_cast<uint32_t>(mag) & 0x7FFFFFFFu);
  AppendBe32(out, encoded);
}

inline void AppendIeeeFloat32Be(std::vector<uint8_t>& out, float v) {
  static_assert(sizeof(float) == sizeof(uint32_t));
  AppendBe32(out, std::bit_cast<uint32_t>(v));
}

// Encodes an IEEE 754 float32 into big-endian IBM System/360 32-bit float
// (1 sign bit, 7-bit base-16 exponent with bias 64, 24-bit mantissa) matching
// ibmfloat_to_iee754() in coda-grib.c.
inline std::array<uint8_t, 4> EncodeIbmFloat32(float v) {
  static_assert(sizeof(float) == sizeof(uint32_t));
  const uint32_t bits = std::bit_cast<uint32_t>(v);
  const uint8_t sign = static_cast<uint8_t>((bits >> 31) & 1u);
  const uint32_t ieee_exp = (bits >> 23) & 0xFFu;
  const uint32_t ieee_mant = bits & 0x7FFFFFu;

  if (ieee_exp == 0 && ieee_mant == 0) {
    return {0, 0, 0, 0};
  }
  if (ieee_exp == 0xFFu) {
    // Clamp Inf/NaN to max finite IBM float magnitude.
    return {static_cast<uint8_t>((static_cast<uint32_t>(sign) << 7) | 0x7Fu),
            0xFF, 0xFF, 0xFF};
  }

  int unbiased_exp2 = 0;
  uint32_t sig = ieee_mant;
  if (ieee_exp == 0) {
    // Subnormal: value = sig * 2^(-23) * 2^(-126)
    unbiased_exp2 = -126;
    while ((sig & 0x800000u) == 0) {
      sig <<= 1;
      --unbiased_exp2;
    }
  } else {
    unbiased_exp2 = static_cast<int>(ieee_exp) - 127;
    sig |= 0x800000u;
  }

  // Value = (-1)^sign * (sig * 2^-24) * 2^(unbiased_exp2 + 1).
  const int e2 = unbiased_exp2 + 1;
  // Choose k = ceil(e2 / 4) so shift = 4*k - e2 is in [0, 3].
  const int k = (e2 >= 0) ? ((e2 + 3) / 4) : (e2 / 4);
  const int shift = 4 * k - e2;
  int ibm_exp = k + 64;
  uint32_t ibm_mant = sig >> shift;

  // Round to nearest when bits were shifted out.
  if (shift > 0) {
    const uint32_t rem = sig & ((1u << shift) - 1u);
    const uint32_t half = 1u << (shift - 1);
    if (rem > half || (rem == half && (ibm_mant & 1u) != 0)) {
      ++ibm_mant;
      if (ibm_mant > 0xFFFFFFu) {
        ibm_mant >>= 4;
        ++ibm_exp;
      }
    }
  }

  if (ibm_exp > 127) {
    return {static_cast<uint8_t>((static_cast<uint32_t>(sign) << 7) | 0x7Fu),
            0xFF, 0xFF, 0xFF};
  }
  if (ibm_exp < 0) {
    const int underflow_shift = 4 * (-ibm_exp);
    if (underflow_shift >= 24) {
      return {0, 0, 0, 0};
    }
    ibm_mant >>= underflow_shift;
    ibm_exp = 0;
    if (ibm_mant == 0) {
      return {0, 0, 0, 0};
    }
  }

  return {
      static_cast<uint8_t>((static_cast<uint32_t>(sign) << 7) |
                           (static_cast<uint32_t>(ibm_exp) & 0x7Fu)),
      static_cast<uint8_t>((ibm_mant >> 16) & 0xFFu),
      static_cast<uint8_t>((ibm_mant >> 8) & 0xFFu),
      static_cast<uint8_t>(ibm_mant & 0xFFu),
  };
}

inline void AppendIbmFloat32(std::vector<uint8_t>& out, float v) {
  const auto bytes = EncodeIbmFloat32(v);
  out.insert(out.end(), bytes.begin(), bytes.end());
}

template <typename MaskRange>
inline std::vector<uint8_t> PackBitmap(const MaskRange& mask,
                                       uint8_t* out_unused_bits = nullptr) {
  GribBitWriter writer;
  for (bool present : mask) {
    writer.WriteBit(present);
  }
  if (out_unused_bits != nullptr) {
    *out_unused_bits = writer.UnusedBitsInLastByte();
  }
  return writer.TakeBytes();
}

inline size_t PopCountBytes(std::span<const uint8_t> bytes) {
  size_t count = 0;
  for (uint8_t b : bytes) {
    count += static_cast<size_t>(std::popcount(b));
  }
  return count;
}

inline std::vector<uint8_t> PackSimpleInts(std::span<const uint64_t> codes,
                                           int bits_per_value,
                                           uint8_t* out_unused_bits = nullptr) {
  if (bits_per_value <= 0) {
    if (out_unused_bits != nullptr) {
      *out_unused_bits = 0;
    }
    return {};
  }
  GribBitWriter writer;
  for (uint64_t code : codes) {
    writer.WriteBits(code, bits_per_value);
  }
  if (out_unused_bits != nullptr) {
    *out_unused_bits = writer.UnusedBitsInLastByte();
  }
  return writer.TakeBytes();
}

inline std::vector<uint8_t> PackSimpleValues(
    std::span<const float> values, float reference_value,
    int16_t binary_scale_factor, int16_t decimal_scale_factor,
    int bits_per_value, uint8_t* out_unused_bits = nullptr) {
  if (bits_per_value <= 0) {
    if (out_unused_bits != nullptr) {
      *out_unused_bits = 0;
    }
    return {};
  }
  const int b = std::min(bits_per_value, 63);
  const uint64_t max_code = (uint64_t{1} << b) - 1ULL;
  const double dec_scale =
      std::pow(10.0, static_cast<int>(decimal_scale_factor));

  GribBitWriter writer;
  for (float v : values) {
    const double scaled = static_cast<double>(v) * dec_scale -
                          static_cast<double>(reference_value);
    const double unscaled =
        std::ldexp(scaled, -static_cast<int>(binary_scale_factor));
    const long long rounded = std::llround(unscaled);
    uint64_t code = 0;
    if (rounded > 0) {
      const uint64_t u = static_cast<uint64_t>(rounded);
      code = std::min(u, max_code);
    }
    writer.WriteBits(code, bits_per_value);
  }
  if (out_unused_bits != nullptr) {
    *out_unused_bits = writer.UnusedBitsInLastByte();
  }
  return writer.TakeBytes();
}

}  // namespace grib_detail

// ---------------------------------------------------------------------------
// GRIB Edition 1 specification structures
// ---------------------------------------------------------------------------

struct Grib1GdsSpec {
  // Supported by coda-grib.c: 0, 4, 10, 14, 20, 24, 30, 34.
  uint8_t data_representation_type = 0;
  uint16_t ni = 2;  // 65535 = missing (quasi-regular grid)
  uint16_t nj = 2;  // 65535 = missing (quasi-regular grid)
  int32_t latitude_first = 0;
  int32_t longitude_first = 0;
  uint8_t resolution_and_component_flags = 0x80;
  int32_t latitude_last = 1000;
  int32_t longitude_last = 1000;
  uint16_t i_direction_increment = 1000;
  // Interpreted as N for Gaussian grids (4, 14, 24, 34), or Dj otherwise.
  uint16_t j_direction_increment_or_n = 1000;
  uint8_t scanning_mode = 0;
  std::array<uint8_t, 4> reserved = {0, 0, 0, 0};
  std::vector<uint8_t> extra_template_bytes = {};
  std::vector<float> coordinate_values =
      {};  // Encoded as IBM float32; NV = size().
  std::vector<uint16_t> list_of_numbers = {};  // Row lengths for reduced grids.
  // Overrides GDS byte 4 (1-based offset of PV/PL list; 255 = absent).
  // When std::nullopt, automatically 33 + extra_template_bytes.size() if
  // coordinate_values or list_of_numbers is non-empty, else 255.
  std::optional<uint8_t> pvl_override = std::nullopt;
};

struct Grib1BmsSpec {
  std::vector<bool> bitmap = {};
  std::vector<uint8_t> raw_bytes =
      {};  // Takes precedence over `bitmap` if non-empty.
  uint16_t predefined_bitmap = 0;  // Must be 0 for CODA; non-zero tests error.
  std::optional<uint8_t> unused_bits_override = std::nullopt;
};

struct Grib1BdsSpec {
  // High nibble flags: 0x80=spherical harmonic, 0x40=complex, 0x20=int,
  // 0x10=extra.
  uint8_t flags = 0;
  int16_t binary_scale_factor = 0;
  float reference_value = 0.0f;  // Encoded as IBM float32 in GRIB1.
  uint8_t bits_per_value = 8;
  // Payload source precedence:
  // 1. raw_packed_bytes (exact payload after the 11-byte BDS header)
  // 2. packed_ints (unsigned integer codes packed at bits_per_value)
  // 3. values (floats packed using reference_value, binary/decimal scale
  // factors)
  // 4. auto zero-padded payload matching grid/bitmap element count.
  std::vector<uint8_t> raw_packed_bytes = {};
  std::vector<uint64_t> packed_ints = {};
  std::vector<float> values = {};
  std::optional<uint8_t> unused_bits_override = std::nullopt;
};

struct Grib1MessageSpec {
  // Section 0
  uint8_t edition_override = 1;

  // Section 1 (PDS)
  uint8_t table2_version = 3;
  uint8_t centre = 98;
  uint8_t generating_process_id = 1;
  uint8_t grid_definition = 255;  // Predefined grid ID when `gds` is absent.
  uint8_t pds_extra_flags = 0;  // Low 6 bits of PDS byte 7; 0x80/0x40 auto-set.
  uint8_t parameter = 11;
  uint8_t level_type = 1;
  uint16_t level = 0;
  uint8_t year_of_century = 26;
  uint8_t month = 1;
  uint8_t day = 1;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t unit_of_time_range = 1;
  uint8_t p1 = 0;
  uint8_t p2 = 0;
  uint8_t time_range_indicator = 0;
  uint16_t number_included_in_average = 0;
  uint8_t number_missing_from_averages = 0;
  uint8_t century = 21;
  uint8_t sub_centre = 0;
  int16_t decimal_scale_factor = 0;
  // Optional PDS extension beyond 28 bytes:
  // If local_use is non-empty, 12 reserved bytes (zero-padded from
  // pds_reserved_bytes) followed by local_use are emitted so section_size > 40.
  // If local_use is empty and pds_reserved_bytes is non-empty, those bytes are
  // emitted directly after byte 28.
  std::vector<uint8_t> pds_reserved_bytes = {};
  std::vector<uint8_t> local_use = {};

  // Section 2 (GDS), optional
  std::optional<Grib1GdsSpec> gds = Grib1GdsSpec{};
  // Section 3 (BMS), optional
  std::optional<Grib1BmsSpec> bms = std::nullopt;
  // Section 4 (BDS)
  Grib1BdsSpec bds = {};
  // Section 5 ('7777')
  std::array<uint8_t, 4> end_marker = {'7', '7', '7', '7'};
  // Overrides the 24-bit message length in Section 0 when set.
  std::optional<uint32_t> message_length_override = std::nullopt;
};

// ---------------------------------------------------------------------------
// GRIB Edition 2 specification structures
// ---------------------------------------------------------------------------

struct Grib2GridSpec {
  uint8_t source_of_grid_definition = 0;
  uint32_t number_of_data_points = 4;
  // When std::nullopt: 2 if list_of_numbers is non-empty, else 0.
  std::optional<uint8_t> number_size_override = std::nullopt;
  // When std::nullopt: 1 if list_of_numbers is non-empty, else 0.
  std::optional<uint8_t> interpretation_override = std::nullopt;
  // Supported by coda-grib.c: 0..3 (lat/lon), 40..43 (Gaussian).
  uint16_t template_number = 0;
  uint8_t shape_of_the_earth = 6;
  uint8_t scale_factor_radius = 0;
  uint32_t scaled_value_radius = 0;
  uint8_t scale_factor_major_axis = 0;
  uint32_t scaled_value_major_axis = 0;
  uint8_t scale_factor_minor_axis = 0;
  uint32_t scaled_value_minor_axis = 0;
  uint32_t ni = 2;  // 0xFFFFFFFF = missing
  uint32_t nj = 2;  // 0xFFFFFFFF = missing
  uint32_t basic_angle = 0;
  uint32_t subdivisions_of_basic_angle = 0xFFFFFFFFu;
  int32_t latitude_first = 0;
  int32_t longitude_first = 0;
  uint8_t resolution_and_component_flags = 0x30;
  int32_t latitude_last = 1000000;
  int32_t longitude_last = 1000000;
  uint32_t i_direction_increment = 1000000;
  // Interpreted as N when template_number in [40..43], or Dj otherwise.
  uint32_t j_direction_increment_or_n = 1000000;
  uint8_t scanning_mode = 0;
  std::vector<uint8_t> extra_template_bytes =
      {};  // Appended when interpretation == 0.
  std::vector<uint32_t> list_of_numbers = {};  // Quasi-regular row counts.
};

struct Grib2ProductSpec {
  // Supported by coda-grib.c: 0..6, 15, 40, 51. (Other values e.g. 8 test
  // error).
  uint16_t template_number = 0;
  uint8_t parameter_category = 0;
  uint8_t parameter_number = 0;
  uint16_t constituent_type = 0;  // Emitted for template_number == 40.
  uint8_t type_of_generating_process = 0;
  uint8_t background_process = 0;
  uint8_t generating_process_id = 0;
  uint16_t hours_after_data_cutoff = 0;
  uint8_t minutes_after_data_cutoff = 0;
  uint8_t indicator_of_unit_of_time_range = 1;
  uint32_t forecast_time = 0;
  uint8_t type_of_first_fixed_surface = 1;
  int8_t scale_factor_first_fixed_surface = 0;
  uint32_t scaled_value_first_fixed_surface = 0;
  uint8_t type_of_second_fixed_surface = 255;  // 255 -> NaN in CODA.
  int8_t scale_factor_second_fixed_surface = 0;
  uint32_t scaled_value_second_fixed_surface = 0;
  std::vector<float> coordinate_values = {};  // IEEE float32 big-endian.
  std::vector<uint8_t> extra_template_bytes = {};
};

struct Grib2DataRepSpec {
  uint32_t number_of_data_points = 4;
  // Supported by coda-grib.c: 0, 1. (Other values e.g. 4 test error).
  uint16_t template_number = 0;
  float reference_value = 0.0f;  // IEEE float32 big-endian in GRIB2.
  int16_t binary_scale_factor = 0;
  int16_t decimal_scale_factor = 0;
  uint8_t bits_per_value = 8;
  uint8_t type_of_original_field_values = 0;
  std::vector<uint8_t> extra_template_bytes = {};
};

struct Grib2BitmapSpec {
  // When std::nullopt: 0 if bitmap or raw_bytes is non-empty, else 255.
  // Explicit values: 0=inline, 254=reuse previous, 255=none,
  // others=unsupported.
  std::optional<uint8_t> indicator_override = std::nullopt;
  std::vector<bool> bitmap = {};
  std::vector<uint8_t> raw_bytes =
      {};  // Takes precedence over `bitmap` if non-empty.
};

struct Grib2DataSpec {
  // Payload source precedence:
  // 1. raw_packed_bytes (exact payload after the 5-byte Section 7 header)
  // 2. packed_ints (unsigned integer codes packed at bits_per_value)
  // 3. values (floats packed using Section 5 parameters)
  // 4. auto zero-padded payload matching data_rep.number_of_data_points /
  // bitmap.
  std::vector<uint8_t> raw_packed_bytes = {};
  std::vector<uint64_t> packed_ints = {};
  std::vector<float> values = {};
};

struct Grib2FieldSpec {
  // Section 2 (Local Use) preceding this field, if engaged. Empty vector emits
  // a 5-byte Section 2; std::nullopt omits Section 2.
  std::optional<std::vector<uint8_t>> local_use = std::nullopt;
  // Section 3 (Grid Definition), optional on repeated fields.
  std::optional<Grib2GridSpec> grid = Grib2GridSpec{};
  Grib2ProductSpec product = {};
  Grib2DataRepSpec data_rep = {};
  // Section 6 (Bit-Map), optional. std::nullopt omits Section 6 completely.
  std::optional<Grib2BitmapSpec> bitmap = Grib2BitmapSpec{};
  Grib2DataSpec data = {};
};

struct Grib2RawSection {
  uint8_t section_number = 0;
  std::vector<uint8_t> body = {};  // Bytes after the 5-byte section header.
};

struct Grib2MessageSpec {
  // Section 0
  uint8_t discipline = 0;
  uint8_t edition_override = 2;
  std::array<uint8_t, 2> section0_reserved = {0, 0};

  // Section 1 (Identification)
  uint8_t section1_number_override = 1;
  uint16_t centre = 98;
  uint16_t sub_centre = 0;
  uint8_t master_tables_version = 2;
  uint8_t local_tables_version = 1;
  uint8_t significance_of_reference_time = 1;
  uint16_t year = 2026;
  uint8_t month = 1;
  uint8_t day = 1;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint8_t production_status = 0;
  uint8_t type_of_processed_data = 1;
  std::vector<uint8_t> ident_extra_bytes = {};

  // Single-field convenience (used when `fields` and `raw_sections` are empty).
  std::optional<std::vector<uint8_t>> local_use = std::nullopt;
  std::optional<Grib2GridSpec> grid = Grib2GridSpec{};
  Grib2ProductSpec product = {};
  Grib2DataRepSpec data_rep = {};
  std::optional<Grib2BitmapSpec> bitmap = Grib2BitmapSpec{};
  Grib2DataSpec data = {};

  // Multi-field sequence (if non-empty, overrides the single-field members).
  std::vector<Grib2FieldSpec> fields = {};

  // Arbitrary raw sections 2..7 (or invalid section numbers) emitted instead of
  // `fields` / single-field members when non-empty.
  std::vector<Grib2RawSection> raw_sections = {};
  // When true, omits sections 2..7 (producing an empty GRIB2 message with only
  // Section 0, Section 1, and Section 8).
  bool omit_data_sections = false;

  // Section 8 ('7777')
  std::array<uint8_t, 4> end_marker = {'7', '7', '7', '7'};
  // Overrides the 64-bit message length in Section 0 when set.
  std::optional<uint64_t> message_length_override = std::nullopt;
};

// ---------------------------------------------------------------------------
// GribBuilder
// ---------------------------------------------------------------------------

class GribBuilder {
 public:
  void AddFiller(std::span<const uint8_t> bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
  }

  void AddFiller(std::string_view text) {
    buffer_.insert(buffer_.end(), text.begin(), text.end());
  }

  void AddGrib1(const Grib1MessageSpec& spec) {
    AppendGrib1Message(buffer_, spec);
  }

  void AddGrib2(const Grib2MessageSpec& spec) {
    AppendGrib2Message(buffer_, spec);
  }

  std::vector<uint8_t> Build() const { return buffer_; }

  static std::vector<uint8_t> BuildGrib1(const Grib1MessageSpec& spec) {
    std::vector<uint8_t> out;
    AppendGrib1Message(out, spec);
    return out;
  }

  static std::vector<uint8_t> BuildGrib2(const Grib2MessageSpec& spec) {
    std::vector<uint8_t> out;
    AppendGrib2Message(out, spec);
    return out;
  }

  // Re-export encoding helpers for callers checking raw values or building
  // custom corrupt payloads.
  static std::array<uint8_t, 4> EncodeIbmFloat32(float v) {
    return grib_detail::EncodeIbmFloat32(v);
  }

  static std::vector<uint8_t> PackSimpleValues(
      std::span<const float> values, float reference_value,
      int16_t binary_scale_factor, int16_t decimal_scale_factor,
      int bits_per_value, uint8_t* out_unused_bits = nullptr) {
    return grib_detail::PackSimpleValues(
        values, reference_value, binary_scale_factor, decimal_scale_factor,
        bits_per_value, out_unused_bits);
  }

  static std::vector<uint8_t> PackSimpleInts(
      std::span<const uint64_t> codes, int bits_per_value,
      uint8_t* out_unused_bits = nullptr) {
    return grib_detail::PackSimpleInts(codes, bits_per_value, out_unused_bits);
  }

  template <typename MaskRange>
  static std::vector<uint8_t> PackBitmap(const MaskRange& mask,
                                         uint8_t* out_unused_bits = nullptr) {
    return grib_detail::PackBitmap(mask, out_unused_bits);
  }

 private:
  static size_t PredefinedGrib1GridSize(uint8_t grid_definition) {
    switch (grid_definition) {
      case 1:
        return 1679;
      case 2:
        return 10512;
      case 3:
        return 65160;
      case 4:
        return 259920;
      case 5:
      case 6:
        return 2385;
      case 8:
        return 5104;
      case 21:
      case 22:
      case 23:
      case 24:
        return 1333;
      case 25:
      case 26:
        return 1297;
      case 27:
      case 28:
        return 4225;
      case 29:
      case 30:
        return 5365;
      case 33:
      case 34:
        return 8326;
      case 50:
        return 964;
      case 53:
        return 5967;
      case 55:
      case 56:
        return 6177;
      case 61:
      case 62:
      case 63:
      case 64:
        return 4096;
      case 75:
      case 76:
      case 77:
        return 12321;
      case 85:
      case 86:
        return 32400;
      case 87:
        return 5022;
      case 90:
        return 12902;
      case 91:
        return 25803;
      case 92:
        return 81213;
      case 93:
        return 162425;
      case 94:
        return 48916;
      case 95:
        return 97831;
      case 96:
        return 41630;
      case 97:
        return 83259;
      case 100:
        return 6889;
      case 101:
        return 10283;
      case 103:
        return 3640;
      case 104:
        return 16170;
      case 105:
        return 6889;
      case 106:
        return 19305;
      case 107:
        return 11040;
      default:
        return 0;
    }
  }

  static void AppendGrib1Message(std::vector<uint8_t>& out,
                                 const Grib1MessageSpec& spec) {
    const size_t msg_start = out.size();

    // Section 0: Indicator Section (8 bytes)
    out.push_back('G');
    out.push_back('R');
    out.push_back('I');
    out.push_back('B');
    const size_t len_offset = out.size();
    grib_detail::AppendBe24(out, 0);  // Patched at end.
    out.push_back(spec.edition_override);

    // Section 1: Product Definition Section (PDS)
    const size_t pds_start = out.size();
    grib_detail::AppendBe24(out, 0);  // Patched below.
    out.push_back(spec.table2_version);
    out.push_back(spec.centre);
    out.push_back(spec.generating_process_id);
    out.push_back(spec.grid_definition);
    const uint8_t pds_flags = static_cast<uint8_t>(
        (spec.gds.has_value() ? 0x80u : 0u) |
        (spec.bms.has_value() ? 0x40u : 0u) | (spec.pds_extra_flags & 0x3Fu));
    out.push_back(pds_flags);
    out.push_back(spec.parameter);
    out.push_back(spec.level_type);
    grib_detail::AppendBe16(out, spec.level);
    out.push_back(spec.year_of_century);
    out.push_back(spec.month);
    out.push_back(spec.day);
    out.push_back(spec.hour);
    out.push_back(spec.minute);
    out.push_back(spec.unit_of_time_range);
    out.push_back(spec.p1);
    out.push_back(spec.p2);
    out.push_back(spec.time_range_indicator);
    grib_detail::AppendBe16(out, spec.number_included_in_average);
    out.push_back(spec.number_missing_from_averages);
    out.push_back(spec.century);
    out.push_back(spec.sub_centre);
    grib_detail::AppendSignMag16(out, spec.decimal_scale_factor);
    if (!spec.local_use.empty()) {
      // Bytes 28..39 are 12 reserved bytes in CODA before `local`.
      for (size_t i = 0; i < 12; ++i) {
        out.push_back(i < spec.pds_reserved_bytes.size()
                          ? spec.pds_reserved_bytes[i]
                          : 0);
      }
      out.insert(out.end(), spec.local_use.begin(), spec.local_use.end());
    } else if (!spec.pds_reserved_bytes.empty()) {
      out.insert(out.end(), spec.pds_reserved_bytes.begin(),
                 spec.pds_reserved_bytes.end());
    }
    grib_detail::PatchBe24(out, pds_start,
                           static_cast<uint32_t>(out.size() - pds_start));

    // Section 2: Grid Description Section (GDS)
    size_t num_elements = 0;
    if (spec.gds.has_value()) {
      const Grib1GdsSpec& gds = *spec.gds;
      const size_t gds_start = out.size();
      const uint8_t pvl = gds.pvl_override.value_or(
          (!gds.coordinate_values.empty() || !gds.list_of_numbers.empty())
              ? static_cast<uint8_t>(33 + gds.extra_template_bytes.size())
              : 255);
      grib_detail::AppendBe24(out, 0);  // Patched below.
      out.push_back(static_cast<uint8_t>(gds.coordinate_values.size()));
      out.push_back(pvl);
      out.push_back(gds.data_representation_type);
      grib_detail::AppendBe16(out, gds.ni);
      grib_detail::AppendBe16(out, gds.nj);
      grib_detail::AppendSignMag24(out, gds.latitude_first);
      grib_detail::AppendSignMag24(out, gds.longitude_first);
      out.push_back(gds.resolution_and_component_flags);
      grib_detail::AppendSignMag24(out, gds.latitude_last);
      grib_detail::AppendSignMag24(out, gds.longitude_last);
      grib_detail::AppendBe16(out, gds.i_direction_increment);
      grib_detail::AppendBe16(out, gds.j_direction_increment_or_n);
      out.push_back(gds.scanning_mode);
      out.insert(out.end(), gds.reserved.begin(), gds.reserved.end());
      out.insert(out.end(), gds.extra_template_bytes.begin(),
                 gds.extra_template_bytes.end());
      for (float coord : gds.coordinate_values) {
        grib_detail::AppendIbmFloat32(out, coord);
      }
      for (uint16_t row_len : gds.list_of_numbers) {
        grib_detail::AppendBe16(out, row_len);
        num_elements += row_len;
      }
      if (gds.list_of_numbers.empty() && gds.ni != 65535 && gds.nj != 65535) {
        num_elements = static_cast<size_t>(gds.ni) * gds.nj;
      }
      grib_detail::PatchBe24(out, gds_start,
                             static_cast<uint32_t>(out.size() - gds_start));
    } else {
      num_elements = PredefinedGrib1GridSize(spec.grid_definition);
    }

    // Section 3: Bit Map Section (BMS)
    std::optional<size_t> num_masked_present;
    if (spec.bms.has_value()) {
      const Grib1BmsSpec& bms = *spec.bms;
      uint8_t unused_bits = 0;
      std::vector<uint8_t> bm_bytes =
          !bms.raw_bytes.empty()
              ? bms.raw_bytes
              : grib_detail::PackBitmap(bms.bitmap, &unused_bits);
      if (bms.unused_bits_override.has_value()) {
        unused_bits = *bms.unused_bits_override;
      }
      num_masked_present = grib_detail::PopCountBytes(bm_bytes);
      const uint32_t bms_len = static_cast<uint32_t>(6 + bm_bytes.size());
      grib_detail::AppendBe24(out, bms_len);
      out.push_back(unused_bits);
      grib_detail::AppendBe16(out, bms.predefined_bitmap);
      out.insert(out.end(), bm_bytes.begin(), bm_bytes.end());
    }

    // Section 4: Binary Data Section (BDS)
    const Grib1BdsSpec& bds = spec.bds;
    uint8_t bds_unused_bits = 0;
    std::vector<uint8_t> payload;
    if (!bds.raw_packed_bytes.empty()) {
      payload = bds.raw_packed_bytes;
    } else if (!bds.packed_ints.empty()) {
      payload = grib_detail::PackSimpleInts(bds.packed_ints, bds.bits_per_value,
                                            &bds_unused_bits);
    } else if (!bds.values.empty()) {
      payload = grib_detail::PackSimpleValues(
          bds.values, bds.reference_value, bds.binary_scale_factor,
          spec.decimal_scale_factor, bds.bits_per_value, &bds_unused_bits);
    } else if (bds.bits_per_value == 0) {
      payload.push_back(0);
    } else {
      const size_t packed_count = num_masked_present.value_or(num_elements);
      const size_t total_bits = packed_count * bds.bits_per_value;
      const size_t total_bytes = (total_bits + 7) / 8;
      payload.assign(total_bytes, 0);
      bds_unused_bits = static_cast<uint8_t>((8 - (total_bits % 8)) % 8);
    }
    if (bds.unused_bits_override.has_value()) {
      bds_unused_bits = *bds.unused_bits_override;
    }

    const uint32_t bds_len = static_cast<uint32_t>(11 + payload.size());
    grib_detail::AppendBe24(out, bds_len);
    out.push_back(
        static_cast<uint8_t>((bds.flags & 0xF0u) | (bds_unused_bits & 0x0Fu)));
    grib_detail::AppendSignMag16(out, bds.binary_scale_factor);
    grib_detail::AppendIbmFloat32(out, bds.reference_value);
    out.push_back(bds.bits_per_value);
    out.insert(out.end(), payload.begin(), payload.end());

    // Section 5: End Section ('7777')
    out.insert(out.end(), spec.end_marker.begin(), spec.end_marker.end());

    const uint32_t msg_len = spec.message_length_override.value_or(
        static_cast<uint32_t>(out.size() - msg_start));
    grib_detail::PatchBe24(out, len_offset, msg_len);
  }

  static void AppendGrib2GridSection(std::vector<uint8_t>& out,
                                     const Grib2GridSpec& grid) {
    const size_t sec_start = out.size();
    const uint8_t interp = grid.interpretation_override.value_or(
        grid.list_of_numbers.empty() ? 0 : 1);
    const uint8_t num_size = grid.number_size_override.value_or(
        grid.list_of_numbers.empty() ? 0 : 2);

    grib_detail::AppendBe32(out, 0);  // Patched below.
    out.push_back(3);
    out.push_back(grid.source_of_grid_definition);
    grib_detail::AppendBe32(out, grid.number_of_data_points);
    out.push_back(num_size);
    out.push_back(interp);
    grib_detail::AppendBe16(out, grid.template_number);
    out.push_back(grid.shape_of_the_earth);
    out.push_back(grid.scale_factor_radius);
    grib_detail::AppendBe32(out, grid.scaled_value_radius);
    out.push_back(grid.scale_factor_major_axis);
    grib_detail::AppendBe32(out, grid.scaled_value_major_axis);
    out.push_back(grid.scale_factor_minor_axis);
    grib_detail::AppendBe32(out, grid.scaled_value_minor_axis);
    grib_detail::AppendBe32(out, grid.ni);
    grib_detail::AppendBe32(out, grid.nj);
    grib_detail::AppendBe32(out, grid.basic_angle);
    grib_detail::AppendBe32(out, grid.subdivisions_of_basic_angle);
    grib_detail::AppendSignMag32(out, grid.latitude_first);
    grib_detail::AppendSignMag32(out, grid.longitude_first);
    out.push_back(grid.resolution_and_component_flags);
    grib_detail::AppendSignMag32(out, grid.latitude_last);
    grib_detail::AppendSignMag32(out, grid.longitude_last);
    grib_detail::AppendBe32(out, grid.i_direction_increment);
    grib_detail::AppendBe32(out, grid.j_direction_increment_or_n);
    out.push_back(grid.scanning_mode);

    if (interp > 0) {
      for (uint32_t row_len : grid.list_of_numbers) {
        switch (num_size) {
          case 1:
            out.push_back(static_cast<uint8_t>(row_len & 0xFFu));
            break;
          case 2:
            grib_detail::AppendBe16(out,
                                    static_cast<uint16_t>(row_len & 0xFFFFu));
            break;
          case 4:
          default:
            grib_detail::AppendBe32(out, row_len);
            break;
        }
      }
    } else {
      out.insert(out.end(), grid.extra_template_bytes.begin(),
                 grid.extra_template_bytes.end());
    }

    grib_detail::PatchBe32(out, sec_start,
                           static_cast<uint32_t>(out.size() - sec_start));
  }

  static void AppendGrib2ProductSection(std::vector<uint8_t>& out,
                                        const Grib2ProductSpec& prod) {
    const size_t sec_start = out.size();
    grib_detail::AppendBe32(out, 0);  // Patched below.
    out.push_back(4);
    grib_detail::AppendBe16(
        out, static_cast<uint16_t>(prod.coordinate_values.size()));
    grib_detail::AppendBe16(out, prod.template_number);
    out.push_back(prod.parameter_category);
    out.push_back(prod.parameter_number);
    if (prod.template_number == 40) {
      grib_detail::AppendBe16(out, prod.constituent_type);
    }
    out.push_back(prod.type_of_generating_process);
    out.push_back(prod.background_process);
    out.push_back(prod.generating_process_id);
    grib_detail::AppendBe16(out, prod.hours_after_data_cutoff);
    out.push_back(prod.minutes_after_data_cutoff);
    out.push_back(prod.indicator_of_unit_of_time_range);
    grib_detail::AppendBe32(out, prod.forecast_time);
    out.push_back(prod.type_of_first_fixed_surface);
    out.push_back(static_cast<uint8_t>(prod.scale_factor_first_fixed_surface));
    grib_detail::AppendBe32(out, prod.scaled_value_first_fixed_surface);
    out.push_back(prod.type_of_second_fixed_surface);
    out.push_back(static_cast<uint8_t>(prod.scale_factor_second_fixed_surface));
    grib_detail::AppendBe32(out, prod.scaled_value_second_fixed_surface);
    for (float coord : prod.coordinate_values) {
      grib_detail::AppendIeeeFloat32Be(out, coord);
    }
    out.insert(out.end(), prod.extra_template_bytes.begin(),
               prod.extra_template_bytes.end());
    grib_detail::PatchBe32(out, sec_start,
                           static_cast<uint32_t>(out.size() - sec_start));
  }

  static void AppendGrib2Field(std::vector<uint8_t>& out,
                               const Grib2FieldSpec& field,
                               std::optional<size_t>& prev_bitmap_popcount) {
    // Section 2: Local Use (optional)
    if (field.local_use.has_value()) {
      const uint32_t sec2_len =
          static_cast<uint32_t>(5 + field.local_use->size());
      grib_detail::AppendBe32(out, sec2_len);
      out.push_back(2);
      out.insert(out.end(), field.local_use->begin(), field.local_use->end());
    }

    // Section 3: Grid Definition (optional on subsequent fields)
    if (field.grid.has_value()) {
      AppendGrib2GridSection(out, *field.grid);
    }

    // Section 4: Product Definition
    AppendGrib2ProductSection(out, field.product);

    // Section 5: Data Representation
    {
      const Grib2DataRepSpec& rep = field.data_rep;
      const uint32_t sec5_len =
          static_cast<uint32_t>(21 + rep.extra_template_bytes.size());
      grib_detail::AppendBe32(out, sec5_len);
      out.push_back(5);
      grib_detail::AppendBe32(out, rep.number_of_data_points);
      grib_detail::AppendBe16(out, rep.template_number);
      grib_detail::AppendIeeeFloat32Be(out, rep.reference_value);
      grib_detail::AppendSignMag16(out, rep.binary_scale_factor);
      grib_detail::AppendSignMag16(out, rep.decimal_scale_factor);
      out.push_back(rep.bits_per_value);
      out.push_back(rep.type_of_original_field_values);
      out.insert(out.end(), rep.extra_template_bytes.begin(),
                 rep.extra_template_bytes.end());
    }

    // Section 6: Bit-Map (optional)
    std::optional<size_t> active_bitmap_popcount;
    if (field.bitmap.has_value()) {
      const Grib2BitmapSpec& bm = *field.bitmap;
      const bool has_inline_data = !bm.raw_bytes.empty() || !bm.bitmap.empty();
      const uint8_t indicator =
          bm.indicator_override.value_or(has_inline_data ? 0 : 255);
      std::vector<uint8_t> bm_bytes;
      if (indicator == 0 || has_inline_data) {
        bm_bytes = !bm.raw_bytes.empty() ? bm.raw_bytes
                                         : grib_detail::PackBitmap(bm.bitmap);
      }
      if (indicator == 0) {
        prev_bitmap_popcount = grib_detail::PopCountBytes(bm_bytes);
        active_bitmap_popcount = prev_bitmap_popcount;
      } else if (indicator == 254) {
        active_bitmap_popcount = prev_bitmap_popcount;
      }
      const uint32_t sec6_len = static_cast<uint32_t>(6 + bm_bytes.size());
      grib_detail::AppendBe32(out, sec6_len);
      out.push_back(6);
      out.push_back(indicator);
      out.insert(out.end(), bm_bytes.begin(), bm_bytes.end());
    }

    // Section 7: Data Section
    {
      const Grib2DataSpec& data = field.data;
      const Grib2DataRepSpec& rep = field.data_rep;
      std::vector<uint8_t> payload;
      if (!data.raw_packed_bytes.empty()) {
        payload = data.raw_packed_bytes;
      } else if (!data.packed_ints.empty()) {
        payload =
            grib_detail::PackSimpleInts(data.packed_ints, rep.bits_per_value);
      } else if (!data.values.empty()) {
        payload = grib_detail::PackSimpleValues(
            data.values, rep.reference_value, rep.binary_scale_factor,
            rep.decimal_scale_factor, rep.bits_per_value);
      } else if (rep.bits_per_value > 0) {
        const size_t packed_count = active_bitmap_popcount.value_or(
            static_cast<size_t>(rep.number_of_data_points));
        const size_t total_bytes = (packed_count * rep.bits_per_value + 7) / 8;
        payload.assign(total_bytes, 0);
      }
      const uint32_t sec7_len = static_cast<uint32_t>(5 + payload.size());
      grib_detail::AppendBe32(out, sec7_len);
      out.push_back(7);
      out.insert(out.end(), payload.begin(), payload.end());
    }
  }

  static void AppendGrib2Message(std::vector<uint8_t>& out,
                                 const Grib2MessageSpec& spec) {
    const size_t msg_start = out.size();

    // Section 0: Indicator Section (16 bytes)
    out.push_back('G');
    out.push_back('R');
    out.push_back('I');
    out.push_back('B');
    out.push_back(spec.section0_reserved[0]);
    out.push_back(spec.section0_reserved[1]);
    out.push_back(spec.discipline);
    out.push_back(spec.edition_override);
    const size_t len_offset = out.size();
    grib_detail::AppendBe64(out, 0);  // Patched at end.

    // Section 1: Identification Section
    const uint32_t sec1_len =
        static_cast<uint32_t>(21 + spec.ident_extra_bytes.size());
    grib_detail::AppendBe32(out, sec1_len);
    out.push_back(spec.section1_number_override);
    grib_detail::AppendBe16(out, spec.centre);
    grib_detail::AppendBe16(out, spec.sub_centre);
    out.push_back(spec.master_tables_version);
    out.push_back(spec.local_tables_version);
    out.push_back(spec.significance_of_reference_time);
    grib_detail::AppendBe16(out, spec.year);
    out.push_back(spec.month);
    out.push_back(spec.day);
    out.push_back(spec.hour);
    out.push_back(spec.minute);
    out.push_back(spec.second);
    out.push_back(spec.production_status);
    out.push_back(spec.type_of_processed_data);
    out.insert(out.end(), spec.ident_extra_bytes.begin(),
               spec.ident_extra_bytes.end());

    if (!spec.omit_data_sections) {
      if (!spec.raw_sections.empty()) {
        for (const Grib2RawSection& sec : spec.raw_sections) {
          grib_detail::AppendBe32(out,
                                  static_cast<uint32_t>(5 + sec.body.size()));
          out.push_back(sec.section_number);
          out.insert(out.end(), sec.body.begin(), sec.body.end());
        }
      } else {
        std::optional<size_t> prev_bitmap_popcount;
        if (!spec.fields.empty()) {
          for (const Grib2FieldSpec& field : spec.fields) {
            AppendGrib2Field(out, field, prev_bitmap_popcount);
          }
        } else {
          Grib2FieldSpec single{
              .local_use = spec.local_use,
              .grid = spec.grid,
              .product = spec.product,
              .data_rep = spec.data_rep,
              .bitmap = spec.bitmap,
              .data = spec.data,
          };
          AppendGrib2Field(out, single, prev_bitmap_popcount);
        }
      }
    }

    // Section 8: End Section ('7777')
    out.insert(out.end(), spec.end_marker.begin(), spec.end_marker.end());

    const uint64_t msg_len = spec.message_length_override.value_or(
        static_cast<uint64_t>(out.size() - msg_start));
    grib_detail::PatchBe64(out, len_offset, msg_len);
  }

  std::vector<uint8_t> buffer_;
};

}  // namespace stcorp_coda

#endif  // THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_GRIB_BUILDER_H_
