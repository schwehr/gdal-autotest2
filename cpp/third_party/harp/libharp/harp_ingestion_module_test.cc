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
// Unit tests for HARP ingestion module registration, definitions, option
// validation, and CODA module lookup (libharp/harp-ingestion-module.c).
//
// Google3 build notes: No checked-in .codadef or testdata files are used;
// minimal stored-ZIP .codadef and NetCDF-3 files are synthesized in
// ::testing::TempDir().
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingestion-module.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingestion.h

#include <bit>
#include <cstdint>
#include <filesystem>  // NOLINT
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/libcoda/coda.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::HasSubstr;

int DummyIngestionInit(const harp_ingestion_module* /*module*/,
                       coda_product* /*product*/,
                       const harp_ingestion_options* /*options*/,
                       harp_product_definition** /*definition*/,
                       void** /*user_data*/) {
  return 0;
}

void DummyIngestionDone(void* /*user_data*/) {}

int DummyReadDimensions(void* /*user_data*/,
                        long dimension[HARP_NUM_DIM_TYPES]) {
  dimension[harp_dimension_time] = 1;
  return 0;
}

int DummyReadDatetimeRange(void* /*user_data*/, double* start, double* stop) {
  *start = 0.0;
  *stop = 1.0;
  return 0;
}

int DummyIncludeYes(void* /*user_data*/) { return 1; }
int DummyIncludeNo(void* /*user_data*/) { return 0; }
int DummyReadAll(void* /*user_data*/, harp_array /*data*/) { return 0; }
long DummyGetMaxRange(void* /*user_data*/) { return 1; }
int DummyReadRange(void* /*user_data*/, long /*offset*/, long /*len*/,
                   harp_array /*data*/) {
  return 0;
}
int DummyReadBlock(void* /*user_data*/, long /*index*/, harp_array /*data*/) {
  return 0;
}

