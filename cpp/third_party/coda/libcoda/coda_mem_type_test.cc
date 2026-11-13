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
// Tests CODA memory backend dynamic type constructors, updates, attributes,
// and validation (libcoda/coda-mem-type.c).
//
// google3 build caveats:
//   - Internal headers lack extern "C" guards and require
//   coda_internal_prelude.h
//     before coda-mem-internal.h.
//   - Upstream coda_mem_array_validate() sets CODA_ERROR_DATA_DEFINITION
//   without
//     returning -1 when a fixed-dimension array's actual element count differs
//     from the definition; a test pins this behavior.
//   - Coverage ceiling on libcoda/coda-mem-type.c is 527/589 (89.5%) = 100%
//     of reachable non-OOM, non-dead lines: all 62 missed lines (120, 125-126,
//     130-131, 157, 179-180, 183, 206, 219, 230, 240, 248, 262, 299, 305,
//     347-348, 350, 445, 447, 461-462, 472-473, 475-476, 519-520, 522, 593,
//     595, 609-610, 620-623, 714-716, 793, 795, 824-827, 840-841, 981, 983,
//     996-997, 1012, 1014, 1025-1026, 1033-1034, 1039-1040) are OOM checks,
//     OOM propagation paths, or the assert(0)/exit(1) mismatch branch at 157.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-mem-type.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-mem-internal.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-type.h

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-mem-internal.h"
}  // extern "C"

#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

template <typename T>
struct CodaTypeReleaser {
  void operator()(T* type) const {
    coda_type_release(reinterpret_cast<coda_type*>(type));
  }
};

template <typename T>
using UniqueCodaType = std::unique_ptr<T, CodaTypeReleaser<T>>;

template <typename T>
struct MemTypeDeleter {
  void operator()(T* type) const {
    coda_mem_type_delete(reinterpret_cast<coda_dynamic_type*>(type));
  }
};

template <typename T>
using UniqueMemType = std::unique_ptr<T, MemTypeDeleter<T>>;

constexpr coda_endianness kNativeEndianness =
    std::endian::native == std::endian::big ? coda_big_endian
                                            : coda_little_endian;

UniqueCodaType<coda_type_number> MakeIntDef(coda_format format,
                                            coda_native_type read_type,
                                            int64_t bit_size) {
  UniqueCodaType<coda_type_number> def(
      coda_type_number_new(format, coda_integer_class));
  if (def == nullptr) {
    return nullptr;
  }
  if (coda_type_set_read_type(reinterpret_cast<coda_type*>(def.get()),
                              read_type) != 0) {
    return nullptr;
  }
  if (coda_type_set_bit_size(reinterpret_cast<coda_type*>(def.get()),
                             bit_size) != 0) {
    return nullptr;
  }
  if (coda_type_number_set_endianness(def.get(), kNativeEndianness) != 0) {
    return nullptr;
  }
  return def;
}

UniqueCodaType<coda_type_number> MakeRealDef(coda_format format,
                                             coda_native_type read_type,
                                             int64_t bit_size) {
  UniqueCodaType<coda_type_number> def(
      coda_type_number_new(format, coda_real_class));
  if (def == nullptr) {
    return nullptr;
  }
  if (coda_type_set_read_type(reinterpret_cast<coda_type*>(def.get()),
                              read_type) != 0) {
    return nullptr;
  }
  if (coda_type_set_bit_size(reinterpret_cast<coda_type*>(def.get()),
                             bit_size) != 0) {
    return nullptr;
  }
  if (coda_type_number_set_endianness(def.get(), kNativeEndianness) != 0) {
    return nullptr;
  }
  return def;
}

bool AddRecordFieldDef(coda_type_record* rec_def, const char* name,
                       coda_type* field_type, bool optional) {
  coda_type_record_field* field = coda_type_record_field_new(name);
  if (field == nullptr) {
    return false;
  }
  if (coda_type_record_field_set_type(field, field_type) != 0) {
    coda_type_record_field_delete(field);
    return false;
  }
  if (optional && coda_type_record_field_set_optional(field) != 0) {
    coda_type_record_field_delete(field);
    return false;
  }
  if (coda_type_record_add_field(rec_def, field) != 0) {
    coda_type_record_field_delete(field);
    return false;
  }
  return true;
}

bool AddAttrDef(coda_type* target, const char* name, coda_type* attr_type) {
  coda_type_record_field* field = coda_type_record_field_new(name);
  if (field == nullptr) {
    return false;
  }
  if (coda_type_record_field_set_type(field, attr_type) != 0) {
    coda_type_record_field_delete(field);
    return false;
  }
  if (coda_type_add_attribute(target, field) != 0) {
    coda_type_record_field_delete(field);
    return false;
  }
  return true;
}

TEST(CodaMemTypeTest, MemTypeDeleteCoversAllTagsAndNullChildren) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Empty record (field_type == NULL, attributes == NULL).
  {
    UniqueCodaType<coda_type_record> empty_def(
        coda_type_record_new(coda_format_xml));
    ASSERT_NE(nullptr, empty_def.get());
    UniqueMemType<coda_mem_record> empty_rec(
        coda_mem_record_new(empty_def.get(), nullptr));
    ASSERT_NE(nullptr, empty_rec.get());
    EXPECT_EQ(tag_mem_record, empty_rec->tag);
    EXPECT_EQ(nullptr, empty_rec->field_type);
    EXPECT_EQ(nullptr, empty_rec->attributes);
  }

  // 2. Non-empty record with one populated field, one NULL slot, and explicit
  //    attributes record.
  {
    UniqueCodaType<coda_type_text> str_def(coda_type_text_new(coda_format_xml));
    ASSERT_NE(nullptr, str_def.get());
    UniqueCodaType<coda_type_record> rec_def(
        coda_type_record_new(coda_format_xml));
    ASSERT_NE(nullptr, rec_def.get());
    ASSERT_TRUE(AddRecordFieldDef(rec_def.get(), "present",
                                  reinterpret_cast<coda_type*>(str_def.get()),
                                  false));
    ASSERT_TRUE(AddRecordFieldDef(rec_def.get(), "absent",
                                  reinterpret_cast<coda_type*>(str_def.get()),
                                  true));

    UniqueCodaType<coda_type_record> attr_def(
        coda_type_record_new(coda_format_xml));
    ASSERT_NE(nullptr, attr_def.get());
    UniqueMemType<coda_mem_record> attrs(
        coda_mem_record_new(attr_def.get(), nullptr));
    ASSERT_NE(nullptr, attrs.get());

    UniqueMemType<coda_mem_record> rec(coda_mem_record_new(
        rec_def.get(), reinterpret_cast<coda_dynamic_type*>(attrs.get())));
    ASSERT_NE(nullptr, rec.get());
    attrs.release();

    UniqueMemType<coda_mem_data> val(
        coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, val.get());
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(
                  rec.get(), "present",
                  reinterpret_cast<coda_dynamic_type*>(val.get()), 0));
    val.release();
  }

  // 3. Empty dynamic array (element == NULL) and partially populated fixed
  //    array with definition-created attributes.
  {
    UniqueCodaType<coda_type_text> str_def(coda_type_text_new(coda_format_xml));
    ASSERT_NE(nullptr, str_def.get());

    UniqueCodaType<coda_type_array> dyn_arr_def(
        coda_type_array_new(coda_format_xml));
    ASSERT_NE(nullptr, dyn_arr_def.get());
    ASSERT_EQ(CODA_SUCCESS, coda_type_array_set_base_type(
                                dyn_arr_def.get(),
                                reinterpret_cast<coda_type*>(str_def.get())));
    ASSERT_EQ(CODA_SUCCESS, coda_type_array_add_variable_dimension(
                                dyn_arr_def.get(), nullptr));
    UniqueMemType<coda_mem_array> dyn_arr(
        coda_mem_array_new(dyn_arr_def.get(), nullptr));
    ASSERT_NE(nullptr, dyn_arr.get());
    EXPECT_EQ(tag_mem_array, dyn_arr->tag);
    EXPECT_EQ(nullptr, dyn_arr->element);

    UniqueCodaType<coda_type_array> fixed_arr_def(
        coda_type_array_new(coda_format_xml));
    ASSERT_NE(nullptr, fixed_arr_def.get());
    ASSERT_EQ(CODA_SUCCESS, coda_type_array_set_base_type(
                                fixed_arr_def.get(),
                                reinterpret_cast<coda_type*>(str_def.get())));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_array_add_fixed_dimension(fixed_arr_def.get(), 2));
    ASSERT_TRUE(AddAttrDef(reinterpret_cast<coda_type*>(fixed_arr_def.get()),
                           "tag", reinterpret_cast<coda_type*>(str_def.get())));

    UniqueMemType<coda_mem_array> fixed_arr(
        coda_mem_array_new(fixed_arr_def.get(), nullptr));
    ASSERT_NE(nullptr, fixed_arr.get());
    ASSERT_NE(nullptr, fixed_arr->attributes);

    UniqueMemType<coda_mem_data> elem(
        coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, elem.get());
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_array_set_element(
                  fixed_arr.get(), 0,
                  reinterpret_cast<coda_dynamic_type*>(elem.get())));
    elem.release();
  }

  // 4. Special no_data type (tag_mem_special with base_type != NULL).
  {
    UniqueMemType<coda_mem_special> no_data(
        coda_mem_no_data_new(coda_format_xml));
    ASSERT_NE(nullptr, no_data.get());
    EXPECT_EQ(tag_mem_special, no_data->tag);
    EXPECT_NE(nullptr, no_data->base_type);
  }
}

