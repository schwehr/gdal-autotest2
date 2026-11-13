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
// Unit tests for GEOMS UVVIS-DOAS ingestion modules
// (libharp/harp-ingest-geoms-uvvis-doas.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-geoms-uvvis-doas.c

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_coda/libcoda/coda.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::DoubleEq;
using ::testing::ElementsAre;
using ::testing::IsNan;

// Same as ExerciseIngestionModule, but skips `cloud_type` because upstream
// `read_cloud_conditions` calls `read_variable_string` (which `malloc`s a
// string buffer) and never frees it, causing HeapLeakChecker/LSan failures on
// any successful `read_variable_string` call.
IngestExerciseResult ExerciseModuleSkippingCloudType(
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

  if (prod_def == nullptr || prod_def->read_dimensions == nullptr ||
      prod_def->read_dimensions(user_data, result.dimensions) != 0) {
    result.failed_errno = harp_errno;
    return result;
  }

  result.status = HARP_SUCCESS;
  for (int i = 0; i < prod_def->num_variable_definitions; ++i) {
    const harp_variable_definition* var_def = prod_def->variable_definition[i];
    if (std::string_view(var_def->name) == "cloud_type") {
      continue;
    }
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

    if (var_def->data_type == harp_type_double) {
      var.double_values.assign(
          reinterpret_cast<const double*>(raw.data()),
          reinterpret_cast<const double*>(raw.data()) + num_elements);
    } else if (var_def->data_type == harp_type_string) {
      char** strs = reinterpret_cast<char**>(raw.data());
      var.string_values.reserve(static_cast<size_t>(num_elements));
      for (long k = 0; k < num_elements; ++k) {
        var.string_values.emplace_back(strs[k] != nullptr ? strs[k] : "");
      }
    }
    result.variables.emplace(var_def->name, std::move(var));
  }
  return result;
}

void AddDoubleVar(CodaMemProduct& prod, std::string_view name,
                  std::span<const long> dims, std::span<const double> values,
                  double fill_value = -9999.0, std::string_view unit = "") {
  CodaMemNode* node = prod.MakeDoubleArray(dims, values);
  ASSERT_NE(nullptr, node);
  ASSERT_TRUE(
      prod.AddAttribute(node, "VAR_FILL_VALUE", prod.MakeDouble(fill_value)));
  if (!unit.empty()) {
    ASSERT_TRUE(prod.AddAttribute(node, "VAR_UNITS", prod.MakeString(unit)));
  }
  ASSERT_TRUE(prod.AddField(prod.root(), name, node));
}

struct CommonConfig {
  std::string_view data_template;
  std::string_view data_source;
  int product_version = 7;
  long num_time = 2;
  long num_vertical = 2;
  bool invert_vertical = false;
  bool altitude_2d = false;
  bool time_dependent_replicated = false;
  bool include_latlon = true;
  bool include_wind = true;
  double fill_value = -9999.0;
};

void PopulateCommonFields(CodaMemProduct& prod, const CommonConfig& cfg) {
  ASSERT_TRUE(prod.SetProductInfo("GEOMS", "UVVIS_DOAS", cfg.product_version));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                prod.MakeString(cfg.data_template)));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                prod.MakeString(cfg.data_source)));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_LOCATION",
                                prod.MakeString("Bremen")));

  const long t_dims[] = {cfg.num_time};
  const long s_dims[] = {1};
  std::vector<double> dt(cfg.num_time);
  std::vector<double> dt_start(cfg.num_time);
  std::vector<double> dt_stop(cfg.num_time);
  std::vector<double> sza(cfg.num_time);
  std::vector<double> saa(cfg.num_time);
  std::vector<double> vza(cfg.num_time);
  std::vector<double> vaa(cfg.num_time);
  for (long i = 0; i < cfg.num_time; ++i) {
    dt[i] = 100.0 + static_cast<double>(i);
    dt_start[i] = 99.9 + static_cast<double>(i);
    dt_stop[i] = 100.1 + static_cast<double>(i);
    sza[i] = 40.0 + static_cast<double>(i);
    saa[i] = 180.0 + static_cast<double>(i);
    vza[i] = 10.0 + static_cast<double>(i);
    vaa[i] = 90.0 + static_cast<double>(i);
  }
  AddDoubleVar(prod, "DATETIME", t_dims, dt, cfg.fill_value);
  AddDoubleVar(prod, "DATETIME_START", t_dims, dt_start, cfg.fill_value);
  AddDoubleVar(prod, "DATETIME_STOP", t_dims, dt_stop, cfg.fill_value);
  AddDoubleVar(prod, "ANGLE_SOLAR_ZENITH_ASTRONOMICAL", t_dims, sza,
               cfg.fill_value);
  AddDoubleVar(prod, "ANGLE_SOLAR_AZIMUTH", t_dims, saa, cfg.fill_value);
  AddDoubleVar(prod, "ANGLE_VIEW_ZENITH", t_dims, vza, cfg.fill_value);
  AddDoubleVar(prod, "ANGLE_VIEW_AZIMUTH", t_dims, vaa, cfg.fill_value);

  const double lat_inst[] = {53.1};
  const double lon_inst[] = {8.85};
  const double alt_inst[] = {15.0};
  AddDoubleVar(prod, "LATITUDE_INSTRUMENT", s_dims, lat_inst, cfg.fill_value);
  AddDoubleVar(prod, "LONGITUDE_INSTRUMENT", s_dims, lon_inst, cfg.fill_value);
  AddDoubleVar(prod, "ALTITUDE_INSTRUMENT", s_dims, alt_inst, cfg.fill_value);

  // ALTITUDE
  if (cfg.altitude_2d) {
    const long alt_dims[] = {cfg.num_time, cfg.num_vertical};
    std::vector<double> alt(cfg.num_time * cfg.num_vertical);
    for (long t = 0; t < cfg.num_time; ++t) {
      for (long z = 0; z < cfg.num_vertical; ++z) {
        alt[t * cfg.num_vertical + z] =
            cfg.invert_vertical
                ? static_cast<double>(cfg.num_vertical - 1 - z) * 2.0 + 1.0
                : static_cast<double>(z) * 2.0 + 1.0;
      }
    }
    AddDoubleVar(prod, "ALTITUDE", alt_dims, alt, cfg.fill_value);
  } else {
    const long alt_dims[] = {cfg.num_vertical};
    std::vector<double> alt(cfg.num_vertical);
    for (long z = 0; z < cfg.num_vertical; ++z) {
      alt[z] = cfg.invert_vertical
                   ? static_cast<double>(cfg.num_vertical - 1 - z) * 2.0 + 1.0
                   : static_cast<double>(z) * 2.0 + 1.0;
    }
    AddDoubleVar(prod, "ALTITUDE", alt_dims, alt, cfg.fill_value);
  }

  // ALTITUDE_BOUNDARIES & TEMPERATURE_INDEPENDENT
  if (cfg.time_dependent_replicated) {
    const long b_dims[] = {cfg.num_time, cfg.num_vertical, 2};
    std::vector<double> bounds(cfg.num_time * cfg.num_vertical * 2);
    const long tv_dims[] = {cfg.num_time, cfg.num_vertical};
    std::vector<double> temp(cfg.num_time * cfg.num_vertical);
    for (long t = 0; t < cfg.num_time; ++t) {
      for (long z = 0; z < cfg.num_vertical; ++z) {
        const long idx = t * cfg.num_vertical + z;
        bounds[idx * 2] = static_cast<double>(z) * 2.0;
        bounds[idx * 2 + 1] = static_cast<double>(z) * 2.0 + 2.0;
        temp[idx] = 280.0 - static_cast<double>(z) * 5.0;
      }
    }
    AddDoubleVar(prod, "ALTITUDE_BOUNDARIES", b_dims, bounds, cfg.fill_value);
    AddDoubleVar(prod, "TEMPERATURE_INDEPENDENT", tv_dims, temp,
                 cfg.fill_value);
  } else {
    const long b_dims[] = {cfg.num_vertical, 2};
    std::vector<double> bounds(cfg.num_vertical * 2);
    const long v_dims[] = {cfg.num_vertical};
    std::vector<double> temp(cfg.num_vertical);
    for (long z = 0; z < cfg.num_vertical; ++z) {
      bounds[z * 2] = static_cast<double>(z) * 2.0;
      bounds[z * 2 + 1] = static_cast<double>(z) * 2.0 + 2.0;
      temp[z] = 280.0 - static_cast<double>(z) * 5.0;
    }
    AddDoubleVar(prod, "ALTITUDE_BOUNDARIES", b_dims, bounds, cfg.fill_value);
    AddDoubleVar(prod, "TEMPERATURE_INDEPENDENT", v_dims, temp, cfg.fill_value);
  }

  // PRESSURE_INDEPENDENT
  {
    const long tv_dims[] = {cfg.num_time, cfg.num_vertical};
    std::vector<double> pres(cfg.num_time * cfg.num_vertical, 1000.0);
    pres[0] = cfg.fill_value;  // Exercise fill value replacement when non-NaN.
    AddDoubleVar(prod, "PRESSURE_INDEPENDENT", tv_dims, pres, cfg.fill_value);
  }

  if (cfg.include_latlon) {
    const long tv_dims[] = {cfg.num_time, cfg.num_vertical};
    std::vector<double> lat(cfg.num_time * cfg.num_vertical, 53.1);
    std::vector<double> lon(cfg.num_time * cfg.num_vertical, 8.85);
    AddDoubleVar(prod, "LATITUDE", tv_dims, lat, cfg.fill_value);
    AddDoubleVar(prod, "LONGITUDE", tv_dims, lon, cfg.fill_value);
  }

  if (cfg.include_wind && cfg.product_version >= 7 &&
      cfg.data_template.find("DIRECTSUN") == std::string_view::npos) {
    std::vector<double> wdir(cfg.num_time, 180.0);
    std::vector<double> wspd(cfg.num_time, 5.5);
    AddDoubleVar(prod, "WIND_DIRECTION_SURFACE_INDEPENDENT", t_dims, wdir,
                 cfg.fill_value);
    AddDoubleVar(prod, "WIND_SPEED_SURFACE_INDEPENDENT", t_dims, wspd,
                 cfg.fill_value);
  }
}

