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
// Tests CODA data dictionary initialization, product class/type/definition
// builders, product variables, detection tree registration/lookup, and
// definition-to-product matching (libcoda/coda-definition.c).
//
// google3 build caveats:
//   - Unreachable OOM error branches (malloc/realloc/strdup/hashtable_new
//     returning NULL) and defensive unreachable asserts in internal dictionary
//     rebuild helpers are not triggered.
//
// See also:
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-definition.c
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-definition.h
//   https://github.com/stcorp/coda/blob/master/libcoda/coda-detection.c

#include <array>
#include <cstdint>
#include <filesystem>  // NOLINT
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/stcorp_coda/google/util/coda_internal_prelude.h"
#include "third_party/stcorp_coda/google/util/scoped_coda.h"
#include "third_party/stcorp_coda/google/util/temp_file.h"
#include "third_party/stcorp_coda/google/util/zip_builder.h"

extern "C" {
#include "third_party/stcorp_coda/libcoda/coda-definition.h"
#include "third_party/stcorp_coda/libcoda/coda-type.h"
}  // extern "C"

namespace stcorp_coda {
namespace {

using ::testing::HasSubstr;

struct TypeReleaser {
  void operator()(coda_type* type) const {
    if (type != nullptr) {
      coda_type_release(type);
    }
  }
};
using ScopedType = std::unique_ptr<coda_type, TypeReleaser>;

struct ProductVarDeleter {
  void operator()(coda_product_variable* var) const {
    if (var != nullptr) {
      coda_product_variable_delete(var);
    }
  }
};
using ScopedProductVar =
    std::unique_ptr<coda_product_variable, ProductVarDeleter>;

struct ProductDefDeleter {
  void operator()(coda_product_definition* def) const {
    if (def != nullptr) {
      coda_product_definition_delete(def);
    }
  }
};
using ScopedProductDef =
    std::unique_ptr<coda_product_definition, ProductDefDeleter>;

struct ProductTypeDeleter {
  void operator()(coda_product_type* ptype) const {
    if (ptype != nullptr) {
      coda_product_type_delete(ptype);
    }
  }
};
using ScopedProductType =
    std::unique_ptr<coda_product_type, ProductTypeDeleter>;

struct ProductClassDeleter {
  void operator()(coda_product_class* pclass) const {
    if (pclass != nullptr) {
      coda_product_class_delete(pclass);
    }
  }
};
using ScopedProductClass =
    std::unique_ptr<coda_product_class, ProductClassDeleter>;

struct DetectionRuleEntryDeleter {
  void operator()(coda_detection_rule_entry* entry) const {
    if (entry != nullptr) {
      coda_detection_rule_entry_delete(entry);
    }
  }
};
using ScopedDetectionRuleEntry =
    std::unique_ptr<coda_detection_rule_entry, DetectionRuleEntryDeleter>;

struct DetectionRuleDeleter {
  void operator()(coda_detection_rule* rule) const {
    if (rule != nullptr) {
      coda_detection_rule_delete(rule);
    }
  }
};
using ScopedDetectionRule =
    std::unique_ptr<coda_detection_rule, DetectionRuleDeleter>;

ScopedExpr ParseExpr(const char* expr_str) {
  coda_expression* expr = nullptr;
  if (coda_expression_from_string(expr_str, &expr) != CODA_SUCCESS) {
    return ScopedExpr();
  }
  return ScopedExpr(expr);
}

ScopedType MakeRawType(coda_format format = coda_format_binary) {
  coda_type_raw* raw = coda_type_raw_new(format);
  if (raw == nullptr) {
    return nullptr;
  }
  return ScopedType(reinterpret_cast<coda_type*>(raw));
}

ScopedType MakeNamedRawType(const char* name,
                            coda_format format = coda_format_binary) {
  ScopedType raw = MakeRawType(format);
  if (!raw) {
    return nullptr;
  }
  if (coda_type_set_name(raw.get(), name) != CODA_SUCCESS) {
    return nullptr;
  }
  return raw;
}

// Builds a binary record type containing an 8-byte text field `magic` and a
// 16-bit little-endian unsigned integer field `val`.
ScopedType MakeMagicAndUint16RecordType() {
  coda_type_record* rec = coda_type_record_new(coda_format_binary);
  if (rec == nullptr) {
    return nullptr;
  }
  ScopedType root(reinterpret_cast<coda_type*>(rec));

  coda_type_text* magic = coda_type_text_new(coda_format_binary);
  if (magic == nullptr) {
    return nullptr;
  }
  ScopedType magic_holder(reinterpret_cast<coda_type*>(magic));
  if (coda_type_set_byte_size(magic_holder.get(), 8) != CODA_SUCCESS ||
      coda_type_record_create_field(rec, "magic", magic_holder.get()) !=
          CODA_SUCCESS) {
    return nullptr;
  }

  coda_type_number* val =
      coda_type_number_new(coda_format_binary, coda_integer_class);
  if (val == nullptr) {
    return nullptr;
  }
  ScopedType val_holder(reinterpret_cast<coda_type*>(val));
  if (coda_type_set_read_type(val_holder.get(), coda_native_type_uint16) !=
          CODA_SUCCESS ||
      coda_type_set_bit_size(val_holder.get(), 16) != CODA_SUCCESS ||
      coda_type_number_set_endianness(val, coda_little_endian) !=
          CODA_SUCCESS ||
      coda_type_record_create_field(rec, "val", val_holder.get()) !=
          CODA_SUCCESS) {
    return nullptr;
  }

  return root;
}

// Builds a single-entry detection rule matching either `path` (when non-null)
// or `expr_str` (when non-null).
ScopedDetectionRule MakeDetectionRule(const char* path, const char* expr_str) {
  ScopedDetectionRule rule(coda_detection_rule_new());
  if (!rule) {
    return nullptr;
  }
  ScopedDetectionRuleEntry entry(coda_detection_rule_entry_new(path));
  if (!entry) {
    return nullptr;
  }
  if (expr_str != nullptr) {
    ScopedExpr expr = ParseExpr(expr_str);
    if (!expr) {
      return nullptr;
    }
    if (coda_detection_rule_entry_set_expression(entry.get(), expr.get()) !=
        CODA_SUCCESS) {
      return nullptr;
    }
    expr.release();
  }
  if (coda_detection_rule_add_entry(rule.get(), entry.get()) != CODA_SUCCESS) {
    return nullptr;
  }
  entry.release();
  return rule;
}

// Helper to attach a validated product variable with optional size expression
// to `def`.
bool AddProductVariable(coda_product_definition* def, const char* name,
                        const char* size_expr_str, const char* init_expr_str) {
  ScopedProductVar var(coda_product_variable_new(name));
  if (!var) {
    return false;
  }
  if (size_expr_str != nullptr) {
    ScopedExpr size_expr = ParseExpr(size_expr_str);
    if (!size_expr || coda_product_variable_set_size_expression(
                          var.get(), size_expr.get()) != CODA_SUCCESS) {
      return false;
    }
    size_expr.release();
  }
  ScopedExpr init_expr = ParseExpr(init_expr_str);
  if (!init_expr || coda_product_variable_set_init_expression(
                        var.get(), init_expr.get()) != CODA_SUCCESS) {
    return false;
  }
  init_expr.release();
  if (coda_product_variable_validate(var.get()) != CODA_SUCCESS) {
    return false;
  }
  if (coda_product_definition_add_product_variable(def, var.get()) !=
      CODA_SUCCESS) {
    return false;
  }
  var.release();
  return true;
}

// Helper to build a single-class -> single-type -> single-definition hierarchy
// with an optional detection rule and register it in the global data
// dictionary. Requires an active CODA session / initialized data dictionary.
bool RegisterSimpleClass(const char* class_name, const char* type_name,
                         const char* def_name, coda_format format, int version,
                         const char* rule_path, const char* rule_expr) {
  ScopedProductClass pclass(coda_product_class_new(class_name));
  if (!pclass) {
    return false;
  }
  ScopedProductType ptype(coda_product_type_new(type_name));
  if (!ptype) {
    return false;
  }
  ScopedProductDef pdef(coda_product_definition_new(def_name, format, version));
  if (!pdef) {
    return false;
  }
  if (format == coda_format_binary || format == coda_format_ascii) {
    ScopedType root = MakeRawType(format);
    if (!root || coda_product_definition_set_root_type(
                     pdef.get(), root.get()) != CODA_SUCCESS) {
      return false;
    }
  }
  if (coda_product_definition_validate(pdef.get()) != CODA_SUCCESS) {
    return false;
  }
  if (rule_path != nullptr || rule_expr != nullptr) {
    ScopedDetectionRule rule = MakeDetectionRule(rule_path, rule_expr);
    if (!rule || coda_product_definition_add_detection_rule(
                     pdef.get(), rule.get()) != CODA_SUCCESS) {
      return false;
    }
    rule.release();
  }
  if (coda_product_type_add_product_definition(ptype.get(), pdef.get()) !=
      CODA_SUCCESS) {
    return false;
  }
  pdef.release();
  if (coda_product_class_add_product_type(pclass.get(), ptype.get()) !=
      CODA_SUCCESS) {
    return false;
  }
  ptype.release();
  if (coda_data_dictionary_add_product_class(pclass.get()) != CODA_SUCCESS) {
    return false;
  }
  pclass.release();
  return true;
}

TEST(CodaDefinitionTest, DataDictionaryInitDoneAndUninitializedErrors) {
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ASSERT_EQ(nullptr, coda_global_data_dictionary);

  ScopedProductClass uninit_class(coda_product_class_new("UninitClass"));
  ASSERT_NE(nullptr, uninit_class.get());

  // 1. Operations on an uninitialized dictionary fail cleanly.
  EXPECT_EQ(0, coda_data_dictionary_has_product_class("UninitClass"));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_data_dictionary_get_product_class("UninitClass"));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("not been initialized"));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_data_dictionary_add_product_class(uninit_class.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_data_dictionary_remove_product_class(uninit_class.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // 2. Direct init/done allocates and clears coda_global_data_dictionary.
  ASSERT_EQ(CODA_SUCCESS, coda_data_dictionary_init());
  const absl::Cleanup done_dict = [] {
    if (coda_global_data_dictionary != nullptr) {
      coda_data_dictionary_done();
    }
  };
  ASSERT_NE(nullptr, coda_global_data_dictionary);
  EXPECT_EQ(0, coda_global_data_dictionary->num_product_classes);
  EXPECT_EQ(nullptr,
            coda_data_dictionary_get_detection_tree(coda_format_binary));
  EXPECT_EQ(nullptr,
            coda_data_dictionary_get_detection_tree(coda_format_ascii));

  coda_data_dictionary_done();
  EXPECT_EQ(nullptr, coda_global_data_dictionary);
}

TEST(CodaDefinitionTest, ProductVariableBuilderValidationAndErrors) {
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Invalid identifiers are rejected.
  for (const char* bad_name : {"", "1bad", "bad-name", "has space"}) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(nullptr, coda_product_variable_new(bad_name)) << bad_name;
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  }

  // 2. Deleting a variable with no expressions exercises NULL guards in
  // coda_product_variable_delete().
  ScopedProductVar minimal_var(coda_product_variable_new("minimal_var"));
  ASSERT_NE(nullptr, minimal_var.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_variable_validate(minimal_var.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("missing init expression"));
  minimal_var.reset();

  // 3. Valid size/init expressions succeed once and reject duplicates.
  ScopedProductVar var(coda_product_variable_new("my_var_1"));
  ASSERT_NE(nullptr, var.get());
  EXPECT_STREQ("my_var_1", var->name);

  ScopedExpr size_expr1 = ParseExpr("3");
  ASSERT_NE(nullptr, size_expr1.get());
  ASSERT_EQ(CODA_SUCCESS, coda_product_variable_set_size_expression(
                              var.get(), size_expr1.get()));
  size_expr1.release();

  ScopedExpr size_expr2 = ParseExpr("4");
  ASSERT_NE(nullptr, size_expr2.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_variable_set_size_expression(var.get(),
                                                          size_expr2.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  ScopedExpr init_expr1 = ParseExpr("$my_var_1[0] = 7");
  ASSERT_NE(nullptr, init_expr1.get());
  ASSERT_EQ(CODA_SUCCESS, coda_product_variable_set_init_expression(
                              var.get(), init_expr1.get()));
  init_expr1.release();

  ScopedExpr init_expr2 = ParseExpr("$my_var_1[0] = 9");
  ASSERT_NE(nullptr, init_expr2.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_variable_set_init_expression(var.get(),
                                                          init_expr2.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  EXPECT_EQ(CODA_SUCCESS, coda_product_variable_validate(var.get()));
}

TEST(CodaDefinitionTest, ProductVariableEvaluationCachingAndErrors) {
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  ScopedProductClass pclass(coda_product_class_new("VarClass"));
  ASSERT_NE(nullptr, pclass.get());
  ScopedProductType ptype(coda_product_type_new("VarType"));
  ASSERT_NE(nullptr, ptype.get());
  ScopedProductDef pdef(
      coda_product_definition_new("VarProd", coda_format_binary, 1));
  ASSERT_NE(nullptr, pdef.get());
  ScopedType root = MakeMagicAndUint16RecordType();
  ASSERT_NE(nullptr, root.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_definition_set_root_type(pdef.get(), root.get()));

  // scalar_var: no size_expr -> defaults to size 1; reads /val (7) + 35 = 42.
  ASSERT_TRUE(AddProductVariable(pdef.get(), "scalar_var", nullptr,
                                 "$scalar_var = int(/val) + 35"));
  // array_var: size_expr = 3; loop initializes all 3 slots.
  ASSERT_TRUE(
      AddProductVariable(pdef.get(), "array_var", "3",
                         "for i = 0 to 2 do $array_var[i] = i * 10 + 5"));
  // bad_size_var: size_expr divides by zero at runtime.
  ASSERT_TRUE(AddProductVariable(pdef.get(), "bad_size_var", "1 / 0",
                                 "$bad_size_var = 1"));
  // bad_size_ptr_var: tested directly via get_pointer first.
  ASSERT_TRUE(AddProductVariable(pdef.get(), "bad_size_ptr_var", "1 / 0",
                                 "$bad_size_ptr_var = 1"));
  // bad_init_var: size_expr = 2 succeeds (allocates buffer), init_expr fails.
  ASSERT_TRUE(AddProductVariable(pdef.get(), "bad_init_var", "2",
                                 "$bad_init_var[0] = 1 / 0"));

  ASSERT_EQ(CODA_SUCCESS, coda_product_definition_validate(pdef.get()));
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_type_add_product_definition(ptype.get(), pdef.get()));
  pdef.release();
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_class_add_product_type(pclass.get(), ptype.get()));
  ptype.release();
  ASSERT_EQ(CODA_SUCCESS, coda_data_dictionary_add_product_class(pclass.get()));
  pclass.release();

  // "CODATEST" + uint16 little-endian 7.
  constexpr auto kProductBytes = std::to_array<uint8_t>(
      {'C', 'O', 'D', 'A', 'T', 'E', 'S', 'T', 0x07, 0x00});
  TempFile product_file("coda_def_var_product.dat");
  ASSERT_TRUE(product_file.WriteBytes(kProductBytes));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open_as(product_file.c_str(), "VarClass",
                                       "VarType", 1, product.OutParam()));

  // 1. Unknown variable names fail for both get_size and get_pointer.
  long size = -1;
  int64_t* ptr = nullptr;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_product_variable_get_size(product.get(), "missing", &size));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_product_variable_get_pointer(product.get(), "missing", 0, &ptr));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // 2. Call get_pointer BEFORE get_size on scalar_var to exercise lazy init
  // inside coda_product_variable_get_pointer(), then verify get_size returns 1.
  ASSERT_EQ(CODA_SUCCESS, coda_product_variable_get_pointer(
                              product.get(), "scalar_var", 0, &ptr));
  ASSERT_NE(nullptr, ptr);
  EXPECT_EQ(42, *ptr);
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_variable_get_size(product.get(), "scalar_var", &size));
  EXPECT_EQ(1, size);

