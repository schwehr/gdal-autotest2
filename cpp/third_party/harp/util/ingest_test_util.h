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
// Helpers for exercising libharp ingestion modules against in-memory
// coda_product instances.

#ifndef THIRD_PARTY_STCORP_HARP_GOOGLE_UTIL_INGEST_TEST_UTIL_H_
#define THIRD_PARTY_STCORP_HARP_GOOGLE_UTIL_INGEST_TEST_UTIL_H_

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/libcoda/coda.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {

// Finds a registered ingestion module by name after ensuring harp_ingestion
// is initialized. Caller must keep a ScopedHarpInit alive.
inline const harp_ingestion_module* FindIngestionModule(
    std::string_view module_name) {
  if (harp_ingestion_init() != 0) {
    return nullptr;
  }
  harp_ingestion_module_register* reg = harp_ingestion_get_module_register();
  if (reg == nullptr) {
    return nullptr;
  }
  for (int i = 0; i < reg->num_ingestion_modules; ++i) {
    if (reg->ingestion_module[i]->name != nullptr &&
        module_name == reg->ingestion_module[i]->name) {
      return reg->ingestion_module[i];
    }
  }
  return nullptr;
}

// Holds typed values read from a single ingested HARP variable.
struct IngestedVariable {
  harp_data_type data_type = harp_type_double;
  long num_elements = 0;
  bool included = true;
  int read_status = HARP_SUCCESS;
  std::vector<int8_t> int8_values;
  std::vector<int16_t> int16_values;
  std::vector<int32_t> int32_values;
  std::vector<float> float_values;
  std::vector<double> double_values;
  std::vector<std::string> string_values;
};

struct IngestExerciseResult {
  int status = -1;
  int failed_errno = HARP_SUCCESS;
  std::string failed_variable;
  long dimensions[HARP_NUM_DIM_TYPES] = {0};
  bool has_datetime_range = false;
  double datetime_start = 0.0;
  double datetime_stop = 0.0;
  std::map<std::string, IngestedVariable> variables;
};

// Initializes `mod` with `cprod` and optional semicolon-separated
// `options_str`, validates options, reads dimensions/datetime_range, and
// invokes every included variable's read_all / read_range / read_block reader.
// Stops on the first failing variable if `stop_on_error` is true, or collects
// status per variable when `stop_on_error` is false.
inline IngestExerciseResult ExerciseIngestionModule(
    const harp_ingestion_module* mod, coda_product* cprod,
    const char* options_str = nullptr, bool stop_on_error = true) {
  IngestExerciseResult result;
  if (mod == nullptr || cprod == nullptr) {
    return result;
  }

  ScopedHarpIngestionOptions opts;
  if (options_str != nullptr) {
    harp_ingestion_options* raw_opts = nullptr;
    if (harp_ingestion_options_from_string(options_str, &raw_opts) != 0) {
      result.failed_errno = harp_errno;
      return result;
    }
    opts.reset(raw_opts);
  } else {
    harp_ingestion_options* raw_opts = nullptr;
    if (harp_ingestion_options_new(&raw_opts) != 0) {
      result.failed_errno = harp_errno;
      return result;
    }
    opts.reset(raw_opts);
  }

  if (harp_ingestion_module_validate_options(
          const_cast<harp_ingestion_module*>(mod), opts.get()) != 0) {
    result.failed_errno = harp_errno;
    return result;
  }

  harp_product_definition* prod_def = nullptr;
  void* user_data = nullptr;
  if (mod->ingestion_init(mod, cprod, opts.get(), &prod_def, &user_data) != 0) {
    result.failed_errno = harp_errno;
    return result;
  }
  auto done_guard = absl::MakeCleanup([&] {
    if (mod->ingestion_done != nullptr && user_data != nullptr) {
      mod->ingestion_done(user_data);
    }
  });

  if (prod_def == nullptr || prod_def->read_dimensions == nullptr) {
    return result;
  }
  if (prod_def->read_dimensions(user_data, result.dimensions) != 0) {
    result.failed_errno = harp_errno;
    return result;
  }
  if (prod_def->read_datetime_range != nullptr) {
    result.has_datetime_range = true;
    if (prod_def->read_datetime_range(user_data, &result.datetime_start,
                                      &result.datetime_stop) != 0) {
      result.failed_errno = harp_errno;
      return result;
    }
  }

  result.status = HARP_SUCCESS;
  for (int i = 0; i < prod_def->num_variable_definitions; ++i) {
    const harp_variable_definition* var_def = prod_def->variable_definition[i];
    IngestedVariable var;
    var.data_type = var_def->data_type;
    if (var_def->include != nullptr && var_def->include(user_data) == 0) {
      var.included = false;
      result.variables.emplace(var_def->name, std::move(var));
      continue;
    }

    long num_elements = 1;
    long dim0 = 1;
    long stride = 1;
    for (int d = 0; d < var_def->num_dimensions; ++d) {
      const long len = var_def->dimension_type[d] == harp_dimension_independent
                           ? var_def->dimension[d]
                           : result.dimensions[var_def->dimension_type[d]];
      if (d == 0) {
        dim0 = len;
      } else {
        stride *= len;
      }
      num_elements *= len;
    }
    var.num_elements = num_elements;

    if (num_elements == 0) {
      result.variables.emplace(var_def->name, std::move(var));
      continue;
    }

    const size_t elem_size =
        static_cast<size_t>(harp_get_size_for_type(var_def->data_type));
    std::vector<uint8_t> raw(static_cast<size_t>(num_elements) * elem_size, 0);
    auto free_strings = absl::MakeCleanup([&] {
      if (var_def->data_type == harp_type_string) {
        char** strs = reinterpret_cast<char**>(raw.data());
        for (long k = 0; k < num_elements; ++k) {
          std::free(strs[k]);
          strs[k] = nullptr;
        }
      }
    });

    harp_array data{};
    data.ptr = raw.data();
    int rc = 0;
    if (var_def->read_all != nullptr) {
      rc = var_def->read_all(user_data, data);
    } else if (var_def->read_range != nullptr) {
      long range_len = dim0;
      if (var_def->get_optimal_range_length != nullptr) {
        long opt = var_def->get_optimal_range_length(user_data);
        if (opt > 0 && opt < range_len) {
          range_len = opt;
        }
      }
      for (long offset = 0; offset < dim0 && rc == 0; offset += range_len) {
        const long chunk =
            (offset + range_len <= dim0) ? range_len : (dim0 - offset);
        harp_array slice{};
        slice.ptr =
            raw.data() + static_cast<size_t>(offset * stride) * elem_size;
        rc = var_def->read_range(user_data, offset, chunk, slice);
      }
    } else if (var_def->read_block != nullptr) {
      for (long blk = 0; blk < dim0 && rc == 0; ++blk) {
        harp_array slice{};
        slice.ptr = raw.data() + static_cast<size_t>(blk * stride) * elem_size;
        rc = var_def->read_block(user_data, blk, slice);
      }
    }

    var.read_status = rc;
    if (rc != 0) {
      if (result.status == HARP_SUCCESS) {
        result.status = rc;
        result.failed_errno = harp_errno;
        result.failed_variable = var_def->name;
      }
      result.variables.emplace(var_def->name, std::move(var));
      if (stop_on_error) {
        return result;
      }
      continue;
    }

    switch (var_def->data_type) {
      case harp_type_int8:
        var.int8_values.assign(
            reinterpret_cast<const int8_t*>(raw.data()),
            reinterpret_cast<const int8_t*>(raw.data()) + num_elements);
        break;
      case harp_type_int16:
        var.int16_values.assign(
            reinterpret_cast<const int16_t*>(raw.data()),
            reinterpret_cast<const int16_t*>(raw.data()) + num_elements);
        break;
      case harp_type_int32:
        var.int32_values.assign(
            reinterpret_cast<const int32_t*>(raw.data()),
            reinterpret_cast<const int32_t*>(raw.data()) + num_elements);
        break;
      case harp_type_float:
        var.float_values.assign(
            reinterpret_cast<const float*>(raw.data()),
            reinterpret_cast<const float*>(raw.data()) + num_elements);
        break;
      case harp_type_double:
        var.double_values.assign(
            reinterpret_cast<const double*>(raw.data()),
            reinterpret_cast<const double*>(raw.data()) + num_elements);
        break;
      case harp_type_string: {
        char** strs = reinterpret_cast<char**>(raw.data());
        var.string_values.reserve(static_cast<size_t>(num_elements));
        for (long k = 0; k < num_elements; ++k) {
          var.string_values.emplace_back(strs[k] != nullptr ? strs[k] : "");
        }
        break;
      }
    }
    result.variables.emplace(var_def->name, std::move(var));
  }
  return result;
}

}  // namespace stcorp_harp

#endif  // THIRD_PARTY_STCORP_HARP_GOOGLE_UTIL_INGEST_TEST_UTIL_H_
