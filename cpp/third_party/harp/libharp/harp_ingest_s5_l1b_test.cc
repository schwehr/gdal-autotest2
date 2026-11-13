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
// Unit tests for Sentinel-5 L1b ingestion modules
// (libharp/harp-ingest-s5_l1b.c).
//
// Google3 build notes: HDF4 is disabled (#undef HAVE_HDF4); CODA products are
// synthesized in memory via CodaMemProduct without .codadef files.
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-s5_l1b.c

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

using ::testing::ElementsAre;
using ::testing::FloatNear;
using ::testing::HasSubstr;

constexpr float kFillFloat = -9999.0f;
constexpr double kFillDouble = -9999.0;
// Note: init_dataset() reads @FillValue[0] as float and decode_uncertainty()
// compares against observable_*_fill_value.int8_data (the low byte of the
// float representation on little-endian). 0.0f has all-zero bytes, so both
// float_data == 0.0f and int8_data == 0 match cleanly.
constexpr int8_t kFillInt8 = 0;

// Local S5 L1b exercise helper mirroring ExerciseIngestionModule, except that
// for "datetime" it allocates one extra double element to accommodate the
// upstream off-by-one loop (`for (j = 0; j <= info->num_pixels; j++)`) in
// read_datetime(), preventing a heap-buffer-overflow under ASan/MSan.
IngestExerciseResult ExerciseS5L1bModule(const harp_ingestion_module* mod,
                                         coda_product* cprod,
                                         const char* options_str = nullptr,
                                         bool stop_on_error = true) {
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
    // Pad datetime buffer by +1 element for the upstream j <= num_pixels bug.
    const long alloc_elements = (std::string_view(var_def->name) == "datetime")
                                    ? (num_elements + 1)
                                    : num_elements;
    std::vector<uint8_t> raw(static_cast<size_t>(alloc_elements) * elem_size,
                             0);
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
      default:
        break;
    }
    result.variables.emplace(var_def->name, std::move(var));
  }
  return result;
}

bool AddFloatDataset(CodaMemProduct& prod, CodaMemNode* rec,
                     std::string_view name, std::span<const long> dims,
                     std::span<const float> values,
                     float fill_value = kFillFloat) {
  CodaMemNode* arr = prod.MakeFloatArray(dims, values);
  if (arr == nullptr) return false;
  constexpr long kOne[] = {1};
  const float fill[] = {fill_value};
  if (!prod.AddAttribute(arr, "FillValue", prod.MakeFloatArray(kOne, fill))) {
    return false;
  }
  return prod.AddField(rec, name, arr);
}

bool AddDoubleDataset(CodaMemProduct& prod, CodaMemNode* rec,
                      std::string_view name, std::span<const long> dims,
                      std::span<const double> values,
                      double fill_value = kFillDouble) {
  CodaMemNode* arr = prod.MakeDoubleArray(dims, values);
  if (arr == nullptr) return false;
  constexpr long kOne[] = {1};
  const double fill[] = {fill_value};
  if (!prod.AddAttribute(arr, "FillValue", prod.MakeDoubleArray(kOne, fill))) {
    return false;
  }
  return prod.AddField(rec, name, arr);
}

bool AddInt8Dataset(CodaMemProduct& prod, CodaMemNode* rec,
                    std::string_view name, std::span<const long> dims,
                    std::span<const int8_t> values,
                    int8_t fill_value = kFillInt8) {
  CodaMemNode* arr = prod.MakeInt8Array(dims, values);
  if (arr == nullptr) return false;
  constexpr long kOne[] = {1};
  const int8_t fill[] = {fill_value};
  if (!prod.AddAttribute(arr, "FillValue", prod.MakeInt8Array(kOne, fill))) {
    return false;
  }
  return prod.AddField(rec, name, arr);
}

struct S5L1bConfig {
  std::string_view product_id_str = "SN5_1B_UVR";
  bool use_metadata_short_name = false;
  bool product_name_as_array = false;
  std::string_view band_name = "band1a";
  bool under_data_group = false;
  bool nest_dims_in_array = false;
  bool is_irr = false;
  bool use_uint_types = false;
  bool orbit_use_start = true;
  bool orbit_as_array = false;
  std::string_view global_id;
  long num_scanlines = 2;
  long num_pixels = 2;
  long num_corners = 4;
  long num_spectral = 2;
};

struct S5L1bHandles {
  CodaMemNode* band = nullptr;
  CodaMemNode* geoloc = nullptr;
  CodaMemNode* inst = nullptr;
  CodaMemNode* obs = nullptr;
};

