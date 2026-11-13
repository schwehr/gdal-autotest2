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
// Header-only C++20 NASA CDF v3.x binary builder for synthesizing valid and
// malformed CDF files in unit tests for libcoda/coda-cdf.c without checked-in
// binary fixtures.
//
// Note on CDF v3 layout vs libcoda/coda-cdf.c expectations:
//   - All CDF v3 metadata records (CDR, GDR, rVDR, zVDR, VXR, VVR header, CPR,
//     CVVR header, ADR, AEDR) are always big-endian on disk. Only variable and
//     attribute element payloads follow the CDR `encoding` endianness.
//   - CVVR records (type 13) require gzip framing (`windowBits = MAX_WBITS`
//     `+ 16 = 31`) because coda-cdf.c calls `inflateInit2(&zs, 31)` and rejects
//     blocks with `csize < 20`. `ZlibCompress` (via `compress2`) is also
//     provided for testing invalid-compression rejection.
//   - In upstream coda-cdf.c, `product_file->rdim_sizes` is never populated
//     from GDR and `read_VDR` omits `offset += 340` for rVariables when
//     `rnum_dims > 0`. Tests exercising valid rVariables with coda-cdf.c
//     should keep `rdims` empty (`rnum_dims == 0`).

#ifndef THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_CDF_BUILDER_H_
#define THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_CDF_BUILDER_H_

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "third_party/rust/zlib_rs/zlib.h"

namespace stcorp_coda {

// Standard CDF file magic word pairs (big-endian on disk).
inline constexpr uint32_t kCdfMagicV1 = 0x0000FFFFU;
inline constexpr uint32_t kCdfMagicV2 = 0xCDF26002U;
inline constexpr uint32_t kCdfMagicV3 = 0xCDF30001U;
inline constexpr uint32_t kCdfMagicUncompressed = 0x0000FFFFU;
inline constexpr uint32_t kCdfMagicCompressed = 0xCCCC0001U;

// CDF record type tags (matching libcoda/coda-cdf.c).
enum class CdfRecordType : int32_t {
  kCdr = 1,
  kGdr = 2,
  kRvdr = 3,
  kAdr = 4,
  kAgrEdr = 5,
  kVxr = 6,
  kVvr = 7,
  kZvdr = 8,
  kAzEdr = 9,
  kCpr = 11,
  kCvvr = 13,
};

// CDF data type codes (matching coda-cdf.c and coda-cdf-type.c).
enum class CdfDataType : int32_t {
  kInt1 = 1,
  kInt2 = 2,
  kInt4 = 4,
  kInt8 = 8,
  kUint1 = 11,
  kUint2 = 12,
  kUint4 = 14,
  kReal4 = 21,
  kReal8 = 22,
  kEpoch = 31,
  kEpoch16 = 32,  // Unsupported by coda-cdf.c
  kTimeTt2000 = 33,
  kByte = 41,
  kFloat = 44,
  kDouble = 45,
  kChar = 51,
  kUchar = 52,
};

// CDF encoding codes in CDR (matching read_file() in coda-cdf.c).
enum class CdfEncoding : int32_t {
  kNetwork = 1,     // Big-endian
  kSun = 2,         // Big-endian
  kVax = 3,         // Unsupported non-IEEE floating point
  kDecstation = 4,  // Little-endian
  kSgi = 5,         // Big-endian
  kIbmPc = 6,       // Little-endian
  kIbmRs = 7,       // Big-endian
  kMac = 9,         // Big-endian
  kHp = 11,         // Big-endian
  kNext = 12,       // Big-endian
  kAlphaOsf1 = 13,  // Little-endian
  kAlphaVmsD = 14,  // Unsupported non-IEEE floating point
  kAlphaVmsG = 15,  // Unsupported non-IEEE floating point
  kAlphaVmsI = 16,  // Little-endian
};

// CDR flag bit masks.
inline constexpr int32_t kCdfFlagRowMajor = 1 << 0;
inline constexpr int32_t kCdfFlagSingleFile = 1 << 1;
inline constexpr int32_t kCdfFlagMd5Bit1 = 1 << 2;
inline constexpr int32_t kCdfFlagMd5Bit2 = 1 << 3;
inline constexpr int32_t kCdfFlagMd5Checksum =
    kCdfFlagMd5Bit1 | kCdfFlagMd5Bit2;

// VDR flag bit masks.
inline constexpr int32_t kCdfVdrFlagRecVarys = 1 << 0;
inline constexpr int32_t kCdfVdrFlagPadValue = 1 << 1;
inline constexpr int32_t kCdfVdrFlagCompression = 1 << 2;

enum class CdfMagicPreset {
  kV3Uncompressed,
  kV3Compressed,
  kV2Uncompressed,
  kV2Compressed,
  kV1Uncompressed,
};

enum class CdfArrayOrdering : int32_t {
  kColumnMajor = 0,  // coda_array_ordering_fortran (CDR bit 0 clear)
  kRowMajor = 1,     // coda_array_ordering_c (CDR bit 0 set)
};

enum class CdfSparseMethod : int32_t {
  kNone = 0,
  kPadded = 1,
  kPrevious = 2,
};

enum class CdfCompression : int32_t {
  kNone = 0,
  kRle = 1,
  kHuff = 2,
  kAhuff = 3,
  kGzip = 5,
};

enum class CdfAttrScope : int32_t {
  kGlobal = 1,
  kVariable = 2,
  kGlobalAssumed = 3,
  kVariableAssumed = 4,
};

// Returns true for big-endian CDR encodings accepted by coda-cdf.c.
[[nodiscard]] constexpr bool IsBigEndianCdfEncoding(int32_t encoding) {
  switch (encoding) {
    case 1:   // NETWORK
    case 2:   // SUN
    case 5:   // SGi
    case 7:   // IBMRS
    case 9:   // MAC
    case 11:  // HP
    case 12:  // NeXT
      return true;
    default:
      return false;
  }
}

// Returns the byte size of one element of `data_type` (0 if unknown).
[[nodiscard]] constexpr int32_t CdfDataTypeByteSize(CdfDataType data_type) {
  switch (data_type) {
    case CdfDataType::kInt1:
    case CdfDataType::kUint1:
    case CdfDataType::kByte:
    case CdfDataType::kChar:
    case CdfDataType::kUchar:
      return 1;
    case CdfDataType::kInt2:
    case CdfDataType::kUint2:
      return 2;
    case CdfDataType::kInt4:
    case CdfDataType::kUint4:
    case CdfDataType::kReal4:
    case CdfDataType::kFloat:
      return 4;
    case CdfDataType::kInt8:
    case CdfDataType::kReal8:
    case CdfDataType::kEpoch:
    case CdfDataType::kTimeTt2000:
    case CdfDataType::kDouble:
      return 8;
    case CdfDataType::kEpoch16:
      return 16;
  }
  return 0;
}

namespace cdf_internal {

inline void AppendBe16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFU));
  out.push_back(static_cast<uint8_t>(v & 0xFFU));
}

inline void AppendLe16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFFU));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFU));
}

inline void AppendBe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFU));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFU));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFU));
  out.push_back(static_cast<uint8_t>(v & 0xFFU));
}