  // 3. Call get_size twice on array_var (init + cached path), then check all
  // elements and out-of-range indices.
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_variable_get_size(product.get(), "array_var", &size));
  EXPECT_EQ(3, size);
  size = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_variable_get_size(product.get(), "array_var", &size));
  EXPECT_EQ(3, size);

  for (long i = 0; i < 3; ++i) {
    ASSERT_EQ(CODA_SUCCESS, coda_product_variable_get_pointer(
                                product.get(), "array_var", i, &ptr));
    ASSERT_NE(nullptr, ptr);
    EXPECT_EQ(i * 10 + 5, *ptr);
  }
  for (const long bad_idx : {-1L, 3L}) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(-1, coda_product_variable_get_pointer(product.get(), "array_var",
                                                    bad_idx, &ptr));
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
    EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("exceeds size"));
  }

  // 4. Runtime failure in size_expr via get_size and via get_pointer.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_product_variable_get_size(product.get(), "bad_size_var", &size));
  EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("while determining length of product variable"));

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_variable_get_pointer(product.get(),
                                                  "bad_size_ptr_var", 0, &ptr));
  EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("while determining length of product variable"));

  // 5. Runtime failure in init_expr after buffer allocation.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_product_variable_get_size(product.get(), "bad_init_var", &size));
  EXPECT_EQ(CODA_ERROR_EXPRESSION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("while initializing product variable"));
}