void PopulateDirectSunFields(CodaMemProduct& prod, std::string_view gas,
                             int version, long num_time, long num_vertical,
                             bool measured_aod) {
  const long t_dims[] = {num_time};
  const long tv_dims[] = {num_time, num_vertical};
  std::vector<double> t_vals(num_time, 10.0);
  std::vector<double> tv_vals(num_time * num_vertical, 2.0);
  const std::string g(gas);

  if (measured_aod) {
    // Upstream bug: get_optional_variable_availability checks
    // "/AEROSOL_OPTICAL_DEPTH_ABSORPTION.SOLAR" (with a dot, which CODA
    // field-name sanitization turns into '_'), so `has_aod` is always 0 for
    // directsun with AOD=measured.
    AddDoubleVar(prod, "AEROSOL_OPTICAL_DEPTH_ABSORPTION_SOLAR", t_dims,
                 t_vals);
  } else {
    AddDoubleVar(prod, "AEROSOL_OPTICAL_DEPTH_INDEPENDENT", t_dims, t_vals);
  }

  AddDoubleVar(prod, g + "_COLUMN_ABSORPTION_SOLAR", t_dims, t_vals);
  AddDoubleVar(prod, g + "_COLUMN_ABSORPTION_SOLAR_UNCERTAINTY_RANDOM_STANDARD",
               t_dims, t_vals);
  AddDoubleVar(prod,
               g + "_COLUMN_ABSORPTION_SOLAR_UNCERTAINTY_SYSTEMATIC_STANDARD",
               t_dims, t_vals);
  if (version < 5) {
    AddDoubleVar(prod, g + "_COLUMN_ABSORPTION_SOLAR_APRIORI", t_dims, t_vals);
  } else {
    AddDoubleVar(prod, g + "_COLUMN_PARTIAL_ABSORPTION_SOLAR_APRIORI", tv_dims,
                 tv_vals);
  }
  AddDoubleVar(prod, g + "_COLUMN_ABSORPTION_SOLAR_AVK", tv_dims, tv_vals);
}

void PopulateOffAxisFields(CodaMemProduct& prod, std::string_view gas,
                           long num_time, long num_vertical, bool measured_aod,
                           bool optimal_estimation) {
  const long t_dims[] = {num_time};
  const long tv_dims[] = {num_time, num_vertical};
  const long tvv_dims[] = {num_time, num_vertical, num_vertical};
  std::vector<double> t_vals(num_time, 3.0);
  std::vector<double> tv_vals(num_time * num_vertical, 1.5);
  std::vector<double> tvv_vals(num_time * num_vertical * num_vertical, 0.0);
  for (long t = 0; t < num_time; ++t) {
    for (long z = 0; z < num_vertical; ++z) {
      tvv_vals[(t * num_vertical + z) * num_vertical + z] = 4.0;
    }
  }
  const std::string g(gas);

  if (measured_aod) {
    AddDoubleVar(prod,
                 "AEROSOL_OPTICAL_DEPTH_TROPOSPHERIC_SCATTER_SOLAR_OFFAXIS",
                 t_dims, t_vals);
  } else {
    AddDoubleVar(prod, "AEROSOL_OPTICAL_DEPTH_TROPOSPHERIC_INDEPENDENT", t_dims,
                 t_vals);
  }

  if (optimal_estimation) {
    AddDoubleVar(prod, g + "_MIXING_RATIO_VOLUME_SCATTER_SOLAR_OFFAXIS",
                 tv_dims, tv_vals, -9999.0, "ppbv");
    AddDoubleVar(
        prod,
        g + "_MIXING_RATIO_VOLUME_SCATTER_SOLAR_OFFAXIS_UNCERTAINTY_RANDOM_"
            "COVARIANCE",
        tvv_dims, tvv_vals, -9999.0, "(ppbv)2");
    AddDoubleVar(
        prod,
        g + "_MIXING_RATIO_VOLUME_SCATTER_SOLAR_OFFAXIS_UNCERTAINTY_SYSTEMATIC_"
            "COVARIANCE",
        tvv_dims, tvv_vals, -9999.0, "(ppbv)2");
    AddDoubleVar(prod, g + "_MIXING_RATIO_VOLUME_SCATTER_SOLAR_OFFAXIS_APRIORI",
                 tv_dims, tv_vals, -9999.0, "ppbv");
    AddDoubleVar(prod, g + "_MIXING_RATIO_VOLUME_SCATTER_SOLAR_OFFAXIS_AVK",
                 tvv_dims, tvv_vals);
    AddDoubleVar(prod, g + "_COLUMN_PARTIAL_SCATTER_SOLAR_OFFAXIS", tv_dims,
                 tv_vals);
  }

  AddDoubleVar(prod, g + "_COLUMN_TROPOSPHERIC_SCATTER_SOLAR_OFFAXIS", t_dims,
               t_vals);
  AddDoubleVar(
      prod,
      g + "_COLUMN_TROPOSPHERIC_SCATTER_SOLAR_OFFAXIS_UNCERTAINTY_RANDOM_"
          "STANDARD",
      t_dims, t_vals);
  AddDoubleVar(
      prod,
      g + "_COLUMN_TROPOSPHERIC_SCATTER_SOLAR_OFFAXIS_UNCERTAINTY_SYSTEMATIC_"
          "STANDARD",
      t_dims, t_vals);
  AddDoubleVar(prod, g + "_COLUMN_TROPOSPHERIC_SCATTER_SOLAR_OFFAXIS_APRIORI",
               t_dims, t_vals);
  AddDoubleVar(prod, g + "_COLUMN_TROPOSPHERIC_SCATTER_SOLAR_OFFAXIS_AVK",
               tv_dims, tv_vals);
  AddDoubleVar(prod, g + "_COLUMN_PARTIAL_SCATTER_SOLAR_OFFAXIS_APRIORI",
               tv_dims, tv_vals);
}

