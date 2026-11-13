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

// Define CODA_INTERNAL before coda.h is included so coda.h does not define
// libcoda_version / coda_errno macros that collide with coda-internal.h.
#define CODA_INTERNAL

#include "third_party/stcorp_harp/google/util/coda_mem_product.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include "third_party/stcorp_coda/libcoda/config.h"
#ifdef THREAD_LOCAL
#undef THREAD_LOCAL
#endif
#define THREAD_LOCAL __thread

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-bin-internal.h"
#include "third_party/stcorp_coda/libcoda/coda-definition.h"
#include "third_party/stcorp_coda/libcoda/coda-mem-internal.h"
#include "third_party/stcorp_coda/libcoda/coda-type.h"
#include "third_party/stcorp_coda/libcoda/coda-xml-internal.h"
}  // extern "C"

namespace stcorp_harp {
namespace {

coda_endianness NativeEndianness() {
  return std::endian::native == std::endian::little ? coda_little_endian
                                                    : coda_big_endian;
}

coda_type_number* MakeNumberDef(coda_format format, coda_type_class type_class,
                                coda_native_type read_type, int64_t bit_size) {
  coda_type_number* def = coda_type_number_new(format, type_class);
  if (def == nullptr) {
    return nullptr;
  }
  if (coda_type_set_read_type(reinterpret_cast<coda_type*>(def), read_type) !=
          0 ||
      coda_type_set_bit_size(reinterpret_cast<coda_type*>(def), bit_size) !=
          0 ||
      coda_type_number_set_endianness(def, NativeEndianness()) != 0) {
    coda_type_release(reinterpret_cast<coda_type*>(def));
    return nullptr;
  }
  return def;
}

coda_type_text* MakeTextDef(coda_format format, coda_native_type read_type,
                            int64_t bit_size) {
  coda_type_text* def = coda_type_text_new(format);
  if (def == nullptr) {
    return nullptr;
  }
  if (coda_type_set_read_type(reinterpret_cast<coda_type*>(def), read_type) !=
      0) {
    coda_type_release(reinterpret_cast<coda_type*>(def));
    return nullptr;
  }
  if (bit_size >= 0 && coda_type_set_bit_size(reinterpret_cast<coda_type*>(def),
                                              bit_size) != 0) {
    coda_type_release(reinterpret_cast<coda_type*>(def));
    return nullptr;
  }
  return def;
}

coda_type_special* MakeTimeDef(coda_format format, coda_type* base_type) {
  coda_expression* expr = nullptr;
  if (coda_expression_from_string("float(.)", &expr) != 0) {
    return nullptr;
  }
  coda_type_special* def = coda_type_time_new(format, expr);
  if (def == nullptr) {
    coda_expression_delete(expr);
    return nullptr;
  }
  if (coda_type_time_set_base_type(def, base_type) != 0) {
    coda_type_release(reinterpret_cast<coda_type*>(def));
    return nullptr;
  }
  return def;
}

coda_type_array* MakeArrayDef(coda_format format, std::span<const long> dims,
                              coda_type* base_type) {
  coda_type_array* arr_def = coda_type_array_new(format);
  if (arr_def == nullptr) {
    return nullptr;
  }
  if (coda_type_array_set_base_type(arr_def, base_type) != 0) {
    coda_type_release(reinterpret_cast<coda_type*>(arr_def));
    return nullptr;
  }
  for (long d : dims) {
    if (coda_type_array_add_fixed_dimension(arr_def, d) != 0) {
      coda_type_release(reinterpret_cast<coda_type*>(arr_def));
      return nullptr;
    }
  }
  return arr_def;
}

void DeleteNodeIfNonNull(CodaMemNode* node) {
  if (node != nullptr) {
    coda_dynamic_type_delete(reinterpret_cast<coda_dynamic_type*>(node));
  }
}

template <typename T, typename NewFn>
CodaMemNode* MakeNumberArrayImpl(coda_format format, coda_product* product,
                                 std::span<const long> dims,
                                 std::span<const T> values,
                                 coda_type_class type_class,
                                 coda_native_type read_type, int64_t bit_size,
                                 NewFn new_fn) {
  coda_type_number* elem_def =
      MakeNumberDef(format, type_class, read_type, bit_size);
  if (elem_def == nullptr) {
    return nullptr;
  }
  coda_type_array* arr_def =
      MakeArrayDef(format, dims, reinterpret_cast<coda_type*>(elem_def));
  if (arr_def == nullptr) {
    coda_type_release(reinterpret_cast<coda_type*>(elem_def));
    return nullptr;
  }
  coda_mem_array* arr = coda_mem_array_new(arr_def, nullptr);
  // Release creator ref on arr_def unconditionally (0-based retain_count).
  coda_type_release(reinterpret_cast<coda_type*>(arr_def));
  if (arr == nullptr) {
    coda_type_release(reinterpret_cast<coda_type*>(elem_def));
    return nullptr;
  }
  for (size_t i = 0; i < values.size(); ++i) {
    coda_mem_data* elem = new_fn(elem_def, nullptr, product, values[i]);
    if (elem == nullptr ||
        coda_mem_array_set_element(
            arr, static_cast<long>(i),
            reinterpret_cast<coda_dynamic_type*>(elem)) != 0) {
      if (elem != nullptr) {
        coda_mem_type_delete(reinterpret_cast<coda_dynamic_type*>(elem));
      }
      coda_mem_type_delete(reinterpret_cast<coda_dynamic_type*>(arr));
      coda_type_release(reinterpret_cast<coda_type*>(elem_def));
      return nullptr;
    }
  }
  coda_type_release(reinterpret_cast<coda_type*>(elem_def));
  return reinterpret_cast<CodaMemNode*>(arr);
}

template <typename DataNewFn, typename TypeT>
CodaMemNode* MakeScalarAndRelease(TypeT* def, coda_product* product,
                                  DataNewFn new_fn) {
  if (def == nullptr) {
    return nullptr;
  }
  coda_mem_data* d = new_fn(def, nullptr, product);
  coda_type_release(reinterpret_cast<coda_type*>(def));
  return reinterpret_cast<CodaMemNode*>(d);
}

}  // namespace

CodaMemProduct::CodaMemProduct(coda_format format, std::string_view filename)
    : format_(format) {
  if (coda_init() != 0) {
    return;
  }
  coda_initialized_ = true;

  constexpr size_t kProdBytes =
      std::max(sizeof(coda_xml_product), sizeof(coda_bin_product));
  auto* prod = static_cast<coda_product*>(std::calloc(1, kProdBytes));
  if (prod == nullptr) {
    return;
  }
  prod->format = format_;
  if (format_ == coda_format_binary || format_ == coda_format_ascii) {
    reinterpret_cast<coda_bin_product*>(prod)->fd = -1;
  }
  const std::string fname(filename);
  prod->filename = strdup(fname.c_str());
  if (prod->filename == nullptr) {
    std::free(prod);
    return;
  }

  coda_type_record* root_def = coda_type_record_new(format_);
  if (root_def == nullptr) {
    std::free(prod->filename);
    std::free(prod);
    return;
  }
  coda_mem_record* root_rec = coda_mem_record_new(root_def, nullptr);
  coda_type_release(reinterpret_cast<coda_type*>(root_def));
  if (root_rec == nullptr) {
    std::free(prod->filename);
    std::free(prod);
    return;
  }
  prod->root_type = reinterpret_cast<coda_dynamic_type*>(root_rec);
  product_ = prod;
  root_ = reinterpret_cast<CodaMemNode*>(root_rec);
}

CodaMemProduct::~CodaMemProduct() {
  if (product_ != nullptr) {
    // Tear down in-memory product fields directly instead of coda_close(),
    // because coda_bin_close() does not delete root_type or mem_ptr.
    if (product_->product_variable_size != nullptr) {
      std::free(product_->product_variable_size);
      product_->product_variable_size = nullptr;
    }
    if (product_->product_variable != nullptr) {
      if (product_->product_definition != nullptr) {
        for (int i = 0; i < product_->product_definition->num_product_variables;
             ++i) {
          std::free(product_->product_variable[i]);
        }
      }
      std::free(product_->product_variable);
      product_->product_variable = nullptr;
    }
    if (product_->root_type != nullptr) {
      coda_dynamic_type_delete(product_->root_type);
      product_->root_type = nullptr;
    }
    std::free(const_cast<uint8_t*>(product_->mem_ptr));
    std::free(product_->filename);
    std::free(product_);
    product_ = nullptr;
  }
  if (product_class_ != nullptr) {
    coda_product_class_delete(static_cast<coda_product_class*>(product_class_));
    product_class_ = nullptr;
  }
  if (coda_initialized_) {
    coda_done();
    coda_initialized_ = false;
  }
}

bool CodaMemProduct::SetRootNode(CodaMemNode* new_root) {
  if (product_ == nullptr || new_root == nullptr) {
    DeleteNodeIfNonNull(new_root);
    return false;
  }
  if (product_->root_type != nullptr) {
    coda_dynamic_type_delete(product_->root_type);
  }
  product_->root_type = reinterpret_cast<coda_dynamic_type*>(new_root);
  root_ = new_root;
  return true;
}

bool CodaMemProduct::SetProductInfo(std::string_view product_class_name,
                                    std::string_view product_type_name,
                                    int version) {
  if (product_ == nullptr || product_class_ != nullptr) {
    return false;
  }
  const std::string cls_str(product_class_name);
  const std::string type_str(product_type_name);
  coda_product_class* pclass = coda_product_class_new(cls_str.c_str());
  if (pclass == nullptr) {
    return false;
  }
  coda_product_type* ptype = coda_product_type_new(type_str.c_str());
  if (ptype == nullptr ||
      coda_product_class_add_product_type(pclass, ptype) != 0) {
    if (ptype != nullptr) {
      coda_product_type_delete(ptype);
    }
    coda_product_class_delete(pclass);
    return false;
  }
  std::string def_str = type_str;
  std::replace(def_str.begin(), def_str.end(), '-', '_');
  coda_product_definition* pdef =
      coda_product_definition_new(def_str.c_str(), format_, version);
  if (pdef == nullptr ||
      coda_product_type_add_product_definition(ptype, pdef) != 0) {
    if (pdef != nullptr) {
      coda_product_definition_delete(pdef);
    }
    coda_product_class_delete(pclass);
    return false;
  }
  product_->product_definition = pdef;
  product_class_ = pclass;
  return true;
}

void CodaMemProduct::SetFileSize(int64_t file_size) {
  if (product_ != nullptr) {
    product_->file_size = file_size;
  }
}

CodaMemNode* CodaMemProduct::MakeRecord() {
  coda_type_record* def = coda_type_record_new(format_);
  if (def == nullptr) {
    return nullptr;
  }
  coda_mem_record* rec = coda_mem_record_new(def, nullptr);
  coda_type_release(reinterpret_cast<coda_type*>(def));
  return reinterpret_cast<CodaMemNode*>(rec);
}

CodaMemNode* CodaMemProduct::MakeUnion(
    std::span<const std::string_view> field_names, long active_index,
    CodaMemNode* active_child) {
  if (active_index < 0 ||
      static_cast<size_t>(active_index) >= field_names.size() ||
      active_child == nullptr) {
    DeleteNodeIfNonNull(active_child);
    return nullptr;
  }
  coda_type_record* def = coda_type_union_new(format_);
  if (def == nullptr) {
    DeleteNodeIfNonNull(active_child);
    return nullptr;
  }
  auto* child_dyn = reinterpret_cast<coda_dynamic_type*>(active_child);
  for (size_t i = 0; i < field_names.size(); ++i) {
    const std::string fname(field_names[i]);
    coda_type* ftype =
        (static_cast<long>(i) == active_index)
            ? child_dyn->definition
            : reinterpret_cast<coda_type*>(coda_type_empty_record(format_));
    coda_type_record_field* field = coda_type_record_field_new(fname.c_str());
    if (field == nullptr || coda_type_record_field_set_optional(field) != 0 ||
        coda_type_record_field_set_type(field, ftype) != 0 ||
        coda_type_record_add_field(def, field) != 0) {
      if (field != nullptr) {
        coda_type_record_field_delete(field);
      }
      coda_type_release(reinterpret_cast<coda_type*>(def));
      DeleteNodeIfNonNull(active_child);
      return nullptr;
    }
  }
  coda_mem_record* rec = coda_mem_record_new(def, nullptr);
  coda_type_release(reinterpret_cast<coda_type*>(def));
  if (rec == nullptr) {
    DeleteNodeIfNonNull(active_child);
    return nullptr;
  }
  rec->field_type[active_index] = child_dyn;
  return reinterpret_cast<CodaMemNode*>(rec);
}

bool CodaMemProduct::AddField(CodaMemNode* record, std::string_view name,
                              CodaMemNode* child) {
  if (record == nullptr || child == nullptr) {
    DeleteNodeIfNonNull(child);
    return false;
  }
  const std::string fname(name);
  if (coda_mem_record_add_field(reinterpret_cast<coda_mem_record*>(record),
                                fname.c_str(),
                                reinterpret_cast<coda_dynamic_type*>(child),
                                /*update_definition=*/1) != 0) {
    DeleteNodeIfNonNull(child);
    return false;
  }
  return true;
}

bool CodaMemProduct::AddUnavailableField(CodaMemNode* record,
                                         std::string_view name) {
  if (record == nullptr) {
    return false;
  }
  auto* rec = reinterpret_cast<coda_mem_record*>(record);
  const std::string fname(name);
  coda_type* dummy =
      reinterpret_cast<coda_type*>(coda_type_empty_record(format_));
  if (coda_type_record_create_field(
          reinterpret_cast<coda_type_record*>(rec->definition), fname.c_str(),
          dummy) != 0) {
    return false;
  }
  auto** new_ft = static_cast<coda_dynamic_type**>(std::realloc(
      rec->field_type, static_cast<size_t>(rec->definition->num_fields) *
                           sizeof(coda_dynamic_type*)));
  if (new_ft == nullptr) {
    return false;
  }
  rec->field_type = new_ft;
  for (long i = rec->num_fields; i < rec->definition->num_fields; ++i) {
    rec->field_type[i] = nullptr;
  }
  rec->num_fields = rec->definition->num_fields;
  return true;
}

bool CodaMemProduct::AddAttribute(CodaMemNode* target, std::string_view name,
                                  CodaMemNode* attr_value) {
  if (target == nullptr || attr_value == nullptr) {
    DeleteNodeIfNonNull(attr_value);
    return false;
  }
  const std::string aname(name);
  if (coda_mem_type_add_attribute(
          reinterpret_cast<coda_mem_type*>(target), aname.c_str(),
          reinterpret_cast<coda_dynamic_type*>(attr_value),
          /*update_definition=*/1) != 0) {
    DeleteNodeIfNonNull(attr_value);
    return false;
  }
  return true;
}

CodaMemNode* CodaMemProduct::MakeInt8(int8_t value) {
  coda_type_number* def =
      MakeNumberDef(format_, coda_integer_class, coda_native_type_int8, 8);
  return MakeScalarAndRelease(
      def, product_,
      [value](coda_type_number* t, coda_dynamic_type* a, coda_product* p) {
        return coda_mem_int8_new(t, a, p, value);
      });
}

CodaMemNode* CodaMemProduct::MakeUint8(uint8_t value) {
  coda_type_number* def =
      MakeNumberDef(format_, coda_integer_class, coda_native_type_uint8, 8);
  return MakeScalarAndRelease(
      def, product_,
      [value](coda_type_number* t, coda_dynamic_type* a, coda_product* p) {
        return coda_mem_uint8_new(t, a, p, value);
      });
}

CodaMemNode* CodaMemProduct::MakeInt16(int16_t value) {
  coda_type_number* def =
      MakeNumberDef(format_, coda_integer_class, coda_native_type_int16, 16);
  return MakeScalarAndRelease(
      def, product_,
      [value](coda_type_number* t, coda_dynamic_type* a, coda_product* p) {
        return coda_mem_int16_new(t, a, p, value);
      });
}

CodaMemNode* CodaMemProduct::MakeUint16(uint16_t value) {
  coda_type_number* def =
      MakeNumberDef(format_, coda_integer_class, coda_native_type_uint16, 16);
  return MakeScalarAndRelease(
      def, product_,
      [value](coda_type_number* t, coda_dynamic_type* a, coda_product* p) {
        return coda_mem_uint16_new(t, a, p, value);
      });
}

CodaMemNode* CodaMemProduct::MakeInt32(int32_t value) {
  coda_type_number* def =
      MakeNumberDef(format_, coda_integer_class, coda_native_type_int32, 32);
  return MakeScalarAndRelease(
      def, product_,
      [value](coda_type_number* t, coda_dynamic_type* a, coda_product* p) {
        return coda_mem_int32_new(t, a, p, value);
      });
}

CodaMemNode* CodaMemProduct::MakeUint32(uint32_t value) {
  coda_type_number* def =
      MakeNumberDef(format_, coda_integer_class, coda_native_type_uint32, 32);
  return MakeScalarAndRelease(
      def, product_,
      [value](coda_type_number* t, coda_dynamic_type* a, coda_product* p) {
        return coda_mem_uint32_new(t, a, p, value);
      });
}

CodaMemNode* CodaMemProduct::MakeInt64(int64_t value) {
  coda_type_number* def =
      MakeNumberDef(format_, coda_integer_class, coda_native_type_int64, 64);
  return MakeScalarAndRelease(
      def, product_,
      [value](coda_type_number* t, coda_dynamic_type* a, coda_product* p) {
        return coda_mem_int64_new(t, a, p, value);
      });
}

CodaMemNode* CodaMemProduct::MakeUint64(uint64_t value) {
  coda_type_number* def =
      MakeNumberDef(format_, coda_integer_class, coda_native_type_uint64, 64);
  return MakeScalarAndRelease(
      def, product_,
      [value](coda_type_number* t, coda_dynamic_type* a, coda_product* p) {
        return coda_mem_uint64_new(t, a, p, value);
      });
}

CodaMemNode* CodaMemProduct::MakeFloat(float value) {
  coda_type_number* def =
      MakeNumberDef(format_, coda_real_class, coda_native_type_float, 32);
  return MakeScalarAndRelease(
      def, product_,
      [value](coda_type_number* t, coda_dynamic_type* a, coda_product* p) {
        return coda_mem_float_new(t, a, p, value);
      });
}

CodaMemNode* CodaMemProduct::MakeDouble(double value) {
  coda_type_number* def =
      MakeNumberDef(format_, coda_real_class, coda_native_type_double, 64);
  return MakeScalarAndRelease(
      def, product_,
      [value](coda_type_number* t, coda_dynamic_type* a, coda_product* p) {
        return coda_mem_double_new(t, a, p, value);
      });
}

CodaMemNode* CodaMemProduct::MakeChar(char value) {
  coda_type_text* def = MakeTextDef(format_, coda_native_type_char, 8);
  return MakeScalarAndRelease(
      def, product_,
      [value](coda_type_text* t, coda_dynamic_type* a, coda_product* p) {
        return coda_mem_char_new(t, a, p, value);
      });
}

CodaMemNode* CodaMemProduct::MakeString(std::string_view value) {
  coda_type_text* def = MakeTextDef(format_, coda_native_type_string, -1);
  if (def == nullptr) return nullptr;
  const std::string s(value);
  coda_mem_data* d = coda_mem_string_new(def, nullptr, product_, s.c_str());
  coda_type_release(reinterpret_cast<coda_type*>(def));
  return reinterpret_cast<CodaMemNode*>(d);
}

CodaMemNode* CodaMemProduct::MakeRaw(std::span<const uint8_t> bytes) {
  coda_type_raw* def = coda_type_raw_new(format_);
  if (def == nullptr) return nullptr;
  if (coda_type_set_bit_size(reinterpret_cast<coda_type*>(def),
                             static_cast<int64_t>(bytes.size()) * 8) != 0) {
    coda_type_release(reinterpret_cast<coda_type*>(def));
    return nullptr;
  }
  coda_mem_data* d = coda_mem_raw_new(
      def, nullptr, product_, static_cast<long>(bytes.size()), bytes.data());
  coda_type_release(reinterpret_cast<coda_type*>(def));
  return reinterpret_cast<CodaMemNode*>(d);
}

CodaMemNode* CodaMemProduct::MakeTime(double seconds_since_2000) {
  coda_type_number* base_def =
      MakeNumberDef(format_, coda_real_class, coda_native_type_double, 64);
  if (base_def == nullptr) return nullptr;
  coda_mem_data* base_data =
      coda_mem_double_new(base_def, nullptr, product_, seconds_since_2000);
  if (base_data == nullptr) {
    coda_type_release(reinterpret_cast<coda_type*>(base_def));
    return nullptr;
  }
  coda_type_special* time_def =
      MakeTimeDef(format_, reinterpret_cast<coda_type*>(base_def));
  coda_type_release(reinterpret_cast<coda_type*>(base_def));
  if (time_def == nullptr) {
    coda_mem_type_delete(reinterpret_cast<coda_dynamic_type*>(base_data));
    return nullptr;
  }
  coda_mem_special* special = coda_mem_time_new(
      time_def, nullptr, reinterpret_cast<coda_dynamic_type*>(base_data));
  coda_type_release(reinterpret_cast<coda_type*>(time_def));
  if (special == nullptr) {
    coda_mem_type_delete(reinterpret_cast<coda_dynamic_type*>(base_data));
    return nullptr;
  }
  return reinterpret_cast<CodaMemNode*>(special);
}

CodaMemNode* CodaMemProduct::MakeNoData() {
  return reinterpret_cast<CodaMemNode*>(coda_mem_no_data_new(format_));
}

CodaMemNode* CodaMemProduct::MakeInt8Array(std::span<const long> dims,
                                           std::span<const int8_t> values) {
  return MakeNumberArrayImpl<int8_t>(format_, product_, dims, values,
                                     coda_integer_class, coda_native_type_int8,
                                     8, coda_mem_int8_new);
}

CodaMemNode* CodaMemProduct::MakeUint8Array(std::span<const long> dims,
                                            std::span<const uint8_t> values) {
  return MakeNumberArrayImpl<uint8_t>(
      format_, product_, dims, values, coda_integer_class,
      coda_native_type_uint8, 8, coda_mem_uint8_new);
}

CodaMemNode* CodaMemProduct::MakeInt16Array(std::span<const long> dims,
                                            std::span<const int16_t> values) {
  return MakeNumberArrayImpl<int16_t>(
      format_, product_, dims, values, coda_integer_class,
      coda_native_type_int16, 16, coda_mem_int16_new);
}

CodaMemNode* CodaMemProduct::MakeUint16Array(std::span<const long> dims,
                                             std::span<const uint16_t> values) {
  return MakeNumberArrayImpl<uint16_t>(
      format_, product_, dims, values, coda_integer_class,
      coda_native_type_uint16, 16, coda_mem_uint16_new);
}

CodaMemNode* CodaMemProduct::MakeInt32Array(std::span<const long> dims,
                                            std::span<const int32_t> values) {
  return MakeNumberArrayImpl<int32_t>(
      format_, product_, dims, values, coda_integer_class,
      coda_native_type_int32, 32, coda_mem_int32_new);
}

CodaMemNode* CodaMemProduct::MakeUint32Array(std::span<const long> dims,
                                             std::span<const uint32_t> values) {
  return MakeNumberArrayImpl<uint32_t>(
      format_, product_, dims, values, coda_integer_class,
      coda_native_type_uint32, 32, coda_mem_uint32_new);
}

CodaMemNode* CodaMemProduct::MakeInt64Array(std::span<const long> dims,
                                            std::span<const int64_t> values) {
  return MakeNumberArrayImpl<int64_t>(
      format_, product_, dims, values, coda_integer_class,
      coda_native_type_int64, 64, coda_mem_int64_new);
}

CodaMemNode* CodaMemProduct::MakeUint64Array(std::span<const long> dims,
                                             std::span<const uint64_t> values) {
  return MakeNumberArrayImpl<uint64_t>(
      format_, product_, dims, values, coda_integer_class,
      coda_native_type_uint64, 64, coda_mem_uint64_new);
}

CodaMemNode* CodaMemProduct::MakeFloatArray(std::span<const long> dims,
                                            std::span<const float> values) {
  return MakeNumberArrayImpl<float>(format_, product_, dims, values,
                                    coda_real_class, coda_native_type_float, 32,
                                    coda_mem_float_new);
}

CodaMemNode* CodaMemProduct::MakeDoubleArray(std::span<const long> dims,
                                             std::span<const double> values) {
  return MakeNumberArrayImpl<double>(format_, product_, dims, values,
                                     coda_real_class, coda_native_type_double,
                                     64, coda_mem_double_new);
}

CodaMemNode* CodaMemProduct::MakeCharArray(std::span<const long> dims,
                                           std::span<const char> values) {
  coda_type_text* elem_def = MakeTextDef(format_, coda_native_type_char, 8);
  if (elem_def == nullptr) return nullptr;
  coda_type_array* arr_def =
      MakeArrayDef(format_, dims, reinterpret_cast<coda_type*>(elem_def));
  if (arr_def == nullptr) {
    coda_type_release(reinterpret_cast<coda_type*>(elem_def));
    return nullptr;
  }
  coda_mem_array* arr = coda_mem_array_new(arr_def, nullptr);
  coda_type_release(reinterpret_cast<coda_type*>(arr_def));
  if (arr == nullptr) {
    coda_type_release(reinterpret_cast<coda_type*>(elem_def));
    return nullptr;
  }
  for (size_t i = 0; i < values.size(); ++i) {
    coda_mem_data* elem =
        coda_mem_char_new(elem_def, nullptr, product_, values[i]);
    if (elem == nullptr ||
        coda_mem_array_set_element(
            arr, static_cast<long>(i),
            reinterpret_cast<coda_dynamic_type*>(elem)) != 0) {
      if (elem != nullptr) {
        coda_mem_type_delete(reinterpret_cast<coda_dynamic_type*>(elem));
      }
      coda_mem_type_delete(reinterpret_cast<coda_dynamic_type*>(arr));
      coda_type_release(reinterpret_cast<coda_type*>(elem_def));
      return nullptr;
    }
  }
  coda_type_release(reinterpret_cast<coda_type*>(elem_def));
  return reinterpret_cast<CodaMemNode*>(arr);
}

CodaMemNode* CodaMemProduct::MakeStringArray(
    std::span<const long> dims, std::span<const std::string_view> values) {
  coda_type_text* elem_def = MakeTextDef(format_, coda_native_type_string, -1);
  if (elem_def == nullptr) return nullptr;
  coda_type_array* arr_def =
      MakeArrayDef(format_, dims, reinterpret_cast<coda_type*>(elem_def));
  if (arr_def == nullptr) {
    coda_type_release(reinterpret_cast<coda_type*>(elem_def));
    return nullptr;
  }
  coda_mem_array* arr = coda_mem_array_new(arr_def, nullptr);
  coda_type_release(reinterpret_cast<coda_type*>(arr_def));
  if (arr == nullptr) {
    coda_type_release(reinterpret_cast<coda_type*>(elem_def));
    return nullptr;
  }
  for (size_t i = 0; i < values.size(); ++i) {
    const std::string s(values[i]);
    coda_mem_data* elem =
        coda_mem_string_new(elem_def, nullptr, product_, s.c_str());
    if (elem == nullptr ||
        coda_mem_array_set_element(
            arr, static_cast<long>(i),
            reinterpret_cast<coda_dynamic_type*>(elem)) != 0) {
      if (elem != nullptr) {
        coda_mem_type_delete(reinterpret_cast<coda_dynamic_type*>(elem));
      }
      coda_mem_type_delete(reinterpret_cast<coda_dynamic_type*>(arr));
      coda_type_release(reinterpret_cast<coda_type*>(elem_def));
      return nullptr;
    }
  }
  coda_type_release(reinterpret_cast<coda_type*>(elem_def));
  return reinterpret_cast<CodaMemNode*>(arr);
}

CodaMemNode* CodaMemProduct::MakeTimeArray(std::span<const long> dims,
                                           std::span<const double> values) {
  coda_type_number* base_def =
      MakeNumberDef(format_, coda_real_class, coda_native_type_double, 64);
  if (base_def == nullptr) return nullptr;
  coda_type_special* elem_def =
      MakeTimeDef(format_, reinterpret_cast<coda_type*>(base_def));
  if (elem_def == nullptr) {
    coda_type_release(reinterpret_cast<coda_type*>(base_def));
    return nullptr;
  }
  coda_type_array* arr_def =
      MakeArrayDef(format_, dims, reinterpret_cast<coda_type*>(elem_def));
  if (arr_def == nullptr) {
    coda_type_release(reinterpret_cast<coda_type*>(elem_def));
    coda_type_release(reinterpret_cast<coda_type*>(base_def));
    return nullptr;
  }
  coda_mem_array* arr = coda_mem_array_new(arr_def, nullptr);
  coda_type_release(reinterpret_cast<coda_type*>(arr_def));
  if (arr == nullptr) {
    coda_type_release(reinterpret_cast<coda_type*>(elem_def));
    coda_type_release(reinterpret_cast<coda_type*>(base_def));
    return nullptr;
  }
  for (size_t i = 0; i < values.size(); ++i) {
    coda_mem_data* base_data =
        coda_mem_double_new(base_def, nullptr, product_, values[i]);
    if (base_data == nullptr) {
      coda_mem_type_delete(reinterpret_cast<coda_dynamic_type*>(arr));
      coda_type_release(reinterpret_cast<coda_type*>(elem_def));
      coda_type_release(reinterpret_cast<coda_type*>(base_def));
      return nullptr;
    }
    coda_mem_special* elem = coda_mem_time_new(
        elem_def, nullptr, reinterpret_cast<coda_dynamic_type*>(base_data));
    if (elem == nullptr ||
        coda_mem_array_set_element(
            arr, static_cast<long>(i),
            reinterpret_cast<coda_dynamic_type*>(elem)) != 0) {
      if (elem != nullptr) {
        coda_mem_type_delete(reinterpret_cast<coda_dynamic_type*>(elem));
      } else {
        coda_mem_type_delete(reinterpret_cast<coda_dynamic_type*>(base_data));
      }
      coda_mem_type_delete(reinterpret_cast<coda_dynamic_type*>(arr));
      coda_type_release(reinterpret_cast<coda_type*>(elem_def));
      coda_type_release(reinterpret_cast<coda_type*>(base_def));
      return nullptr;
    }
  }
  coda_type_release(reinterpret_cast<coda_type*>(elem_def));
  coda_type_release(reinterpret_cast<coda_type*>(base_def));
  return reinterpret_cast<CodaMemNode*>(arr);
}

CodaMemNode* CodaMemProduct::MakeNodeArray(
    std::span<const long> dims, std::span<CodaMemNode* const> elements,
    CodaMemNode* sample_elem_for_empty) {
  coda_type* base_def = nullptr;
  if (!elements.empty()) {
    base_def = reinterpret_cast<coda_dynamic_type*>(elements[0])->definition;
  } else if (sample_elem_for_empty != nullptr) {
    base_def =
        reinterpret_cast<coda_dynamic_type*>(sample_elem_for_empty)->definition;
  } else {
    return nullptr;
  }

  coda_type_array* arr_def = MakeArrayDef(format_, dims, base_def);
  if (arr_def == nullptr) {
    DeleteNodeIfNonNull(sample_elem_for_empty);
    for (CodaMemNode* elem : elements) {
      DeleteNodeIfNonNull(elem);
    }
    return nullptr;
  }
  coda_mem_array* arr = coda_mem_array_new(arr_def, nullptr);
  coda_type_release(reinterpret_cast<coda_type*>(arr_def));
  DeleteNodeIfNonNull(sample_elem_for_empty);
  if (arr == nullptr) {
    for (CodaMemNode* elem : elements) {
      DeleteNodeIfNonNull(elem);
    }
    return nullptr;
  }
  for (size_t i = 0; i < elements.size(); ++i) {
    auto* elem = reinterpret_cast<coda_dynamic_type*>(elements[i]);
    if (elem->definition != base_def) {
      coda_type* old_def = elem->definition;
      elem->definition = base_def;
      base_def->retain_count++;
      coda_type_release(old_def);
    }
    auto* mem_elem = reinterpret_cast<coda_mem_type*>(elem);
    if (mem_elem->attributes != nullptr && base_def->attributes != nullptr &&
        mem_elem->attributes->definition !=
            reinterpret_cast<coda_type*>(base_def->attributes)) {
      coda_type* old_attr_def = mem_elem->attributes->definition;
      coda_type* new_attr_def =
          reinterpret_cast<coda_type*>(base_def->attributes);
      mem_elem->attributes->definition = new_attr_def;
      new_attr_def->retain_count++;
      coda_type_release(old_attr_def);
    }
    if (coda_mem_array_set_element(arr, static_cast<long>(i), elem) != 0) {
      coda_mem_type_delete(reinterpret_cast<coda_dynamic_type*>(arr));
      for (size_t j = i; j < elements.size(); ++j) {
        DeleteNodeIfNonNull(elements[j]);
      }
      return nullptr;
    }
  }
  return reinterpret_cast<CodaMemNode*>(arr);
}

CodaMemNode* CodaMemProduct::MakeBinaryDoubleArray(
    std::span<const long> dims, std::span<const double> values) {
  coda_type_number* elem_def = MakeNumberDef(
      coda_format_binary, coda_real_class, coda_native_type_double, 64);
  if (elem_def == nullptr) return nullptr;
  coda_type_array* arr_def = MakeArrayDef(
      coda_format_binary, dims, reinterpret_cast<coda_type*>(elem_def));
  coda_type_release(reinterpret_cast<coda_type*>(elem_def));
  if (arr_def == nullptr) {
    return nullptr;
  }
  const long total_bytes = static_cast<long>(values.size() * sizeof(double));
  coda_mem_data* data = coda_mem_data_new(
      reinterpret_cast<coda_type*>(arr_def), nullptr, product_, total_bytes,
      reinterpret_cast<const uint8_t*>(values.data()));
  coda_type_release(reinterpret_cast<coda_type*>(arr_def));
  return reinterpret_cast<CodaMemNode*>(data);
}

CodaMemNode* CodaMemProduct::MakeBinaryDoubleRecordArray(
    std::span<const long> dims, std::span<const std::string_view> field_names,
    std::span<const double> packed_values) {
  coda_type_number* double_def = MakeNumberDef(
      coda_format_binary, coda_real_class, coda_native_type_double, 64);
  if (double_def == nullptr) return nullptr;
  coda_type_record* rec_def = coda_type_record_new(coda_format_binary);
  if (rec_def == nullptr) {
    coda_type_release(reinterpret_cast<coda_type*>(double_def));
    return nullptr;
  }
  for (std::string_view name : field_names) {
    const std::string fname(name);
    if (coda_type_record_create_field(
            rec_def, fname.c_str(), reinterpret_cast<coda_type*>(double_def)) !=
        0) {
      coda_type_release(reinterpret_cast<coda_type*>(rec_def));
      coda_type_release(reinterpret_cast<coda_type*>(double_def));
      return nullptr;
    }
  }
  coda_type_release(reinterpret_cast<coda_type*>(double_def));

  coda_type_array* arr_def = MakeArrayDef(
      coda_format_binary, dims, reinterpret_cast<coda_type*>(rec_def));
  coda_type_release(reinterpret_cast<coda_type*>(rec_def));
  if (arr_def == nullptr) {
    return nullptr;
  }
  const long total_bytes =
      static_cast<long>(packed_values.size() * sizeof(double));
  coda_mem_data* data = coda_mem_data_new(
      reinterpret_cast<coda_type*>(arr_def), nullptr, product_, total_bytes,
      reinterpret_cast<const uint8_t*>(packed_values.data()));
  coda_type_release(reinterpret_cast<coda_type*>(arr_def));
  return reinterpret_cast<CodaMemNode*>(data);
}

}  // namespace stcorp_harp