inline void AppendLe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFFU));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFU));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFU));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFU));
}

inline void AppendBe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((v >> shift) & 0xFFU));
  }
}

inline void AppendLe64(std::vector<uint8_t>& out, uint64_t v) {
  for (int shift = 0; shift <= 56; shift += 8) {
    out.push_back(static_cast<uint8_t>((v >> shift) & 0xFFU));
  }
}

inline void PatchBe32(std::vector<uint8_t>& out, size_t pos, int32_t v) {
  const uint32_t u = static_cast<uint32_t>(v);
  out[pos + 0] = static_cast<uint8_t>((u >> 24) & 0xFFU);
  out[pos + 1] = static_cast<uint8_t>((u >> 16) & 0xFFU);
  out[pos + 2] = static_cast<uint8_t>((u >> 8) & 0xFFU);
  out[pos + 3] = static_cast<uint8_t>(u & 0xFFU);
}

inline void PatchBe64(std::vector<uint8_t>& out, size_t pos, int64_t v) {
  const uint64_t u = static_cast<uint64_t>(v);
  for (int i = 0; i < 8; ++i) {
    out[pos + static_cast<size_t>(i)] =
        static_cast<uint8_t>((u >> (56 - 8 * i)) & 0xFFU);
  }
}

inline void AppendFixedName(std::vector<uint8_t>& out, std::string_view name,
                            size_t field_len, bool space_pad) {
  const size_t copy_len = std::min(name.size(), field_len);
  out.insert(out.end(), name.begin(),
             name.begin() + static_cast<std::ptrdiff_t>(copy_len));
  const uint8_t pad_byte = space_pad ? static_cast<uint8_t>(' ') : 0U;
  out.insert(out.end(), field_len - copy_len, pad_byte);
}

template <typename T>
inline void AppendScalar(std::vector<uint8_t>& out, T value, bool big_endian) {
  static_assert(std::is_trivially_copyable_v<T>);
  if constexpr (sizeof(T) == 1) {
    out.push_back(std::bit_cast<uint8_t>(value));
  } else if constexpr (sizeof(T) == 2) {
    const auto bits = std::bit_cast<uint16_t>(value);
    if (big_endian) {
      AppendBe16(out, bits);
    } else {
      AppendLe16(out, bits);
    }
  } else if constexpr (sizeof(T) == 4) {
    const auto bits = std::bit_cast<uint32_t>(value);
    if (big_endian) {
      AppendBe32(out, bits);
    } else {
      AppendLe32(out, bits);
    }
  } else if constexpr (sizeof(T) == 8) {
    const auto bits = std::bit_cast<uint64_t>(value);
    if (big_endian) {
      AppendBe64(out, bits);
    } else {
      AppendLe64(out, bits);
    }
  } else {
    static_assert(sizeof(T) <= 8, "Unsupported scalar width");
  }
}

}  // namespace cdf_internal

// Contiguous range of records stored in a VVR (type 7) or CVVR (type 13).
struct CdfVarChunkSpec {
  int32_t first_rec = 0;
  int32_t last_rec = 0;
  std::vector<uint8_t> data;
  bool compressed = false;  // false -> VVR (7), true -> CVVR (13)
  // Optional pre-built compressed bytes for CVVR (e.g., zlib `compress2`
  // output or corrupt/truncated blocks). When set, emitted verbatim instead of
  // gzip-compressing `data`.
  std::optional<std::vector<uint8_t>> raw_cvvr_payload;
  // Corruption overrides:
  std::optional<int32_t> record_type_override;
  std::optional<int64_t> csize_override;
};

struct CdfVxrNode;

// Single entry inside a VXR record (type 6).
struct CdfVxrEntry {
  int32_t first_rec = 0;
  int32_t last_rec = 0;
  std::optional<CdfVarChunkSpec> chunk;
  std::shared_ptr<CdfVxrNode> child_vxr;
  // Optional explicit offset written to VXR's Offset[i] slot (e.g., 0, -1, OOB,
  // or a self/parent cycle offset). Takes precedence over child_vxr/chunk.
  std::optional<int64_t> vr_offset_override;
};

// VXR index node (record type 6). Supports both linked `next_vxr` chains and
// multi-level nested VXR trees (`entry.child_vxr`).
struct CdfVxrNode {
  std::vector<CdfVxrEntry> entries;
  std::shared_ptr<CdfVxrNode> next_vxr;
  // Corruption overrides:
  std::optional<int32_t> record_type_override;
  std::optional<int64_t> vxr_next_override;
  std::optional<int32_t> n_entries_override;
  std::optional<int32_t> nused_entries_override;
};

// Variable specification for rVDR (type 3) or zVDR (type 8).
struct CdfVarSpec {
  std::string name;
  CdfDataType data_type = CdfDataType::kInt4;
  int32_t num_elems = 1;
  bool rec_varys = true;
  std::vector<int32_t> dims;       // Used by zVariables (ignored for rVars).
  std::vector<int32_t> dim_varys;  // Empty -> all dims vary (1).
  CdfSparseMethod sparse_records = CdfSparseMethod::kNone;
  int32_t blocking_factor = 1;
  bool space_pad_name = false;

  // Compression / CPR (type 11) configuration:
  bool has_compression = false;
  CdfCompression compression = CdfCompression::kGzip;
  std::vector<int32_t> cparms = {6};
  bool emit_cpr = true;
  // When `has_compression` is true and `data` is used without explicit
  // `chunks`, controls whether `data` is emitted as CVVR (true) or VVR (false).
  bool compress_data = true;

  // Variable data & index configuration (in order of precedence):
  //   1. `vxr_root`: explicit VXR tree/chain.
  //   2. `chunks`: explicit list of VVR/CVVR record chunks.
  //   3. `data`: flat uncompressed bytes as a single [0..max_rec] chunk.
  std::shared_ptr<CdfVxrNode> vxr_root;
  std::vector<CdfVarChunkSpec> chunks;
  std::vector<uint8_t> data;
  // When > 0 and `chunks` is used, splits `chunks` across multiple VXRs
  // (linked via `vxr_next` when `nested_vxr_tree == false`, or a multi-level
  // nested VXR tree when `nested_vxr_tree == true`).
  int32_t max_vxr_entries = 0;
  bool nested_vxr_tree = false;

  // Corruption & override hooks:
  std::optional<int32_t> raw_data_type_override;
  std::optional<int32_t> record_type_override;
  std::optional<int64_t> vdr_next_override;
  std::optional<int32_t> max_rec_override;
  std::optional<int64_t> vxr_head_override;
  std::optional<int64_t> vxr_tail_override;
  std::optional<int32_t> flags_override;
  std::optional<int32_t> num_elems_override;
  std::optional<int32_t> var_num_override;
  std::optional<int64_t> cpr_offset_override;
  std::optional<int32_t> num_dims_override;
  std::optional<int32_t> cpr_record_type_override;
  std::optional<int32_t> cpr_ctype_override;
};

