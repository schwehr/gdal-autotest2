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
// Minimal in-memory NetCDF classic (CDF\x01, CDF\x02, CDF\x05) binary builder
// for synthesizing valid and malformed NetCDF files without checked-in data.

#ifndef THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_NETCDF_BUILDER_H_
#define THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_NETCDF_BUILDER_H_

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace stcorp_coda {

// NetCDF classic type tags (matching coda-netcdf.c).
enum class NetcdfType : int32_t {
  kByte = 1,
  kChar = 2,
  kShort = 3,
  kInt = 4,
  kFloat = 5,
  kDouble = 6,
};

struct NetcdfDimSpec {
  std::string_view name;
  int64_t size;  // 0 for unlimited (record) dimension.
};

struct NetcdfAttrSpec {
  std::string_view name;
  NetcdfType type;
  int64_t count;  // Number of elements (for kChar, string byte length).
  std::vector<uint8_t> raw_values;  // Big-endian encoded element bytes.
};

struct NetcdfVarSpec {
  std::string_view name;
  std::vector<int32_t> dim_ids;
  std::vector<NetcdfAttrSpec> attrs;
  NetcdfType type;
  int64_t
      vsize;  // Byte size per record (or total for fixed vars), 4-byte padded.
  std::vector<uint8_t> data;  // Big-endian raw data bytes for this variable.
};

class NetcdfBuilder {
 public:
  explicit NetcdfBuilder(int version = 1) : version_(version) {}

  void set_num_recs(int64_t num_recs) { num_recs_ = num_recs; }
  void AddDim(NetcdfDimSpec dim) { dims_.push_back(std::move(dim)); }
  void AddGlobalAttr(NetcdfAttrSpec attr) {
    global_attrs_.push_back(std::move(attr));
  }
  void AddVar(NetcdfVarSpec var) { vars_.push_back(std::move(var)); }

  static void AppendBe16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
  }

  static void AppendBe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
  }

  static void AppendBe64(std::vector<uint8_t>& out, uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
    }
  }

  static void AppendPad4(std::vector<uint8_t>& out, size_t nbytes) {
    const size_t rem = nbytes % 4;
    if (rem != 0) {
      out.insert(out.end(), 4 - rem, 0);
    }
  }

  void AppendLength(std::vector<uint8_t>& out, int64_t v) const {
    if (version_ == 5) {
      AppendBe64(out, static_cast<uint64_t>(v));
    } else {
      AppendBe32(out, static_cast<uint32_t>(v));
    }
  }

  void AppendName(std::vector<uint8_t>& out, std::string_view name) const {
    AppendLength(out, static_cast<int64_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
    AppendPad4(out, name.size());
  }

  void AppendOffset(std::vector<uint8_t>& out, int64_t off) const {
    if (version_ == 1) {
      AppendBe32(out, static_cast<uint32_t>(off));
    } else {
      AppendBe64(out, static_cast<uint64_t>(off));
    }
  }

  void AppendAttrArray(std::vector<uint8_t>& out,
                       std::span<const NetcdfAttrSpec> attrs) const {
    if (attrs.empty()) {
      AppendBe32(out, 0);  // ABSENT
      AppendLength(out, 0);
      return;
    }
    AppendBe32(out, 12);  // NC_ATTRIBUTE
    AppendLength(out, static_cast<int64_t>(attrs.size()));
    for (const NetcdfAttrSpec& attr : attrs) {
      AppendName(out, attr.name);
      AppendBe32(out, static_cast<uint32_t>(attr.type));
      AppendLength(out, attr.count);
      out.insert(out.end(), attr.raw_values.begin(), attr.raw_values.end());
      AppendPad4(out, attr.raw_values.size());
    }
  }

  std::vector<uint8_t> Build() const {
    std::vector<uint8_t> out;
    out.push_back('C');
    out.push_back('D');
    out.push_back('F');
    out.push_back(static_cast<uint8_t>(version_));
    AppendLength(out, num_recs_);

    // dim_array
    if (dims_.empty()) {
      AppendBe32(out, 0);
      AppendLength(out, 0);
    } else {
      AppendBe32(out, 10);  // NC_DIMENSION
      AppendLength(out, static_cast<int64_t>(dims_.size()));
      for (const NetcdfDimSpec& dim : dims_) {
        AppendName(out, dim.name);
        AppendLength(out, dim.size);
      }
    }

    // gatt_array
    AppendAttrArray(out, global_attrs_);

    // var_array
    std::vector<size_t> begin_patch_offsets;
    if (vars_.empty()) {
      AppendBe32(out, 0);
      AppendLength(out, 0);
    } else {
      AppendBe32(out, 11);  // NC_VARIABLE
      AppendLength(out, static_cast<int64_t>(vars_.size()));
      for (const NetcdfVarSpec& var : vars_) {
        AppendName(out, var.name);
        AppendLength(out, static_cast<int64_t>(var.dim_ids.size()));
        for (const int32_t dim_id : var.dim_ids) {
          AppendLength(out, dim_id);
        }
        AppendAttrArray(out, var.attrs);
        AppendBe32(out, static_cast<uint32_t>(var.type));
        AppendLength(out, var.vsize);
        begin_patch_offsets.push_back(out.size());
        AppendOffset(out, 0);  // Patched below once header size is known.
      }
    }

    // Emit variable data sequentially in AddVar() order (supports at most one
    // record variable placed last).
    for (size_t i = 0; i < vars_.size(); ++i) {
      const int64_t data_offset = static_cast<int64_t>(out.size());
      const size_t patch_pos = begin_patch_offsets[i];
      if (version_ == 1) {
        out[patch_pos + 0] = static_cast<uint8_t>((data_offset >> 24) & 0xFF);
        out[patch_pos + 1] = static_cast<uint8_t>((data_offset >> 16) & 0xFF);
        out[patch_pos + 2] = static_cast<uint8_t>((data_offset >> 8) & 0xFF);
        out[patch_pos + 3] = static_cast<uint8_t>(data_offset & 0xFF);
      } else {
        for (int b = 0; b < 8; ++b) {
          out[patch_pos + b] =
              static_cast<uint8_t>((data_offset >> (56 - 8 * b)) & 0xFF);
        }
      }
      out.insert(out.end(), vars_[i].data.begin(), vars_[i].data.end());
      AppendPad4(out, vars_[i].data.size());
    }
    return out;
  }

 private:
  int version_ = 1;
  int64_t num_recs_ = 0;
  std::vector<NetcdfDimSpec> dims_;
  std::vector<NetcdfAttrSpec> global_attrs_;
  std::vector<NetcdfVarSpec> vars_;
};

}  // namespace stcorp_coda

#endif  // THIRD_PARTY_STCORP_CODA_GOOGLE_UTIL_NETCDF_BUILDER_H_