void AppendU16Le(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void AppendU32Le(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void AppendU32Be(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void AppendNetcdfString(std::vector<uint8_t>& out, std::string_view s) {
  AppendU32Be(out, static_cast<uint32_t>(s.size()));
  for (char c : s) {
    out.push_back(static_cast<uint8_t>(c));
  }
  while (out.size() % 4 != 0) {
    out.push_back(0);
  }
}

struct ZipEntrySpec {
  std::string_view name;
  std::string_view data;
};

bool WriteStoredZipCodadef(const std::filesystem::path& path,
                           std::span<const ZipEntrySpec> entries) {
  std::vector<uint8_t> buf;
  struct CentralInfo {
    uint32_t offset;
    ZipEntrySpec spec;
  };
  std::vector<CentralInfo> central;
  central.reserve(entries.size());

  for (const auto& entry : entries) {
    const uint32_t offset = static_cast<uint32_t>(buf.size());
    central.push_back({offset, entry});
    const uint16_t name_len = static_cast<uint16_t>(entry.name.size());
    const uint32_t data_len = static_cast<uint32_t>(entry.data.size());

    AppendU32Le(buf, 0x04034B50U);
    AppendU16Le(buf, 20);
    AppendU16Le(buf, 0);
    AppendU16Le(buf, 0);
    AppendU16Le(buf, 0);
    AppendU16Le(buf, 0);
    AppendU32Le(buf, 0);
    AppendU32Le(buf, data_len);
    AppendU32Le(buf, data_len);
    AppendU16Le(buf, name_len);
    AppendU16Le(buf, 0);
    for (char c : entry.name) {
      buf.push_back(static_cast<uint8_t>(c));
    }
    for (char c : entry.data) {
      buf.push_back(static_cast<uint8_t>(c));
    }
  }

  const uint32_t cd_offset = static_cast<uint32_t>(buf.size());
  for (const auto& c : central) {
    const uint16_t name_len = static_cast<uint16_t>(c.spec.name.size());
    const uint32_t data_len = static_cast<uint32_t>(c.spec.data.size());

    AppendU32Le(buf, 0x02014B50U);
    AppendU16Le(buf, 20);
    AppendU16Le(buf, 20);
    AppendU16Le(buf, 0);
    AppendU16Le(buf, 0);
    AppendU16Le(buf, 0);
    AppendU16Le(buf, 0);
    AppendU32Le(buf, 0);
    AppendU32Le(buf, data_len);
    AppendU32Le(buf, data_len);
    AppendU16Le(buf, name_len);
    AppendU16Le(buf, 0);
    AppendU16Le(buf, 0);
    AppendU16Le(buf, 0);
    AppendU16Le(buf, 0);
    AppendU32Le(buf, 0);
    AppendU32Le(buf, c.offset);
    for (char ch : c.spec.name) {
      buf.push_back(static_cast<uint8_t>(ch));
    }
  }

  const uint32_t cd_size = static_cast<uint32_t>(buf.size()) - cd_offset;
  AppendU32Le(buf, 0x06054B50U);
  AppendU16Le(buf, 0);
  AppendU16Le(buf, 0);
  AppendU16Le(buf, static_cast<uint16_t>(entries.size()));
  AppendU16Le(buf, static_cast<uint16_t>(entries.size()));
  AppendU32Le(buf, cd_size);
  AppendU32Le(buf, cd_offset);
  AppendU16Le(buf, 0);

  return WriteFileBytes(path, buf);
}

// Writes a minimal NetCDF-3 classic file with a single global attribute
// `Ozone_field_date = {2020, 5}` so CODA detection rules can match it.
bool WriteMinimalNetcdf3(const std::filesystem::path& path) {
  std::vector<uint8_t> buf;
  buf.insert(buf.end(), {'C', 'D', 'F', 1});
  AppendU32Be(buf, 0);            // numrecs
  AppendU32Be(buf, 0x0000000AU);  // NC_DIMENSION
  AppendU32Be(buf, 1);
  AppendNetcdfString(buf, "x");
  AppendU32Be(buf, 1);
  AppendU32Be(buf, 0x0000000CU);  // NC_ATTRIBUTE
  AppendU32Be(buf, 1);
  AppendNetcdfString(buf, "Ozone_field_date");
  AppendU32Be(buf, 4);  // NC_INT
  AppendU32Be(buf, 2);
  AppendU32Be(buf, 2020);
  AppendU32Be(buf, 5);
  AppendU32Be(buf, 0);  // NC_VARIABLE ABSENT
  AppendU32Be(buf, 0);
  return WriteFileBytes(path, buf);
}

TEST(HarpIngestionModuleTest, InitIdempotencyDoneAndIndexVariable) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  EXPECT_EQ(nullptr, harp_ingestion_get_module_register());
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_init());
  // Second init hits early-return branch.
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_init());

  harp_ingestion_module_register* reg = harp_ingestion_get_module_register();
  ASSERT_NE(nullptr, reg);
  EXPECT_GT(reg->num_ingestion_modules, 0);

  // Locate a time-dependent built-in product definition and verify the
  // auto-registered `index` variable invokes `read_index`.
  const harp_variable_definition* index_def = nullptr;
  for (int i = 0; i < reg->num_ingestion_modules && index_def == nullptr; ++i) {
    for (int j = 0; j < reg->ingestion_module[i]->num_product_definitions;
         ++j) {
      const harp_product_definition* pdef =
          reg->ingestion_module[i]->product_definition[j];
      if (harp_product_definition_has_dimension_type(pdef,
                                                     harp_dimension_time)) {
        index_def = harp_product_definition_find_variable(pdef, "index");
        break;
      }
    }
  }
  ASSERT_NE(nullptr, index_def);
  ASSERT_NE(nullptr, index_def->read_block);
  int32_t idx_val = -1;
  harp_array idx_arr{};
  idx_arr.int32_data = &idx_val;
  EXPECT_EQ(HARP_SUCCESS, index_def->read_block(nullptr, 42, idx_arr));
  EXPECT_EQ(42, idx_val);

  harp_ingestion_done();
  EXPECT_EQ(nullptr, harp_ingestion_get_module_register());
  // Calling done() when already NULL is a safe no-op.
  harp_ingestion_done();
}