TEST(CodaMemTypeTest, MemRecordNewValidatesArgumentsAndUnions) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  EXPECT_EQ(nullptr, coda_mem_record_new(nullptr, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // Unions with a union_field_expr are disallowed by the memory backend.
  UniqueCodaType<coda_type_record> union_with_expr_def(
      coda_type_union_new(coda_format_xml));
  ASSERT_NE(nullptr, union_with_expr_def.get());
  coda_expression* field_expr = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("0", &field_expr));
  ASSERT_EQ(CODA_SUCCESS, coda_type_union_set_field_expression(
                              union_with_expr_def.get(), field_expr));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_mem_record_new(union_with_expr_def.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);
  EXPECT_THAT(coda_errno_to_string(CODA_ERROR_INVALID_ARGUMENT),
              HasSubstr("union definition with field expression"));

  // Unions without a union_field_expr are allowed.
  UniqueCodaType<coda_type_record> union_def(
      coda_type_union_new(coda_format_xml));
  ASSERT_NE(nullptr, union_def.get());
  UniqueCodaType<coda_type_text> str_def(coda_type_text_new(coda_format_xml));
  ASSERT_NE(nullptr, str_def.get());
  ASSERT_TRUE(AddRecordFieldDef(union_def.get(), "choice",
                                reinterpret_cast<coda_type*>(str_def.get()),
                                true));
  ASSERT_TRUE(AddAttrDef(reinterpret_cast<coda_type*>(union_def.get()),
                         "attr_a",
                         reinterpret_cast<coda_type*>(str_def.get())));

  UniqueMemType<coda_mem_record> union_rec(
      coda_mem_record_new(union_def.get(), nullptr));
  ASSERT_NE(nullptr, union_rec.get());
  EXPECT_EQ(1, union_rec->num_fields);
  EXPECT_EQ(nullptr, union_rec->field_type[0]);
  ASSERT_NE(nullptr, union_rec->attributes);
}

TEST(CodaMemTypeTest, MemRecordAddFieldAndValidateCoverStaticAndDynamicModes) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  UniqueCodaType<coda_type_text> str_def(coda_type_text_new(coda_format_xml));
  ASSERT_NE(nullptr, str_def.get());
  UniqueCodaType<coda_type_text> other_str_def(
      coda_type_text_new(coda_format_xml));
  ASSERT_NE(nullptr, other_str_def.get());

  UniqueCodaType<coda_type_record> rec_def(
      coda_type_record_new(coda_format_xml));
  ASSERT_NE(nullptr, rec_def.get());
  ASSERT_TRUE(AddRecordFieldDef(rec_def.get(), "req",
                                reinterpret_cast<coda_type*>(str_def.get()),
                                false));
  ASSERT_TRUE(AddRecordFieldDef(
      rec_def.get(), "opt", reinterpret_cast<coda_type*>(str_def.get()), true));

  UniqueMemType<coda_mem_record> rec(
      coda_mem_record_new(rec_def.get(), nullptr));
  ASSERT_NE(nullptr, rec.get());

  UniqueMemType<coda_mem_data> req_val(
      coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, req_val.get());

  // Argument validation errors.
  EXPECT_EQ(-1, coda_mem_record_add_field(
                    nullptr, "req",
                    reinterpret_cast<coda_dynamic_type*>(req_val.get()), 0));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_record_add_field(rec.get(), "req", nullptr, 0));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_record_validate(nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // update_definition == 0 error branches.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_record_add_field(
                    rec.get(), "missing",
                    reinterpret_cast<coda_dynamic_type*>(req_val.get()), 0));
  EXPECT_EQ(CODA_ERROR_INVALID_NAME, coda_errno);

  UniqueMemType<coda_mem_data> wrong_val(
      coda_mem_string_new(other_str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, wrong_val.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_record_add_field(
                    rec.get(), "req",
                    reinterpret_cast<coda_dynamic_type*>(wrong_val.get()), 0));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // Validate fails while mandatory field "req" is NULL.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_record_validate(rec.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Populate mandatory field "req"; optional field "opt" stays NULL.
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(
                rec.get(), "req",
                reinterpret_cast<coda_dynamic_type*>(req_val.get()), 0));
  req_val.release();

  // Setting "req" a second time with update_definition == 0 fails.
  UniqueMemType<coda_mem_data> dup_val(
      coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, dup_val.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_record_add_field(
                    rec.get(), "req",
                    reinterpret_cast<coda_dynamic_type*>(dup_val.get()), 0));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  EXPECT_EQ(CODA_SUCCESS, coda_mem_record_validate(rec.get()));

  // update_definition == 1 error paths: NULL name and incompatible format.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_record_add_field(
                    rec.get(), nullptr,
                    reinterpret_cast<coda_dynamic_type*>(dup_val.get()), 1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  UniqueCodaType<coda_type_raw> hdf5_raw_def(
      coda_type_raw_new(coda_format_hdf5));
  ASSERT_NE(nullptr, hdf5_raw_def.get());
  UniqueMemType<coda_mem_data> hdf5_val(
      coda_mem_raw_new(hdf5_raw_def.get(), nullptr, nullptr, 0, nullptr));
  ASSERT_NE(nullptr, hdf5_val.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_record_add_field(
                    rec.get(), "bad_fmt",
                    reinterpret_cast<coda_dynamic_type*>(hdf5_val.get()), 1));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // update_definition == 1 success grows field_type array and definition.
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(
                rec.get(), "dyn-name",
                reinterpret_cast<coda_dynamic_type*>(dup_val.get()), 1));
  dup_val.release();
  EXPECT_EQ(3, rec->num_fields);
  EXPECT_EQ(3, rec->definition->num_fields);
  EXPECT_EQ(CODA_SUCCESS, coda_mem_record_validate(rec.get()));
}