bool BuildS5L1bProduct(CodaMemProduct& prod, const S5L1bConfig& cfg,
                       S5L1bHandles* out = nullptr) {
  if (!prod.ok()) return false;

  if (cfg.use_metadata_short_name) {
    CodaMemNode* meta = prod.MakeRecord();
    CodaMemNode* gran = prod.MakeRecord();
    if (!prod.AddAttribute(gran, "ProductShortName",
                           prod.MakeString(cfg.product_id_str)) ||
        !prod.AddField(meta, "GRANULE_DESCRIPTION", gran) ||
        !prod.AddField(prod.root(), "METADATA", meta)) {
      return false;
    }
  } else if (cfg.product_name_as_array) {
    constexpr long kOne[] = {1};
    const std::string_view vals[] = {cfg.product_id_str};
    if (!prod.AddAttribute(prod.root(), "product_name",
                           prod.MakeStringArray(kOne, vals))) {
      return false;
    }
  } else {
    if (!prod.AddAttribute(prod.root(), "product_name",
                           prod.MakeString(cfg.product_id_str))) {
      return false;
    }
  }

  if (!cfg.global_id.empty()) {
    if (!prod.AddAttribute(prod.root(), "id", prod.MakeString(cfg.global_id))) {
      return false;
    }
  }

  const char* orbit_attr = cfg.orbit_use_start ? "orbit_start" : "orbit";
  if (cfg.orbit_as_array) {
    constexpr long kOne[] = {1};
    if (cfg.use_uint_types) {
      constexpr uint32_t kOrbit[] = {4321};
      if (!prod.AddAttribute(prod.root(), orbit_attr,
                             prod.MakeUint32Array(kOne, kOrbit))) {
        return false;
      }
    } else {
      constexpr int32_t kOrbit[] = {4321};
      if (!prod.AddAttribute(prod.root(), orbit_attr,
                             prod.MakeInt32Array(kOne, kOrbit))) {
        return false;
      }
    }
  } else {
    if (cfg.use_uint_types) {
      if (!prod.AddAttribute(prod.root(), orbit_attr, prod.MakeUint32(4321))) {
        return false;
      }
    } else {
      if (!prod.AddAttribute(prod.root(), orbit_attr, prod.MakeInt32(4321))) {
        return false;
      }
    }
  }

  CodaMemNode* band = prod.MakeRecord();
  CodaMemNode* geoloc = prod.MakeRecord();
  CodaMemNode* inst = prod.MakeRecord();
  CodaMemNode* obs = prod.MakeRecord();

  // Dimension marker 1D arrays under band.
  auto make_dim_marker = [&](long len) -> CodaMemNode* {
    std::vector<int32_t> v(static_cast<size_t>(len), 0);
    const long d[] = {len};
    return prod.MakeInt32Array(d, v);
  };
  CodaMemNode* dim_holder = band;
  if (cfg.nest_dims_in_array) {
    CodaMemNode* inner = prod.MakeRecord();
    // Include an empty array first to exercise the num_elements == 0 skip in
    // find_dimension_length_recursive.
    constexpr long kZero[] = {0};
    if (!prod.AddField(band, "empty_arr", prod.MakeInt32Array(kZero, {}))) {
      return false;
    }
    constexpr long kOne[] = {1};
    CodaMemNode* elems[] = {inner};
    if (!prod.AddField(band, "dim_arr", prod.MakeNodeArray(kOne, elems))) {
      return false;
    }
    dim_holder = inner;
  }
  if (!prod.AddField(dim_holder, "scanline",
                     make_dim_marker(cfg.num_scanlines)) ||
      !prod.AddField(dim_holder, cfg.is_irr ? "pixel" : "ground_pixel",
                     make_dim_marker(cfg.num_pixels)) ||
      !prod.AddField(dim_holder, "spectral_channel",
                     make_dim_marker(cfg.num_spectral))) {
    return false;
  }
  if (!cfg.is_irr) {
    if (!prod.AddField(dim_holder, "pixel_corners",
                       make_dim_marker(cfg.num_corners))) {
      return false;
    }
  }

  const long n_scan = cfg.num_scanlines;
  const long n_sp = n_scan * cfg.num_pixels;
  const long n_spc = n_sp * cfg.num_corners;
  const long n_spec = n_sp * cfg.num_spectral;
  const long n_coeff = n_sp * 4;

  const long d_scan[] = {n_scan};
  const long d_sp[] = {n_scan, cfg.num_pixels};
  const long d_spc[] = {n_scan, cfg.num_pixels, cfg.num_corners};
  const long d_spec[] = {n_scan, cfg.num_pixels, cfg.num_spectral};
  const long d_coeff[] = {n_scan, cfg.num_pixels, 4};

  // Geolocation data.
  std::vector<float> sat_alt(static_cast<size_t>(n_scan), 824000.0f);
  std::vector<float> sat_lat(static_cast<size_t>(n_scan), 12.5f);
  std::vector<float> sat_lon(static_cast<size_t>(n_scan), -45.5f);
  if (!AddFloatDataset(prod, geoloc, "satellite_altitude", d_scan, sat_alt) ||
      !AddFloatDataset(prod, geoloc, "satellite_latitude", d_scan, sat_lat) ||
      !AddFloatDataset(prod, geoloc, "satellite_longitude", d_scan, sat_lon)) {
    return false;
  }
  if (!cfg.is_irr) {
    std::vector<float> lat(static_cast<size_t>(n_sp), 52.0f);
    std::vector<float> lon(static_cast<size_t>(n_sp), 5.0f);
    std::vector<float> sza(static_cast<size_t>(n_sp), 30.0f);
    std::vector<float> saa(static_cast<size_t>(n_sp), 120.0f);
    std::vector<float> vza(static_cast<size_t>(n_sp), 15.0f);
    std::vector<float> vaa(static_cast<size_t>(n_sp), -60.0f);
    std::vector<float> lat_b(static_cast<size_t>(n_spc), 51.5f);
    std::vector<float> lon_b(static_cast<size_t>(n_spc), 4.5f);
    lat[0] = kFillFloat;  // Exercise float fill replacement.
    if (!AddFloatDataset(prod, geoloc, "latitude", d_sp, lat) ||
        !AddFloatDataset(prod, geoloc, "longitude", d_sp, lon) ||
        !AddFloatDataset(prod, geoloc, "latitude_bounds", d_spc, lat_b) ||
        !AddFloatDataset(prod, geoloc, "longitude_bounds", d_spc, lon_b) ||
        !AddFloatDataset(prod, geoloc, "solar_zenith_angle", d_sp, sza) ||
        !AddFloatDataset(prod, geoloc, "solar_azimuth_angle", d_sp, saa) ||
        !AddFloatDataset(prod, geoloc, "viewing_zenith_angle", d_sp, vza) ||
        !AddFloatDataset(prod, geoloc, "viewing_azimuth_angle", d_sp, vaa)) {
      return false;
    }
  }

  // Observation data.
  constexpr long kOne[] = {1};
  const double time_ref[] = {10.0};
  std::vector<double> delta_time(static_cast<size_t>(n_scan));
  for (long i = 0; i < n_scan; ++i) {
    delta_time[static_cast<size_t>(i)] = static_cast<double>(i) * 1.5;
  }
  if (!AddDoubleDataset(prod, obs, "time", kOne, time_ref) ||
      !AddDoubleDataset(prod, obs, "delta_time", d_scan, delta_time)) {
    return false;
  }

  if (cfg.use_uint_types) {
    std::vector<uint16_t> mq(static_cast<size_t>(n_scan), 3);
    std::vector<uint8_t> scq(static_cast<size_t>(n_spec), 2);
    if (!prod.AddField(obs, "measurement_quality",
                       prod.MakeUint16Array(d_scan, mq)) ||
        !prod.AddField(obs, "spectral_channel_quality",
                       prod.MakeUint8Array(d_spec, scq))) {
      return false;
    }
  } else {
    std::vector<int16_t> mq(static_cast<size_t>(n_scan), 3);
    std::vector<int8_t> scq(static_cast<size_t>(n_spec), 2);
    if (!prod.AddField(obs, "measurement_quality",
                       prod.MakeInt16Array(d_scan, mq)) ||
        !prod.AddField(obs, "spectral_channel_quality",
                       prod.MakeInt8Array(d_spec, scq))) {
      return false;
    }
  }

  std::vector<float> rad(static_cast<size_t>(n_spec), 100.0f);
  std::vector<int8_t> rad_err(static_cast<size_t>(n_spec), 20);
  std::vector<int8_t> rad_noise(static_cast<size_t>(n_spec), 40);
  if (n_spec >= 2) {
    // Exercise fill value branches in decode_uncertainty.
    rad[n_spec - 1] = kFillFloat;
    rad_err[0] = kFillInt8;
    rad_noise[0] = kFillInt8;
  }
  const std::string_view obs_name = cfg.is_irr ? "irradiance" : "radiance";
  const std::string_view err_name =
      cfg.is_irr ? "irradiance_error" : "radiance_error";
  const std::string_view noise_name =
      cfg.is_irr ? "irradiance_noise" : "radiance_noise";
  if (!AddFloatDataset(prod, obs, obs_name, d_spec, rad) ||
      !AddInt8Dataset(prod, obs, err_name, d_spec, rad_err) ||
      !AddInt8Dataset(prod, obs, noise_name, d_spec, rad_noise)) {
    return false;
  }

  // Instrument data.
  std::vector<float> coeff(static_cast<size_t>(n_coeff), 0.0f);
  std::vector<float> coeff_err(static_cast<size_t>(n_coeff), 0.1f);
  for (long i = 0; i < n_sp; ++i) {
    coeff[static_cast<size_t>(i * 4 + 0)] = 300.0f;
    coeff[static_cast<size_t>(i * 4 + 1)] = 10.0f;
    coeff[static_cast<size_t>(i * 4 + 2)] = 1.0f;
    coeff[static_cast<size_t>(i * 4 + 3)] = 0.5f;
  }
  if (!AddFloatDataset(prod, inst, "calibrated_wavelength_coefficients",
                       d_coeff, coeff) ||
      !AddFloatDataset(prod, inst, "nominal_wavelength_coefficients", d_coeff,
                       coeff) ||
      !AddFloatDataset(prod, inst, "calibrated_wavelength_coefficients_error",
                       d_coeff, coeff_err) ||
      !AddFloatDataset(prod, inst, "nominal_wavelength_coefficients_error",
                       d_coeff, coeff_err)) {
    return false;
  }
  if (cfg.use_uint_types) {
    std::vector<uint16_t> cal_q(static_cast<size_t>(n_sp), 5);
    if (!prod.AddField(inst, "spectral_calibration_quality",
                       prod.MakeUint16Array(d_sp, cal_q))) {
      return false;
    }
  } else {
    std::vector<int16_t> cal_q(static_cast<size_t>(n_sp), 5);
    if (!prod.AddField(inst, "spectral_calibration_quality",
                       prod.MakeInt16Array(d_sp, cal_q))) {
      return false;
    }
  }

  if (!prod.AddField(band, "geolocation_data", geoloc) ||
      !prod.AddField(band, "instrument_data", inst) ||
      !prod.AddField(band, "observation_data", obs)) {
    return false;
  }

  if (cfg.under_data_group) {
    CodaMemNode* data = prod.MakeRecord();
    if (!prod.AddField(data, cfg.band_name, band) ||
        !prod.AddField(prod.root(), "data", data)) {
      return false;
    }
  } else {
    if (!prod.AddField(prod.root(), cfg.band_name, band)) {
      return false;
    }
  }

  if (out != nullptr) {
    out->band = band;
    out->geoloc = geoloc;
    out->inst = inst;
    out->obs = obs;
  }
  return true;
}