TEST(HarpIngestionModuleTest, RegisterModuleProductOptionVariableAndMappings) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_init());

  // 1. Register a module with NULL product_class/type/description/init and
  // verify block-growth across > BLOCK_SIZE (16) module registrations.
  harp_ingestion_module* null_cls_mod = harp_ingestion_register_module(
      "test_null_cls_mod", "TEST_GROUP", nullptr, nullptr, nullptr, nullptr,
      DummyIngestionDone);
  ASSERT_NE(nullptr, null_cls_mod);
  for (int i = 0; i < 18; ++i) {
    const std::string mname = "test_extra_mod_" + std::to_string(i);
    ASSERT_NE(nullptr,
              harp_ingestion_register_module(
                  mname.c_str(), "TEST_GROUP", "TEST_CLS", mname.c_str(),
                  "extra", DummyIngestionInit, DummyIngestionDone));
  }

  // 2. Register a full module with > BLOCK_SIZE options, products, variables,
  // and mappings to exercise realloc growth and cleanup paths.
  harp_ingestion_module* mod = harp_ingestion_register_module(
      "test_full_mod", "TEST_GROUP", "TEST_CLS", "test_full_type",
      "full synthetic test module", DummyIngestionInit, DummyIngestionDone);
  ASSERT_NE(nullptr, mod);

  const char* kAllowed[] = {"alpha", "beta"};
  harp_ingestion_option_definition* opt0 =
      harp_ingestion_register_option(mod, "mode", "mode option", 2, kAllowed);
  ASSERT_NE(nullptr, opt0);
  // Option with NULL description and 0 allowed values.
  ASSERT_NE(nullptr, harp_ingestion_register_option(mod, "empty_opt", nullptr,
                                                    0, nullptr));
  for (int i = 0; i < 18; ++i) {
    const std::string oname = "opt_" + std::to_string(i);
    ASSERT_NE(nullptr, harp_ingestion_register_option(mod, oname.c_str(),
                                                      "desc", 1, kAllowed));
  }
  EXPECT_EQ(20, mod->num_option_definitions);

  // Register > BLOCK_SIZE products.
  harp_product_definition* prod = harp_ingestion_register_product(
      mod, "test_full_prod", "full product", DummyReadDimensions);
  ASSERT_NE(nullptr, prod);
  harp_ingestion_register_datetime_range_read(prod, DummyReadDatetimeRange);
  for (int i = 0; i < 18; ++i) {
    const std::string pname = "extra_prod_" + std::to_string(i);
    ASSERT_NE(nullptr, harp_ingestion_register_product(
                           mod, pname.c_str(), nullptr, DummyReadDimensions));
  }
  EXPECT_EQ(19, mod->num_product_definitions);

  // Product mapping: initial set, append, and ingestion_option.
  harp_product_definition_add_mapping(prod, "part1 ", nullptr);
  harp_product_definition_add_mapping(prod, "part2", "mode=alpha");
  EXPECT_STREQ("part1 part2", prod->mapping_description);
  EXPECT_STREQ("mode=alpha", prod->ingestion_option);

  // Register scalar, 1D, 2D (with independent dim), full/range/block, and
  // string variables (> BLOCK_SIZE total).
  harp_variable_definition* scalar_var =
      harp_ingestion_register_variable_full_read(
          prod, "scalar_f", harp_type_float, 0, nullptr, nullptr, nullptr,
          nullptr, DummyIncludeNo, DummyReadAll);
  ASSERT_NE(nullptr, scalar_var);
  EXPECT_EQ(0, harp_variable_definition_include(scalar_var, nullptr));

  const harp_dimension_type time_dim[] = {harp_dimension_time};
  harp_variable_definition* range_var =
      harp_ingestion_register_variable_range_read(
          prod, "range_d", harp_type_double, 1, time_dim, nullptr, "range var",
          "m", DummyIncludeYes, DummyGetMaxRange, DummyReadRange);
  ASSERT_NE(nullptr, range_var);
  EXPECT_EQ(1, harp_variable_definition_include(range_var, nullptr));

  const harp_dimension_type two_dims[] = {harp_dimension_time,
                                          harp_dimension_independent};
  const long two_lens[] = {-1, 4};
  harp_variable_definition* block_var =
      harp_ingestion_register_variable_block_read(
          prod, "block_s", harp_type_string, 2, two_dims, two_lens, "block str",
          nullptr, nullptr, DummyReadBlock);
  ASSERT_NE(nullptr, block_var);
  EXPECT_EQ(1, harp_variable_definition_include(block_var, nullptr));
  EXPECT_EQ(1, harp_variable_definition_has_dimension_type(
                   range_var, harp_dimension_time));
  EXPECT_EQ(0, harp_variable_definition_has_dimension_type(
                   block_var, harp_dimension_time));
  EXPECT_EQ(
      1, harp_variable_definition_has_dimension_types(block_var, 2, two_dims));
  const harp_dimension_type mismatch_dims[] = {harp_dimension_time,
                                               harp_dimension_latitude};
  EXPECT_EQ(0, harp_variable_definition_has_dimension_types(block_var, 2,
                                                            mismatch_dims));

  // Add > BLOCK_SIZE mappings with different combinations of NULL/non-NULL.
  harp_variable_definition_add_mapping(range_var, "mode=alpha", "cond", "/path",
                                       "desc");
  harp_variable_definition_add_mapping(range_var, nullptr, nullptr,
                                       "/only_path", nullptr);
  for (int i = 0; i < 16; ++i) {
    const std::string p = "/p_" + std::to_string(i);
    harp_variable_definition_add_mapping(range_var, nullptr, nullptr, p.c_str(),
                                         nullptr);
  }
  EXPECT_EQ(18, range_var->num_mappings);

  // Valid range setters for all numeric types.
  harp_variable_definition* v_i8 = harp_ingestion_register_variable_full_read(
      prod, "v_i8", harp_type_int8, 1, time_dim, nullptr, "i8", "1", nullptr,
      DummyReadAll);
  harp_variable_definition* v_i16 = harp_ingestion_register_variable_full_read(
      prod, "v_i16", harp_type_int16, 1, time_dim, nullptr, "i16", "1", nullptr,
      DummyReadAll);
  harp_variable_definition* v_i32 = harp_ingestion_register_variable_full_read(
      prod, "v_i32", harp_type_int32, 1, time_dim, nullptr, "i32", "1", nullptr,
      DummyReadAll);
  harp_variable_definition_set_valid_range_int8(v_i8, -5, 5);
  EXPECT_EQ(-5, v_i8->valid_min.int8_data);
  EXPECT_EQ(5, v_i8->valid_max.int8_data);
  harp_variable_definition_set_valid_range_int16(v_i16, -100, 100);
  EXPECT_EQ(-100, v_i16->valid_min.int16_data);
  EXPECT_EQ(100, v_i16->valid_max.int16_data);
  harp_variable_definition_set_valid_range_int32(v_i32, -1000, 1000);
  EXPECT_EQ(-1000, v_i32->valid_min.int32_data);
  EXPECT_EQ(1000, v_i32->valid_max.int32_data);
  harp_variable_definition_set_valid_range_float(scalar_var, -1.5f, 1.5f);
  EXPECT_FLOAT_EQ(-1.5f, scalar_var->valid_min.float_data);
  EXPECT_FLOAT_EQ(1.5f, scalar_var->valid_max.float_data);
  harp_variable_definition_set_valid_range_double(range_var, 0.0, 100.0);
  EXPECT_DOUBLE_EQ(0.0, range_var->valid_min.double_data);
  EXPECT_DOUBLE_EQ(100.0, range_var->valid_max.double_data);

  // Enumeration values for int8, int16, int32.
  const char* kEnums[] = {"clear", "cloudy"};
  harp_variable_definition_set_enumeration_values(v_i8, 2, kEnums);
  harp_variable_definition_set_enumeration_values(v_i16, 2, kEnums);
  harp_variable_definition_set_enumeration_values(v_i32, 2, kEnums);
  EXPECT_EQ(2, v_i8->num_enum_values);
  EXPECT_EQ(0, v_i8->valid_min.int8_data);
  EXPECT_EQ(1, v_i8->valid_max.int8_data);
  EXPECT_EQ(1, v_i16->valid_max.int16_data);
  EXPECT_EQ(1, v_i32->valid_max.int32_data);

  // Add remaining variables to exceed BLOCK_SIZE (16).
  for (int i = 0; i < 14; ++i) {
    const std::string vname = "extra_var_" + std::to_string(i);
    ASSERT_NE(nullptr, harp_ingestion_register_variable_full_read(
                           prod, vname.c_str(), harp_type_double, 1, time_dim,
                           nullptr, nullptr, nullptr, nullptr, DummyReadAll));
  }
  EXPECT_EQ(20, prod->num_variable_definitions);

  // Product definition lookup helpers.
  EXPECT_EQ(
      1, harp_product_definition_has_dimension_type(prod, harp_dimension_time));
  EXPECT_EQ(0, harp_product_definition_has_dimension_type(
                   prod, harp_dimension_spectral));
  EXPECT_EQ(1, harp_product_definition_has_variable(prod, "range_d"));
  EXPECT_EQ(0, harp_product_definition_has_variable(prod, "nonexistent"));
  EXPECT_EQ(range_var, harp_product_definition_find_variable(prod, "range_d"));
  EXPECT_EQ(nullptr,
            harp_product_definition_find_variable(prod, "nonexistent"));
  EXPECT_EQ(1, harp_product_definition_get_variable_index(prod, "range_d"));
  EXPECT_EQ(-1,
            harp_product_definition_get_variable_index(prod, "nonexistent"));
}