TEST(CodaMemTypeTest, MemTypeAddAttributeCoversAllUpdateAndErrorBranches) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  UniqueCodaType<coda_type_text> str_def(coda_type_text_new(coda_format_xml));
  ASSERT_NE(nullptr, str_def.get());
  UniqueCodaType<coda_type_text> other_str_def(
      coda_type_text_new(coda_format_xml));
  ASSERT_NE(nullptr, other_str_def.get());
  UniqueCodaType<coda_type_record> rec_def(
      coda_type_record_new(coda_format_xml));
  ASSERT_NE(nullptr, rec_def.get());

  UniqueMemType<coda_mem_record> rec(
      coda_mem_record_new(rec_def.get(), nullptr));
  ASSERT_NE(nullptr, rec.get());
  UniqueMemType<coda_mem_data> attr_val(
      coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, attr_val.get());

  // 1. NULL argument checks.
  EXPECT_EQ(-1, coda_mem_type_add_attribute(
                    nullptr, "a",
                    reinterpret_cast<coda_dynamic_type*>(attr_val.get()), 1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_type_add_attribute(
                    reinterpret_cast<coda_mem_type*>(rec.get()), nullptr,
                    reinterpret_cast<coda_dynamic_type*>(attr_val.get()), 1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_mem_type_add_attribute(
                reinterpret_cast<coda_mem_type*>(rec.get()), "a", nullptr, 1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 2. type->attributes == NULL with update_definition == 0 fails.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_type_add_attribute(
                    reinterpret_cast<coda_mem_type*>(rec.get()), "a",
                    reinterpret_cast<coda_dynamic_type*>(attr_val.get()), 0));
  EXPECT_EQ(CODA_ERROR_INVALID_NAME, coda_errno);

  // 3. type->attributes == NULL with update_definition == 1 creates
  //    definition->attributes and type->attributes.
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_type_add_attribute(
                reinterpret_cast<coda_mem_type*>(rec.get()), "first",
                reinterpret_cast<coda_dynamic_type*>(attr_val.get()), 1));
  attr_val.release();
  ASSERT_NE(nullptr, rec->attributes);
  EXPECT_EQ(1, reinterpret_cast<coda_mem_record*>(rec->attributes)->num_fields);

  // 4. Adding a duplicate name with update_definition == 1 creates a second
  //    uniquified field and reallocs attributes->field_type.
  UniqueMemType<coda_mem_data> attr_dup(
      coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, attr_dup.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_type_add_attribute(
                reinterpret_cast<coda_mem_type*>(rec.get()), "first",
                reinterpret_cast<coda_dynamic_type*>(attr_dup.get()), 1));
  attr_dup.release();
  EXPECT_EQ(2, reinterpret_cast<coda_mem_record*>(rec->attributes)->num_fields);

  // 5. Incompatible format attribute fails in update_definition == 1.
  UniqueCodaType<coda_type_raw> hdf5_def(coda_type_raw_new(coda_format_hdf5));
  ASSERT_NE(nullptr, hdf5_def.get());
  UniqueMemType<coda_mem_data> hdf5_attr(
      coda_mem_raw_new(hdf5_def.get(), nullptr, nullptr, 0, nullptr));
  ASSERT_NE(nullptr, hdf5_attr.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_type_add_attribute(
                    reinterpret_cast<coda_mem_type*>(rec.get()), "bad_fmt",
                    reinterpret_cast<coda_dynamic_type*>(hdf5_attr.get()), 1));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // 6. Non-memory backend attributes record rejects
  // coda_mem_type_add_attribute.
  //    coda_type_record has coda_format_binary as its first field, which
  //    coda_dynamic_type_delete() releases cleanly via coda_type_release().
  UniqueCodaType<coda_type_record> bin_attr_rec(
      coda_type_record_new(coda_format_binary));
  ASSERT_NE(nullptr, bin_attr_rec.get());
  UniqueCodaType<coda_type_record> host_def(
      coda_type_record_new(coda_format_xml));
  ASSERT_NE(nullptr, host_def.get());
  UniqueMemType<coda_mem_record> host_rec(coda_mem_record_new(
      host_def.get(),
      reinterpret_cast<coda_dynamic_type*>(bin_attr_rec.get())));
  ASSERT_NE(nullptr, host_rec.get());
  bin_attr_rec.release();

  UniqueMemType<coda_mem_data> some_attr(
      coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, some_attr.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_type_add_attribute(
                    reinterpret_cast<coda_mem_type*>(host_rec.get()), "x",
                    reinterpret_cast<coda_dynamic_type*>(some_attr.get()), 1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 7. Static (update_definition == 0) branches on pre-declared attributes.
  UniqueCodaType<coda_type_record> static_def(
      coda_type_record_new(coda_format_xml));
  ASSERT_NE(nullptr, static_def.get());
  ASSERT_TRUE(AddAttrDef(reinterpret_cast<coda_type*>(static_def.get()), "k1",
                         reinterpret_cast<coda_type*>(str_def.get())));
  ASSERT_TRUE(AddAttrDef(reinterpret_cast<coda_type*>(static_def.get()), "k2",
                         reinterpret_cast<coda_type*>(str_def.get())));
  UniqueMemType<coda_mem_record> static_rec(
      coda_mem_record_new(static_def.get(), nullptr));
  ASSERT_NE(nullptr, static_rec.get());

  // Unknown name.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_mem_type_add_attribute(
                reinterpret_cast<coda_mem_type*>(static_rec.get()), "unknown",
                reinterpret_cast<coda_dynamic_type*>(some_attr.get()), 0));
  EXPECT_EQ(CODA_ERROR_INVALID_NAME, coda_errno);

  // Incompatible attribute type.
  UniqueMemType<coda_mem_data> wrong_attr(
      coda_mem_string_new(other_str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, wrong_attr.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_type_add_attribute(
                    reinterpret_cast<coda_mem_type*>(static_rec.get()), "k1",
                    reinterpret_cast<coda_dynamic_type*>(wrong_attr.get()), 0));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // Populate "k1" with update_definition == 0.
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_type_add_attribute(
                reinterpret_cast<coda_mem_type*>(static_rec.get()), "k1",
                reinterpret_cast<coda_dynamic_type*>(some_attr.get()), 0));
  some_attr.release();

  // Duplicate "k1" fails when update_definition == 0.
  UniqueMemType<coda_mem_data> k1_dup(
      coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, k1_dup.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_type_add_attribute(
                    reinterpret_cast<coda_mem_type*>(static_rec.get()), "k1",
                    reinterpret_cast<coda_dynamic_type*>(k1_dup.get()), 0));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // Pre-declared empty slot "k2" filled with update_definition == 1 reuses
  // the existing slot without creating a duplicate field.
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_type_add_attribute(
                reinterpret_cast<coda_mem_type*>(static_rec.get()), "k2",
                reinterpret_cast<coda_dynamic_type*>(k1_dup.get()), 1));
  k1_dup.release();
  EXPECT_EQ(2, static_def->attributes->num_fields);

  // 8. type->attributes == NULL while type->definition->attributes != NULL:
  //    attach definition attribute after constructing mem_record, then add via
  //    update_definition == 1.
  UniqueCodaType<coda_type_record> late_def(
      coda_type_record_new(coda_format_xml));
  ASSERT_NE(nullptr, late_def.get());
  UniqueMemType<coda_mem_record> late_rec(
      coda_mem_record_new(late_def.get(), nullptr));
  ASSERT_NE(nullptr, late_rec.get());
  ASSERT_EQ(nullptr, late_rec->attributes);
  ASSERT_TRUE(AddAttrDef(reinterpret_cast<coda_type*>(late_def.get()), "late",
                         reinterpret_cast<coda_type*>(str_def.get())));
  UniqueMemType<coda_mem_data> late_val(
      coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, late_val.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_type_add_attribute(
                reinterpret_cast<coda_mem_type*>(late_rec.get()), "late",
                reinterpret_cast<coda_dynamic_type*>(late_val.get()), 1));
  late_val.release();
  ASSERT_NE(nullptr, late_rec->attributes);
}

