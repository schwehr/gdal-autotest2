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
// In-memory coda_product builder for hermetic unit testing of libharp
// ingestion modules without .codadef files or binary testdata fixtures.

#ifndef THIRD_PARTY_STCORP_HARP_GOOGLE_UTIL_CODA_MEM_PRODUCT_H_
#define THIRD_PARTY_STCORP_HARP_GOOGLE_UTIL_CODA_MEM_PRODUCT_H_

#include <cstdint>
#include <span>
#include <string_view>

#include "third_party/stcorp_coda/libcoda/coda.h"

namespace stcorp_harp {

// Opaque handle to a node in an in-memory CODA product tree (owned by
// CodaMemProduct once attached to the root or an ancestor).
struct CodaMemNode;

// Builds and owns an in-memory coda_product (backed by coda_mem_* types) and
// optionally an owning coda_product_class / product_type / version metadata.
class CodaMemProduct {
 public:
  // Constructs an empty in-memory product with an empty root record.
  // `format` defaults to coda_format_xml; pass coda_format_grib when testing
  // modules that assert coda_get_product_format == coda_format_grib.
  explicit CodaMemProduct(coda_format format = coda_format_xml,
                          std::string_view filename = "mem_product.dat");
  ~CodaMemProduct();

  CodaMemProduct(const CodaMemProduct&) = delete;
  CodaMemProduct& operator=(const CodaMemProduct&) = delete;

  // Returns the underlying coda_product* (never null if ok()).
  coda_product* product() const { return product_; }
  bool ok() const { return product_ != nullptr; }

  // Returns the root record node.
  CodaMemNode* root() const { return root_; }

  // Replaces the product's root node with `new_root` (deleting the prior root).
  bool SetRootNode(CodaMemNode* new_root);

  // Attaches product_class, product_type, and version metadata so
  // coda_get_product_class / coda_get_product_type / coda_get_product_version
  // return these values. Hyphens in `product_type` (e.g. Sentinel-4
  // "UVN-2-ALH") are preserved in coda_get_product_type(), while hyphens in
  // the internal coda_product_definition name are replaced with '_' so
  // coda_product_definition_new's identifier check succeeds.
  bool SetProductInfo(std::string_view product_class,
                      std::string_view product_type, int version);

  // Updates product->file_size.
  void SetFileSize(int64_t file_size);

  // Record and union builders.
  CodaMemNode* MakeRecord();
  // Creates a union record with `field_names` (0..N-1) where only
  // `active_index` is populated with `active_child`.
  CodaMemNode* MakeUnion(std::span<const std::string_view> field_names,
                         long active_index, CodaMemNode* active_child);
  bool AddField(CodaMemNode* record, std::string_view name, CodaMemNode* child);
  // Registers field `name` in `record`'s definition while leaving its value
  // unavailable (so coda_cursor_get_record_field_available_status returns 0).
  bool AddUnavailableField(CodaMemNode* record, std::string_view name);

  // Attribute builder (works on root(), any record, array, or scalar node).
  bool AddAttribute(CodaMemNode* target, std::string_view name,
                    CodaMemNode* attr_value);

  // Scalar builders.
  CodaMemNode* MakeInt8(int8_t value);
  CodaMemNode* MakeUint8(uint8_t value);
  CodaMemNode* MakeInt16(int16_t value);
  CodaMemNode* MakeUint16(uint16_t value);
  CodaMemNode* MakeInt32(int32_t value);
  CodaMemNode* MakeUint32(uint32_t value);
  CodaMemNode* MakeInt64(int64_t value);
  CodaMemNode* MakeUint64(uint64_t value);
  CodaMemNode* MakeFloat(float value);
  CodaMemNode* MakeDouble(double value);
  CodaMemNode* MakeChar(char value);
  CodaMemNode* MakeString(std::string_view value);
  CodaMemNode* MakeRaw(std::span<const uint8_t> bytes);
  CodaMemNode* MakeTime(double seconds_since_2000);
  CodaMemNode* MakeNoData();

  // Typed 1D/ND array builders (product of `dims` must equal `values.size()`).
  // Passing a 0 in `dims` with empty `values` creates a valid 0-element array.
  CodaMemNode* MakeInt8Array(std::span<const long> dims,
                             std::span<const int8_t> values);
  CodaMemNode* MakeUint8Array(std::span<const long> dims,
                              std::span<const uint8_t> values);
  CodaMemNode* MakeInt16Array(std::span<const long> dims,
                              std::span<const int16_t> values);
  CodaMemNode* MakeUint16Array(std::span<const long> dims,
                               std::span<const uint16_t> values);
  CodaMemNode* MakeInt32Array(std::span<const long> dims,
                              std::span<const int32_t> values);
  CodaMemNode* MakeUint32Array(std::span<const long> dims,
                               std::span<const uint32_t> values);
  CodaMemNode* MakeInt64Array(std::span<const long> dims,
                              std::span<const int64_t> values);
  CodaMemNode* MakeUint64Array(std::span<const long> dims,
                               std::span<const uint64_t> values);
  CodaMemNode* MakeFloatArray(std::span<const long> dims,
                              std::span<const float> values);
  CodaMemNode* MakeDoubleArray(std::span<const long> dims,
                               std::span<const double> values);
  CodaMemNode* MakeCharArray(std::span<const long> dims,
                             std::span<const char> values);
  CodaMemNode* MakeStringArray(std::span<const long> dims,
                               std::span<const std::string_view> values);
  CodaMemNode* MakeTimeArray(std::span<const long> dims,
                             std::span<const double> values);

  // Generic array of heterogeneous-value but identical-schema nodes (e.g.
  // records). For empty arrays, pass empty `elements` and a template
  // `sample_elem_for_empty` (which will be consumed/freed).
  CodaMemNode* MakeNodeArray(std::span<const long> dims,
                             std::span<CodaMemNode* const> elements,
                             CodaMemNode* sample_elem_for_empty = nullptr);

  // Packed binary 2D+ array (supports coda_cursor_goto_array_element with
  // num_subs > 1, as used by sciamachy_l2).
  CodaMemNode* MakeBinaryDoubleArray(std::span<const long> dims,
                                     std::span<const double> values);

  // Packed binary 2D+ array of uniform `double`-field records (supports
  // coda_cursor_goto_array_element(num_subs > 1) -> record field reads).
  CodaMemNode* MakeBinaryDoubleRecordArray(
      std::span<const long> dims, std::span<const std::string_view> field_names,
      std::span<const double> packed_values);

 private:
  bool coda_initialized_ = false;
  coda_format format_;
  coda_product* product_ = nullptr;
  CodaMemNode* root_ = nullptr;
  void* product_class_ = nullptr;  // Owned coda_product_class*.
};

}  // namespace stcorp_harp

#endif  // THIRD_PARTY_STCORP_HARP_GOOGLE_UTIL_CODA_MEM_PRODUCT_H_