// Attribute entry specification for AgrEDR (type 5) or AzEDR (type 9).
struct CdfAttrEntrySpec {
  int32_t entry_num = 0;  // Variable index (0..N-1) or 0 for global scope.
  CdfDataType data_type = CdfDataType::kChar;
  int32_t num_elems = 1;
  std::vector<uint8_t> raw_value;
  bool is_z_entry = false;  // false -> AgrEDR (type 5), true -> AzEDR (type 9)

  // Corruption & override hooks:
  std::optional<int32_t> raw_data_type_override;
  std::optional<int32_t> record_type_override;
  std::optional<int64_t> aedr_next_override;
  std::optional<int32_t> attr_num_override;
  std::optional<int32_t> entry_num_override;
  std::optional<int32_t> num_elems_override;

  [[nodiscard]] static CdfAttrEntrySpec String(int32_t entry_num,
                                               std::string_view text,
                                               bool is_z_entry = false) {
    CdfAttrEntrySpec spec;
    spec.entry_num = entry_num;
    spec.data_type = CdfDataType::kChar;
    spec.num_elems = static_cast<int32_t>(text.size());
    spec.raw_value.assign(text.begin(), text.end());
    spec.is_z_entry = is_z_entry;
    return spec;
  }

  template <typename T>
  [[nodiscard]] static CdfAttrEntrySpec Scalar(int32_t entry_num,
                                               CdfDataType type, T value,
                                               bool big_endian,
                                               bool is_z_entry = false) {
    CdfAttrEntrySpec spec;
    spec.entry_num = entry_num;
    spec.data_type = type;
    spec.num_elems = 1;
    cdf_internal::AppendScalar(spec.raw_value, value, big_endian);
    spec.is_z_entry = is_z_entry;
    return spec;
  }

  template <typename T>
  [[nodiscard]] static CdfAttrEntrySpec Array(int32_t entry_num,
                                              CdfDataType type,
                                              std::span<const T> values,
                                              bool big_endian,
                                              bool is_z_entry = false) {
    CdfAttrEntrySpec spec;
    spec.entry_num = entry_num;
    spec.data_type = type;
    spec.num_elems = static_cast<int32_t>(values.size());
    for (const T& v : values) {
      cdf_internal::AppendScalar(spec.raw_value, v, big_endian);
    }
    spec.is_z_entry = is_z_entry;
    return spec;
  }

  template <typename T>
  [[nodiscard]] static CdfAttrEntrySpec Array(int32_t entry_num,
                                              CdfDataType type,
                                              std::initializer_list<T> values,
                                              bool big_endian,
                                              bool is_z_entry = false) {
    return Array(entry_num, type,
                 std::span<const T>(values.begin(), values.size()), big_endian,
                 is_z_entry);
  }

  template <typename T>
  [[nodiscard]] static CdfAttrEntrySpec Array(int32_t entry_num,
                                              CdfDataType type,
                                              const std::vector<T>& values,
                                              bool big_endian,
                                              bool is_z_entry = false) {
    return Array(entry_num, type, std::span<const T>(values), big_endian,
                 is_z_entry);
  }

  template <typename T, size_t N>
  [[nodiscard]] static CdfAttrEntrySpec Array(int32_t entry_num,
                                              CdfDataType type,
                                              const std::array<T, N>& values,
                                              bool big_endian,
                                              bool is_z_entry = false) {
    return Array(entry_num, type, std::span<const T>(values), big_endian,
                 is_z_entry);
  }
};

// Attribute descriptor specification for ADR (type 4).
struct CdfAttrSpec {
  std::string name;
  CdfAttrScope scope = CdfAttrScope::kGlobal;
  std::vector<CdfAttrEntrySpec> entries;
  bool space_pad_name = false;

  // Corruption & override hooks:
  std::optional<int32_t> raw_scope_override;
  std::optional<int32_t> record_type_override;
  std::optional<int64_t> adr_next_override;
  std::optional<int64_t> agredr_head_override;
  std::optional<int64_t> azedr_head_override;
  std::optional<int32_t> attr_num_override;
  std::optional<int32_t> ngr_entries_override;
  std::optional<int32_t> nz_entries_override;
};

// Serialized CDF buffer along with exact record offsets for inspection or
// targeted post-build byte patching.
struct CdfBuildResult {
  std::vector<uint8_t> bytes;
  int64_t cdr_offset = 0;
  int64_t gdr_offset = 0;
  std::vector<int64_t> rvar_offsets;
  std::vector<int64_t> zvar_offsets;
  std::vector<int64_t> adr_offsets;
};

class CdfBuilder {
 public:
  CdfBuilder() = default;

  // --- Static compression and encoding utilities ---

  // Compresses `input` in gzip format (`windowBits = MAX_WBITS + 16` = 31) as
  // required by coda-cdf.c CVVR (type 13) decompression.
  [[nodiscard]] static bool GzipCompress(std::span<const uint8_t> input,
                                         std::vector<uint8_t>& output,
                                         int level = Z_DEFAULT_COMPRESSION) {
    z_stream zs{};
    if (deflateInit2(&zs, level, Z_DEFLATED, MAX_WBITS + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
      return false;
    }
    zs.next_in = input.empty() ? nullptr : const_cast<Bytef*>(input.data());
    zs.avail_in = static_cast<uInt>(input.size());
    output.resize(deflateBound(&zs, static_cast<uLong>(input.size())) + 16);
    zs.next_out = output.data();
    zs.avail_out = static_cast<uInt>(output.size());
    const int rc = deflate(&zs, Z_FINISH);
    const uLong total_out = zs.total_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) {
      output.clear();
      return false;
    }
    output.resize(total_out);
    return true;
  }

  // Compresses `input` in zlib format via `compress2` (useful for testing that
  // coda-cdf.c rejects non-gzip zlib payloads in CVVR records).
  [[nodiscard]] static bool ZlibCompress(std::span<const uint8_t> input,
                                         std::vector<uint8_t>& output,
                                         int level = Z_DEFAULT_COMPRESSION) {
    uLong dest_len = compressBound(static_cast<uLong>(input.size()));
    output.resize(dest_len);
    const Bytef empty_byte = 0;
    const Bytef* src = input.empty() ? &empty_byte : input.data();
    const int rc = compress2(output.data(), &dest_len, src,
                             static_cast<uLong>(input.size()), level);
    if (rc != Z_OK) {
      output.clear();
      return false;
    }
    output.resize(dest_len);
    return true;
  }

  template <typename T>
  [[nodiscard]] static std::vector<uint8_t> EncodeValue(T value,
                                                        bool big_endian) {
    std::vector<uint8_t> out;
    cdf_internal::AppendScalar(out, value, big_endian);
    return out;
  }

  template <typename T>
  [[nodiscard]] static std::vector<uint8_t> EncodeValues(
      std::span<const T> values, bool big_endian) {
    std::vector<uint8_t> out;
    out.reserve(values.size() * sizeof(T));
    for (const T& v : values) {
      cdf_internal::AppendScalar(out, v, big_endian);
    }
    return out;
  }

  template <typename T>
  [[nodiscard]] static std::vector<uint8_t> EncodeValues(
      std::initializer_list<T> values, bool big_endian) {
    return EncodeValues(std::span<const T>(values.begin(), values.size()),
                        big_endian);
  }