TEST(HarpIngestS5L1bTest, IngestUvrBandsAndLambdaOptionsSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("S5_L1B_UVR");
  ASSERT_NE(nullptr, mod);

  struct BandCase {
    const char* options;
    std::string_view band_name;
    bool use_metadata;
    bool product_name_arr;
    bool under_data;
    bool nest_dims;
    bool use_uints;
    bool orbit_start;
    bool orbit_arr;
    std::string_view global_id;
  };
  const BandCase kCases[] = {
      {.options = nullptr,
       .band_name = "band1a",
       .use_metadata = false,
       .product_name_arr = false,
       .under_data = false,
       .nest_dims = false,
       .use_uints = false,
       .orbit_start = true,
       .orbit_arr = false,
       .global_id = "S5_OFFL_L1B_RA_BD1A_20200101T000000_20200101T010000_12345_"
                    "01_020000_"
                    "20200101T020000"},
      {.options = "band=1a;lambda=calibrated",
       .band_name = "band1a",
       .use_metadata = true,
       .product_name_arr = false,
       .under_data = true,
       .nest_dims = true,
       .use_uints = true,
       .orbit_start = false,
       .orbit_arr = true,
       .global_id = "short_id"},
      {.options = "band=1b;lambda=nominal",
       .band_name = "band1b",
       .use_metadata = false,
       .product_name_arr = true,
       .under_data = true,
       .nest_dims = false,
       .use_uints = false,
       .orbit_start = false,
       .orbit_arr = false,
       .global_id = ""},
      {.options = "band=2",
       .band_name = "band2",
       .use_metadata = false,
       .product_name_arr = false,
       .under_data = false,
       .nest_dims = false,
       .use_uints = true,
       .orbit_start = true,
       .orbit_arr = true,
       .global_id = ""},
  };

  for (const auto& tc : kCases) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5L1bConfig cfg;
    // Also exercise dash_to_underscore when product_name uses dashes.
    cfg.product_id_str = tc.product_name_arr ? "SN5-1B-UVR" : "SN5_1B_UVR";
    cfg.use_metadata_short_name = tc.use_metadata;
    cfg.product_name_as_array = tc.product_name_arr;
    cfg.band_name = tc.band_name;
    cfg.under_data_group = tc.under_data;
    cfg.nest_dims_in_array = tc.nest_dims;
    cfg.use_uint_types = tc.use_uints;
    cfg.orbit_use_start = tc.orbit_start;
    cfg.orbit_as_array = tc.orbit_arr;
    cfg.global_id = tc.global_id;
    ASSERT_TRUE(BuildS5L1bProduct(prod, cfg));

    IngestExerciseResult res =
        ExerciseS5L1bModule(mod, prod.product(), tc.options);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(4, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(2, res.dimensions[harp_dimension_spectral]);
    EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(4321));
    EXPECT_TRUE(std::isnan(res.variables["latitude"].float_values[0]));
    EXPECT_EQ(52.0f, res.variables["latitude"].float_values[1]);
    EXPECT_THAT(res.variables["sensor_altitude"].float_values,
                ElementsAre(824000.0f, 824000.0f, 824000.0f, 824000.0f));
    EXPECT_THAT(res.variables["validity"].int16_values,
                ElementsAre(3, 3, 3, 3));
    EXPECT_THAT(res.variables["datetime_length"].double_values,
                ElementsAre(1.5));
    EXPECT_EQ(10.0 * 86400.0, res.variables["datetime"].double_values[0]);
    // Note: upstream `j <= info->num_pixels` loop overwrites index 2 (scanline
    // 1, pixel 0) with scanline 0's time; index 3 holds scanline 1's time.
    EXPECT_EQ(10.0 * 86400.0 + 1.5, res.variables["datetime"].double_values[3]);
    // At k=0 (xi=-1): T0=1, T1=-1, T2=1, T3=-1 -> 300 - 10 + 1 - 0.5 = 290.5.
    EXPECT_FLOAT_EQ(290.5f, res.variables["wavelength"].float_values[0]);
    // At k=1 (xi=+1): T0=1, T1=1, T2=1, T3=1 -> 300 + 10 + 1 + 0.5 = 311.5.
    EXPECT_FLOAT_EQ(311.5f, res.variables["wavelength"].float_values[1]);
    EXPECT_THAT(res.variables["wavelength_uncertainty"].float_values[0],
                FloatNear(0.2f, 1e-5f));
    EXPECT_THAT(res.variables["wavelength_validity"].int16_values,
                ElementsAre(5, 5, 5, 5));
    EXPECT_TRUE(
        std::isnan(res.variables["photon_radiance_uncertainty_systematic"]
                       .float_values[0]));
    EXPECT_TRUE(
        std::isnan(res.variables["photon_radiance_uncertainty_systematic"]
                       .float_values[7]));
    EXPECT_THAT(
        res.variables["photon_radiance_uncertainty_systematic"].float_values[4],
        FloatNear(100.0f / std::exp(1.0f), 1e-4f));
  }
}