TEST(CodaDefinitionTest, ProductDefinitionBuilderValidationAndGrowth) {
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Invalid identifiers are rejected.
  for (const char* bad_name : {"", "9bad", "bad-def"}) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(nullptr,
              coda_product_definition_new(bad_name, coda_format_binary, 1))
        << bad_name;
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  }

  // 2. Ascii and binary definitions require a root type during validation,
  // whereas XML definitions do not.
  ScopedProductDef ascii_def(
      coda_product_definition_new("AsciiDef", coda_format_ascii, 1));
  ASSERT_NE(nullptr, ascii_def.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_definition_validate(ascii_def.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("missing root type"));

  ScopedProductDef xml_def(
      coda_product_definition_new("XmlDef", coda_format_xml, 1));
  ASSERT_NE(nullptr, xml_def.get());
  EXPECT_EQ(CODA_SUCCESS, coda_product_definition_validate(xml_def.get()));
  EXPECT_EQ(1, xml_def->initialized);

  // 3. Description, root type, and product variable growth (> BLOCK_SIZE=16).
  ScopedProductDef def(
      coda_product_definition_new("BinDef", coda_format_binary, 1));
  ASSERT_NE(nullptr, def.get());

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_definition_validate(def.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  ASSERT_EQ(CODA_SUCCESS,
            coda_product_definition_set_description(def.get(), nullptr));
  EXPECT_EQ(nullptr, def->description);
  ASSERT_EQ(CODA_SUCCESS, coda_product_definition_set_description(
                              def.get(), "binary test def"));
  EXPECT_STREQ("binary test def", def->description);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_definition_set_description(def.get(), "dup"));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  ScopedType root1 = MakeRawType(coda_format_binary);
  ScopedType root2 = MakeRawType(coda_format_binary);
  ASSERT_NE(nullptr, root1.get());
  ASSERT_NE(nullptr, root2.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_definition_set_root_type(def.get(), root1.get()));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_definition_set_root_type(def.get(), root2.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  for (int i = 0; i < 17; ++i) {
    const std::string var_name = absl::StrCat("v_", i);
    ASSERT_TRUE(
        AddProductVariable(def.get(), var_name.c_str(), nullptr, "$v_0 = 1"))
        << i;
  }
  EXPECT_EQ(17, def->num_product_variables);

  // Duplicate product variable fails and caller retains ownership.
  ScopedProductVar dup_var(coda_product_variable_new("v_0"));
  ASSERT_NE(nullptr, dup_var.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_definition_add_product_variable(def.get(),
                                                             dup_var.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  dup_var.reset();

  // 4. Empty detection rule is rejected.
  ScopedDetectionRule empty_rule(coda_detection_rule_new());
  ASSERT_NE(nullptr, empty_rule.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_definition_add_detection_rule(def.get(),
                                                           empty_rule.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  empty_rule.reset();

  ASSERT_EQ(CODA_SUCCESS, coda_product_definition_validate(def.get()));
  EXPECT_EQ(1, def->initialized);

  // 5. Register `def` in the dictionary with a detection rule, then verify a
  // shadowed detection rule on a second definition is rejected cleanly.
  ScopedDetectionRule rule1 =
      MakeDetectionRule(nullptr, "bytes(., 0, 8) == \"SHADOW01\"");
  ASSERT_NE(nullptr, rule1.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_definition_add_detection_rule(def.get(), rule1.get()));
  rule1.release();

  ScopedProductType ptype(coda_product_type_new("ShadowType"));
  ASSERT_NE(nullptr, ptype.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_type_add_product_definition(ptype.get(), def.get()));
  def.release();
  ScopedProductClass pclass(coda_product_class_new("ShadowClass"));
  ASSERT_NE(nullptr, pclass.get());
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_class_add_product_type(pclass.get(), ptype.get()));
  ptype.release();
  ASSERT_EQ(CODA_SUCCESS, coda_data_dictionary_add_product_class(pclass.get()));
  pclass.release();

  ScopedProductDef def2(
      coda_product_definition_new("BinDef2", coda_format_binary, 1));
  ASSERT_NE(nullptr, def2.get());
  ScopedDetectionRule shadowed_rule =
      MakeDetectionRule(nullptr, "bytes(., 0, 8) == \"SHADOW01\"");
  ASSERT_NE(nullptr, shadowed_rule.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_definition_add_detection_rule(
                    def2.get(), shadowed_rule.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno), HasSubstr("shadowed"));
}

TEST(CodaDefinitionTest, ProductTypeBuilderVersionSelectionAndGrowth) {
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedProductType ptype(coda_product_type_new("MultiVersionType"));
  ASSERT_NE(nullptr, ptype.get());

  // 1. Description can be set to NULL once, then a string, then rejects
  // repeats.
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_type_set_description(ptype.get(), nullptr));
  EXPECT_EQ(nullptr, ptype->description);
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_type_set_description(ptype.get(), "type description"));
  EXPECT_STREQ("type description", ptype->description);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_type_set_description(ptype.get(), "second"));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // 2. Empty product type errors on version queries.
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr,
            coda_product_type_get_latest_product_definition(ptype.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_product_type_get_product_definition_by_version(
                         ptype.get(), 1));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // 3. Add 17 definitions in non-monotonic version order so both '>' branches
  // in get_latest_product_definition() and BLOCK_SIZE (16) growth trigger.
  constexpr std::array<int, 17> kVersions = {10, 5, 25, 15, 1,  2,  3,  4, 6,
                                             7,  8, 9,  11, 12, 13, 14, 16};
  for (const int version : kVersions) {
    const std::string def_name = absl::StrCat("Def_v", version);
    ScopedProductDef pdef(coda_product_definition_new(
        def_name.c_str(), coda_format_binary, version));
    ASSERT_NE(nullptr, pdef.get());
    ASSERT_EQ(CODA_SUCCESS, coda_product_type_add_product_definition(
                                ptype.get(), pdef.get()));
    pdef.release();
  }
  EXPECT_EQ(17, ptype->num_product_definitions);

  coda_product_definition* latest =
      coda_product_type_get_latest_product_definition(ptype.get());
  ASSERT_NE(nullptr, latest);
  EXPECT_EQ(25, latest->version);
  EXPECT_STREQ("Def_v25", latest->name);

  coda_product_definition* v5 =
      coda_product_type_get_product_definition_by_version(ptype.get(), 5);
  ASSERT_NE(nullptr, v5);
  EXPECT_STREQ("Def_v5", v5->name);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr, coda_product_type_get_product_definition_by_version(
                         ptype.get(), 999));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // 4. Reject re-using a definition already belonging to a product type,
  // duplicate version number, and duplicate definition name.
  ScopedProductType other_ptype(coda_product_type_new("OtherType"));
  ASSERT_NE(nullptr, other_ptype.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_product_type_add_product_definition(other_ptype.get(), v5));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("more than one product type"));

  ScopedProductDef dup_ver(
      coda_product_definition_new("UniqueName", coda_format_binary, 5));
  ASSERT_NE(nullptr, dup_ver.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(
      -1, coda_product_type_add_product_definition(ptype.get(), dup_ver.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("multiple product definitions with version"));

  ScopedProductDef dup_name(
      coda_product_definition_new("Def_v5", coda_format_binary, 99));
  ASSERT_NE(nullptr, dup_name.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_type_add_product_definition(ptype.get(),
                                                         dup_name.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  EXPECT_THAT(coda_errno_to_string(coda_errno),
              HasSubstr("duplicate product definition"));
}

TEST(CodaDefinitionTest, ProductClassBuilderNamedTypesAndProductTypes) {
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  // 1. Invalid identifiers are rejected.
  for (const char* bad_name : {"", "1cls", "bad-cls"}) {
    coda_set_error(CODA_SUCCESS, nullptr);
    EXPECT_EQ(nullptr, coda_product_class_new(bad_name)) << bad_name;
    EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  }

  ScopedProductClass pclass(coda_product_class_new("ValidClass"));
  ASSERT_NE(nullptr, pclass.get());

  // 2. Description, definition_file, and revision getters/setters.
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_class_set_description(pclass.get(), nullptr));
  EXPECT_EQ(nullptr, pclass->description);
  ASSERT_EQ(CODA_SUCCESS,
            coda_product_class_set_description(pclass.get(), "class desc"));
  EXPECT_STREQ("class desc", pclass->description);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_class_set_description(pclass.get(), "dup"));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  ASSERT_EQ(CODA_SUCCESS,
            coda_product_class_set_definition_file(pclass.get(), nullptr));
  EXPECT_EQ(nullptr, pclass->definition_file);
  ASSERT_EQ(CODA_SUCCESS, coda_product_class_set_definition_file(
                              pclass.get(), "/synthetic/test.codadef"));
  EXPECT_STREQ("/synthetic/test.codadef", pclass->definition_file);
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_class_set_definition_file(pclass.get(), "/dup"));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  EXPECT_EQ(0, coda_product_class_get_revision(pclass.get()));
  ASSERT_EQ(CODA_SUCCESS, coda_product_class_set_revision(pclass.get(), 42));
  EXPECT_EQ(42, coda_product_class_get_revision(pclass.get()));

  // 3. Named type addition (> BLOCK_SIZE=16), lookup, and duplicate rejection.
  EXPECT_EQ(0, coda_product_class_has_named_type(pclass.get(), "missing_nt"));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr,
            coda_product_class_get_named_type(pclass.get(), "missing_nt"));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  for (int i = 0; i < 17; ++i) {
    const std::string nt_name = absl::StrCat("nt_", i);
    ScopedType nt = MakeNamedRawType(nt_name.c_str());
    ASSERT_NE(nullptr, nt.get());
    ASSERT_EQ(CODA_SUCCESS,
              coda_product_class_add_named_type(pclass.get(), nt.get()))
        << i;
    EXPECT_EQ(1,
              coda_product_class_has_named_type(pclass.get(), nt_name.c_str()));
    EXPECT_EQ(nt.get(),
              coda_product_class_get_named_type(pclass.get(), nt_name.c_str()));
  }
  EXPECT_EQ(17, pclass->num_named_types);

  ScopedType dup_nt = MakeNamedRawType("nt_0");
  ASSERT_NE(nullptr, dup_nt.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_product_class_add_named_type(pclass.get(), dup_nt.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // 4. Product type addition (> BLOCK_SIZE=16), lookup, and duplicate
  // rejection.
  EXPECT_EQ(0, coda_product_class_has_product_type(pclass.get(), "missing_pt"));
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(nullptr,
            coda_product_class_get_product_type(pclass.get(), "missing_pt"));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  for (int i = 0; i < 17; ++i) {
    const std::string pt_name = absl::StrCat("pt_", i);
    ScopedProductType pt(coda_product_type_new(pt_name.c_str()));
    ASSERT_NE(nullptr, pt.get());
    coda_product_type* raw_pt = pt.get();
    ASSERT_EQ(CODA_SUCCESS,
              coda_product_class_add_product_type(pclass.get(), raw_pt))
        << i;
    pt.release();
    EXPECT_EQ(
        1, coda_product_class_has_product_type(pclass.get(), pt_name.c_str()));
    EXPECT_EQ(raw_pt, coda_product_class_get_product_type(pclass.get(),
                                                          pt_name.c_str()));
  }
  EXPECT_EQ(17, pclass->num_product_types);

  ScopedProductType dup_pt(coda_product_type_new("pt_0"));
  ASSERT_NE(nullptr, dup_pt.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_product_class_add_product_type(pclass.get(), dup_pt.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
}

TEST(CodaDefinitionTest,
     DataDictionarySortedInsertLookupRemovalAndDetectionTreeRebuild) {
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. Register classes out of alphabetical order across binary, ascii, and xml
  // formats, plus 15 extra classes to exceed BLOCK_SIZE (16).
  ASSERT_TRUE(RegisterSimpleClass("Zulu", "ZuluType", "ZuluDef_v1",
                                  coda_format_binary, 1, nullptr,
                                  "bytes(., 0, 8) == \"ZULU0001\""));
  ASSERT_TRUE(RegisterSimpleClass("Alpha", "AlphaType", "AlphaDef_v1",
                                  coda_format_ascii, 1, nullptr,
                                  "bytes(., 0, 8) == \"ALPHA001\""));
  ASSERT_TRUE(RegisterSimpleClass("Mike", "MikeType", "MikeDef_v2",
                                  coda_format_xml, 2, "/mike_root", nullptr));

  // Add a second version to AlphaType and an empty product type to Alpha.
  coda_product_class* alpha_cls =
      coda_data_dictionary_get_product_class("Alpha");
  ASSERT_NE(nullptr, alpha_cls);
  coda_product_type* alpha_type =
      coda_product_class_get_product_type(alpha_cls, "AlphaType");
  ASSERT_NE(nullptr, alpha_type);
  {
    ScopedProductDef alpha_v3(
        coda_product_definition_new("AlphaDef_v3", coda_format_ascii, 3));
    ASSERT_NE(nullptr, alpha_v3.get());
    ScopedType root = MakeRawType(coda_format_ascii);
    ASSERT_NE(nullptr, root.get());
    ASSERT_EQ(CODA_SUCCESS, coda_product_definition_set_root_type(
                                alpha_v3.get(), root.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_product_definition_validate(alpha_v3.get()));
    ASSERT_EQ(CODA_SUCCESS, coda_product_type_add_product_definition(
                                alpha_type, alpha_v3.get()));
    alpha_v3.release();
  }
  {
    ScopedProductType empty_type(coda_product_type_new("EmptyType"));
    ASSERT_NE(nullptr, empty_type.get());
    ASSERT_EQ(CODA_SUCCESS,
              coda_product_class_add_product_type(alpha_cls, empty_type.get()));
    empty_type.release();
  }

  for (int i = 0; i < 15; ++i) {
    const std::string cname = absl::StrCat("Extra_", i);
    ASSERT_TRUE(RegisterSimpleClass(cname.c_str(), "ExtraType", "ExtraDef",
                                    coda_format_binary, 1, nullptr, nullptr));
  }
  EXPECT_EQ(18, coda_global_data_dictionary->num_product_classes);

  // Verify lexicographic sorting in product_class[] and duplicate rejection.
  for (int i = 1; i < coda_global_data_dictionary->num_product_classes; ++i) {
    EXPECT_LT(
        std::string_view(
            coda_global_data_dictionary->product_class[i - 1]->name),
        std::string_view(coda_global_data_dictionary->product_class[i]->name));
  }

  ScopedProductClass dup_alpha(coda_product_class_new("Alpha"));
  ASSERT_NE(nullptr, dup_alpha.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_data_dictionary_add_product_class(dup_alpha.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
  dup_alpha.reset();

  // 2. Verify coda_data_dictionary_get_definition() success & error paths.
  coda_product_definition* found_def = nullptr;
  EXPECT_EQ(CODA_SUCCESS, coda_data_dictionary_get_definition(
                              "Alpha", "AlphaType", 1, &found_def));
  ASSERT_NE(nullptr, found_def);
  EXPECT_EQ(1, found_def->version);

  EXPECT_EQ(CODA_SUCCESS, coda_data_dictionary_get_definition(
                              "Alpha", "AlphaType", -1, &found_def));
  ASSERT_NE(nullptr, found_def);
  EXPECT_EQ(3, found_def->version);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_data_dictionary_get_definition("NoClass", "AlphaType", -1,
                                                    &found_def));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_data_dictionary_get_definition("Alpha", "NoType", -1,
                                                    &found_def));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_data_dictionary_get_definition("Alpha", "EmptyType", -1,
                                                    &found_def));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_data_dictionary_get_definition("Alpha", "AlphaType", 99,
                                                    &found_def));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);

  // 3. Verify detection trees for ascii/binary and xml.
  EXPECT_NE(nullptr,
            coda_data_dictionary_get_detection_tree(coda_format_binary));
  EXPECT_EQ(coda_data_dictionary_get_detection_tree(coda_format_binary),
            coda_data_dictionary_get_detection_tree(coda_format_ascii));
  EXPECT_NE(nullptr, coda_data_dictionary_get_detection_tree(coda_format_xml));

  // 4. Remove "Mike" (middle class) -> shifts array, deletes Mike, rebuilds
  // hashtable and detection trees.
  coda_product_class* mike_cls = coda_data_dictionary_get_product_class("Mike");
  ASSERT_NE(nullptr, mike_cls);
  ASSERT_EQ(CODA_SUCCESS, coda_data_dictionary_remove_product_class(mike_cls));
  EXPECT_EQ(0, coda_data_dictionary_has_product_class("Mike"));
  EXPECT_EQ(17, coda_global_data_dictionary->num_product_classes);
  EXPECT_EQ(nullptr, coda_data_dictionary_get_detection_tree(coda_format_xml));
  EXPECT_NE(nullptr,
            coda_data_dictionary_get_detection_tree(coda_format_binary));
  EXPECT_EQ(1, coda_data_dictionary_has_product_class("Alpha"));
  EXPECT_EQ(1, coda_data_dictionary_has_product_class("Zulu"));
  EXPECT_NE(nullptr, coda_data_dictionary_get_product_class("Zulu"));

  // Removing a class not registered in the dictionary fails and leaves caller
  // owning it.
  ScopedProductClass unregistered(coda_product_class_new("Unregistered"));
  ASSERT_NE(nullptr, unregistered.get());
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1, coda_data_dictionary_remove_product_class(unregistered.get()));
  EXPECT_EQ(CODA_ERROR_DATA_DEFINITION, coda_errno);
}