TEST(HarpIngestionModuleTest, ValidateOptionsSuccessAndErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_init());

  harp_ingestion_module* mod = harp_ingestion_register_module(
      "test_val_mod", "TEST_GROUP", "TEST_CLS", "test_val_type", "desc",
      DummyIngestionInit, DummyIngestionDone);
  ASSERT_NE(nullptr, mod);
  const char* kAllowed[] = {"fast", "accurate"};
  ASSERT_NE(nullptr, harp_ingestion_register_option(mod, "quality", "quality",
                                                    2, kAllowed));

  harp_ingestion_options* raw_opts = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
  ScopedHarpIngestionOptions opts(raw_opts);

  // 1. NULL module or NULL options.
  EXPECT_EQ(-1, harp_ingestion_module_validate_options(nullptr, opts.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_ingestion_module_validate_options(mod, nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // 2. Unknown option name.
  ASSERT_EQ(HARP_SUCCESS,
            harp_ingestion_options_set_option(opts.get(), "unknown", "x"));
  EXPECT_EQ(-1, harp_ingestion_module_validate_options(mod, opts.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_INGESTION_OPTION, harp_errno);
  ASSERT_EQ(HARP_SUCCESS,
            harp_ingestion_options_remove_option(opts.get(), "unknown"));

  // 3. Invalid option value.
  ASSERT_EQ(HARP_SUCCESS,
            harp_ingestion_options_set_option(opts.get(), "quality", "bad"));
  EXPECT_EQ(-1, harp_ingestion_module_validate_options(mod, opts.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_INGESTION_OPTION_VALUE, harp_errno);

  // 4. Valid option value.
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_set_option(
                              opts.get(), "quality", "accurate"));
  EXPECT_EQ(HARP_SUCCESS,
            harp_ingestion_module_validate_options(mod, opts.get()));
}

TEST(HarpIngestionModuleTest, FindModuleErrorsAndCodadefMatch) {
  const TempDir dir("harp_ingestion_module_find");
  const TempFile missing("harp_ingestion_module_find/missing.nc");
  const TempFile invalid("harp_ingestion_module_find/invalid.txt");
  const TempFile nc_file("harp_ingestion_module_find/sample.nc");
  const TempFile codadef("harp_ingestion_module_find/test.codadef");

  ASSERT_TRUE(WriteFileText(invalid.path(), "not a supported product\n"));
  ASSERT_TRUE(WriteMinimalNetcdf3(nc_file.path()));

  auto reset_coda =
      absl::MakeCleanup([] { harp_set_coda_definition_path(nullptr); });
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_init());

  // Also register a module with NULL product_class/type to cover the skip
  // branch inside harp_ingestion_find_module.
  ASSERT_NE(nullptr, harp_ingestion_register_module(
                         "test_no_cls", "TEST_GROUP", nullptr, nullptr, nullptr,
                         nullptr, DummyIngestionDone));

  harp_ingestion_module* mod = nullptr;
  coda_product* cprod = nullptr;

  // 1. Missing file -> HARP_ERROR_CODA.
  EXPECT_EQ(-1, harp_ingestion_find_module(missing.c_str(), &mod, &cprod));
  EXPECT_EQ(HARP_ERROR_CODA, harp_errno);

  // 1b. Directory path -> triggers CODA_ERROR_FILE_OPEN fallback retry in
  // harp_ingestion_find_module.
  EXPECT_EQ(-1, harp_ingestion_find_module(dir.c_str(), &mod, &cprod));
  EXPECT_EQ(HARP_ERROR_CODA, harp_errno);

  // 2. Unsupported raw text file -> HARP_ERROR_UNSUPPORTED_PRODUCT.
  EXPECT_EQ(-1, harp_ingestion_find_module(invalid.c_str(), &mod, &cprod));
  EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, harp_errno);

  // 3. NetCDF-3 file without a matching .codadef (NULL class/type).
  EXPECT_EQ(-1, harp_ingestion_find_module(nc_file.c_str(), &mod, &cprod));
  EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, harp_errno);

  // 4. NetCDF-3 file with a .codadef matching an unknown class/type.
  harp_ingestion_done();
  constexpr std::string_view kUnknownIndexXml =
      R"(<?xml version="1.0" encoding="UTF-8"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="UNKNOWN_CLS">
  <cd:ProductType name="unknown_type">
    <cd:ProductDefinition id="UNKNOWN_DEF" format="netcdf" version="1">
      <cd:DetectionRule>
        <cd:MatchExpression>exists(/@Ozone_field_date)</cd:MatchExpression>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)";
  constexpr std::string_view kUnknownProdXml =
      R"(<?xml version="1.0" encoding="UTF-8"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="UNKNOWN_DEF" format="netcdf"/>
)";
  const ZipEntrySpec unknown_entries[] = {
      {.name = "VERSION", .data = "20260101"},
      {.name = "index.xml", .data = kUnknownIndexXml},
      {.name = "products/UNKNOWN_DEF.xml", .data = kUnknownProdXml},
  };
  ASSERT_TRUE(WriteStoredZipCodadef(codadef.path(), unknown_entries));
  ASSERT_EQ(HARP_SUCCESS, harp_set_coda_definition_path(dir.c_str()));
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_init());

  EXPECT_EQ(-1, harp_ingestion_find_module(nc_file.c_str(), &mod, &cprod));
  EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, harp_errno);
  EXPECT_THAT(harp_errno_to_string(harp_errno),
              HasSubstr("UNKNOWN_CLS/unknown_type"));

  // 5. NetCDF-3 file with a .codadef matching TEMIS/o3field succeeds.
  harp_ingestion_done();
  constexpr std::string_view kTemisIndexXml =
      R"(<?xml version="1.0" encoding="UTF-8"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="TEMIS">
  <cd:ProductType name="o3field">
    <cd:ProductDefinition id="TEMIS_o3field" format="netcdf" version="1">
      <cd:DetectionRule>
        <cd:MatchExpression>exists(/@Ozone_field_date)</cd:MatchExpression>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)";
  constexpr std::string_view kTemisProdXml =
      R"(<?xml version="1.0" encoding="UTF-8"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="TEMIS_o3field" format="netcdf"/>
)";
  const ZipEntrySpec temis_entries[] = {
      {.name = "VERSION", .data = "20260101"},
      {.name = "index.xml", .data = kTemisIndexXml},
      {.name = "products/TEMIS_o3field.xml", .data = kTemisProdXml},
  };
  ASSERT_TRUE(WriteStoredZipCodadef(codadef.path(), temis_entries));
  ASSERT_EQ(HARP_SUCCESS, harp_set_coda_definition_path(dir.c_str()));
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_init());

  ASSERT_EQ(HARP_SUCCESS,
            harp_ingestion_find_module(nc_file.c_str(), &mod, &cprod));
  auto close_cprod = absl::MakeCleanup([&] {
    if (cprod != nullptr) {
      coda_close(cprod);
      cprod = nullptr;
    }
  });
  // 6. NetCDF-3 file with a .codadef matching a custom module registered after
  // `test_no_cls` and a same-class/different-type module (covers NULL
  // product_class/type skip and product_type mismatch continue).
  harp_ingestion_done();
  constexpr std::string_view kCustomIndexXml =
      R"(<?xml version="1.0" encoding="UTF-8"?>