TEST(HarpIngestS5L1bTest, IngestNirSwrAndIrrAllBandsSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  struct ModuleBandCase {
    const char* module_name;
    std::string_view product_id;
    const char* options;
    std::string_view band_name;
    bool is_irr;
  };
  const ModuleBandCase kCases[] = {
      {.module_name = "S5_L1B_NIR",
       .product_id = "SN5_1B_NIR",
       .options = nullptr,
       .band_name = "band3a",
       .is_irr = false},
      {.module_name = "S5_L1B_NIR",
       .product_id = "SN5_1B_NIR",
       .options = "band=3a",
       .band_name = "band3a",
       .is_irr = false},
      {.module_name = "S5_L1B_NIR",
       .product_id = "SN5_1B_NIR",
       .options = "band=3b",
       .band_name = "band3b",
       .is_irr = false},
      {.module_name = "S5_L1B_NIR",
       .product_id = "SN5_1B_NIR",
       .options = "band=3c",
       .band_name = "band3c",
       .is_irr = false},
      {.module_name = "S5_L1B_SWR",
       .product_id = "SN5_1B_SWR",
       .options = "band=4",
       .band_name = "band4",
       .is_irr = false},
      {.module_name = "S5_L1B_SWR",
       .product_id = "SN5_1B_SWR",
       .options = "band=5",
       .band_name = "band5",
       .is_irr = false},
      {.module_name = "S5_L1B_IRR",
       .product_id = "SN5_1B_IRR",
       .options = "band=1a",
       .band_name = "band1a",
       .is_irr = true},
      {.module_name = "S5_L1B_IRR",
       .product_id = "SN5_1B_IRR",
       .options = "band=1b",
       .band_name = "band1b",
       .is_irr = true},
      {.module_name = "S5_L1B_IRR",
       .product_id = "SN5_1B_IRR",
       .options = "band=2",
       .band_name = "band2",
       .is_irr = true},
      {.module_name = "S5_L1B_IRR",
       .product_id = "SN5_1B_IRR",
       .options = "band=3a",
       .band_name = "band3a",
       .is_irr = true},
      {.module_name = "S5_L1B_IRR",
       .product_id = "SN5_1B_IRR",
       .options = "band=3b",
       .band_name = "band3b",
       .is_irr = true},
      {.module_name = "S5_L1B_IRR",
       .product_id = "SN5_1B_IRR",
       .options = "band=3c",
       .band_name = "band3c",
       .is_irr = true},
      {.module_name = "S5_L1B_IRR",
       .product_id = "SN5_1B_IRR",
       .options = "band=4",
       .band_name = "band4",
       .is_irr = true},
      {.module_name = "S5_L1B_IRR",
       .product_id = "SN5_1B_IRR",
       .options = "band=5",
       .band_name = "band5",
       .is_irr = true},
  };

  for (const auto& tc : kCases) {
    const harp_ingestion_module* mod = FindIngestionModule(tc.module_name);
    ASSERT_NE(nullptr, mod) << tc.module_name;

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5L1bConfig cfg;
    cfg.product_id_str = tc.product_id;
    cfg.band_name = tc.band_name;
    cfg.is_irr = tc.is_irr;
    ASSERT_TRUE(BuildS5L1bProduct(prod, cfg));

    IngestExerciseResult res =
        ExerciseS5L1bModule(mod, prod.product(), tc.options);
    ASSERT_EQ(HARP_SUCCESS, res.status)
        << tc.module_name << " " << tc.band_name;
    EXPECT_EQ(4, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(2, res.dimensions[harp_dimension_spectral]);
    if (tc.is_irr) {
      EXPECT_EQ(8u, res.variables["photon_irradiance"].float_values.size());
      EXPECT_EQ(8u, res.variables["photon_irradiance_uncertainty_systematic"]
                        .float_values.size());
      EXPECT_EQ(8u, res.variables["photon_irradiance_uncertainty_random"]
                        .float_values.size());
      EXPECT_EQ(8u,
                res.variables["photon_irradiance_validity"].int8_values.size());
    } else {
      EXPECT_EQ(8u, res.variables["photon_radiance"].float_values.size());
    }
  }
}

TEST(HarpIngestS5L1bTest, InitErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* uvr = FindIngestionModule("S5_L1B_UVR");
  const harp_ingestion_module* irr = FindIngestionModule("S5_L1B_IRR");
  ASSERT_NE(nullptr, uvr);
  ASSERT_NE(nullptr, irr);

  // 1. Missing product identifier attribute.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseS5L1bModule(uvr, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 2. Empty / too-long / wrong-type / unknown product_name.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "product_name", prod.MakeString("")));
    IngestExerciseResult res = ExerciseS5L1bModule(uvr, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString(std::string(300, 'X'))));
    IngestExerciseResult res = ExerciseS5L1bModule(uvr, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "product_name", prod.MakeInt32(123)));
    IngestExerciseResult res = ExerciseS5L1bModule(uvr, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("UNKNOWN_PROD")));
    IngestExerciseResult res = ExerciseS5L1bModule(uvr, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    // Empty array for @product_name fails goto_first_array_element.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kZero[] = {0};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeStringArray(kZero, {})));
    IngestExerciseResult res = ExerciseS5L1bModule(uvr, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Invalid @id attribute (non-string fails coda_cursor_read_string).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_UVR")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "id", prod.MakeInt32(7)));
    IngestExerciseResult res = ExerciseS5L1bModule(uvr, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. Missing band1a / data/band1a and missing subgroups.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_UVR")));
    IngestExerciseResult res = ExerciseS5L1bModule(uvr, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_UVR")));
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", prod.MakeRecord()));
    IngestExerciseResult res = ExerciseS5L1bModule(uvr, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_UVR")));
    CodaMemNode* band = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(band, "geolocation_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));
    IngestExerciseResult res = ExerciseS5L1bModule(uvr, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_UVR")));
    CodaMemNode* band = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(band, "geolocation_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));
    IngestExerciseResult res = ExerciseS5L1bModule(uvr, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 5. Missing/invalid dimensions (scanline, ground_pixel, pixel_corners != 4,
  // 2D dimension field, missing spectral_channel).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_UVR")));
    CodaMemNode* band = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(band, "geolocation_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "observation_data", prod.MakeRecord()));
    // Missing scanline.
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));
    EXPECT_EQ(-1, ExerciseS5L1bModule(uvr, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_UVR")));
    CodaMemNode* band = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(band, "geolocation_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "observation_data", prod.MakeRecord()));
    constexpr long k2D[] = {2, 2};
    constexpr int32_t kVals[] = {0, 0, 0, 0};
    // 2D scanline triggers "Field 'scanline' is not a 1D array".
    ASSERT_TRUE(
        prod.AddField(band, "scanline", prod.MakeInt32Array(k2D, kVals)));
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));
    IngestExerciseResult res = ExerciseS5L1bModule(uvr, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_UVR")));
    CodaMemNode* band = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(band, "geolocation_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "observation_data", prod.MakeRecord()));
    constexpr long kOne[] = {1};
    constexpr int32_t kZero[] = {0};
    ASSERT_TRUE(
        prod.AddField(band, "scanline", prod.MakeInt32Array(kOne, kZero)));
    // Missing ground_pixel.
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));
    EXPECT_EQ(-1, ExerciseS5L1bModule(uvr, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_UVR")));
    CodaMemNode* band = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(band, "geolocation_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "observation_data", prod.MakeRecord()));
    constexpr long kOne[] = {1};
    constexpr int32_t kZero[] = {0};
    ASSERT_TRUE(
        prod.AddField(band, "scanline", prod.MakeInt32Array(kOne, kZero)));
    ASSERT_TRUE(
        prod.AddField(band, "ground_pixel", prod.MakeInt32Array(kOne, kZero)));
    // Missing pixel_corners.
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));
    EXPECT_EQ(-1, ExerciseS5L1bModule(uvr, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    S5L1bConfig cfg;
    cfg.num_corners = 3;  // Expected 4.
    ASSERT_TRUE(BuildS5L1bProduct(prod, cfg));
    IngestExerciseResult res = ExerciseS5L1bModule(uvr, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_IRR")));
    CodaMemNode* band = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(band, "geolocation_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "observation_data", prod.MakeRecord()));
    constexpr long kOne[] = {1};
    constexpr int32_t kZero[] = {0};
    ASSERT_TRUE(
        prod.AddField(band, "scanline", prod.MakeInt32Array(kOne, kZero)));
    ASSERT_TRUE(prod.AddField(band, "pixel", prod.MakeInt32Array(kOne, kZero)));
    // Missing spectral_channel.
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));
    EXPECT_EQ(-1, ExerciseS5L1bModule(irr, prod.product()).status);
  }

  // 6. Missing / mismatched / bad FillValue in init_dataset for radiance,
  // radiance_error, radiance_noise, irradiance, irradiance_error,
  // irradiance_noise.
  const std::string_view kRadVars[] = {"radiance", "radiance_error",
                                       "radiance_noise"};
  for (std::string_view missing_var : kRadVars) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_UVR")));
    CodaMemNode* band = prod.MakeRecord();
    CodaMemNode* obs = prod.MakeRecord();
    constexpr long kDim[] = {2};
    constexpr int32_t kVals[] = {0, 0};
    constexpr long kCorners[] = {4};
    constexpr int32_t kCornerVals[] = {0, 0, 0, 0};
    ASSERT_TRUE(
        prod.AddField(band, "scanline", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(
        prod.AddField(band, "ground_pixel", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(prod.AddField(band, "pixel_corners",
                              prod.MakeInt32Array(kCorners, kCornerVals)));
    ASSERT_TRUE(prod.AddField(band, "spectral_channel",
                              prod.MakeInt32Array(kDim, kVals)));
    const long d_spec[] = {2, 2, 2};
    std::vector<float> f8(8, 1.0f);
    std::vector<int8_t> i8(8, 1);
    if (missing_var != "radiance") {
      ASSERT_TRUE(AddFloatDataset(prod, obs, "radiance", d_spec, f8));
    }
    if (missing_var != "radiance_error") {
      ASSERT_TRUE(AddInt8Dataset(prod, obs, "radiance_error", d_spec, i8));
    }
    if (missing_var != "radiance_noise") {
      ASSERT_TRUE(AddInt8Dataset(prod, obs, "radiance_noise", d_spec, i8));
    }
    ASSERT_TRUE(prod.AddField(band, "geolocation_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "observation_data", obs));
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));
    EXPECT_EQ(-1, ExerciseS5L1bModule(uvr, prod.product()).status);
  }

  // Wrong element count and missing/bad FillValue in init_dataset.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_IRR")));
    CodaMemNode* band = prod.MakeRecord();
    CodaMemNode* obs = prod.MakeRecord();
    constexpr long kDim[] = {2};
    constexpr int32_t kVals[] = {0, 0};
    ASSERT_TRUE(
        prod.AddField(band, "scanline", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(prod.AddField(band, "pixel", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(prod.AddField(band, "spectral_channel",
                              prod.MakeInt32Array(kDim, kVals)));
    // Missing FillValue on irradiance.
    const long d_spec[] = {2, 2, 2};
    std::vector<float> f8(8, 1.0f);
    ASSERT_TRUE(
        prod.AddField(obs, "irradiance", prod.MakeFloatArray(d_spec, f8)));
    ASSERT_TRUE(prod.AddField(band, "geolocation_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "observation_data", obs));
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));
    EXPECT_EQ(-1, ExerciseS5L1bModule(irr, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_IRR")));
    CodaMemNode* band = prod.MakeRecord();
    CodaMemNode* obs = prod.MakeRecord();
    constexpr long kDim[] = {2};
    constexpr int32_t kVals[] = {0, 0};
    ASSERT_TRUE(
        prod.AddField(band, "scanline", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(prod.AddField(band, "pixel", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(prod.AddField(band, "spectral_channel",
                              prod.MakeInt32Array(kDim, kVals)));
    const long d_spec[] = {2, 2, 2};
    std::vector<float> f8(8, 1.0f);
    std::vector<int8_t> i8(8, 1);
    ASSERT_TRUE(AddFloatDataset(prod, obs, "irradiance", d_spec, f8));
    // Missing irradiance_error.
    ASSERT_TRUE(prod.AddField(band, "geolocation_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "observation_data", obs));
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));
    EXPECT_EQ(-1, ExerciseS5L1bModule(irr, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_IRR")));
    CodaMemNode* band = prod.MakeRecord();
    CodaMemNode* obs = prod.MakeRecord();
    constexpr long kDim[] = {2};
    constexpr int32_t kVals[] = {0, 0};
    ASSERT_TRUE(
        prod.AddField(band, "scanline", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(prod.AddField(band, "pixel", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(prod.AddField(band, "spectral_channel",
                              prod.MakeInt32Array(kDim, kVals)));
    const long d_spec[] = {2, 2, 2};
    std::vector<float> f8(8, 1.0f);
    std::vector<int8_t> i8(8, 1);
    ASSERT_TRUE(AddFloatDataset(prod, obs, "irradiance", d_spec, f8));
    ASSERT_TRUE(AddInt8Dataset(prod, obs, "irradiance_error", d_spec, i8));
    // Missing irradiance_noise.
    ASSERT_TRUE(prod.AddField(band, "geolocation_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "observation_data", obs));
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));
    EXPECT_EQ(-1, ExerciseS5L1bModule(irr, prod.product()).status);
  }
  {
    // Wrong element count in init_dataset + non-float FillValue.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_IRR")));
    CodaMemNode* band = prod.MakeRecord();
    CodaMemNode* obs = prod.MakeRecord();
    constexpr long kDim[] = {2};
    constexpr int32_t kVals[] = {0, 0};
    ASSERT_TRUE(
        prod.AddField(band, "scanline", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(prod.AddField(band, "pixel", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(prod.AddField(band, "spectral_channel",
                              prod.MakeInt32Array(kDim, kVals)));
    const long d_bad[] = {2};
    const float f2[] = {1.0f, 2.0f};
    ASSERT_TRUE(AddFloatDataset(prod, obs, "irradiance", d_bad, f2));
    ASSERT_TRUE(prod.AddField(band, "geolocation_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "observation_data", obs));
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));
    IngestExerciseResult res = ExerciseS5L1bModule(irr, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_IRR")));
    CodaMemNode* band = prod.MakeRecord();
    CodaMemNode* obs = prod.MakeRecord();
    constexpr long kDim[] = {2};
    constexpr int32_t kVals[] = {0, 0};
    ASSERT_TRUE(
        prod.AddField(band, "scanline", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(prod.AddField(band, "pixel", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(prod.AddField(band, "spectral_channel",
                              prod.MakeInt32Array(kDim, kVals)));
    const long d_spec[] = {2, 2, 2};
    std::vector<float> f8(8, 1.0f);
    CodaMemNode* arr = prod.MakeFloatArray(d_spec, f8);
    constexpr long kOne[] = {1};
    const std::string_view bad_fill[] = {"nan"};
    ASSERT_TRUE(prod.AddAttribute(arr, "FillValue",
                                  prod.MakeStringArray(kOne, bad_fill)));
    ASSERT_TRUE(prod.AddField(obs, "irradiance", arr));
    ASSERT_TRUE(prod.AddField(band, "geolocation_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "observation_data", obs));
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));
    IngestExerciseResult res = ExerciseS5L1bModule(irr, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

TEST(HarpIngestS5L1bTest, VariableReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* uvr = FindIngestionModule("S5_L1B_UVR");
  const harp_ingestion_module* irr = FindIngestionModule("S5_L1B_IRR");
  ASSERT_NE(nullptr, uvr);
  ASSERT_NE(nullptr, irr);

  // 1. Product with only the 3 required init datasets (radiance,
  // radiance_error, radiance_noise) and empty geolocation/instrument groups,
  // exercising all variable read failures when stop_on_error=false.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_UVR")));
    CodaMemNode* band = prod.MakeRecord();
    CodaMemNode* obs = prod.MakeRecord();
    constexpr long kDim[] = {2};
    constexpr int32_t kVals[] = {0, 0};
    constexpr long kCorners[] = {4};
    constexpr int32_t kCornerVals[] = {0, 0, 0, 0};
    ASSERT_TRUE(
        prod.AddField(band, "scanline", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(
        prod.AddField(band, "ground_pixel", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(prod.AddField(band, "pixel_corners",
                              prod.MakeInt32Array(kCorners, kCornerVals)));
    ASSERT_TRUE(prod.AddField(band, "spectral_channel",
                              prod.MakeInt32Array(kDim, kVals)));
    const long d_spec[] = {2, 2, 2};
    std::vector<float> f8(8, 1.0f);
    std::vector<int8_t> i8(8, 1);
    ASSERT_TRUE(AddFloatDataset(prod, obs, "radiance", d_spec, f8));
    ASSERT_TRUE(AddInt8Dataset(prod, obs, "radiance_error", d_spec, i8));
    ASSERT_TRUE(AddInt8Dataset(prod, obs, "radiance_noise", d_spec, i8));
    ASSERT_TRUE(prod.AddField(band, "geolocation_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "observation_data", obs));
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));

    IngestExerciseResult res = ExerciseS5L1bModule(
        uvr, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_latitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_longitude"].read_status);
    EXPECT_EQ(-1, res.variables["solar_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["solar_azimuth_angle"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_zenith_angle"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_azimuth_angle"].read_status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
    EXPECT_EQ(HARP_SUCCESS, res.variables["photon_radiance"].read_status);
    EXPECT_EQ(-1, res.variables["photon_radiance_validity"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["wavelength_validity"].read_status);
  }

  // 2. Wrong types / element counts / missing @FillValue during read_dataset,
  // read_orbit_index, read_datetime, read_datetime_length.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_IRR")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "orbit_start",
                                  prod.MakeString("bad_orbit")));
    CodaMemNode* band = prod.MakeRecord();
    CodaMemNode* geoloc = prod.MakeRecord();
    CodaMemNode* inst = prod.MakeRecord();
    CodaMemNode* obs = prod.MakeRecord();
    constexpr long kDim[] = {2};
    constexpr int32_t kVals[] = {0, 0};
    ASSERT_TRUE(
        prod.AddField(band, "scanline", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(prod.AddField(band, "pixel", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(prod.AddField(band, "spectral_channel",
                              prod.MakeInt32Array(kDim, kVals)));
    const long d_spec[] = {2, 2, 2};
    std::vector<float> f8(8, 1.0f);
    std::vector<int8_t> i8(8, 1);
    ASSERT_TRUE(AddFloatDataset(prod, obs, "irradiance", d_spec, f8));
    ASSERT_TRUE(AddInt8Dataset(prod, obs, "irradiance_error", d_spec, i8));
    ASSERT_TRUE(AddInt8Dataset(prod, obs, "irradiance_noise", d_spec, i8));

    // Wrong element count for satellite_altitude, string type for
    // measurement_quality and spectral_channel_quality.
    constexpr long kOne[] = {1};
    constexpr float kAlt1[] = {800000.0f};
    ASSERT_TRUE(
        AddFloatDataset(prod, geoloc, "satellite_altitude", kOne, kAlt1));
    // Missing FillValue on satellite_latitude.
    constexpr float kLat2[] = {0.0f, 1.0f};
    ASSERT_TRUE(prod.AddField(geoloc, "satellite_latitude",
                              prod.MakeFloatArray(kDim, kLat2)));
    // Non-float satellite_longitude.
    constexpr std::string_view kBadStr2[] = {"a", "b"};
    ASSERT_TRUE(AddFloatDataset(
        prod, obs, "time", kOne,
        kAlt1));  // wrong size/type check? No, time expects double[1]
    ASSERT_TRUE(prod.AddField(obs, "measurement_quality",
                              prod.MakeStringArray(kDim, kBadStr2)));
    const std::string_view kBadStr8[8] = {"a", "b", "c", "d",
                                          "e", "f", "g", "h"};
    ASSERT_TRUE(prod.AddField(obs, "spectral_channel_quality",
                              prod.MakeStringArray(d_spec, kBadStr8)));
    // Non-double delta_time (1 element of string) triggers read_double error
    // in read_datetime_length and read_double_array error in read_datetime.
    const std::string_view kBadDelta[] = {"bad"};
    ASSERT_TRUE(prod.AddField(obs, "delta_time",
                              prod.MakeStringArray(kOne, kBadDelta)));
    ASSERT_TRUE(prod.AddField(geoloc, "satellite_longitude",
                              prod.MakeStringArray(kDim, kBadStr2)));

    ASSERT_TRUE(prod.AddField(band, "geolocation_data", geoloc));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", inst));
    ASSERT_TRUE(prod.AddField(band, "observation_data", obs));
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));

    IngestExerciseResult res = ExerciseS5L1bModule(
        irr, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_latitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_longitude"].read_status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
    EXPECT_EQ(-1, res.variables["photon_irradiance_validity"].read_status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
  }

  // 3. Additional edge cases: time valid + delta_time 1 element (fails
  // read_datetime and read_datetime_length); empty orbit_start array; scalar
  // int8/int16 fields failing goto_first_array_element.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_IRR")));
    constexpr long kZero[] = {0};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "orbit_start",
                                  prod.MakeInt32Array(kZero, {})));
    CodaMemNode* band = prod.MakeRecord();
    CodaMemNode* obs = prod.MakeRecord();
    constexpr long kDim[] = {2};
    constexpr int32_t kVals[] = {0, 0};
    ASSERT_TRUE(
        prod.AddField(band, "scanline", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(prod.AddField(band, "pixel", prod.MakeInt32Array(kDim, kVals)));
    ASSERT_TRUE(prod.AddField(band, "spectral_channel",
                              prod.MakeInt32Array(kDim, kVals)));
    const long d_spec[] = {2, 2, 2};
    std::vector<float> f8(8, 1.0f);
    std::vector<int8_t> i8(8, 1);
    ASSERT_TRUE(AddFloatDataset(prod, obs, "irradiance", d_spec, f8));
    ASSERT_TRUE(AddInt8Dataset(prod, obs, "irradiance_error", d_spec, i8));
    ASSERT_TRUE(AddInt8Dataset(prod, obs, "irradiance_noise", d_spec, i8));
    constexpr long kOne[] = {1};
    constexpr double kTime1[] = {10.0};
    // time without FillValue attribute fails read_dataset(harp_type_double).
    ASSERT_TRUE(prod.AddField(obs, "time", prod.MakeDoubleArray(kOne, kTime1)));
    // 1-element delta_time fails goto_next_array_element in
    // read_datetime_length and fails element count check in read_datetime.
    ASSERT_TRUE(AddDoubleDataset(prod, obs, "delta_time", kOne, kTime1));

    ASSERT_TRUE(prod.AddField(band, "geolocation_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "observation_data", obs));
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));

    IngestExerciseResult res = ExerciseS5L1bModule(
        irr, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
  }

  // 4. Cover string FillValue on float/double datasets, empty delta_time,
  // scalar int16 measurement_quality, and decode_uncertainty / radiance /
  // irradiance read errors.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_UVR")));
    CodaMemNode* band = prod.MakeRecord();
    CodaMemNode* geoloc = prod.MakeRecord();
    CodaMemNode* obs = prod.MakeRecord();
    constexpr long kOne[] = {1};
    constexpr long kTwo[] = {2};
    constexpr long kZero[] = {0};
    constexpr long kFour[] = {4};
    constexpr int32_t kZero1[] = {0};
    constexpr int32_t kZero2[] = {0, 0};
    constexpr int32_t kZero4[] = {0, 0, 0, 0};
    ASSERT_TRUE(
        prod.AddField(band, "scanline", prod.MakeInt32Array(kOne, kZero1)));
    ASSERT_TRUE(
        prod.AddField(band, "ground_pixel", prod.MakeInt32Array(kOne, kZero1)));
    ASSERT_TRUE(prod.AddField(band, "pixel_corners",
                              prod.MakeInt32Array(kFour, kZero4)));
    ASSERT_TRUE(prod.AddField(band, "spectral_channel",
                              prod.MakeInt32Array(kTwo, kZero2)));

    // String FillValue on float satellite_latitude and double time.
    constexpr float kSatLat1[] = {10.0f};
    constexpr double kTime1[] = {10.0};
    constexpr std::string_view kBadFill[] = {"nan"};
    CodaMemNode* sat_lat = prod.MakeFloatArray(kOne, kSatLat1);
    ASSERT_TRUE(prod.AddAttribute(sat_lat, "FillValue",
                                  prod.MakeStringArray(kOne, kBadFill)));
    ASSERT_TRUE(prod.AddField(geoloc, "satellite_latitude", sat_lat));
    CodaMemNode* time_node = prod.MakeDoubleArray(kOne, kTime1);
    ASSERT_TRUE(prod.AddAttribute(time_node, "FillValue",
                                  prod.MakeStringArray(kOne, kBadFill)));
    ASSERT_TRUE(prod.AddField(obs, "time", time_node));
    // Empty delta_time fails goto_first_array_element in read_datetime_length.
    ASSERT_TRUE(
        prod.AddField(obs, "delta_time", prod.MakeDoubleArray(kZero, {})));
    // Scalar int16 measurement_quality has 1 element but is not an array.
    ASSERT_TRUE(prod.AddField(obs, "measurement_quality", prod.MakeInt16(1)));

    // String radiance + radiance_error with float FillValue passes init_dataset
    // and fails read_observation_radiance and decode_uncertainty.
    const long d_spec[] = {1, 1, 2};
    constexpr std::string_view kBadSpec[] = {"a", "b"};
    constexpr float kFillF[] = {0.0f};
    CodaMemNode* rad = prod.MakeStringArray(d_spec, kBadSpec);
    ASSERT_TRUE(
        prod.AddAttribute(rad, "FillValue", prod.MakeFloatArray(kOne, kFillF)));
    ASSERT_TRUE(prod.AddField(obs, "radiance", rad));
    CodaMemNode* rad_err = prod.MakeStringArray(d_spec, kBadSpec);
    ASSERT_TRUE(prod.AddAttribute(rad_err, "FillValue",
                                  prod.MakeFloatArray(kOne, kFillF)));
    ASSERT_TRUE(prod.AddField(obs, "radiance_error", rad_err));
    constexpr int8_t kNoise[] = {1, 1};
    ASSERT_TRUE(AddInt8Dataset(prod, obs, "radiance_noise", d_spec, kNoise));

    ASSERT_TRUE(prod.AddField(band, "geolocation_data", geoloc));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "observation_data", obs));
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));

    IngestExerciseResult res = ExerciseS5L1bModule(
        uvr, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["sensor_latitude"].read_status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
    EXPECT_EQ(-1, res.variables["photon_radiance"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["photon_radiance_uncertainty_systematic"].read_status);
    EXPECT_EQ(-1,
              res.variables["photon_radiance_uncertainty_random"].read_status);
  }
  {
    // String irradiance with float FillValue fails read_observation_irradiance.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "product_name",
                                  prod.MakeString("SN5_1B_IRR")));
    CodaMemNode* band = prod.MakeRecord();
    CodaMemNode* obs = prod.MakeRecord();
    constexpr long kOne[] = {1};
    constexpr long kTwo[] = {2};
    constexpr int32_t kZero1[] = {0};
    constexpr int32_t kZero2[] = {0, 0};
    ASSERT_TRUE(
        prod.AddField(band, "scanline", prod.MakeInt32Array(kOne, kZero1)));
    ASSERT_TRUE(
        prod.AddField(band, "pixel", prod.MakeInt32Array(kOne, kZero1)));
    ASSERT_TRUE(prod.AddField(band, "spectral_channel",
                              prod.MakeInt32Array(kTwo, kZero2)));
    const long d_spec[] = {1, 1, 2};
    constexpr std::string_view kBadSpec[] = {"a", "b"};
    constexpr float kFillF[] = {0.0f};
    CodaMemNode* irr_arr = prod.MakeStringArray(d_spec, kBadSpec);
    ASSERT_TRUE(prod.AddAttribute(irr_arr, "FillValue",
                                  prod.MakeFloatArray(kOne, kFillF)));
    ASSERT_TRUE(prod.AddField(obs, "irradiance", irr_arr));
    constexpr int8_t kI8[] = {1, 1};
    ASSERT_TRUE(AddInt8Dataset(prod, obs, "irradiance_error", d_spec, kI8));
    ASSERT_TRUE(AddInt8Dataset(prod, obs, "irradiance_noise", d_spec, kI8));
    ASSERT_TRUE(prod.AddField(band, "geolocation_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "instrument_data", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(band, "observation_data", obs));
    ASSERT_TRUE(prod.AddField(prod.root(), "band1a", band));

    IngestExerciseResult res = ExerciseS5L1bModule(
        irr, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["photon_irradiance"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