TEST(CodaDefinitionTest, FindDefinitionForProductAndAsciiFormatMapping) {
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  ScopedCodaSession session;
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  // 1. NULL product fails in coda_data_dictionary_find_definition_for_product()
  // via coda_cursor_set_product().
  coda_product_definition* def = nullptr;
  coda_set_error(CODA_SUCCESS, nullptr);
  EXPECT_EQ(-1,
            coda_data_dictionary_find_definition_for_product(nullptr, &def));
  EXPECT_EQ(CODA_ERROR_INVALID_ARGUMENT, coda_errno);

  // 2. Register an ASCII product definition whose rule lives in the binary
  // detection tree.
  ASSERT_TRUE(RegisterSimpleClass("AsciiClass", "AsciiType", "AsciiProd",
                                  coda_format_ascii, 4, nullptr,
                                  "bytes(., 0, 8) == \"ASCIITST\""));

  TempFile ascii_file("coda_def_ascii_match.txt");
  ASSERT_TRUE(ascii_file.WriteText("ASCIITST hello\n"));

  int64_t file_size = -1;
  coda_format format = coda_format_binary;
  const char* pclass = nullptr;
  const char* ptype = nullptr;
  int version = -1;
  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(ascii_file.c_str(), &file_size, &format,
                                &pclass, &ptype, &version));
  EXPECT_EQ(15, file_size);
  EXPECT_EQ(coda_format_ascii, format);
  EXPECT_STREQ("AsciiClass", pclass);
  EXPECT_STREQ("AsciiType", ptype);
  EXPECT_EQ(4, version);

  ScopedProduct opened_ascii;
  ASSERT_EQ(CODA_SUCCESS,
            coda_open(ascii_file.c_str(), opened_ascii.OutParam()));
  ASSERT_EQ(CODA_SUCCESS, coda_get_product_format(opened_ascii.get(), &format));
  EXPECT_EQ(coda_format_ascii, format);

  // 3. Non-matching file returns CODA_SUCCESS with NULL class/type.
  TempFile unknown_file("coda_def_unknown.bin");
  ASSERT_TRUE(unknown_file.WriteText("NOMATCH_12345"));
  ASSERT_EQ(CODA_SUCCESS,
            coda_recognize_file(unknown_file.c_str(), &file_size, &format,
                                &pclass, &ptype, &version));
  EXPECT_EQ(coda_format_binary, format);
  EXPECT_EQ(nullptr, pclass);
  EXPECT_EQ(nullptr, ptype);
  EXPECT_EQ(-1, version);
}