TEST(CodaMemTypeTest, MemTypeSetAttributesCoversStaticDynamicAndErrorPaths) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  UniqueCodaType<coda_type_record> target_def(
      coda_type_record_new(coda_format_xml));
  ASSERT_NE(nullptr, target_def.get());
  UniqueCodaType<coda_type_record> attr_def(
      coda_type_record_new(coda_format_xml));
  ASSERT_NE(nullptr, attr_def.get());
  UniqueCodaType<coda_type_record> other_attr_def(
      coda_type_record_new(coda_format_xml));
  ASSERT_NE(nullptr, other_attr_def.get());

  UniqueMemType<coda_mem_record> target(
      coda_mem_record_new(target_def.get(), nullptr));
  ASSERT_NE(nullptr, target.get());
  UniqueMemType<coda_mem_record> attrs(
      coda_mem_record_new(attr_def.get(), nullptr));
  ASSERT_NE(nullptr, attrs.get());
  UniqueMemType<coda_mem_record> other_attrs(
      coda_mem_record_new(other_attr_def.get(), nullptr));
  ASSERT_NE(nullptr, other_attrs.get());

  EXPECT_EQ(-1,
            coda_mem_type_set_attributes(
                nullptr, reinterpret_cast<coda_dynamic_type*>(attrs.get()), 1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_mem_type_set_attributes(
                reinterpret_cast<coda_mem_type*>(target.get()), nullptr, 1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // update_definition == 0 when definition->attributes == NULL fails.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_type_set_attributes(
                    reinterpret_cast<coda_mem_type*>(target.get()),
                    reinterpret_cast<coda_dynamic_type*>(attrs.get()), 0));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // update_definition == 1 sets definition->attributes and target->attributes.
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_type_set_attributes(
                reinterpret_cast<coda_mem_type*>(target.get()),
                reinterpret_cast<coda_dynamic_type*>(attrs.get()), 1));
  attrs.release();
  EXPECT_EQ(reinterpret_cast<coda_type_record*>(attr_def.get()),
            target_def->attributes);

  // Calling again when target->attributes != NULL fails.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_mem_type_set_attributes(
                reinterpret_cast<coda_mem_type*>(target.get()),
                reinterpret_cast<coda_dynamic_type*>(other_attrs.get()), 0));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // update_definition == 1 fails if definition->attributes is already set
  // while mem_type->attributes is NULL.
  UniqueCodaType<coda_type_record> pre_def(
      coda_type_record_new(coda_format_xml));
  ASSERT_NE(nullptr, pre_def.get());
  UniqueMemType<coda_mem_record> pre_rec(
      coda_mem_record_new(pre_def.get(), nullptr));
  ASSERT_NE(nullptr, pre_rec.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_set_attributes(
                reinterpret_cast<coda_type*>(pre_def.get()), attr_def.get()));

  UniqueMemType<coda_mem_record> new_attrs(
      coda_mem_record_new(attr_def.get(), nullptr));
  ASSERT_NE(nullptr, new_attrs.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_type_set_attributes(
                    reinterpret_cast<coda_mem_type*>(pre_rec.get()),
                    reinterpret_cast<coda_dynamic_type*>(new_attrs.get()), 1));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // update_definition == 0 succeeds when definition->attributes matches.
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_type_set_attributes(
                reinterpret_cast<coda_mem_type*>(pre_rec.get()),
                reinterpret_cast<coda_dynamic_type*>(new_attrs.get()), 0));
  new_attrs.release();
}

TEST(CodaMemTypeTest, MemArraySetAddAndValidateCoverFixedDynamicAndZeroDim) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  EXPECT_EQ(nullptr, coda_mem_array_new(nullptr, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_array_validate(nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  UniqueCodaType<coda_type_text> str_def(coda_type_text_new(coda_format_xml));
  ASSERT_NE(nullptr, str_def.get());
  UniqueCodaType<coda_type_text> wrong_def(coda_type_text_new(coda_format_xml));
  ASSERT_NE(nullptr, wrong_def.get());

  // 1. Fixed-size array of 3 elements.
  UniqueCodaType<coda_type_array> fixed_def(
      coda_type_array_new(coda_format_xml));
  ASSERT_NE(nullptr, fixed_def.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_set_base_type(
                fixed_def.get(), reinterpret_cast<coda_type*>(str_def.get())));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_add_fixed_dimension(fixed_def.get(), 3));

  UniqueMemType<coda_mem_array> fixed_arr(
      coda_mem_array_new(fixed_def.get(), nullptr));
  ASSERT_NE(nullptr, fixed_arr.get());
  EXPECT_EQ(3, fixed_arr->num_elements);

  UniqueMemType<coda_mem_data> elem0(
      coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, elem0.get());
  UniqueMemType<coda_mem_data> wrong_elem(
      coda_mem_string_new(wrong_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, wrong_elem.get());

  // coda_mem_array_set_element error checks.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_mem_array_set_element(
                nullptr, 0, reinterpret_cast<coda_dynamic_type*>(elem0.get())));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_array_set_element(
                    fixed_arr.get(), -1,
                    reinterpret_cast<coda_dynamic_type*>(elem0.get())));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_array_set_element(
                    fixed_arr.get(), 3,
                    reinterpret_cast<coda_dynamic_type*>(elem0.get())));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_array_set_element(fixed_arr.get(), 0, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_array_set_element(
                    fixed_arr.get(), 0,
                    reinterpret_cast<coda_dynamic_type*>(wrong_elem.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Set elements 0 and 2 via set_element.
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_array_set_element(
                fixed_arr.get(), 0,
                reinterpret_cast<coda_dynamic_type*>(elem0.get())));
  elem0.release();

  UniqueMemType<coda_mem_data> elem2(
      coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, elem2.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_array_set_element(
                fixed_arr.get(), 2,
                reinterpret_cast<coda_dynamic_type*>(elem2.get())));
  elem2.release();

  // Re-setting already occupied slot 0 fails.
  UniqueMemType<coda_mem_data> elem_extra(
      coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, elem_extra.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_array_set_element(
                    fixed_arr.get(), 0,
                    reinterpret_cast<coda_dynamic_type*>(elem_extra.get())));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // Validate fails while slot 1 is missing.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_array_validate(fixed_arr.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // coda_mem_array_add_element error checks.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_mem_array_add_element(
              nullptr, reinterpret_cast<coda_dynamic_type*>(elem_extra.get())));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_array_add_element(fixed_arr.get(), nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_array_add_element(
                    fixed_arr.get(),
                    reinterpret_cast<coda_dynamic_type*>(wrong_elem.get())));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // Fills the remaining free slot (index 1).
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_array_add_element(
                fixed_arr.get(),
                reinterpret_cast<coda_dynamic_type*>(elem_extra.get())));
  elem_extra.release();
  EXPECT_EQ(CODA_SUCCESS, coda_mem_array_validate(fixed_arr.get()));

  // Adding to a full fixed array fails with CODA_ERROR_INVALID_INDEX.
  UniqueMemType<coda_mem_data> overflow_elem(
      coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, overflow_elem.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_mem_array_add_element(
                    fixed_arr.get(),
                    reinterpret_cast<coda_dynamic_type*>(overflow_elem.get())));
  EXPECT_EQ(CODA_ERROR_INVALID_INDEX, coda_errno);

  // 2. Dynamic-size array grows across BLOCK_SIZE (16) boundary.
  UniqueCodaType<coda_type_array> dyn_def(coda_type_array_new(coda_format_xml));
  ASSERT_NE(nullptr, dyn_def.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_set_base_type(
                dyn_def.get(), reinterpret_cast<coda_type*>(str_def.get())));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_add_variable_dimension(dyn_def.get(), nullptr));
  UniqueMemType<coda_mem_array> dyn_arr(
      coda_mem_array_new(dyn_def.get(), nullptr));
  ASSERT_NE(nullptr, dyn_arr.get());

  for (int i = 0; i < 20; ++i) {
    UniqueMemType<coda_mem_data> item(
        coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
    ASSERT_NE(nullptr, item.get());
    ASSERT_EQ(
        CODA_SUCCESS,
        coda_mem_array_add_element(
            dyn_arr.get(), reinterpret_cast<coda_dynamic_type*>(item.get())));
    item.release();
  }
  EXPECT_EQ(20, dyn_arr->num_elements);
  EXPECT_EQ(CODA_SUCCESS, coda_mem_array_validate(dyn_arr.get()));

  // 3. Fixed 0-element array where add_element creates 1 element: upstream
  //    coda_mem_array_validate() sets CODA_ERROR_DATA_DEFINITION but still
  //    returns 0 because it omits an early return.
  UniqueCodaType<coda_type_array> zero_def(
      coda_type_array_new(coda_format_xml));
  ASSERT_NE(nullptr, zero_def.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_set_base_type(
                zero_def.get(), reinterpret_cast<coda_type*>(str_def.get())));
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_array_add_fixed_dimension(zero_def.get(), 0));
  UniqueMemType<coda_mem_array> zero_arr(
      coda_mem_array_new(zero_def.get(), nullptr));
  ASSERT_NE(nullptr, zero_arr.get());
  UniqueMemType<coda_mem_data> extra(
      coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, extra.get());
  ASSERT_EQ(
      CODA_SUCCESS,
      coda_mem_array_add_element(
          zero_arr.get(), reinterpret_cast<coda_dynamic_type*>(extra.get())));
  extra.release();
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(CODA_SUCCESS, coda_mem_array_validate(zero_arr.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
}

TEST(CodaMemTypeTest, MemDataNewRejectsInvalidArgsAndMismatchedLengths) {
  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  coda_product product{};
  product.format = coda_format_xml;
  const absl::Cleanup free_mem = [&product] { std::free(product.mem_ptr); };

  constexpr uint8_t kTwoBytes[] = {0x12, 0x34};

  // NULL definition.
  EXPECT_EQ(nullptr, coda_mem_data_new(nullptr, nullptr, &product, 0, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // length > 0 with NULL data.
  UniqueCodaType<coda_type_raw> dyn_raw(coda_type_raw_new(coda_format_binary));
  ASSERT_NE(nullptr, dyn_raw.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr,
            coda_mem_data_new(reinterpret_cast<coda_type*>(dyn_raw.get()),
                              nullptr, &product, 2, nullptr));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // Non-byte-aligned bit_size (5 bits -> expected 1 byte).
  UniqueCodaType<coda_type_number> five_bit_int =
      MakeIntDef(coda_format_binary, coda_native_type_uint8, 5);
  ASSERT_NE(nullptr, five_bit_int.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr,
            coda_mem_data_new(reinterpret_cast<coda_type*>(five_bit_int.get()),
                              nullptr, &product, 2, kTwoBytes));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);

  UniqueMemType<coda_mem_data> one_byte(
      coda_mem_data_new(reinterpret_cast<coda_type*>(five_bit_int.get()),
                        nullptr, &product, 1, kTwoBytes));
  ASSERT_NE(nullptr, one_byte.get());
  EXPECT_EQ(1, one_byte->length);

  // Byte-aligned fixed bit_size mismatch (16 bits vs 1 byte).
  UniqueCodaType<coda_type_number> i16_def =
      MakeIntDef(coda_format_binary, coda_native_type_int16, 16);
  ASSERT_NE(nullptr, i16_def.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr,
            coda_mem_data_new(reinterpret_cast<coda_type*>(i16_def.get()),
                              nullptr, &product, 1, kTwoBytes));
  EXPECT_EQ(CODA_ERROR_PRODUCT, coda_errno);

  // coda_native_type_char with bit_size == -1 requires length == 1.
  UniqueCodaType<coda_type_text> char_var_def(
      coda_type_text_new(coda_format_ascii));
  ASSERT_NE(nullptr, char_var_def.get());
  ASSERT_EQ(CODA_SUCCESS, coda_type_set_read_type(
                              reinterpret_cast<coda_type*>(char_var_def.get()),
                              coda_native_type_char));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr,
            coda_mem_data_new(reinterpret_cast<coda_type*>(char_var_def.get()),
                              nullptr, &product, 2, kTwoBytes));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // length > 0 with NULL product.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr,
            coda_mem_data_new(reinterpret_cast<coda_type*>(dyn_raw.get()),
                              nullptr, nullptr, 2, kTwoBytes));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // Explicit attributes skip create_attributes_record() when attributes !=
  // NULL.
  UniqueCodaType<coda_type_record> attr_def(
      coda_type_record_new(coda_format_xml));
  ASSERT_NE(nullptr, attr_def.get());
  UniqueMemType<coda_mem_record> explicit_attrs(
      coda_mem_record_new(attr_def.get(), nullptr));
  ASSERT_NE(nullptr, explicit_attrs.get());
  UniqueCodaType<coda_type_text> str_def(coda_type_text_new(coda_format_xml));
  ASSERT_NE(nullptr, str_def.get());
  UniqueMemType<coda_mem_data> str_with_attrs(coda_mem_string_new(
      str_def.get(), reinterpret_cast<coda_dynamic_type*>(explicit_attrs.get()),
      &product, "ok"));
  ASSERT_NE(nullptr, str_with_attrs.get());
  explicit_attrs.release();
  EXPECT_NE(nullptr, str_with_attrs->attributes);
  EXPECT_EQ(2, str_with_attrs->length);
}