  template <typename T>
  [[nodiscard]] static std::vector<uint8_t> EncodeValues(
      const std::vector<T>& values, bool big_endian) {
    return EncodeValues(std::span<const T>(values), big_endian);
  }

  template <typename T, size_t N>
  [[nodiscard]] static std::vector<uint8_t> EncodeValues(
      const std::array<T, N>& values, bool big_endian) {
    return EncodeValues(std::span<const T>(values), big_endian);
  }

  static void PatchBe32(std::vector<uint8_t>& buf, size_t offset,
                        int32_t value) {
    cdf_internal::PatchBe32(buf, offset, value);
  }

  static void PatchBe64(std::vector<uint8_t>& buf, size_t offset,
                        int64_t value) {
    cdf_internal::PatchBe64(buf, offset, value);
  }

  // Builds a VXR tree or linked chain from `chunks`.
  //   - `max_vxr_entries <= 0`: single VXR containing all chunks.
  //   - `nested_vxr_tree == false`: linked list via `next_vxr`.
  //   - `nested_vxr_tree == true`: bottom-up nested VXR tree (branching >= 2).
  [[nodiscard]] static std::shared_ptr<CdfVxrNode> BuildVxrTreeFromChunks(
      std::span<const CdfVarChunkSpec> chunks, int32_t max_vxr_entries = 0,
      bool nested_vxr_tree = false) {
    if (chunks.empty()) {
      return nullptr;
    }
    if (max_vxr_entries <= 0 ||
        static_cast<size_t>(max_vxr_entries) >= chunks.size()) {
      auto root = std::make_shared<CdfVxrNode>();
      root->entries.reserve(chunks.size());
      for (const CdfVarChunkSpec& c : chunks) {
        root->entries.push_back({
            .first_rec = c.first_rec,
            .last_rec = c.last_rec,
            .chunk = c,
        });
      }
      return root;
    }

    const size_t fanout =
        nested_vxr_tree
            ? std::max<size_t>(2, static_cast<size_t>(max_vxr_entries))
            : static_cast<size_t>(max_vxr_entries);

    std::vector<std::shared_ptr<CdfVxrNode>> leaves;
    for (size_t i = 0; i < chunks.size(); i += fanout) {
      const size_t end = std::min(chunks.size(), i + fanout);
      auto leaf = std::make_shared<CdfVxrNode>();
      for (size_t j = i; j < end; ++j) {
        leaf->entries.push_back({
            .first_rec = chunks[j].first_rec,
            .last_rec = chunks[j].last_rec,
            .chunk = chunks[j],
        });
      }
      leaves.push_back(std::move(leaf));
    }

    if (!nested_vxr_tree) {
      for (size_t i = 0; i + 1 < leaves.size(); ++i) {
        leaves[i]->next_vxr = leaves[i + 1];
      }
      return leaves.front();
    }

    // Wrap leaves bottom-up until a single root VXR remains.
    auto node_range = [](const CdfVxrNode& n) -> std::pair<int32_t, int32_t> {
      int32_t lo = n.entries.front().first_rec;
      int32_t hi = n.entries.front().last_rec;
      for (const CdfVxrEntry& e : n.entries) {
        lo = std::min(lo, e.first_rec);
        hi = std::max(hi, e.last_rec);
      }
      return {lo, hi};
    };

    std::vector<std::shared_ptr<CdfVxrNode>> level = std::move(leaves);
    while (level.size() > 1) {
      std::vector<std::shared_ptr<CdfVxrNode>> parents;
      for (size_t i = 0; i < level.size(); i += fanout) {
        const size_t end = std::min(level.size(), i + fanout);
        auto parent = std::make_shared<CdfVxrNode>();
        for (size_t j = i; j < end; ++j) {
          const auto [lo, hi] = node_range(*level[j]);
          parent->entries.push_back({
              .first_rec = lo,
              .last_rec = hi,
              .child_vxr = level[j],
          });
        }
        parents.push_back(std::move(parent));
      }
      level = std::move(parents);
    }
    return level.front();
  }

  // --- Header & CDR configuration ---

  CdfBuilder& SetMagic(uint32_t magic0, uint32_t magic1) {
    magic0_ = magic0;
    magic1_ = magic1;
    return *this;
  }

  CdfBuilder& SetMagicPreset(CdfMagicPreset preset) {
    switch (preset) {
      case CdfMagicPreset::kV3Uncompressed:
        return SetMagic(kCdfMagicV3, kCdfMagicUncompressed);
      case CdfMagicPreset::kV3Compressed:
        return SetMagic(kCdfMagicV3, kCdfMagicCompressed);
      case CdfMagicPreset::kV2Uncompressed:
        return SetMagic(kCdfMagicV2, kCdfMagicUncompressed);
      case CdfMagicPreset::kV2Compressed:
        return SetMagic(kCdfMagicV2, kCdfMagicCompressed);
      case CdfMagicPreset::kV1Uncompressed:
        return SetMagic(kCdfMagicV1, kCdfMagicUncompressed);
    }
    return *this;
  }

  CdfBuilder& SetVersion(int32_t version, int32_t release,
                         int32_t increment = 0) {
    cdf_version_ = version;
    cdf_release_ = release;
    cdf_increment_ = increment;
    return *this;
  }

  CdfBuilder& SetEncoding(CdfEncoding encoding) {
    encoding_ = static_cast<int32_t>(encoding);
    return *this;
  }

  CdfBuilder& SetEncodingRaw(int32_t encoding) {
    encoding_ = encoding;
    return *this;
  }

  [[nodiscard]] int32_t encoding() const { return encoding_; }
  [[nodiscard]] bool is_big_endian() const {
    return IsBigEndianCdfEncoding(encoding_);
  }

  CdfBuilder& SetArrayOrdering(CdfArrayOrdering ordering) {
    ordering_ = ordering;
    return *this;
  }

  CdfBuilder& SetSingleFile(bool single_file) {
    single_file_ = single_file;
    return *this;
  }

  CdfBuilder& SetMd5Checksum(bool enabled, bool append_trailer = false) {
    md5_checksum_ = enabled;
    append_md5_trailer_ = append_trailer;
    return *this;
  }

  CdfBuilder& SetCopyright(std::string copyright) {
    copyright_ = std::move(copyright);
    return *this;
  }

  CdfBuilder& SetRDims(std::vector<int32_t> rdims) {
    rdims_ = std::move(rdims);
    return *this;
  }

  // --- Adding variables & attributes ---

  CdfBuilder& AddRVar(CdfVarSpec var) {
    rvars_.push_back(std::move(var));
    return *this;
  }

  CdfBuilder& AddZVar(CdfVarSpec var) {
    zvars_.push_back(std::move(var));
    return *this;
  }

  CdfBuilder& AddAttr(CdfAttrSpec attr) {
    attrs_.push_back(std::move(attr));
    return *this;
  }

  // --- CDR / GDR corruption & override hooks ---

