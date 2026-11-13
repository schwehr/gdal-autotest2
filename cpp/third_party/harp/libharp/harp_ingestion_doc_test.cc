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
// Unit tests for HARP ingestion reStructuredText documentation generator
// (libharp/harp-ingestion-doc.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingestion-doc.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp.h.in

#include <filesystem>  // NOLINT
#include <fstream>
#include <iterator>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::HasSubstr;

int DummyInit(const harp_ingestion_module* /*module*/,
              coda_product* /*product*/,
              const harp_ingestion_options* /*options*/,
              harp_product_definition** /*definition*/, void** /*user_data*/) {
  return 0;
}
void DummyDone(void* /*user_data*/) {}
int DummyReadDimensions(void* /*user_data*/,
                        long dimension[HARP_NUM_DIM_TYPES]) {
  dimension[harp_dimension_time] = 1;
  return 0;
}
int DummyInclude(void* /*user_data*/) { return 1; }
int DummyReadAll(void* /*user_data*/, harp_array /*data*/) { return 0; }

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

TEST(HarpIngestionDocTest, ErrorPathsNullAndUnwritableDirectory) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // 1. NULL path -> HARP_ERROR_INVALID_ARGUMENT.
  EXPECT_EQ(-1, harp_doc_export_ingestion_definitions(nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // 2. Non-existent directory -> fopen(index.rst) fails ->
  // HARP_ERROR_FILE_OPEN.
  const TempDir missing_parent("harp_ingestion_doc_missing");
  const std::filesystem::path bad_dir = missing_parent.child("does_not_exist");
  EXPECT_EQ(-1, harp_doc_export_ingestion_definitions(bad_dir.c_str()));
  EXPECT_EQ(HARP_ERROR_FILE_OPEN, harp_errno);
}

TEST(HarpIngestionDocTest, ExportAllBuiltinsAndEdgeCaseSyntheticModules) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_init());

  // 1. Module with NULL class/type/description and single same-named product
  // (exercises `:doc:` direct link and empty CODA type in index.rst).
  harp_ingestion_module* minimal_mod = harp_ingestion_register_module(
      "doc_minimal_mod", "ZZZ_DOC_TEST_GROUP", nullptr, nullptr, nullptr,
      nullptr, DummyDone);
  ASSERT_NE(nullptr, minimal_mod);
  harp_product_definition* minimal_prod = harp_ingestion_register_product(
      minimal_mod, "doc_minimal_mod", nullptr, DummyReadDimensions);
  ASSERT_NE(nullptr, minimal_prod);
  harp_variable_definition* empty_unit_enum =
      harp_ingestion_register_variable_full_read(
          minimal_prod, "enum_no_desc", harp_type_int8, 0, nullptr, nullptr,
          nullptr, "", nullptr, DummyReadAll);
  const char* kEnums[] = {"state_a", "state_b"};
  harp_variable_definition_set_enumeration_values(empty_unit_enum, 2, kEnums);

  // 2. Module with multiple products, options with/without allowed values and
  // descriptions, and mapping table formatting variants.
  harp_ingestion_module* rich_mod = harp_ingestion_register_module(
      "doc_rich_mod", "ZZZ_DOC_TEST_GROUP", "DOC_CLS", "doc_type", nullptr,
      DummyInit, DummyDone);
  ASSERT_NE(nullptr, rich_mod);
  const char* kAllowed[] = {"v1", "v2"};
  ASSERT_NE(nullptr,
            harp_ingestion_register_option(rich_mod, "opt_with_desc",
                                           "opt description", 2, kAllowed));
  ASSERT_NE(nullptr, harp_ingestion_register_option(rich_mod, "opt_bare",
                                                    nullptr, 0, nullptr));

  // 2a. Product with product-level mapping_description only (triggers
  // span_width expansion because column_width[1] + 3 + column_width[2] <
  // strlen("mapping description")).
  harp_product_definition* span_prod = harp_ingestion_register_product(
      rich_mod, "doc_span_prod", nullptr, DummyReadDimensions);
  ASSERT_NE(nullptr, span_prod);
  harp_product_definition_add_mapping(span_prod, "Product-level mapping note.",
                                      nullptr);
  ASSERT_NE(nullptr,
            harp_ingestion_register_variable_full_read(
                span_prod, "scalar_plain", harp_type_double, 0, nullptr,
                nullptr, "plain scalar", nullptr, nullptr, DummyReadAll));

  // 2b. Product exercising every `first_row` == 1 and `first_row` == 0 branch
  // for condition, path, and description in generate_product_definition.
  harp_product_definition* map_prod = harp_ingestion_register_product(
      rich_mod, "doc_map_prod", "mapping variants", DummyReadDimensions);
  ASSERT_NE(nullptr, map_prod);
  harp_product_definition_add_mapping(map_prod, "Notes.", "opt_with_desc=v1");

  const harp_dimension_type two_dims[] = {harp_dimension_independent,
                                          harp_dimension_time};
  const long two_lens[] = {3, -1};
  harp_variable_definition* v_cond = harp_ingestion_register_variable_full_read(
      map_prod, "v_cond", harp_type_int32, 2, two_dims, two_lens, "cond first",
      "1", nullptr, DummyReadAll);
  harp_variable_definition_add_mapping(v_cond, nullptr, "flag == 1", nullptr,
                                       nullptr);
  harp_variable_definition_add_mapping(v_cond, "opt_with_desc=v1", nullptr,
                                       "/a", "second mapping");

  harp_variable_definition* v_path = harp_ingestion_register_variable_full_read(
      map_prod, "v_path", harp_type_double, 0, nullptr, nullptr, "path first",
      "m", nullptr, DummyReadAll);
  harp_variable_definition_add_mapping(v_path, nullptr, nullptr, "/only_path",
                                       nullptr);
  harp_variable_definition_add_mapping(v_path, nullptr, nullptr, "/second_path",
                                       "path desc");

  harp_variable_definition* v_desc = harp_ingestion_register_variable_full_read(
      map_prod, "v_desc", harp_type_double, 0, nullptr, nullptr, "desc first",
      "K", nullptr, DummyReadAll);
  harp_variable_definition_add_mapping(v_desc, nullptr, nullptr, nullptr,
                                       "only desc");
  harp_variable_definition_add_mapping(v_desc, "opt_with_desc=v2", "c2", "/p2",
                                       "full second");

  harp_variable_definition* v_opt_include =
      harp_ingestion_register_variable_full_read(
          map_prod, "v_opt_include", harp_type_double, 0, nullptr, nullptr,
          "optional var", nullptr, DummyInclude, DummyReadAll);
  harp_variable_definition_add_mapping(v_opt_include, "opt_bare=1", nullptr,
                                       nullptr, nullptr);

  const TempDir out_dir("harp_ingestion_doc_export");
  ASSERT_EQ(HARP_SUCCESS,
            harp_doc_export_ingestion_definitions(out_dir.c_str()));

  const std::string index_rst = ReadTextFile(out_dir.child("index.rst"));
  EXPECT_THAT(index_rst, HasSubstr("Ingestion definitions"));
  EXPECT_THAT(index_rst, HasSubstr("ZZZ_DOC_TEST_GROUP products"));
  EXPECT_THAT(index_rst, HasSubstr(":doc:`doc_minimal_mod`"));
  EXPECT_THAT(index_rst, HasSubstr(":ref:`doc_rich_mod`"));
  EXPECT_THAT(index_rst, HasSubstr("`DOC_CLS/doc_type`"));

  const std::string min_rst =
      ReadTextFile(out_dir.child("doc_minimal_mod.rst"));
  EXPECT_THAT(min_rst,
              HasSubstr("enumeration values: state_a (0), state_b (1)"));
  EXPECT_THAT(min_rst, HasSubstr("\"[]\""));

  const std::string span_rst = ReadTextFile(out_dir.child("doc_span_prod.rst"));
  EXPECT_THAT(span_rst, HasSubstr("Product-level mapping note."));
  EXPECT_THAT(span_rst, HasSubstr("| field name | mapping description |"));

  const std::string map_rst = ReadTextFile(out_dir.child("doc_map_prod.rst"));
  EXPECT_THAT(map_rst, HasSubstr("{3, *time*}"));
  EXPECT_THAT(map_rst, HasSubstr("This definition is only applicable when:"));
  EXPECT_THAT(map_rst, HasSubstr("*available*"));
  EXPECT_THAT(map_rst, HasSubstr("*condition*"));
  EXPECT_THAT(map_rst, HasSubstr("*path*"));
  EXPECT_THAT(map_rst, HasSubstr("*description*"));
  EXPECT_THAT(map_rst, HasSubstr("opt_with_desc=v2 and c2"));
}

TEST(HarpIngestionDocTest, UnwritableProductDefinitionFileFails) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_init());

  harp_ingestion_module* mod = harp_ingestion_register_module(
      "doc_bad_path_mod", "ZZZ_DOC_BAD", "DOC_CLS", "bad_type", "desc",
      DummyInit, DummyDone);
  ASSERT_NE(nullptr, mod);
  ASSERT_NE(nullptr,
            harp_ingestion_register_product(mod, "no_such_subdir/bad_prod",
                                            "bad", DummyReadDimensions));

  const TempDir out_dir("harp_ingestion_doc_bad_prod");
  EXPECT_EQ(-1, harp_doc_export_ingestion_definitions(out_dir.c_str()));
  EXPECT_EQ(HARP_ERROR_FILE_OPEN, harp_errno);
}

}  // namespace
}  // namespace stcorp_harp