TEST(CodaMemTypeTest, TypedDataConstructorsAndBlockGrowthStoreExactValues) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  coda_product product{};
  product.format = coda_format_binary;
  const absl::Cleanup free_mem = [&product] { std::free(product.mem_ptr); };

  UniqueCodaType<coda_type_record> root_def(
      coda_type_record_new(coda_format_binary));
  ASSERT_NE(nullptr, root_def.get());

  UniqueCodaType<coda_type_number> i8_def =
      MakeIntDef(coda_format_binary, coda_native_type_int8, 8);
  UniqueCodaType<coda_type_number> u8_def =
      MakeIntDef(coda_format_binary, coda_native_type_uint8, 8);
  UniqueCodaType<coda_type_number> i16_def =
      MakeIntDef(coda_format_binary, coda_native_type_int16, 16);
  UniqueCodaType<coda_type_number> u16_def =
      MakeIntDef(coda_format_binary, coda_native_type_uint16, 16);
  UniqueCodaType<coda_type_number> i32_def =
      MakeIntDef(coda_format_binary, coda_native_type_int32, 32);
  UniqueCodaType<coda_type_number> u32_def =
      MakeIntDef(coda_format_binary, coda_native_type_uint32, 32);
  UniqueCodaType<coda_type_number> i64_def =
      MakeIntDef(coda_format_binary, coda_native_type_int64, 64);
  UniqueCodaType<coda_type_number> u64_def =
      MakeIntDef(coda_format_binary, coda_native_type_uint64, 64);
  UniqueCodaType<coda_type_number> f32_def =
      MakeRealDef(coda_format_binary, coda_native_type_float, 32);
  UniqueCodaType<coda_type_number> f64_def =
      MakeRealDef(coda_format_binary, coda_native_type_double, 64);

  UniqueCodaType<coda_type_text> ch_def(coda_type_text_new(coda_format_binary));
  ASSERT_NE(nullptr, ch_def.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_set_read_type(reinterpret_cast<coda_type*>(ch_def.get()),
                                    coda_native_type_char));
  ASSERT_EQ(CODA_SUCCESS, coda_type_set_bit_size(
                              reinterpret_cast<coda_type*>(ch_def.get()), 8));

  UniqueCodaType<coda_type_text> str_def(
      coda_type_text_new(coda_format_binary));
  ASSERT_NE(nullptr, str_def.get());
  UniqueCodaType<coda_type_raw> raw_def(coda_type_raw_new(coda_format_binary));
  ASSERT_NE(nullptr, raw_def.get());

  UniqueMemType<coda_mem_record> root_rec(
      coda_mem_record_new(root_def.get(), nullptr));
  ASSERT_NE(nullptr, root_rec.get());

  auto add_field = [&](const char* name, coda_mem_data* node) {
    ASSERT_NE(nullptr, node);
    UniqueMemType<coda_mem_data> owned(node);
    ASSERT_EQ(CODA_SUCCESS,
              coda_mem_record_add_field(
                  root_rec.get(), name,
                  reinterpret_cast<coda_dynamic_type*>(owned.get()), 1));
    owned.release();
  };

  constexpr int8_t kI8 = -42;
  constexpr uint8_t kU8 = 213U;
  constexpr int16_t kI16 = -12345;
  constexpr uint16_t kU16 = 54321U;
  constexpr int32_t kI32 = -1000003;
  constexpr uint32_t kU32 = 3000000007U;
  constexpr int64_t kI64 = -9000000000001LL;
  constexpr uint64_t kU64 = 12000000000007ULL;
  constexpr float kF32 = 3.25F;
  constexpr double kF64 = -123.625;
  constexpr char kCh = 'Z';
  constexpr char kStr[] = "hello-mem";

  ASSERT_NO_FATAL_FAILURE(
      add_field("i8", coda_mem_int8_new(i8_def.get(), nullptr, &product, kI8)));
  ASSERT_NO_FATAL_FAILURE(add_field(
      "u8", coda_mem_uint8_new(u8_def.get(), nullptr, &product, kU8)));
  ASSERT_NO_FATAL_FAILURE(add_field(
      "i16", coda_mem_int16_new(i16_def.get(), nullptr, &product, kI16)));
  ASSERT_NO_FATAL_FAILURE(add_field(
      "u16", coda_mem_uint16_new(u16_def.get(), nullptr, &product, kU16)));
  ASSERT_NO_FATAL_FAILURE(add_field(
      "i32", coda_mem_int32_new(i32_def.get(), nullptr, &product, kI32)));
  ASSERT_NO_FATAL_FAILURE(add_field(
      "u32", coda_mem_uint32_new(u32_def.get(), nullptr, &product, kU32)));
  ASSERT_NO_FATAL_FAILURE(add_field(
      "i64", coda_mem_int64_new(i64_def.get(), nullptr, &product, kI64)));
  ASSERT_NO_FATAL_FAILURE(add_field(
      "u64", coda_mem_uint64_new(u64_def.get(), nullptr, &product, kU64)));
  ASSERT_NO_FATAL_FAILURE(add_field(
      "f32", coda_mem_float_new(f32_def.get(), nullptr, &product, kF32)));
  ASSERT_NO_FATAL_FAILURE(add_field(
      "f64", coda_mem_double_new(f64_def.get(), nullptr, &product, kF64)));
  ASSERT_NO_FATAL_FAILURE(
      add_field("ch", coda_mem_char_new(ch_def.get(), nullptr, &product, kCh)));
  ASSERT_NO_FATAL_FAILURE(add_field(
      "str", coda_mem_string_new(str_def.get(), nullptr, &product, kStr)));
  ASSERT_NO_FATAL_FAILURE(add_field(
      "empty_str",
      coda_mem_string_new(str_def.get(), nullptr, &product, nullptr)));
  ASSERT_NO_FATAL_FAILURE(add_field(
      "empty_raw",
      coda_mem_raw_new(raw_def.get(), nullptr, &product, 0, nullptr)));

  // Append a payload larger than DATA_BLOCK_SIZE (4096) to exercise realloc
  // block growth in coda_mem_data_new().
  std::vector<uint8_t> big_raw(5000);
  for (size_t i = 0; i < big_raw.size(); ++i) {
    big_raw[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
  }
  ASSERT_NO_FATAL_FAILURE(add_field(
      "big_raw",
      coda_mem_raw_new(raw_def.get(), nullptr, &product,
                       static_cast<long>(big_raw.size()), big_raw.data())));

  // Read everything back through a CODA cursor to verify offset and byte
  // layout.
  product.root_type = reinterpret_cast<coda_dynamic_type*>(root_rec.get());
  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, &product));

  auto goto_field = [&](const char* name) {
    ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, &product));
    ASSERT_EQ(CODA_SUCCESS,
              coda_cursor_goto_record_field_by_name(&cursor, name));
  };

  int8_t got_i8 = 0;
  ASSERT_NO_FATAL_FAILURE(goto_field("i8"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int8(&cursor, &got_i8));
  EXPECT_EQ(kI8, got_i8);

  uint8_t got_u8 = 0;
  ASSERT_NO_FATAL_FAILURE(goto_field("u8"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint8(&cursor, &got_u8));
  EXPECT_EQ(kU8, got_u8);

  int16_t got_i16 = 0;
  ASSERT_NO_FATAL_FAILURE(goto_field("i16"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int16(&cursor, &got_i16));
  EXPECT_EQ(kI16, got_i16);

  uint16_t got_u16 = 0;
  ASSERT_NO_FATAL_FAILURE(goto_field("u16"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint16(&cursor, &got_u16));
  EXPECT_EQ(kU16, got_u16);

  int32_t got_i32 = 0;
  ASSERT_NO_FATAL_FAILURE(goto_field("i32"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int32(&cursor, &got_i32));
  EXPECT_EQ(kI32, got_i32);

  uint32_t got_u32 = 0;
  ASSERT_NO_FATAL_FAILURE(goto_field("u32"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint32(&cursor, &got_u32));
  EXPECT_EQ(kU32, got_u32);

  int64_t got_i64 = 0;
  ASSERT_NO_FATAL_FAILURE(goto_field("i64"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_int64(&cursor, &got_i64));
  EXPECT_EQ(kI64, got_i64);

  uint64_t got_u64 = 0;
  ASSERT_NO_FATAL_FAILURE(goto_field("u64"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_uint64(&cursor, &got_u64));
  EXPECT_EQ(kU64, got_u64);

  float got_f32 = 0.0F;
  ASSERT_NO_FATAL_FAILURE(goto_field("f32"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_float(&cursor, &got_f32));
  EXPECT_FLOAT_EQ(kF32, got_f32);

  double got_f64 = 0.0;
  ASSERT_NO_FATAL_FAILURE(goto_field("f64"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_double(&cursor, &got_f64));
  EXPECT_DOUBLE_EQ(kF64, got_f64);

  char got_ch = '\0';
  ASSERT_NO_FATAL_FAILURE(goto_field("ch"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_read_char(&cursor, &got_ch));
  EXPECT_EQ(kCh, got_ch);

  ASSERT_NO_FATAL_FAILURE(goto_field("str"));
  long str_len = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &str_len));
  EXPECT_EQ(9, str_len);
  std::string got_str(static_cast<size_t>(str_len) + 1, '\0');
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&cursor, got_str.data(), str_len + 1));
  EXPECT_STREQ(kStr, got_str.c_str());

  ASSERT_NO_FATAL_FAILURE(goto_field("empty_str"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &str_len));
  EXPECT_EQ(0, str_len);

  ASSERT_NO_FATAL_FAILURE(goto_field("empty_raw"));
  int64_t raw_byte_size = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_byte_size(&cursor, &raw_byte_size));
  EXPECT_EQ(0, raw_byte_size);

  ASSERT_NO_FATAL_FAILURE(goto_field("big_raw"));
  std::vector<uint8_t> got_raw(big_raw.size(), 0);
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_bytes(&cursor, got_raw.data(), 0,
                                   static_cast<int64_t>(got_raw.size())));
  EXPECT_EQ(big_raw, got_raw);
}

TEST(CodaMemTypeTest, MemTimeNewAndNoDataNewCoverSpecialTypes) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  coda_set_error(CODA_SUCCESS, nullptr);
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  UniqueCodaType<coda_type_text> str_def(coda_type_text_new(coda_format_xml));
  ASSERT_NE(nullptr, str_def.get());
  UniqueCodaType<coda_type_text> other_str_def(
      coda_type_text_new(coda_format_xml));
  ASSERT_NE(nullptr, other_str_def.get());

  UniqueMemType<coda_mem_data> base_data(
      coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, base_data.get());

  // 1. NULL definition.
  EXPECT_EQ(
      nullptr,
      coda_mem_time_new(nullptr, nullptr,
                        reinterpret_cast<coda_dynamic_type*>(base_data.get())));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 2. Non-special definition.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr,
            coda_mem_time_new(
                reinterpret_cast<coda_type_special*>(str_def.get()), nullptr,
                reinterpret_cast<coda_dynamic_type*>(base_data.get())));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 3. Special definition that is not coda_special_time.
  UniqueCodaType<coda_type_special> complex_def(
      coda_type_complex_new(coda_format_xml));
  ASSERT_NE(nullptr, complex_def.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      nullptr,
      coda_mem_time_new(complex_def.get(), nullptr,
                        reinterpret_cast<coda_dynamic_type*>(base_data.get())));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 4. Mismatched base_type definition.
  coda_expression* val_expr = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("0.0", &val_expr));
  UniqueCodaType<coda_type_special> time_def(
      coda_type_time_new(coda_format_xml, val_expr));
  ASSERT_NE(nullptr, time_def.get());
  ASSERT_EQ(
      CODA_SUCCESS,
      coda_type_time_set_base_type(
          time_def.get(), reinterpret_cast<coda_type*>(other_str_def.get())));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      nullptr,
      coda_mem_time_new(time_def.get(), nullptr,
                        reinterpret_cast<coda_dynamic_type*>(base_data.get())));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 5. Valid coda_mem_time_new with definition-created attributes.
  ASSERT_TRUE(AddAttrDef(reinterpret_cast<coda_type*>(time_def.get()), "tz",
                         reinterpret_cast<coda_type*>(str_def.get())));
  UniqueMemType<coda_mem_data> matching_base(
      coda_mem_string_new(other_str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, matching_base.get());
  UniqueMemType<coda_mem_special> time_node(coda_mem_time_new(
      time_def.get(), nullptr,
      reinterpret_cast<coda_dynamic_type*>(matching_base.get())));
  ASSERT_NE(nullptr, time_node.get());
  matching_base.release();
  EXPECT_EQ(tag_mem_special, time_node->tag);
  EXPECT_NE(nullptr, time_node->attributes);

  // 6. Valid coda_mem_time_new with explicit attributes.
  UniqueCodaType<coda_type_record> attr_def(
      coda_type_record_new(coda_format_xml));
  ASSERT_NE(nullptr, attr_def.get());
  UniqueMemType<coda_mem_record> explicit_attrs(
      coda_mem_record_new(attr_def.get(), nullptr));
  ASSERT_NE(nullptr, explicit_attrs.get());
  UniqueMemType<coda_mem_data> matching_base2(
      coda_mem_string_new(other_str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, matching_base2.get());
  UniqueMemType<coda_mem_special> time_node2(coda_mem_time_new(
      time_def.get(),
      reinterpret_cast<coda_dynamic_type*>(explicit_attrs.get()),
      reinterpret_cast<coda_dynamic_type*>(matching_base2.get())));
  ASSERT_NE(nullptr, time_node2.get());
  explicit_attrs.release();
  matching_base2.release();
  EXPECT_NE(nullptr, time_node2->attributes);

  // 7. coda_mem_no_data_new for XML and binary formats.
  UniqueMemType<coda_mem_special> no_data_bin(
      coda_mem_no_data_new(coda_format_binary));
  ASSERT_NE(nullptr, no_data_bin.get());
  EXPECT_EQ(tag_mem_special, no_data_bin->tag);
  EXPECT_EQ(coda_special_no_data, no_data_bin->definition->special_type);
  ASSERT_NE(nullptr, no_data_bin->base_type);
  EXPECT_EQ(coda_raw_class, no_data_bin->base_type->definition->type_class);
}

TEST(CodaMemTypeTest, MemTypeUpdateCoversAsciiBinaryAndXmlConversions) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Early return when backend is coda_backend_ascii or coda_backend_binary.
  UniqueCodaType<coda_type_text> ascii_def(
      coda_type_text_new(coda_format_ascii));
  ASSERT_NE(nullptr, ascii_def.get());
  coda_dynamic_type* ascii_dyn =
      reinterpret_cast<coda_dynamic_type*>(ascii_def.get());
  EXPECT_EQ(CODA_SUCCESS,
            coda_mem_type_update(
                &ascii_dyn, reinterpret_cast<coda_type*>(ascii_def.get())));

  UniqueCodaType<coda_type_number> bin_def =
      MakeIntDef(coda_format_binary, coda_native_type_int32, 32);
  ASSERT_NE(nullptr, bin_def.get());
  coda_dynamic_type* bin_dyn =
      reinterpret_cast<coda_dynamic_type*>(bin_def.get());
  EXPECT_EQ(CODA_SUCCESS,
            coda_mem_type_update(&bin_dyn,
                                 reinterpret_cast<coda_type*>(bin_def.get())));

  // 2. Empty XML record converted to XML text when text_def has attributes
  //    (exercises deleting temporary mem_type->attributes and late attribute
  //    record creation).
  {
    UniqueCodaType<coda_type_record> empty_rec_def(
        coda_type_record_new(coda_format_xml));
    ASSERT_NE(nullptr, empty_rec_def.get());
    UniqueMemType<coda_dynamic_type> node(reinterpret_cast<coda_dynamic_type*>(
        coda_mem_record_new(empty_rec_def.get(), nullptr)));
    ASSERT_NE(nullptr, node.get());

    UniqueCodaType<coda_type_text> str_def(coda_type_text_new(coda_format_xml));
    ASSERT_NE(nullptr, str_def.get());
    UniqueCodaType<coda_type_text> unit_attr_def(
        coda_type_text_new(coda_format_xml));
    ASSERT_NE(nullptr, unit_attr_def.get());
    ASSERT_TRUE(AddAttrDef(reinterpret_cast<coda_type*>(str_def.get()), "unit",
                           reinterpret_cast<coda_type*>(unit_attr_def.get())));

    coda_dynamic_type* raw_ptr = node.release();
    const int rc = coda_mem_type_update(
        &raw_ptr, reinterpret_cast<coda_type*>(str_def.get()));
    node.reset(raw_ptr);
    ASSERT_EQ(CODA_SUCCESS, rc);
    EXPECT_EQ(tag_mem_data, reinterpret_cast<coda_mem_type*>(node.get())->tag);
    EXPECT_NE(nullptr,
              reinterpret_cast<coda_mem_type*>(node.get())->attributes);
  }

  // 3. Empty XML record wrapped into an XML array of XML text, preserving
  //    matching attributes across record->text conversion.
  {
    UniqueCodaType<coda_type_record> shared_attr_def(
        coda_type_record_new(coda_format_xml));
    ASSERT_NE(nullptr, shared_attr_def.get());

    UniqueCodaType<coda_type_record> empty_rec_def(
        coda_type_record_new(coda_format_xml));
    ASSERT_NE(nullptr, empty_rec_def.get());
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_set_attributes(
                  reinterpret_cast<coda_type*>(empty_rec_def.get()),
                  shared_attr_def.get()));

    UniqueCodaType<coda_type_text> str_def(coda_type_text_new(coda_format_xml));
    ASSERT_NE(nullptr, str_def.get());
    ASSERT_EQ(CODA_SUCCESS, coda_type_set_attributes(
                                reinterpret_cast<coda_type*>(str_def.get()),
                                shared_attr_def.get()));

    UniqueCodaType<coda_type_array> arr_def(
        coda_type_array_new(coda_format_xml));
    ASSERT_NE(nullptr, arr_def.get());
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_array_set_base_type(
                  arr_def.get(), reinterpret_cast<coda_type*>(str_def.get())));
    ASSERT_EQ(CODA_SUCCESS,
              coda_type_array_add_variable_dimension(arr_def.get(), nullptr));
    ASSERT_TRUE(AddAttrDef(reinterpret_cast<coda_type*>(arr_def.get()), "count",
                           reinterpret_cast<coda_type*>(str_def.get())));

    UniqueMemType<coda_dynamic_type> node(reinterpret_cast<coda_dynamic_type*>(
        coda_mem_record_new(empty_rec_def.get(), nullptr)));
    ASSERT_NE(nullptr, node.get());

    coda_dynamic_type* raw_ptr = node.release();
    const int rc = coda_mem_type_update(
        &raw_ptr, reinterpret_cast<coda_type*>(arr_def.get()));
    node.reset(raw_ptr);
    ASSERT_EQ(CODA_SUCCESS, rc);

    coda_mem_array* arr = reinterpret_cast<coda_mem_array*>(node.get());
    EXPECT_EQ(tag_mem_array, arr->tag);
    EXPECT_EQ(1, arr->num_elements);
    ASSERT_NE(nullptr, arr->attributes);
    ASSERT_NE(nullptr, arr->element[0]);
    EXPECT_EQ(tag_mem_data,
              reinterpret_cast<coda_mem_type*>(arr->element[0])->tag);
    EXPECT_NE(nullptr,
              reinterpret_cast<coda_mem_type*>(arr->element[0])->attributes);
  }
}