  CdfBuilder& SetCdrRecordTypeOverride(std::optional<int32_t> val) {
    cdr_record_type_override_ = val;
    return *this;
  }
  CdfBuilder& SetCdrFlagsOverride(std::optional<int32_t> val) {
    cdr_flags_override_ = val;
    return *this;
  }
  CdfBuilder& SetGdrOffsetOverride(std::optional<int64_t> val) {
    gdr_offset_override_ = val;
    return *this;
  }
  CdfBuilder& SetGdrRecordTypeOverride(std::optional<int32_t> val) {
    gdr_record_type_override_ = val;
    return *this;
  }
  CdfBuilder& SetRvdrHeadOverride(std::optional<int64_t> val) {
    rvdr_head_override_ = val;
    return *this;
  }
  CdfBuilder& SetZvdrHeadOverride(std::optional<int64_t> val) {
    zvdr_head_override_ = val;
    return *this;
  }
  CdfBuilder& SetAdrHeadOverride(std::optional<int64_t> val) {
    adr_head_override_ = val;
    return *this;
  }
  CdfBuilder& SetEofOverride(std::optional<int64_t> val) {
    eof_override_ = val;
    return *this;
  }
  CdfBuilder& SetNrVarsOverride(std::optional<int32_t> val) {
    nr_vars_override_ = val;
    return *this;
  }
  CdfBuilder& SetNumAttrOverride(std::optional<int32_t> val) {
    num_attr_override_ = val;
    return *this;
  }
  CdfBuilder& SetRnumDimsOverride(std::optional<int32_t> val) {
    rnum_dims_override_ = val;
    return *this;
  }
  CdfBuilder& SetNzVarsOverride(std::optional<int32_t> val) {
    nz_vars_override_ = val;
    return *this;
  }

  // --- Serialization ---

  [[nodiscard]] CdfBuildResult BuildWithOffsets() const {
    CdfBuildResult res;
    std::vector<uint8_t>& out = res.bytes;

    // 1. Magic (8 bytes).
    cdf_internal::AppendBe32(out, magic0_);
    cdf_internal::AppendBe32(out, magic1_);

    // 2. CDR (record type 1, starts at offset 8).
    res.cdr_offset = static_cast<int64_t>(out.size());
    const int64_t cdr_size = 56 + static_cast<int64_t>(copyright_.size());
    int32_t cdr_flags = 0;
    if (ordering_ == CdfArrayOrdering::kRowMajor) {
      cdr_flags |= kCdfFlagRowMajor;
    }
    if (single_file_) {
      cdr_flags |= kCdfFlagSingleFile;
    }
    if (md5_checksum_) {
      cdr_flags |= kCdfFlagMd5Checksum;
    }
    if (cdr_flags_override_.has_value()) {
      cdr_flags = *cdr_flags_override_;
    }

    cdf_internal::AppendBe64(out, static_cast<uint64_t>(cdr_size));
    cdf_internal::AppendBe32(
        out, static_cast<uint32_t>(cdr_record_type_override_.value_or(1)));
    const size_t cdr_gdr_patch_pos = out.size();
    cdf_internal::AppendBe64(out, 0);  // GDRoffset patched below.
    cdf_internal::AppendBe32(out, static_cast<uint32_t>(cdf_version_));
    cdf_internal::AppendBe32(out, static_cast<uint32_t>(cdf_release_));
    cdf_internal::AppendBe32(out, static_cast<uint32_t>(encoding_));
    cdf_internal::AppendBe32(out, static_cast<uint32_t>(cdr_flags));
    cdf_internal::AppendBe32(out, 0);  // rfuA
    cdf_internal::AppendBe32(out, 0);  // rfuB
    cdf_internal::AppendBe32(out, static_cast<uint32_t>(cdf_increment_));
    cdf_internal::AppendBe32(out, 0);  // rfuD
    cdf_internal::AppendBe32(out, 0);  // rfuE
    out.insert(out.end(), copyright_.begin(), copyright_.end());

    // 3. GDR (record type 2).
    res.gdr_offset = static_cast<int64_t>(out.size());
    cdf_internal::PatchBe64(out, cdr_gdr_patch_pos,
                            gdr_offset_override_.value_or(res.gdr_offset));

    const int32_t rnum_dims =
        rnum_dims_override_.value_or(static_cast<int32_t>(rdims_.size()));
    const int64_t gdr_size = 84 + static_cast<int64_t>(rdims_.size()) * 4;
    cdf_internal::AppendBe64(out, static_cast<uint64_t>(gdr_size));
    cdf_internal::AppendBe32(
        out, static_cast<uint32_t>(gdr_record_type_override_.value_or(2)));
    const size_t gdr_rvdr_patch_pos = out.size();
    cdf_internal::AppendBe64(out, 0);  // rVDRhead
    const size_t gdr_zvdr_patch_pos = out.size();
    cdf_internal::AppendBe64(out, 0);  // zVDRhead
    const size_t gdr_adr_patch_pos = out.size();
    cdf_internal::AppendBe64(out, 0);  // ADRhead
    const size_t gdr_eof_patch_pos = out.size();
    cdf_internal::AppendBe64(out, 0);  // eof
    const int32_t nr_vars =
        nr_vars_override_.value_or(static_cast<int32_t>(rvars_.size()));
    const int32_t num_attr =
        num_attr_override_.value_or(static_cast<int32_t>(attrs_.size()));
    const int32_t nz_vars =
        nz_vars_override_.value_or(static_cast<int32_t>(zvars_.size()));
    cdf_internal::AppendBe32(out, static_cast<uint32_t>(nr_vars));
    cdf_internal::AppendBe32(out, static_cast<uint32_t>(num_attr));
    cdf_internal::AppendBe32(out, static_cast<uint32_t>(ComputeRMaxRec()));
    cdf_internal::AppendBe32(out, static_cast<uint32_t>(rnum_dims));
    cdf_internal::AppendBe32(out, static_cast<uint32_t>(nz_vars));
    cdf_internal::AppendBe64(out, 0);  // u_eof
    cdf_internal::AppendBe32(out, 0);  // rfuC
    cdf_internal::AppendBe32(out, 0);  // leap_second_last_updated
    cdf_internal::AppendBe32(out, 0);  // rfuE
    for (const int32_t dim : rdims_) {
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(dim));
    }

    // 4. rVDRs (var numbers 0 .. rvars_.size()-1).
    int32_t next_var_num = 0;
    const int64_t rvdr_head = EmitVarList(out, rvars_, /*is_zvar=*/false,
                                          next_var_num, res.rvar_offsets);
    cdf_internal::PatchBe64(out, gdr_rvdr_patch_pos,
                            rvdr_head_override_.value_or(rvdr_head));

    // 5. zVDRs (var numbers rvars_.size() .. rvars_.size()+zvars_.size()-1).
    const int64_t zvdr_head = EmitVarList(out, zvars_, /*is_zvar=*/true,
                                          next_var_num, res.zvar_offsets);
    cdf_internal::PatchBe64(out, gdr_zvdr_patch_pos,
                            zvdr_head_override_.value_or(zvdr_head));

    // 6. ADRs and AEDRs.
    const int64_t adr_head = EmitAttrList(out, res.adr_offsets);
    cdf_internal::PatchBe64(out, gdr_adr_patch_pos,
                            adr_head_override_.value_or(adr_head));