TEST(CodaDefinitionTest, CodadefZipIntegrationAndRevisionReplacement) {
  const absl::Cleanup reset_error = [] {
    coda_set_error(CODA_SUCCESS, nullptr);
  };

  constexpr std::string_view kIndexXml = R"xml(<?xml version="1.0"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="ZipClass">
  <cd:Description>Synthesized ZIP codadef class</cd:Description>
  <cd:ProductType name="ZipType">
    <cd:Description>Synthesized ZIP product type</cd:Description>
    <cd:ProductDefinition id="ZipProd" format="binary" version="1">
      <cd:Description>Synthesized ZIP product v1</cd:Description>
      <cd:DetectionRule>
        <cd:MatchData offset="0">ZIPMAGIC</cd:MatchData>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)xml";

  constexpr std::string_view kProductXml = R"xml(<?xml version="1.0"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="ZipProd" format="binary">
  <cd:ProductVariable name="scaled_val">
    <cd:Init>$scaled_val = int(/val) * 3</cd:Init>
  </cd:ProductVariable>
  <cd:ProductVariable name="seq_var">
    <cd:Dimension>3</cd:Dimension>
    <cd:Init>for i = 0 to 2 do $seq_var[i] = i + 100</cd:Init>
  </cd:ProductVariable>
  <cd:Record>
    <cd:Field name="magic">
      <cd:Text>
        <cd:ByteSize>8</cd:ByteSize>
      </cd:Text>
    </cd:Field>
    <cd:Field name="val">
      <cd:Integer>
        <cd:BitSize>16</cd:BitSize>
        <cd:NativeType>uint16</cd:NativeType>
        <cd:LittleEndian/>
      </cd:Integer>
    </cd:Field>
  </cd:Record>