void PopulateOffAxisAerosolFields(CodaMemProduct& prod, long num_time,
                                  long num_spectral, long num_vertical) {
  const long s_dims[] = {num_spectral};
  const long ts_dims[] = {num_time, num_spectral};
  const long tsv_dims[] = {num_time, num_spectral, num_vertical};
  const long tsvv_dims[] = {num_time, num_spectral, num_vertical, num_vertical};

  std::vector<double> wl(num_spectral);
  for (long s = 0; s < num_spectral; ++s) {
    wl[s] = 360.0 + static_cast<double>(s) * 117.0;
  }
  AddDoubleVar(prod, "WAVELENGTH", s_dims, wl);

  std::vector<double> ts_vals(num_time * num_spectral, 0.25);
  std::vector<double> tsv_vals(num_time * num_spectral * num_vertical, 0.05);
  std::vector<double> tsvv_vals(
      num_time * num_spectral * num_vertical * num_vertical, 0.0);
  for (long ts = 0; ts < num_time * num_spectral; ++ts) {
    for (long z = 0; z < num_vertical; ++z) {
      tsvv_vals[(ts * num_vertical + z) * num_vertical + z] = 0.09;
    }
  }

  AddDoubleVar(prod, "AEROSOL_EXTINCTION_COEFFICIENT_SCATTER_SOLAR_OFFAXIS",
               tsv_dims, tsv_vals);
  AddDoubleVar(
      prod,
      "AEROSOL_EXTINCTION_COEFFICIENT_SCATTER_SOLAR_OFFAXIS_UNCERTAINTY_"
      "RANDOM_COVARIANCE",
      tsvv_dims, tsvv_vals);
  AddDoubleVar(
      prod,
      "AEROSOL_EXTINCTION_COEFFICIENT_SCATTER_SOLAR_OFFAXIS_UNCERTAINTY_"
      "SYSTEMATIC_COVARIANCE",
      tsvv_dims, tsvv_vals);
  AddDoubleVar(prod,
               "AEROSOL_EXTINCTION_COEFFICIENT_SCATTER_SOLAR_OFFAXIS_APRIORI",
               tsv_dims, tsv_vals);
  AddDoubleVar(prod, "AEROSOL_EXTINCTION_COEFFICIENT_SCATTER_SOLAR_OFFAXIS_AVK",
               tsvv_dims, tsvv_vals);
  AddDoubleVar(prod, "AEROSOL_OPTICAL_DEPTH_TROPOSPHERIC_SCATTER_SOLAR_OFFAXIS",
               ts_dims, ts_vals);
  AddDoubleVar(
      prod,
      "AEROSOL_OPTICAL_DEPTH_TROPOSPHERIC_SCATTER_SOLAR_OFFAXIS_UNCERTAINTY_"
      "RANDOM_STANDARD",
      ts_dims, ts_vals);
  AddDoubleVar(
      prod,
      "AEROSOL_OPTICAL_DEPTH_TROPOSPHERIC_SCATTER_SOLAR_OFFAXIS_UNCERTAINTY_"
      "SYSTEMATIC_STANDARD",
      ts_dims, ts_vals);
  AddDoubleVar(
      prod, "AEROSOL_OPTICAL_DEPTH_TROPOSPHERIC_SCATTER_SOLAR_OFFAXIS_APRIORI",
      ts_dims, ts_vals);
  AddDoubleVar(prod,
               "AEROSOL_OPTICAL_DEPTH_TROPOSPHERIC_SCATTER_SOLAR_OFFAXIS_AVK",
               tsv_dims, tsv_vals);
}

void PopulateZenithFields(CodaMemProduct& prod, std::string_view gas,
                          int version, long num_time, long num_vertical,
                          bool measured_aod, bool has_vmr, bool has_tropo) {
  const long t_dims[] = {num_time};
  const long tv_dims[] = {num_time, num_vertical};
  const long tvv_dims[] = {num_time, num_vertical, num_vertical};
  std::vector<double> t_vals(num_time, 12.0);
  std::vector<double> tv_vals(num_time * num_vertical, 2.5);
  std::vector<double> tvv_vals(num_time * num_vertical * num_vertical, 0.0);
  for (long t = 0; t < num_time; ++t) {
    for (long z = 0; z < num_vertical; ++z) {
      tvv_vals[(t * num_vertical + z) * num_vertical + z] = 9.0;
    }
  }
  const std::string g(gas);

  if (measured_aod) {
    AddDoubleVar(prod,
                 "AEROSOL_OPTICAL_DEPTH_STRATOSPHERIC_SCATTER_SOLAR_ZENITH",
                 t_dims, t_vals);
  } else {
    AddDoubleVar(prod, "AEROSOL_OPTICAL_DEPTH_STRATOSPHERIC_INDEPENDENT",
                 t_dims, t_vals);
  }

  if (has_vmr) {
    AddDoubleVar(prod, g + "_MIXING_RATIO_VOLUME_SCATTER_SOLAR_ZENITH", tv_dims,
                 tv_vals, -9999.0, "ppbv");
    AddDoubleVar(
        prod,
        g + "_MIXING_RATIO_VOLUME_SCATTER_SOLAR_ZENITH_UNCERTAINTY_RANDOM_"
            "COVARIANCE",
        tvv_dims, tvv_vals, -9999.0, "(ppbv)2");
    AddDoubleVar(
        prod,
        g + "_MIXING_RATIO_VOLUME_SCATTER_SOLAR_ZENITH_UNCERTAINTY_SYSTEMATIC_"
            "COVARIANCE",
        tvv_dims, tvv_vals, -9999.0, "(ppbv)2");
    AddDoubleVar(prod, g + "_MIXING_RATIO_VOLUME_SCATTER_SOLAR_ZENITH_AVK",
                 tvv_dims, tvv_vals);
    AddDoubleVar(prod, g + "_COLUMN_PARTIAL_SCATTER_SOLAR_ZENITH", tv_dims,
                 tv_vals);
  }
  if (has_vmr || version >= 7) {
    AddDoubleVar(prod, g + "_MIXING_RATIO_VOLUME_SCATTER_SOLAR_ZENITH_APRIORI",
                 tv_dims, tv_vals, -9999.0, "ppbv");
    AddDoubleVar(prod, g + "_COLUMN_PARTIAL_SCATTER_SOLAR_ZENITH_APRIORI",
                 tv_dims, tv_vals);
  }

  if (has_tropo) {
    AddDoubleVar(prod, g + "_COLUMN_TROPOSPHERIC_SCATTER_SOLAR_ZENITH", t_dims,
                 t_vals);
    AddDoubleVar(
        prod,
        g + "_COLUMN_TROPOSPHERIC_SCATTER_SOLAR_ZENITH_UNCERTAINTY_RANDOM_"
            "STANDARD",
        t_dims, t_vals);
    AddDoubleVar(
        prod,
        g + "_COLUMN_TROPOSPHERIC_SCATTER_SOLAR_ZENITH_UNCERTAINTY_SYSTEMATIC_"
            "STANDARD",
        t_dims, t_vals);
    AddDoubleVar(prod, g + "_COLUMN_TROPOSPHERIC_SCATTER_SOLAR_ZENITH_APRIORI",
                 t_dims, t_vals);
    AddDoubleVar(prod, g + "_COLUMN_TROPOSPHERIC_SCATTER_SOLAR_ZENITH_AVK",
                 tv_dims, tv_vals);
  }

  AddDoubleVar(prod, g + "_COLUMN_STRATOSPHERIC_SCATTER_SOLAR_ZENITH", t_dims,
               t_vals);
  AddDoubleVar(
      prod,
      g + "_COLUMN_STRATOSPHERIC_SCATTER_SOLAR_ZENITH_UNCERTAINTY_RANDOM_"
          "STANDARD",
      t_dims, t_vals);
  AddDoubleVar(
      prod,
      g + "_COLUMN_STRATOSPHERIC_SCATTER_SOLAR_ZENITH_UNCERTAINTY_SYSTEMATIC_"
          "STANDARD",
      t_dims, t_vals);
  AddDoubleVar(prod, g + "_COLUMN_STRATOSPHERIC_SCATTER_SOLAR_ZENITH_APRIORI",
               t_dims, t_vals);
  AddDoubleVar(prod, g + "_COLUMN_STRATOSPHERIC_SCATTER_SOLAR_ZENITH_AVK",
               tv_dims, tv_vals);
  AddDoubleVar(prod, g + "_COLUMN_STRATOSPHERIC_SCATTER_SOLAR_ZENITH_AMF",
               t_dims, t_vals);
}