    // 7. Optional MD5 trailer (16 bytes) and EOF patching.
    if (md5_checksum_ && append_md5_trailer_) {
      out.insert(out.end(), 16, 0U);
    }
    const int64_t eof =
        eof_override_.value_or(static_cast<int64_t>(out.size()));
    cdf_internal::PatchBe64(out, gdr_eof_patch_pos, eof);

    return res;
  }

  [[nodiscard]] std::vector<uint8_t> Build() const {
    return BuildWithOffsets().bytes;
  }

  [[nodiscard]] std::vector<uint8_t> Finalize() const { return Build(); }

 private:
  [[nodiscard]] int64_t RecordByteSize(const CdfVarSpec& var,
                                       bool is_zvar) const {
    const int32_t type_code = var.raw_data_type_override.value_or(
        static_cast<int32_t>(var.data_type));
    const int64_t elem_size =
        (type_code == static_cast<int32_t>(CdfDataType::kChar) ||
         type_code == static_cast<int32_t>(CdfDataType::kUchar))
            ? static_cast<int64_t>(var.num_elems)
            : static_cast<int64_t>(CdfDataTypeByteSize(var.data_type));
    int64_t vals_per_rec = 1;
    const std::vector<int32_t>& dims = is_zvar ? var.dims : rdims_;
    for (size_t i = 0; i < dims.size(); ++i) {
      const bool varies = var.dim_varys.empty() ||
                          (i < var.dim_varys.size() && var.dim_varys[i] != 0);
      if (varies) {
        vals_per_rec *= static_cast<int64_t>(dims[i]);
      }
    }
    return vals_per_rec * elem_size;
  }

  [[nodiscard]] static int32_t MaxRecInVxr(const CdfVxrNode& node) {
    int32_t max_rec = 0;
    for (const CdfVxrEntry& entry : node.entries) {
      max_rec = std::max(max_rec, entry.last_rec);
      if (entry.child_vxr != nullptr) {
        max_rec = std::max(max_rec, MaxRecInVxr(*entry.child_vxr));
      }
    }
    if (node.next_vxr != nullptr) {
      max_rec = std::max(max_rec, MaxRecInVxr(*node.next_vxr));
    }
    return max_rec;
  }

  [[nodiscard]] int32_t ComputeVarMaxRec(const CdfVarSpec& var,
                                         bool is_zvar) const {
    if (var.max_rec_override.has_value()) {
      return *var.max_rec_override;
    }
    if (!var.rec_varys) {
      return 0;
    }
    if (var.vxr_root != nullptr) {
      return MaxRecInVxr(*var.vxr_root);
    }
    if (!var.chunks.empty()) {
      int32_t max_rec = 0;
      for (const CdfVarChunkSpec& c : var.chunks) {
        max_rec = std::max(max_rec, c.last_rec);
      }
      return max_rec;
    }
    if (!var.data.empty()) {
      const int64_t rec_bytes = RecordByteSize(var, is_zvar);
      if (rec_bytes > 0) {
        const int64_t nrecs = static_cast<int64_t>(var.data.size()) / rec_bytes;
        return nrecs > 0 ? static_cast<int32_t>(nrecs - 1) : 0;
      }
    }
    return 0;
  }

  [[nodiscard]] int32_t ComputeRMaxRec() const {
    if (rvars_.empty()) {
      return -1;
    }
    int32_t max_rec = 0;
    for (const CdfVarSpec& rvar : rvars_) {
      max_rec = std::max(max_rec, ComputeVarMaxRec(rvar, /*is_zvar=*/false));
    }
    return max_rec;
  }

  [[nodiscard]] static int64_t EmitChunkRecord(std::vector<uint8_t>& out,
                                               const CdfVarChunkSpec& chunk) {
    const int64_t offset = static_cast<int64_t>(out.size());
    if (chunk.compressed) {
      std::vector<uint8_t> compressed_buf;
      std::span<const uint8_t> payload;
      if (chunk.raw_cvvr_payload.has_value()) {
        payload = *chunk.raw_cvvr_payload;
      } else {
        (void)GzipCompress(chunk.data, compressed_buf);
        payload = compressed_buf;
      }
      const int64_t csize =
          chunk.csize_override.value_or(static_cast<int64_t>(payload.size()));
      const int64_t rec_size = 24 + static_cast<int64_t>(payload.size());
      const int32_t rec_type = chunk.record_type_override.value_or(
          static_cast<int32_t>(CdfRecordType::kCvvr));
      cdf_internal::AppendBe64(out, static_cast<uint64_t>(rec_size));
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(rec_type));
      cdf_internal::AppendBe32(out, 0);  // rfuA
      cdf_internal::AppendBe64(out, static_cast<uint64_t>(csize));
      out.insert(out.end(), payload.begin(), payload.end());
    } else {
      const int64_t rec_size = 12 + static_cast<int64_t>(chunk.data.size());
      const int32_t rec_type = chunk.record_type_override.value_or(
          static_cast<int32_t>(CdfRecordType::kVvr));
      cdf_internal::AppendBe64(out, static_cast<uint64_t>(rec_size));
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(rec_type));
      out.insert(out.end(), chunk.data.begin(), chunk.data.end());
    }
    return offset;
  }

  static void EmitVxrChain(std::vector<uint8_t>& out, const CdfVxrNode& node,
                           int64_t& first_offset, int64_t& last_offset) {
    const int64_t node_offset = static_cast<int64_t>(out.size());
    if (first_offset == 0) {
      first_offset = node_offset;
    }
    last_offset = node_offset;

    const int32_t rec_type = node.record_type_override.value_or(
        static_cast<int32_t>(CdfRecordType::kVxr));
    const int32_t n_entries = node.n_entries_override.value_or(
        static_cast<int32_t>(node.entries.size()));
    const int32_t nused_entries = node.nused_entries_override.value_or(
        static_cast<int32_t>(node.entries.size()));
    const size_t table_slots =
        n_entries > static_cast<int32_t>(node.entries.size())
            ? static_cast<size_t>(n_entries)
            : node.entries.size();

    const int64_t rec_size = 28 + static_cast<int64_t>(table_slots) * 16;
    cdf_internal::AppendBe64(out, static_cast<uint64_t>(rec_size));
    cdf_internal::AppendBe32(out, static_cast<uint32_t>(rec_type));
    const size_t vxr_next_patch_pos = out.size();
    cdf_internal::AppendBe64(out, 0);  // vxr_next
    cdf_internal::AppendBe32(out, static_cast<uint32_t>(n_entries));
    cdf_internal::AppendBe32(out, static_cast<uint32_t>(nused_entries));

    const size_t table_base = out.size();
    out.insert(out.end(), table_slots * 16, 0U);

    // Emit each entry's target (child VXR or VVR/CVVR) and populate table.
    for (size_t i = 0; i < node.entries.size(); ++i) {
      const CdfVxrEntry& entry = node.entries[i];
      int64_t target_offset = 0;
      if (entry.child_vxr != nullptr) {
        int64_t child_last = 0;
        EmitVxrChain(out, *entry.child_vxr, target_offset, child_last);
      } else if (entry.chunk.has_value()) {
        target_offset = EmitChunkRecord(out, *entry.chunk);
      }
      if (entry.vr_offset_override.has_value()) {
        target_offset = *entry.vr_offset_override;
      }

      cdf_internal::PatchBe32(out, table_base + i * 4, entry.first_rec);
      cdf_internal::PatchBe32(out, table_base + (table_slots + i) * 4,
                              entry.last_rec);
      cdf_internal::PatchBe64(out, table_base + (table_slots + i) * 8,
                              target_offset);
    }

    int64_t next_offset = 0;
    if (node.next_vxr != nullptr) {
      int64_t unused_last = 0;
      EmitVxrChain(out, *node.next_vxr, next_offset, unused_last);
      last_offset = unused_last;
    }
    if (node.vxr_next_override.has_value()) {
      next_offset = *node.vxr_next_override;
    }
    cdf_internal::PatchBe64(out, vxr_next_patch_pos, next_offset);
  }

  [[nodiscard]] static int64_t EmitCpr(std::vector<uint8_t>& out,
                                       const CdfVarSpec& var) {
    const int64_t offset = static_cast<int64_t>(out.size());
    const int32_t pcount = static_cast<int32_t>(var.cparms.size());
    const int64_t rec_size = 24 + static_cast<int64_t>(pcount) * 4;
    const int32_t rec_type = var.cpr_record_type_override.value_or(
        static_cast<int32_t>(CdfRecordType::kCpr));
    const int32_t ctype =
        var.cpr_ctype_override.value_or(static_cast<int32_t>(var.compression));

    cdf_internal::AppendBe64(out, static_cast<uint64_t>(rec_size));
    cdf_internal::AppendBe32(out, static_cast<uint32_t>(rec_type));
    cdf_internal::AppendBe32(out, static_cast<uint32_t>(ctype));
    cdf_internal::AppendBe32(out, 0);  // rfuA
    cdf_internal::AppendBe32(out, static_cast<uint32_t>(pcount));
    for (const int32_t parm : var.cparms) {
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(parm));
    }
    return offset;
  }

  [[nodiscard]] int64_t EmitVarList(std::vector<uint8_t>& out,
                                    const std::vector<CdfVarSpec>& vars,
                                    bool is_zvar, int32_t& next_var_num,
                                    std::vector<int64_t>& vdr_offsets) const {
    if (vars.empty()) {
      return 0;
    }
    int64_t head_offset = 0;
    size_t prev_next_patch_pos = 0;

    for (size_t i = 0; i < vars.size(); ++i) {
      const CdfVarSpec& var = vars[i];
      const int64_t vdr_offset = static_cast<int64_t>(out.size());
      vdr_offsets.push_back(vdr_offset);
      if (i == 0) {
        head_offset = vdr_offset;
      } else if (!vars[i - 1].vdr_next_override.has_value()) {
        cdf_internal::PatchBe64(out, prev_next_patch_pos, vdr_offset);
      }

      const int32_t num_dims = is_zvar
                                   ? var.num_dims_override.value_or(
                                         static_cast<int32_t>(var.dims.size()))
                                   : static_cast<int32_t>(rdims_.size());
      const int64_t dim_fields_bytes =
          is_zvar ? (4 + static_cast<int64_t>(var.dims.size()) * 8)
                  : (static_cast<int64_t>(rdims_.size()) * 4);
      const int64_t vdr_size = 340 + dim_fields_bytes;

      const int32_t default_rec_type =
          is_zvar ? static_cast<int32_t>(CdfRecordType::kZvdr)
                  : static_cast<int32_t>(CdfRecordType::kRvdr);
      const int32_t rec_type =
          var.record_type_override.value_or(default_rec_type);
      const int32_t data_type = var.raw_data_type_override.value_or(
          static_cast<int32_t>(var.data_type));
      const int32_t max_rec = ComputeVarMaxRec(var, is_zvar);
      int32_t flags = 0;
      if (var.rec_varys) {
        flags |= kCdfVdrFlagRecVarys;
      }
      if (var.has_compression) {
        flags |= kCdfVdrFlagCompression;
      }
      if (var.flags_override.has_value()) {
        flags = *var.flags_override;
      }
      const int32_t num_elems = var.num_elems_override.value_or(var.num_elems);
      const int32_t var_num = var.var_num_override.value_or(next_var_num);
      ++next_var_num;

      cdf_internal::AppendBe64(out, static_cast<uint64_t>(vdr_size));
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(rec_type));
      prev_next_patch_pos = out.size();
      cdf_internal::AppendBe64(
          out, static_cast<uint64_t>(var.vdr_next_override.value_or(0)));
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(data_type));
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(max_rec));
      const size_t vxr_head_patch_pos = out.size();
      cdf_internal::AppendBe64(out, 0);  // vxr_head
      const size_t vxr_tail_patch_pos = out.size();
      cdf_internal::AppendBe64(out, 0);  // vxr_tail
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(flags));
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(var.sparse_records));
      cdf_internal::AppendBe32(out, 0);  // rfuB
      cdf_internal::AppendBe32(out, 0);  // rfuC
      cdf_internal::AppendBe32(out, 0);  // rfuF
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(num_elems));
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(var_num));
      const size_t cpr_patch_pos = out.size();
      cdf_internal::AppendBe64(out, static_cast<uint64_t>(-1));  // cpr_spr_off
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(var.blocking_factor));
      cdf_internal::AppendFixedName(out, var.name, 256, var.space_pad_name);

      const size_t dim_count = is_zvar ? var.dims.size() : rdims_.size();
      if (is_zvar) {
        cdf_internal::AppendBe32(out, static_cast<uint32_t>(num_dims));
        for (const int32_t d : var.dims) {
          cdf_internal::AppendBe32(out, static_cast<uint32_t>(d));
        }
      }
      for (size_t d = 0; d < dim_count; ++d) {
        const int32_t vary =
            var.dim_varys.empty()
                ? 1
                : (d < var.dim_varys.size() ? var.dim_varys[d] : 0);
        cdf_internal::AppendBe32(out, static_cast<uint32_t>(vary));
      }

      // Emit CPR if compressed.
      int64_t cpr_offset = -1;
      if (var.has_compression && var.emit_cpr) {
        cpr_offset = EmitCpr(out, var);
      }
      if (var.cpr_offset_override.has_value()) {
        cpr_offset = *var.cpr_offset_override;
      }
      cdf_internal::PatchBe64(out, cpr_patch_pos, cpr_offset);

      // Emit VXR tree/chain and VVR/CVVR chunks.
      std::shared_ptr<CdfVxrNode> root = var.vxr_root;
      if (root == nullptr && !var.chunks.empty()) {
        root = BuildVxrTreeFromChunks(var.chunks, var.max_vxr_entries,
                                      var.nested_vxr_tree);
      } else if (root == nullptr && !var.data.empty()) {
        std::vector<CdfVarChunkSpec> single(1);
        single[0].first_rec = 0;
        single[0].last_rec = max_rec;
        single[0].data = var.data;
        single[0].compressed = var.has_compression && var.compress_data;
        root = BuildVxrTreeFromChunks(single);
      }

      int64_t vxr_head = 0;
      int64_t vxr_tail = 0;
      if (root != nullptr) {
        EmitVxrChain(out, *root, vxr_head, vxr_tail);
      }
      cdf_internal::PatchBe64(out, vxr_head_patch_pos,
                              var.vxr_head_override.value_or(vxr_head));
      cdf_internal::PatchBe64(out, vxr_tail_patch_pos,
                              var.vxr_tail_override.value_or(vxr_tail));
    }

    return head_offset;
  }

  [[nodiscard]] static int64_t EmitAedrList(
      std::vector<uint8_t>& out,
      std::span<const CdfAttrEntrySpec* const> entries, int32_t attr_num) {
    if (entries.empty()) {
      return 0;
    }
    int64_t head_offset = 0;
    size_t prev_next_patch_pos = 0;

    for (size_t i = 0; i < entries.size(); ++i) {
      const CdfAttrEntrySpec& e = *entries[i];
      const int64_t offset = static_cast<int64_t>(out.size());
      if (i == 0) {
        head_offset = offset;
      } else if (!entries[i - 1]->aedr_next_override.has_value()) {
        cdf_internal::PatchBe64(out, prev_next_patch_pos, offset);
      }

      const int64_t rec_size = 56 + static_cast<int64_t>(e.raw_value.size());
      const int32_t default_type =
          e.is_z_entry ? static_cast<int32_t>(CdfRecordType::kAzEdr)
                       : static_cast<int32_t>(CdfRecordType::kAgrEdr);
      const int32_t rec_type = e.record_type_override.value_or(default_type);
      const int32_t data_type =
          e.raw_data_type_override.value_or(static_cast<int32_t>(e.data_type));
      const int32_t eff_attr_num = e.attr_num_override.value_or(attr_num);
      const int32_t eff_entry_num = e.entry_num_override.value_or(e.entry_num);
      const int32_t eff_num_elems = e.num_elems_override.value_or(e.num_elems);

      cdf_internal::AppendBe64(out, static_cast<uint64_t>(rec_size));
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(rec_type));
      prev_next_patch_pos = out.size();
      cdf_internal::AppendBe64(
          out, static_cast<uint64_t>(e.aedr_next_override.value_or(0)));
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(eff_attr_num));
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(data_type));
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(eff_entry_num));
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(eff_num_elems));
      for (int r = 0; r < 5; ++r) {
        cdf_internal::AppendBe32(out, 0);  // rfuA..rfuE
      }
      out.insert(out.end(), e.raw_value.begin(), e.raw_value.end());
    }
    return head_offset;
  }

  [[nodiscard]] int64_t EmitAttrList(std::vector<uint8_t>& out,
                                     std::vector<int64_t>& adr_offsets) const {
    if (attrs_.empty()) {
      return 0;
    }
    int64_t head_offset = 0;
    size_t prev_next_patch_pos = 0;

    for (size_t i = 0; i < attrs_.size(); ++i) {
      const CdfAttrSpec& attr = attrs_[i];
      const int64_t adr_offset = static_cast<int64_t>(out.size());
      adr_offsets.push_back(adr_offset);
      if (i == 0) {
        head_offset = adr_offset;
      } else if (!attrs_[i - 1].adr_next_override.has_value()) {
        cdf_internal::PatchBe64(out, prev_next_patch_pos, adr_offset);
      }

      std::vector<const CdfAttrEntrySpec*> gr_entries;
      std::vector<const CdfAttrEntrySpec*> z_entries;
      int32_t max_gr = -1;
      int32_t max_z = -1;
      for (const CdfAttrEntrySpec& e : attr.entries) {
        if (e.is_z_entry) {
          z_entries.push_back(&e);
          max_z = std::max(max_z, e.entry_num);
        } else {
          gr_entries.push_back(&e);
          max_gr = std::max(max_gr, e.entry_num);
        }
      }

      const int32_t attr_num =
          attr.attr_num_override.value_or(static_cast<int32_t>(i));
      const int32_t scope =
          attr.raw_scope_override.value_or(static_cast<int32_t>(attr.scope));
      const int32_t rec_type = attr.record_type_override.value_or(
          static_cast<int32_t>(CdfRecordType::kAdr));
      const int32_t ngr = attr.ngr_entries_override.value_or(
          static_cast<int32_t>(gr_entries.size()));
      const int32_t nz = attr.nz_entries_override.value_or(
          static_cast<int32_t>(z_entries.size()));

      cdf_internal::AppendBe64(out, 324);  // RecordSize (68 + 256)
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(rec_type));
      prev_next_patch_pos = out.size();
      cdf_internal::AppendBe64(
          out, static_cast<uint64_t>(attr.adr_next_override.value_or(0)));
      const size_t agredr_patch_pos = out.size();
      cdf_internal::AppendBe64(out, 0);  // AgrEDRhead
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(scope));
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(attr_num));
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(ngr));
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(max_gr));
      cdf_internal::AppendBe32(out, 0);  // rfuA
      const size_t azedr_patch_pos = out.size();
      cdf_internal::AppendBe64(out, 0);  // AzEDRhead
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(nz));
      cdf_internal::AppendBe32(out, static_cast<uint32_t>(max_z));
      cdf_internal::AppendBe32(out, 0);  // rfuE
      cdf_internal::AppendFixedName(out, attr.name, 256, attr.space_pad_name);

      const int64_t gr_head = EmitAedrList(out, gr_entries, attr_num);
      const int64_t z_head = EmitAedrList(out, z_entries, attr_num);
      cdf_internal::PatchBe64(out, agredr_patch_pos,
                              attr.agredr_head_override.value_or(gr_head));
      cdf_internal::PatchBe64(out, azedr_patch_pos,
                              attr.azedr_head_override.value_or(z_head));
    }
    return head_offset;
  }

  uint32_t magic0_ = kCdfMagicV3;
  uint32_t magic1_ = kCdfMagicUncompressed;
  int32_t cdf_version_ = 3;
  int32_t cdf_release_ = 8;
  int32_t cdf_increment_ = 0;
  int32_t encoding_ = static_cast<int32_t>(CdfEncoding::kNetwork);
  CdfArrayOrdering ordering_ = CdfArrayOrdering::kRowMajor;
  bool single_file_ = true;
  bool md5_checksum_ = false;
  bool append_md5_trailer_ = false;
  std::string copyright_;

  std::vector<int32_t> rdims_;
  std::vector<CdfVarSpec> rvars_;
  std::vector<CdfVarSpec> zvars_;
  std::vector<CdfAttrSpec> attrs_;

  std::optional<int32_t> cdr_record_type_override_;
  std::optional<int32_t> cdr_flags_override_;
  std::optional<int64_t> gdr_offset_override_;
  std::optional<int32_t> gdr_record_type_override_;
  std::optional<int64_t> rvdr_head_override_;
  std::optional<int64_t> zvdr_head_override_;
  std::optional<int64_t> adr_head_override_;
  std::optional<int64_t> eof_override_;
  std::optional<int32_t> nr_vars_override_;
  std::optional<int32_t> num_attr_override_;
  std::optional<int32_t> rnum_dims_override_;
  std::optional<int32_t> nz_vars_override_;
};

}  // namespace stcorp_coda

#endif  // THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_CDF_BUILDER_H_