TEST(CodaMemTypeTest, MemTypeUpdateRecursivelyUpdatesRecordsArraysAndSpecials) {
  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // Build child types: nested record, array, and time special.
  UniqueCodaType<coda_type_text> str_def(coda_type_text_new(coda_format_xml));
  ASSERT_NE(nullptr, str_def.get());

  UniqueCodaType<coda_type_record> child_rec_def(
      coda_type_record_new(coda_format_xml));
  ASSERT_NE(nullptr, child_rec_def.get());
  ASSERT_TRUE(AddRecordFieldDef(child_rec_def.get(), "leaf",
                                reinterpret_cast<coda_type*>(str_def.get()),
                                false));

  UniqueCodaType<coda_type_array> child_arr_def(
      coda_type_array_new(coda_format_xml));
  ASSERT_NE(nullptr, child_arr_def.get());
  ASSERT_EQ(CODA_SUCCESS, coda_type_array_set_base_type(
                              child_arr_def.get(),
                              reinterpret_cast<coda_type*>(str_def.get())));
  ASSERT_EQ(CODA_SUCCESS, coda_type_array_add_variable_dimension(
                              child_arr_def.get(), nullptr));

  coda_expression* val_expr = nullptr;
  ASSERT_EQ(CODA_SUCCESS, coda_expression_from_string("0.0", &val_expr));
  UniqueCodaType<coda_type_special> time_def(
      coda_type_time_new(coda_format_xml, val_expr));
  ASSERT_NE(nullptr, time_def.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_type_time_set_base_type(
                time_def.get(), reinterpret_cast<coda_type*>(str_def.get())));

  // Root record initially has 3 fields; more fields are added to root_def
  // after instantiating root_rec so coda_mem_type_update() grows field_type.
  UniqueCodaType<coda_type_record> root_def(
      coda_type_record_new(coda_format_xml));
  ASSERT_NE(nullptr, root_def.get());
  ASSERT_TRUE(AddRecordFieldDef(
      root_def.get(), "sub_rec",
      reinterpret_cast<coda_type*>(child_rec_def.get()), false));
  ASSERT_TRUE(AddRecordFieldDef(
      root_def.get(), "sub_arr",
      reinterpret_cast<coda_type*>(child_arr_def.get()), false));
  ASSERT_TRUE(AddRecordFieldDef(root_def.get(), "sub_time",
                                reinterpret_cast<coda_type*>(time_def.get()),
                                false));

  UniqueMemType<coda_mem_record> root_rec(
      coda_mem_record_new(root_def.get(), nullptr));
  ASSERT_NE(nullptr, root_rec.get());

  // Populate sub_rec, sub_arr, sub_time.
  UniqueMemType<coda_mem_record> sub_rec(
      coda_mem_record_new(child_rec_def.get(), nullptr));
  ASSERT_NE(nullptr, sub_rec.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(
                root_rec.get(), "sub_rec",
                reinterpret_cast<coda_dynamic_type*>(sub_rec.get()), 0));
  sub_rec.release();

  UniqueMemType<coda_mem_array> sub_arr(
      coda_mem_array_new(child_arr_def.get(), nullptr));
  ASSERT_NE(nullptr, sub_arr.get());
  UniqueMemType<coda_mem_data> arr_item(
      coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, arr_item.get());
  ASSERT_EQ(
      CODA_SUCCESS,
      coda_mem_array_add_element(
          sub_arr.get(), reinterpret_cast<coda_dynamic_type*>(arr_item.get())));
  arr_item.release();
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(
                root_rec.get(), "sub_arr",
                reinterpret_cast<coda_dynamic_type*>(sub_arr.get()), 0));
  sub_arr.release();

  UniqueMemType<coda_mem_data> time_base(
      coda_mem_string_new(str_def.get(), nullptr, nullptr, nullptr));
  ASSERT_NE(nullptr, time_base.get());
  UniqueMemType<coda_mem_special> sub_time(
      coda_mem_time_new(time_def.get(), nullptr,
                        reinterpret_cast<coda_dynamic_type*>(time_base.get())));
  ASSERT_NE(nullptr, sub_time.get());
  time_base.release();
  ASSERT_EQ(CODA_SUCCESS,
            coda_mem_record_add_field(
                root_rec.get(), "sub_time",
                reinterpret_cast<coda_dynamic_type*>(sub_time.get()), 0));
  sub_time.release();

  // Add a mandatory missing field, an already-optional missing field, and a
  // late attribute definition to root_def before calling coda_mem_type_update.
  ASSERT_TRUE(AddRecordFieldDef(root_def.get(), "late_req",
                                reinterpret_cast<coda_type*>(str_def.get()),
                                false));
  ASSERT_TRUE(AddRecordFieldDef(root_def.get(), "late_opt",
                                reinterpret_cast<coda_type*>(str_def.get()),
                                true));
  ASSERT_TRUE(AddAttrDef(reinterpret_cast<coda_type*>(root_def.get()),
                         "root_attr",
                         reinterpret_cast<coda_type*>(str_def.get())));

  coda_dynamic_type* root_dyn =
      reinterpret_cast<coda_dynamic_type*>(root_rec.release());
  const int rc = coda_mem_type_update(
      &root_dyn, reinterpret_cast<coda_type*>(root_def.get()));
  root_rec.reset(reinterpret_cast<coda_mem_record*>(root_dyn));
  ASSERT_EQ(CODA_SUCCESS, rc);

  EXPECT_EQ(5, root_rec->num_fields);
  EXPECT_EQ(nullptr, root_rec->field_type[3]);
  EXPECT_EQ(nullptr, root_rec->field_type[4]);
  // Missing mandatory fields are marked optional by coda_mem_type_update().
  EXPECT_EQ(1, root_def->field[3]->optional);
  EXPECT_EQ(1, root_def->field[4]->optional);
  EXPECT_EQ(1, child_rec_def->field[0]->optional);
  EXPECT_NE(nullptr, root_rec->attributes);
}