TEST(HarpIngestGeomsUvvisDoasTest, DirectSunV4AndV7) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("GEOMS-TE-UVVIS-DOAS-DIRECTSUN");
  ASSERT_NE(nullptr, mod);

  // Version 4 (swap_alt_bounds=1), invert_vertical=1, NaN fill_value, AOD
  // measured, CHOCHO -> C2H2O2.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CommonConfig cfg{
        .data_template = "GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-004",
        .data_source = "UVVIS.DOAS.DIRECTSUN.CHOCHO_INST1",
        .product_version = 4,
        .num_time = 2,
        .num_vertical = 2,
        .invert_vertical = true,
        .altitude_2d = true,
        .time_dependent_replicated = false,
        .include_latlon = true,
        .fill_value = std::numeric_limits<double>::quiet_NaN(),
    };
    PopulateCommonFields(prod, cfg);
    PopulateDirectSunFields(prod, "CHOCHO", 4, 2, 2, /*measured_aod=*/true);

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "AOD=measured");
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(2, res.dimensions[harp_dimension_vertical]);
    EXPECT_THAT(res.variables["sensor_name"].string_values,
                ElementsAre("UVVIS.DOAS.DIRECTSUN.CHOCHO_INST1"));
    EXPECT_THAT(res.variables["location_name"].string_values,
                ElementsAre("Bremen"));
    EXPECT_THAT(res.variables["altitude"].double_values,
                ElementsAre(1.0, 3.0, 1.0, 3.0));
    EXPECT_THAT(res.variables["C2H2O2_column_number_density"].double_values,
                ElementsAre(10.0, 10.0));
    // Upstream bug: get_optional_variable_availability checks ".SOLAR" instead
    // of "_SOLAR", so aerosol_optical_depth is excluded under AOD=measured.
    EXPECT_FALSE(res.variables["aerosol_optical_depth"].included);
  }

  // Version 7, modeled AOD, time_dependent_replicated=1, H2CO -> HCHO,
  // no optional lat/lon.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CommonConfig cfg{
        .data_template = "GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-007",
        .data_source = "UVVIS.DOAS.DIRECTSUN.H2CO_INST2",
        .product_version = 7,
        .num_time = 2,
        .num_vertical = 2,
        .invert_vertical = true,
        .altitude_2d = false,
        .time_dependent_replicated = true,
        .include_latlon = false,
    };
    PopulateCommonFields(prod, cfg);
    PopulateDirectSunFields(prod, "H2CO", 7, 2, 2, /*measured_aod=*/false);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_FALSE(res.variables["latitude"].included);
    EXPECT_FALSE(res.variables["longitude"].included);
    EXPECT_THAT(res.variables["pressure"].double_values,
                ElementsAre(1000.0, IsNan(), 1000.0, 1000.0));
    EXPECT_THAT(res.variables["HCHO_column_number_density"].double_values,
                ElementsAre(10.0, 10.0));
  }
}

TEST(HarpIngestGeomsUvvisDoasTest, OffAxisOptimalEstimationAndModes) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("GEOMS-TE-UVVIS-DOAS-OFFAXIS");
  ASSERT_NE(nullptr, mod);

  // Version 7 with optimal estimation, wind, invert_vertical=1, AOD=measured,
  // 1D string CLOUD_CONDITIONS covering all valid strings.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CommonConfig cfg{
        .data_template = "GEOMS-TE-UVVIS-DOAS-OFFAXIS-GAS-007",
        .data_source = "UVVIS.DOAS.OFFAXIS.NO2_INST",
        .product_version = 7,
        .num_time = 6,
        .num_vertical = 2,
        .invert_vertical = true,
        .altitude_2d = false,
        .time_dependent_replicated = false,
        .include_latlon = true,
        .include_wind = true,
    };
    PopulateCommonFields(prod, cfg);
    PopulateOffAxisFields(prod, "NO2", 6, 2, /*measured_aod=*/true,
                          /*optimal_estimation=*/true);
    const long cc_dims[] = {6};
    constexpr std::string_view kClouds[] = {
        "", " ", "clear-sky", "thin clouds", "thick clouds", "broken clouds"};
    ASSERT_TRUE(prod.AddField(prod.root(), "CLOUD_CONDITIONS",
                              prod.MakeStringArray(cc_dims, kClouds)));

    IngestExerciseResult res =
        ExerciseModuleSkippingCloudType(mod, prod.product(), "AOD=measured");
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_THAT(res.variables["surface_wind_direction"].double_values,
                ElementsAre(180.0, 180.0, 180.0, 180.0, 180.0, 180.0));
    // 1.5 ppbv == 0.0015 ppmv
    EXPECT_THAT(res.variables["NO2_volume_mixing_ratio"].double_values[0],
                DoubleEq(0.0015));
    // sqrt(4.0) ppbv == 0.002 ppmv
    EXPECT_THAT(res.variables["NO2_volume_mixing_ratio_uncertainty_random"]
                    .double_values[0],
                DoubleEq(0.002));
  }

  // Version 4 without optimal estimation, modeled AOD, HONO -> HNO2.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CommonConfig cfg{
        .data_template = "GEOMS-TE-UVVIS-DOAS-OFFAXIS-GAS-004",
        .data_source = "UVVIS.DOAS.OFFAXIS.HONO_INST",
        .product_version = 4,
        .num_time = 2,
        .num_vertical = 2,
        .invert_vertical = false,
        .altitude_2d = false,
        .time_dependent_replicated = false,
        .include_latlon = false,
        .include_wind = false,
    };
    PopulateCommonFields(prod, cfg);
    PopulateOffAxisFields(prod, "HONO", 2, 2, /*measured_aod=*/false,
                          /*optimal_estimation=*/false);
    const long cc_dims[] = {2, 9};
    constexpr char kClouds[] = "clear-skyclear-sky";
    ASSERT_TRUE(prod.AddField(
        prod.root(), "CLOUD_CONDITIONS",
        prod.MakeCharArray(cc_dims, std::span<const char>(kClouds, 18))));

    IngestExerciseResult res =
        ExerciseModuleSkippingCloudType(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_FALSE(res.variables["HNO2_volume_mixing_ratio"].included);
  }
}