<cd:ProductClass xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" name="CUSTOM_CLS">
  <cd:ProductType name="match_type">
    <cd:ProductDefinition id="CUSTOM_DEF" format="netcdf" version="1">
      <cd:DetectionRule>
        <cd:MatchExpression>exists(/@Ozone_field_date)</cd:MatchExpression>
      </cd:DetectionRule>
    </cd:ProductDefinition>
  </cd:ProductType>
</cd:ProductClass>
)";
  constexpr std::string_view kCustomProdXml =
      R"(<?xml version="1.0" encoding="UTF-8"?>
<cd:ProductDefinition xmlns:cd="http://www.stcorp.nl/coda/definition/2008/07" id="CUSTOM_DEF" format="netcdf"/>
)";
  const ZipEntrySpec custom_entries[] = {
      {.name = "VERSION", .data = "20260101"},
      {.name = "index.xml", .data = kCustomIndexXml},
      {.name = "products/CUSTOM_DEF.xml", .data = kCustomProdXml},
  };
  ASSERT_TRUE(WriteStoredZipCodadef(codadef.path(), custom_entries));
  ASSERT_EQ(HARP_SUCCESS, harp_set_coda_definition_path(dir.c_str()));
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_init());
  ASSERT_NE(nullptr, harp_ingestion_register_module(
                         "test_no_cls_2", "TEST_GROUP", nullptr, nullptr,
                         nullptr, nullptr, DummyIngestionDone));
  ASSERT_NE(nullptr,
            harp_ingestion_register_module(
                "test_same_cls_other_type", "TEST_GROUP", "CUSTOM_CLS",
                "other_type", "other", DummyIngestionInit, DummyIngestionDone));
  ASSERT_NE(nullptr,
            harp_ingestion_register_module(
                "test_custom_match", "TEST_GROUP", "CUSTOM_CLS", "match_type",
                "match", DummyIngestionInit, DummyIngestionDone));

  coda_product* custom_cprod = nullptr;
  harp_ingestion_module* custom_mod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_find_module(
                              nc_file.c_str(), &custom_mod, &custom_cprod));
  auto close_custom_cprod = absl::MakeCleanup([&] {
    if (custom_cprod != nullptr) {
      coda_close(custom_cprod);
      custom_cprod = nullptr;
    }
  });
  ASSERT_NE(nullptr, custom_mod);
  EXPECT_STREQ("test_custom_match", custom_mod->name);
}

}  // namespace
}  // namespace stcorp_harp