TEST(CodaMemTypeTest, XmlOpenExercisesMemTypeTreeThroughPublicApi) {
  TempFile xml_file("coda_mem_type_xml.xml");
  ASSERT_TRUE(
      xml_file.WriteText("<?xml version=\"1.0\"?>\n"
                         "<root attr=\"v\">\n"
                         "  <item>one</item>\n"
                         "  <item>two</item>\n"
                         "  <nested><leaf>ok</leaf></nested>\n"
                         "</root>\n"));

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(xml_file.c_str(), product.OutParam()));

  coda_cursor cursor;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_set_product(&cursor, product.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/root/item[1]"));

  long len = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &len));
  EXPECT_EQ(3, len);
  std::string value(static_cast<size_t>(len) + 1, '\0');
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&cursor, value.data(), len + 1));
  EXPECT_STREQ("two", value.c_str());

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/root@attr"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &len));
  EXPECT_EQ(1, len);
  std::string attr_val(static_cast<size_t>(len) + 1, '\0');
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&cursor, attr_val.data(), len + 1));
  EXPECT_STREQ("v", attr_val.c_str());

  ASSERT_EQ(CODA_SUCCESS, coda_cursor_goto(&cursor, "/root/nested/leaf"));
  ASSERT_EQ(CODA_SUCCESS, coda_cursor_get_string_length(&cursor, &len));
  EXPECT_EQ(2, len);
  std::string leaf_val(static_cast<size_t>(len) + 1, '\0');
  ASSERT_EQ(CODA_SUCCESS,
            coda_cursor_read_string(&cursor, leaf_val.data(), len + 1));
  EXPECT_STREQ("ok", leaf_val.c_str());
}

// TODO(schwehr): 89.5% reachable ceiling in libcoda/coda-mem-type.c (527/589);
// remaining 62 missed lines are 61 OOM/allocator branches and 1 unreachable
// default assert(0)/exit(1) at line 157.

}  // namespace
}  // namespace stcorp_coda