TEST(HarpIngestGeomsUvvisDoasTest, OffAxisAerosolSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("GEOMS-TE-UVVIS-DOAS-OFFAXIS-AEROSOL");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  CommonConfig cfg{
      .data_template = "GEOMS-TE-UVVIS-DOAS-OFFAXIS-AEROSOL-007",
      .data_source = "ANY_AEROSOL_SOURCE",
      .product_version = 7,
      .num_time = 2,
      .num_vertical = 2,
      .invert_vertical = true,
      .altitude_2d = false,
      .time_dependent_replicated = false,
      .include_latlon = true,
      .include_wind = true,
  };
  PopulateCommonFields(prod, cfg);
  PopulateOffAxisAerosolFields(prod, 2, 2, 2);
  const long cc_dims[] = {2};
  constexpr std::string_view kClouds[] = {"clear-sky", "broken clouds"};
  ASSERT_TRUE(prod.AddField(prod.root(), "CLOUD_CONDITIONS",
                            prod.MakeStringArray(cc_dims, kClouds)));

  IngestExerciseResult res =
      ExerciseModuleSkippingCloudType(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
  EXPECT_EQ(2, res.dimensions[harp_dimension_spectral]);
  EXPECT_THAT(res.variables["wavelength"].double_values,
              ElementsAre(360.0, 477.0));
  // sqrt(0.09) == 0.3
  EXPECT_THAT(res.variables["aerosol_extinction_coefficient_uncertainty_random"]
                  .double_values[0],
              DoubleEq(0.3));
  EXPECT_THAT(res.variables["tropospheric_aerosol_optical_depth"].double_values,
              ElementsAre(0.25, 0.25, 0.25, 0.25));
}

TEST(HarpIngestGeomsUvvisDoasTest, ZenithAllBranches) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("GEOMS-TE-UVVIS-DOAS-ZENITH");
  ASSERT_NE(nullptr, mod);

  // Version 7 with VMR, tropospheric column, wind, AOD=measured, invert=1.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CommonConfig cfg{
        .data_template = "GEOMS-TE-UVVIS-DOAS-ZENITH-GAS-007",
        .data_source = "UVVIS.DOAS.ZENITH.O3_INST",
        .product_version = 7,
        .num_time = 2,
        .num_vertical = 2,
        .invert_vertical = true,
        .altitude_2d = false,
        .time_dependent_replicated = false,
        .include_latlon = true,
        .include_wind = true,
    };
    PopulateCommonFields(prod, cfg);
    PopulateZenithFields(prod, "O3", 7, 2, 2, /*measured_aod=*/true,
                         /*has_vmr=*/true, /*has_tropo=*/true);
    const long cc_dims[] = {2};
    constexpr std::string_view kClouds[] = {"thin clouds", "thick clouds"};
    ASSERT_TRUE(prod.AddField(prod.root(), "CLOUD_CONDITIONS",
                              prod.MakeStringArray(cc_dims, kClouds)));

    IngestExerciseResult res =
        ExerciseModuleSkippingCloudType(mod, prod.product(), "AOD=measured");
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_THAT(res.variables["O3_volume_mixing_ratio_uncertainty_random"]
                    .double_values[0],
                DoubleEq(0.003));
    EXPECT_THAT(
        res.variables["stratospheric_O3_column_number_density"].double_values,
        ElementsAre(12.0, 12.0));
  }

  // Version 4 without VMR and without tropospheric column, modeled AOD.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CommonConfig cfg{
        .data_template = "GEOMS-TE-UVVIS-DOAS-ZENITH-GAS-004",
        .data_source = "UVVIS.DOAS.ZENITH.BrO_INST",
        .product_version = 4,
        .num_time = 2,
        .num_vertical = 2,
        .invert_vertical = false,
        .altitude_2d = false,
        .time_dependent_replicated = false,
        .include_latlon = false,
        .include_wind = false,
    };
    PopulateCommonFields(prod, cfg);
    PopulateZenithFields(prod, "BrO", 4, 2, 2, /*measured_aod=*/false,
                         /*has_vmr=*/false, /*has_tropo=*/false);
    const long cc_dims[] = {2};
    constexpr std::string_view kClouds[] = {"clear-sky", "clear-sky"};
    ASSERT_TRUE(prod.AddField(prod.root(), "CLOUD_CONDITIONS",
                              prod.MakeStringArray(cc_dims, kClouds)));

    IngestExerciseResult res =
        ExerciseModuleSkippingCloudType(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_FALSE(res.variables["BrO_volume_mixing_ratio"].included);
    EXPECT_FALSE(
        res.variables["tropospheric_BrO_column_number_density"].included);
  }
}

TEST(HarpIngestGeomsUvvisDoasTest, InitAndMetadataErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* ds_mod =
      FindIngestionModule("GEOMS-TE-UVVIS-DOAS-DIRECTSUN");
  const harp_ingestion_module* aer_mod =
      FindIngestionModule("GEOMS-TE-UVVIS-DOAS-OFFAXIS-AEROSOL");
  const harp_ingestion_module* oa_mod =
      FindIngestionModule("GEOMS-TE-UVVIS-DOAS-OFFAXIS");
  const harp_ingestion_module* zen_mod =
      FindIngestionModule("GEOMS-TE-UVVIS-DOAS-ZENITH");
  ASSERT_NE(nullptr, ds_mod);
  ASSERT_NE(nullptr, aer_mod);
  ASSERT_NE(nullptr, oa_mod);
  ASSERT_NE(nullptr, zen_mod);

  // 1. Missing product info (coda_get_product_version fails).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(ds_mod, prod.product()).status);
  }

  // 2. Missing @DATA_TEMPLATE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GEOMS", "UVVIS_DOAS", 7));
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 3. Wrong type for @DATA_TEMPLATE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GEOMS", "UVVIS_DOAS", 7));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "DATA_TEMPLATE", prod.MakeInt32(1)));
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. Invalid @DATA_TEMPLATE length.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GEOMS", "UVVIS_DOAS", 7));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("SHORT")));
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 5. Invalid @DATA_TEMPLATE prefix with length 37.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GEOMS", "UVVIS_DOAS", 7));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("XXXXX-TE-UVVIS-DOAS-DIRECTSUN-GAS-007")));
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 6. Unsupported offaxis-aerosol template version.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GEOMS", "UVVIS_DOAS", 999));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("GEOMS-TE-UVVIS-DOAS-OFFAXIS-AEROSOL-999")));
    IngestExerciseResult res = ExerciseIngestionModule(aer_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 7. Missing @DATA_SOURCE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GEOMS", "UVVIS_DOAS", 7));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-007")));
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 8. Wrong type for @DATA_SOURCE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GEOMS", "UVVIS_DOAS", 7));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-007")));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "DATA_SOURCE", prod.MakeInt32(1)));
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 9. @DATA_SOURCE bad prefix, mismatched types (directsun/offaxis/zenith),
  // and unsupported gas/version.
  const struct {
    const harp_ingestion_module* mod;
    std::string_view tmpl;
    std::string_view src;
  } kBadSources[] = {
      {ds_mod, "GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-007", "BAD.PREFIX.O3_X"},
      {ds_mod, "GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-007",
       "UVVIS.DOAS.OFFAXIS.O3_X"},
      {oa_mod, "GEOMS-TE-UVVIS-DOAS-OFFAXIS-GAS-007",
       "UVVIS.DOAS.DIRECTSUN.O3_X"},
      {zen_mod, "GEOMS-TE-UVVIS-DOAS-ZENITH-GAS-007",
       "UVVIS.DOAS.OFFAXIS.O3_X"},
      {ds_mod, "GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-999",
       "UVVIS.DOAS.DIRECTSUN.O3_X"},
      {ds_mod, "GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-007",
       "UVVIS.DOAS.DIRECTSUN.NOEXIST_X"},
  };
  for (const auto& tc : kBadSources) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GEOMS", "UVVIS_DOAS", 7));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString(tc.tmpl)));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "DATA_SOURCE", prod.MakeString(tc.src)));
    IngestExerciseResult res = ExerciseIngestionModule(tc.mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }
}