</cd:ProductDefinition>
)xml";

  TempDir def_dir("coda_def_zip_dir");
  const std::filesystem::path rev1_path = def_dir.child("zip_rev1.codadef");
  const std::filesystem::path rev2_path = def_dir.child("zip_rev2.codadef");

  const std::array<std::pair<std::string_view, std::string_view>, 2>
      rev1_entries = {
          {{"VERSION", "1"}, {"products/ZipProd.xml", kProductXml}}};
  const std::array<std::pair<std::string_view, std::string_view>, 2>
      rev2_entries = {
          {{"VERSION", "2"}, {"products/ZipProd.xml", kProductXml}}};
  ASSERT_TRUE(
      WriteFileBytes(rev1_path, BuildCodadefBytes(kIndexXml, rev1_entries)));
  ASSERT_TRUE(
      WriteFileBytes(rev2_path, BuildCodadefBytes(kIndexXml, rev2_entries)));

  // Colon-separated path ensures rev1 loads first and rev2 replaces it.
  const std::string def_path =
      absl::StrCat(rev1_path.native(), ":", rev2_path.native());
  ScopedCodaSession session;
  const absl::Cleanup clear_def_path = [&session] {
    if (!session.active()) {
      coda_set_definition_path(nullptr);
    }
  };
  ASSERT_EQ(CODA_SUCCESS, coda_set_definition_path(def_path.c_str()));
  ASSERT_EQ(CODA_SUCCESS, session.Init());

  coda_product_class* zip_class =
      coda_data_dictionary_get_product_class("ZipClass");
  ASSERT_NE(nullptr, zip_class);
  EXPECT_EQ(2, coda_product_class_get_revision(zip_class));

  // "ZIPMAGIC" + uint16 little-endian 9 -> scaled_val = 27.
  constexpr auto kZipProductBytes = std::to_array<uint8_t>(
      {'Z', 'I', 'P', 'M', 'A', 'G', 'I', 'C', 0x09, 0x00});
  TempFile product_file("coda_def_zip_product.dat");
  ASSERT_TRUE(product_file.WriteBytes(kZipProductBytes));

  ScopedProduct product;
  ASSERT_EQ(CODA_SUCCESS, coda_open(product_file.c_str(), product.OutParam()));

  const char* def_file = nullptr;
  ASSERT_EQ(CODA_SUCCESS,
            coda_get_product_definition_file(product.get(), &def_file));
  ASSERT_NE(nullptr, def_file);
  EXPECT_STREQ(rev2_path.c_str(), def_file);

  int64_t val = -1;
  ASSERT_EQ(CODA_SUCCESS, coda_get_product_variable_value(
                              product.get(), "scaled_val", 0, &val));
  EXPECT_EQ(27, val);

  for (long i = 0; i < 3; ++i) {
    ASSERT_EQ(CODA_SUCCESS, coda_get_product_variable_value(
                                product.get(), "seq_var", i, &val));
    EXPECT_EQ(100 + i, val);
  }
}

// TODO(schwehr): 90.2% line coverage in libcoda/coda-definition.c (644/714);
// remaining missed lines are OOM/realloc error branches and defensive
// NULL/setter error guards on freshly created dictionary nodes.

}  // namespace
}  // namespace stcorp_coda