TEST(HarpIngestGeomsUvvisDoasTest, DimensionsAndDynamicUnitsErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* ds_mod =
      FindIngestionModule("GEOMS-TE-UVVIS-DOAS-DIRECTSUN");
  const harp_ingestion_module* oa_mod =
      FindIngestionModule("GEOMS-TE-UVVIS-DOAS-OFFAXIS");
  const harp_ingestion_module* aer_mod =
      FindIngestionModule("GEOMS-TE-UVVIS-DOAS-OFFAXIS-AEROSOL");
  const harp_ingestion_module* zen_mod =
      FindIngestionModule("GEOMS-TE-UVVIS-DOAS-ZENITH");
  ASSERT_NE(nullptr, ds_mod);
  ASSERT_NE(nullptr, oa_mod);
  ASSERT_NE(nullptr, aer_mod);
  ASSERT_NE(nullptr, zen_mod);

  auto make_valid_header = [&](CodaMemProduct& prod, std::string_view tmpl,
                               std::string_view src, int version) {
    ASSERT_TRUE(prod.SetProductInfo("GEOMS", "UVVIS_DOAS", version));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "DATA_TEMPLATE", prod.MakeString(tmpl)));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "DATA_SOURCE", prod.MakeString(src)));
  };

  // 1. Missing /DATETIME.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-007",
                      "UVVIS.DOAS.DIRECTSUN.O3_X", 7);
    EXPECT_EQ(-1, ExerciseIngestionModule(ds_mod, prod.product()).status);
  }
  // 2. Non-array /DATETIME (record fails get_num_elements).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-007",
                      "UVVIS.DOAS.DIRECTSUN.O3_X", 7);
    ASSERT_TRUE(prod.AddField(prod.root(), "DATETIME", prod.MakeRecord()));
    EXPECT_EQ(-1, ExerciseIngestionModule(ds_mod, prod.product()).status);
  }
  // 3. String /DATETIME (fails read_double_partial_array) & descending
  // /DATETIME.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-007",
                      "UVVIS.DOAS.DIRECTSUN.O3_X", 7);
    const long dims[] = {2};
    constexpr std::string_view kBad[] = {"a", "b"};
    ASSERT_TRUE(prod.AddField(prod.root(), "DATETIME",
                              prod.MakeStringArray(dims, kBad)));
    EXPECT_EQ(-1, ExerciseIngestionModule(ds_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-007",
                      "UVVIS.DOAS.DIRECTSUN.O3_X", 7);
    const long dims[] = {2};
    const double desc[] = {20.0, 10.0};
    AddDoubleVar(prod, "DATETIME", dims, desc);
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 4. Offaxis-aerosol WAVELENGTH errors (missing, record, string, descending).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-OFFAXIS-AEROSOL-007", "A", 7);
    const long dims[] = {1};
    const double dt[] = {10.0};
    AddDoubleVar(prod, "DATETIME", dims, dt);
    EXPECT_EQ(-1, ExerciseIngestionModule(aer_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-OFFAXIS-AEROSOL-007", "A", 7);
    const long dims[] = {1};
    const double dt[] = {10.0};
    AddDoubleVar(prod, "DATETIME", dims, dt);
    ASSERT_TRUE(prod.AddField(prod.root(), "WAVELENGTH", prod.MakeRecord()));
    EXPECT_EQ(-1, ExerciseIngestionModule(aer_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-OFFAXIS-AEROSOL-007", "A", 7);
    const long dims1[] = {1};
    const double dt[] = {10.0};
    AddDoubleVar(prod, "DATETIME", dims1, dt);
    const long dims2[] = {2};
    constexpr std::string_view kBad[] = {"a", "b"};
    ASSERT_TRUE(prod.AddField(prod.root(), "WAVELENGTH",
                              prod.MakeStringArray(dims2, kBad)));
    EXPECT_EQ(-1, ExerciseIngestionModule(aer_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-OFFAXIS-AEROSOL-007", "A", 7);
    const long dims1[] = {1};
    const double dt[] = {10.0};
    AddDoubleVar(prod, "DATETIME", dims1, dt);
    const long dims2[] = {2};
    const double desc[] = {500.0, 300.0};
    AddDoubleVar(prod, "WAVELENGTH", dims2, desc);
    IngestExerciseResult res = ExerciseIngestionModule(aer_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 5. ALTITUDE errors: missing, record (get_array_dim fails), 3D, string
  // array.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-007",
                      "UVVIS.DOAS.DIRECTSUN.O3_X", 7);
    const long dims[] = {1};
    const double dt[] = {10.0};
    AddDoubleVar(prod, "DATETIME", dims, dt);
    EXPECT_EQ(-1, ExerciseIngestionModule(ds_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-007",
                      "UVVIS.DOAS.DIRECTSUN.O3_X", 7);
    const long dims[] = {1};
    const double dt[] = {10.0};
    AddDoubleVar(prod, "DATETIME", dims, dt);
    ASSERT_TRUE(prod.AddField(prod.root(), "ALTITUDE", prod.MakeRecord()));
    EXPECT_EQ(-1, ExerciseIngestionModule(ds_mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-007",
                      "UVVIS.DOAS.DIRECTSUN.O3_X", 7);
    const long dims[] = {1};
    const double dt[] = {10.0};
    AddDoubleVar(prod, "DATETIME", dims, dt);
    const long alt3_dims[] = {1, 1, 2};
    const double alt3[] = {1.0, 2.0};
    AddDoubleVar(prod, "ALTITUDE", alt3_dims, alt3);
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-007",
                      "UVVIS.DOAS.DIRECTSUN.O3_X", 7);
    const long dims1[] = {1};
    const double dt[] = {10.0};
    AddDoubleVar(prod, "DATETIME", dims1, dt);
    const long dims2[] = {2};
    constexpr std::string_view kBad[] = {"a", "b"};
    ASSERT_TRUE(prod.AddField(prod.root(), "ALTITUDE",
                              prod.MakeStringArray(dims2, kBad)));
    EXPECT_EQ(-1, ExerciseIngestionModule(ds_mod, prod.product()).status);
  }

  // 6. get_dynamic_units errors for offaxis & zenith.
  {
    // Offaxis: missing @VAR_UNITS on VMR.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-OFFAXIS-GAS-007",
                      "UVVIS.DOAS.OFFAXIS.O3_X", 7);
    const long dims[] = {1};
    const double v[] = {1.0};
    AddDoubleVar(prod, "DATETIME", dims, v);
    AddDoubleVar(prod, "ALTITUDE", dims, v);
    AddDoubleVar(prod, "O3_MIXING_RATIO_VOLUME_SCATTER_SOLAR_OFFAXIS", dims, v);
    EXPECT_EQ(-1, ExerciseIngestionModule(oa_mod, prod.product()).status);
  }
  {
    // Offaxis: missing covariance variable when VMR has @VAR_UNITS.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-OFFAXIS-GAS-007",
                      "UVVIS.DOAS.OFFAXIS.O3_X", 7);
    const long dims[] = {1};
    const double v[] = {1.0};
    AddDoubleVar(prod, "DATETIME", dims, v);
    AddDoubleVar(prod, "ALTITUDE", dims, v);
    AddDoubleVar(prod, "O3_MIXING_RATIO_VOLUME_SCATTER_SOLAR_OFFAXIS", dims, v,
                 -9999.0, "ppbv");
    EXPECT_EQ(-1, ExerciseIngestionModule(oa_mod, prod.product()).status);
  }
  {
    // Offaxis: non-string @VAR_UNITS on covariance.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-OFFAXIS-GAS-007",
                      "UVVIS.DOAS.OFFAXIS.O3_X", 7);
    const long dims[] = {1};
    const double v[] = {1.0};
    AddDoubleVar(prod, "DATETIME", dims, v);
    AddDoubleVar(prod, "ALTITUDE", dims, v);
    AddDoubleVar(prod, "O3_MIXING_RATIO_VOLUME_SCATTER_SOLAR_OFFAXIS", dims, v,
                 -9999.0, "ppbv");
    CodaMemNode* cov = prod.MakeDoubleArray(dims, v);
    ASSERT_TRUE(prod.AddAttribute(cov, "VAR_UNITS", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(),
                              "O3_MIXING_RATIO_VOLUME_SCATTER_SOLAR_OFFAXIS_"
                              "UNCERTAINTY_RANDOM_COVARIANCE",
                              cov));
    EXPECT_EQ(-1, ExerciseIngestionModule(oa_mod, prod.product()).status);
  }
  {
    // Zenith v7: missing VMR apriori.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-ZENITH-GAS-007",
                      "UVVIS.DOAS.ZENITH.O3_X", 7);
    const long dims[] = {1};
    const double v[] = {1.0};
    AddDoubleVar(prod, "DATETIME", dims, v);
    AddDoubleVar(prod, "ALTITUDE", dims, v);
    EXPECT_EQ(-1, ExerciseIngestionModule(zen_mod, prod.product()).status);
  }
  {
    // Zenith v7 with VMR: missing VMR covariance.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    make_valid_header(prod, "GEOMS-TE-UVVIS-DOAS-ZENITH-GAS-007",
                      "UVVIS.DOAS.ZENITH.O3_X", 7);
    const long dims[] = {1};
    const double v[] = {1.0};
    AddDoubleVar(prod, "DATETIME", dims, v);
    AddDoubleVar(prod, "ALTITUDE", dims, v);
    AddDoubleVar(prod, "O3_MIXING_RATIO_VOLUME_SCATTER_SOLAR_ZENITH", dims, v,
                 -9999.0, "ppbv");
    AddDoubleVar(prod, "O3_MIXING_RATIO_VOLUME_SCATTER_SOLAR_ZENITH_APRIORI",
                 dims, v, -9999.0, "ppbv");
    EXPECT_EQ(-1, ExerciseIngestionModule(zen_mod, prod.product()).status);
  }
}

TEST(HarpIngestGeomsUvvisDoasTest, VariableReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* ds_mod =
      FindIngestionModule("GEOMS-TE-UVVIS-DOAS-DIRECTSUN");
  const harp_ingestion_module* oa_mod =
      FindIngestionModule("GEOMS-TE-UVVIS-DOAS-OFFAXIS");
  const harp_ingestion_module* zen_mod =
      FindIngestionModule("GEOMS-TE-UVVIS-DOAS-ZENITH");
  ASSERT_NE(nullptr, ds_mod);
  ASSERT_NE(nullptr, oa_mod);
  ASSERT_NE(nullptr, zen_mod);

  // 1. Directsun: missing fields, wrong element counts, wrong types, missing
  // @VAR_FILL_VALUE, wrong @VAR_FILL_VALUE type, record field.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GEOMS", "UVVIS_DOAS", 7));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-007")));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "DATA_SOURCE",
                          prod.MakeString("UVVIS.DOAS.DIRECTSUN.O3_X")));
    // Missing @DATA_LOCATION -> read_attribute fails.
    const long t_dims[] = {2};
    const long bad_dims[] = {3};
    const double dt[] = {10.0, 20.0};
    const double bad_vals[] = {1.0, 2.0, 3.0};
    AddDoubleVar(prod, "DATETIME", t_dims, dt);
    // ALTITUDE descending so invert_vertical=1 is active on error paths too.
    const double alt[] = {5.0, 1.0};
    AddDoubleVar(prod, "ALTITUDE", t_dims, alt);
    // Wrong size DATETIME_START -> actual_num_elements != num_elements.
    AddDoubleVar(prod, "DATETIME_START", bad_dims, bad_vals);
    // String array DATETIME_STOP of right size -> read_double_array fails.
    constexpr std::string_view kStrs[] = {"a", "b"};
    ASSERT_TRUE(prod.AddField(prod.root(), "DATETIME_STOP",
                              prod.MakeStringArray(t_dims, kStrs)));
    // LATITUDE_INSTRUMENT as record -> get_num_elements fails.
    ASSERT_TRUE(
        prod.AddField(prod.root(), "LATITUDE_INSTRUMENT", prod.MakeRecord()));
    // LONGITUDE_INSTRUMENT without @VAR_FILL_VALUE.
    const long s_dims[] = {1};
    const double s_val[] = {1.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "LONGITUDE_INSTRUMENT",
                              prod.MakeDoubleArray(s_dims, s_val)));
    // ALTITUDE_INSTRUMENT with non-double @VAR_FILL_VALUE.
    CodaMemNode* alt_inst = prod.MakeDoubleArray(s_dims, s_val);
    ASSERT_TRUE(
        prod.AddAttribute(alt_inst, "VAR_FILL_VALUE", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "ALTITUDE_INSTRUMENT", alt_inst));
    // Replicated error paths: ALTITUDE_BOUNDARIES wrong count,
    // TEMPERATURE_INDEPENDENT as record, PRESSURE_INDEPENDENT missing.
    AddDoubleVar(prod, "ALTITUDE_BOUNDARIES", bad_dims, bad_vals);
    ASSERT_TRUE(prod.AddField(prod.root(), "TEMPERATURE_INDEPENDENT",
                              prod.MakeRecord()));

    IngestExerciseResult res =
        ExerciseIngestionModule(ds_mod, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["location_name"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_latitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_longitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["altitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["temperature"].read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
  }

  // 2. Non-string @DATA_LOCATION & replicated string/no-fill errors.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("GEOMS", "UVVIS_DOAS", 7));
    ASSERT_TRUE(prod.AddAttribute(
        prod.root(), "DATA_TEMPLATE",
        prod.MakeString("GEOMS-TE-UVVIS-DOAS-DIRECTSUN-GAS-007")));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "DATA_SOURCE",
                          prod.MakeString("UVVIS.DOAS.DIRECTSUN.O3_X")));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "DATA_LOCATION", prod.MakeInt32(42)));
    const long t_dims[] = {2};
    const double dt[] = {10.0, 20.0};
    AddDoubleVar(prod, "DATETIME", t_dims, dt);
    AddDoubleVar(prod, "ALTITUDE", t_dims, dt);
    constexpr std::string_view kStrs[] = {"a", "b"};
    ASSERT_TRUE(prod.AddField(prod.root(), "TEMPERATURE_INDEPENDENT",
                              prod.MakeStringArray(t_dims, kStrs)));
    const long b_dims[] = {2, 2};
    const double b_vals[] = {0.0, 1.0, 1.0, 2.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "ALTITUDE_BOUNDARIES",
                              prod.MakeDoubleArray(b_dims, b_vals)));
    // Missing VAR_FILL_VALUE on ALTITUDE_BOUNDARIES, and wrong type
    // VAR_FILL_VALUE on LATITUDE:
    CodaMemNode* lat = prod.MakeDoubleArray(b_dims, b_vals);
    ASSERT_TRUE(
        prod.AddAttribute(lat, "VAR_FILL_VALUE", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "LATITUDE", lat));

    IngestExerciseResult res =
        ExerciseIngestionModule(ds_mod, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["location_name"].read_status);
    EXPECT_EQ(-1, res.variables["temperature"].read_status);
    EXPECT_EQ(-1, res.variables["altitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
  }

  // 3. Offaxis: incompatible @VAR_UNITS triggers harp_convert_unit failures,
  // missing systematic covariance triggers sqrt_2dtrace error, and invalid
  // CLOUD_CONDITIONS values/dims trigger read_cloud_conditions errors.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CommonConfig cfg{
        .data_template = "GEOMS-TE-UVVIS-DOAS-OFFAXIS-GAS-007",
        .data_source = "UVVIS.DOAS.OFFAXIS.O3_X",
        .product_version = 7,
        .num_time = 2,
        .num_vertical = 2,
        .invert_vertical = true,
    };
    PopulateCommonFields(prod, cfg);
    const long tv_dims[] = {2, 2};
    const long tvv_dims[] = {2, 2, 2};
    const double tv[] = {1.0, 2.0, 3.0, 4.0};
    const double tvv[] = {1.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0};
    // Incompatible units ("m" instead of VMR unit).
    AddDoubleVar(prod, "O3_MIXING_RATIO_VOLUME_SCATTER_SOLAR_OFFAXIS", tv_dims,
                 tv, -9999.0, "m");
    AddDoubleVar(
        prod,
        "O3_MIXING_RATIO_VOLUME_SCATTER_SOLAR_OFFAXIS_UNCERTAINTY_RANDOM_"
        "COVARIANCE",
        tvv_dims, tvv, -9999.0, "m2");
    AddDoubleVar(prod, "O3_MIXING_RATIO_VOLUME_SCATTER_SOLAR_OFFAXIS_APRIORI",
                 tv_dims, tv, -9999.0, "m");
    // Deliberately omit SYSTEMATIC_COVARIANCE so sqrt_2dtrace fails.
    const long cc_dims[] = {2};
    constexpr std::string_view kBadCloud[] = {"overcast", "clear-sky"};
    ASSERT_TRUE(prod.AddField(prod.root(), "CLOUD_CONDITIONS",
                              prod.MakeStringArray(cc_dims, kBadCloud)));

    IngestExerciseResult res = ExerciseModuleSkippingCloudType(
        oa_mod, prod.product(), /*options_str=*/nullptr,
        /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_volume_mixing_ratio_covariance"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["O3_volume_mixing_ratio_uncertainty_random"].read_status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio_uncertainty_systematic"]
                      .read_status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio_apriori"].read_status);
  }

  // 4. CLOUD_CONDITIONS error paths in read_variable_string: missing,
  // wrong dim[0], non-array (record), int32 array (get_string_length fails).
  const auto test_cloud_error = [&](auto setup_cloud) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CommonConfig cfg{
        .data_template = "GEOMS-TE-UVVIS-DOAS-OFFAXIS-GAS-007",
        .data_source = "UVVIS.DOAS.OFFAXIS.O3_X",
        .product_version = 7,
        .num_time = 2,
        .num_vertical = 2,
    };
    PopulateCommonFields(prod, cfg);
    PopulateOffAxisFields(prod, "O3", 2, 2, false, false);
    setup_cloud(prod);
    IngestExerciseResult res =
        ExerciseIngestionModule(oa_mod, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["cloud_type"].read_status);
  };

  test_cloud_error([](CodaMemProduct&) {});
  test_cloud_error([](CodaMemProduct& p) {
    const long dims[] = {1};
    constexpr std::string_view s[] = {"clear-sky"};
    ASSERT_TRUE(
        p.AddField(p.root(), "CLOUD_CONDITIONS", p.MakeStringArray(dims, s)));
  });
  test_cloud_error([](CodaMemProduct& p) {
    ASSERT_TRUE(p.AddField(p.root(), "CLOUD_CONDITIONS", p.MakeRecord()));
  });
  test_cloud_error([](CodaMemProduct& p) {
    const long dims[] = {2};
    const int32_t v[] = {1, 2};
    ASSERT_TRUE(
        p.AddField(p.root(), "CLOUD_CONDITIONS", p.MakeInt32Array(dims, v)));
  });

  // 5. Zenith: incompatible @VAR_UNITS & missing systematic covariance.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CommonConfig cfg{
        .data_template = "GEOMS-TE-UVVIS-DOAS-ZENITH-GAS-007",
        .data_source = "UVVIS.DOAS.ZENITH.O3_X",
        .product_version = 7,
        .num_time = 2,
        .num_vertical = 2,
    };
    PopulateCommonFields(prod, cfg);
    const long tv_dims[] = {2, 2};
    const long tvv_dims[] = {2, 2, 2};
    const double tv[] = {1.0, 2.0, 3.0, 4.0};
    const double tvv[] = {1.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0};
    AddDoubleVar(prod, "O3_MIXING_RATIO_VOLUME_SCATTER_SOLAR_ZENITH", tv_dims,
                 tv, -9999.0, "m");
    AddDoubleVar(
        prod,
        "O3_MIXING_RATIO_VOLUME_SCATTER_SOLAR_ZENITH_UNCERTAINTY_RANDOM_"
        "COVARIANCE",
        tvv_dims, tvv, -9999.0, "m2");
    AddDoubleVar(prod, "O3_MIXING_RATIO_VOLUME_SCATTER_SOLAR_ZENITH_APRIORI",
                 tv_dims, tv, -9999.0, "m");
    IngestExerciseResult res = ExerciseIngestionModule(zen_mod, prod.product(),
                                                       /*options_str=*/nullptr,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_volume_mixing_ratio_covariance"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["O3_volume_mixing_ratio_uncertainty_random"].read_status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio_uncertainty_systematic"]
                      .read_status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio_apriori"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
