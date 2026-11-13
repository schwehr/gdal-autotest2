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
// Unit tests for Sentinel-5 L2 ingestion modules (libharp/harp-ingest-s5_l2.c).
//
// Google3 build notes: HDF4 is disabled (#undef HAVE_HDF4); CODA products are
// synthesized in memory via CodaMemProduct without .codadef files.
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-s5_l2.c

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
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

using ::testing::ElementsAre;
using ::testing::FloatNear;
using ::testing::HasSubstr;

constexpr long kScanlines = 2;
constexpr long kPixels = 2;
constexpr long kCorners = 4;
constexpr long kLayers = 3;
constexpr long kSifWavelengths = 4;
constexpr long kWavelengths = 2;

// Local S5 exercise helper mirroring ExerciseIngestionModule, except that for
// "datetime" it allocates one extra double element to accommodate the upstream
// off-by-one loop (`for (j = 0; j <= info->num_pixels; j++)`) in
// read_datetime(), preventing a heap-buffer-overflow under ASan/MSan.
IngestExerciseResult ExerciseS5Module(const harp_ingestion_module* mod,
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
      case harp_type_string:
        break;
    }
    result.variables.emplace(var_def->name, std::move(var));
  }
  return result;
}

template <typename T>
std::vector<T> MakeSeq(long count, T start, T step) {
  std::vector<T> v(static_cast<size_t>(count));
  for (long i = 0; i < count; ++i) {
    v[static_cast<size_t>(i)] =
        static_cast<T>(start + static_cast<T>(i) * step);
  }
  return v;
}

bool AddFloatDataset(CodaMemProduct& prod, CodaMemNode* rec,
                     std::string_view name, std::span<const long> dims,
                     std::span<const float> values, float fill_val = -999.0f) {
  CodaMemNode* arr = prod.MakeFloatArray(dims, values);
  if (arr == nullptr) {
    return false;
  }
  constexpr long kOneDim[] = {1};
  const float fill_arr[] = {fill_val};
  if (!prod.AddAttribute(arr, "FillValue",
                         prod.MakeFloatArray(kOneDim, fill_arr))) {
    return false;
  }
  return prod.AddField(rec, name, arr);
}

bool AddDoubleDataset(CodaMemProduct& prod, CodaMemNode* rec,
                      std::string_view name, std::span<const long> dims,
                      std::span<const double> values,
                      double fill_val = -999.0) {
  CodaMemNode* arr = prod.MakeDoubleArray(dims, values);
  if (arr == nullptr) {
    return false;
  }
  constexpr long kOneDim[] = {1};
  const double fill_arr[] = {fill_val};
  if (!prod.AddAttribute(arr, "FillValue",
                         prod.MakeDoubleArray(kOneDim, fill_arr))) {
    return false;
  }
  return prod.AddField(rec, name, arr);
}

bool AddUniformFloat(CodaMemProduct& prod, CodaMemNode* rec,
                     std::string_view name, std::span<const long> dims,
                     float start, float step = 1.0f, float fill_val = -999.0f) {
  long n = 1;
  for (long d : dims) {
    n *= d;
  }
  return AddFloatDataset(prod, rec, name, dims, MakeSeq<float>(n, start, step),
                         fill_val);
}

bool AddUniformDouble(CodaMemProduct& prod, CodaMemNode* rec,
                      std::string_view name, std::span<const long> dims,
                      double start, double step = 1.0,
                      double fill_val = -999.0) {
  long n = 1;
  for (long d : dims) {
    n *= d;
  }
  return AddDoubleDataset(prod, rec, name, dims,
                          MakeSeq<double>(n, start, step), fill_val);
}

struct S5Groups {
  CodaMemNode* product = nullptr;
  CodaMemNode* support = nullptr;
  CodaMemNode* geoloc = nullptr;
  CodaMemNode* detailed = nullptr;
  CodaMemNode* input = nullptr;
};

S5Groups CreateBandGroup(CodaMemProduct& prod, CodaMemNode* parent,
                         std::string_view product_field_name,
                         bool with_detailed_and_input) {
  S5Groups g;
  g.product = prod.MakeRecord();
  g.support = prod.MakeRecord();
  g.geoloc = prod.MakeRecord();
  EXPECT_TRUE(prod.AddField(parent, product_field_name, g.product));
  EXPECT_TRUE(prod.AddField(g.product, "SUPPORT_DATA", g.support));
  EXPECT_TRUE(prod.AddField(g.support, "GEOLOCATIONS", g.geoloc));
  if (with_detailed_and_input) {
    g.detailed = prod.MakeRecord();
    g.input = prod.MakeRecord();
    EXPECT_TRUE(prod.AddField(g.support, "DETAILED_RESULTS", g.detailed));
    EXPECT_TRUE(prod.AddField(g.support, "INPUT_DATA", g.input));
  }
  return g;
}

S5Groups CreateStandardGroups(CodaMemProduct& prod) {
  CodaMemNode* data_rec = prod.MakeRecord();
  EXPECT_TRUE(prod.AddField(prod.root(), "data", data_rec));
  return CreateBandGroup(prod, data_rec, "PRODUCT",
                         /*with_detailed_and_input=*/true);
}

void PopulateDimensionsAndCore(CodaMemProduct& prod, const S5Groups& g,
                               long num_scanlines, long num_pixels,
                               long num_layers, long num_sif,
                               long num_wavelengths, bool uint_qa = false) {
  const long s_dim[] = {num_scanlines};
  const long p_dim[] = {num_pixels};
  const long c_dim[] = {kCorners};
  const long sp_dim[] = {num_scanlines * num_pixels};

  ASSERT_TRUE(prod.AddField(
      g.product, "scanline",
      prod.MakeInt32Array(s_dim, MakeSeq<int32_t>(num_scanlines, 0, 1))));
  ASSERT_TRUE(prod.AddField(
      g.product, "ground_pixel",
      prod.MakeInt32Array(p_dim, MakeSeq<int32_t>(num_pixels, 0, 1))));
  ASSERT_TRUE(prod.AddField(
      g.product, "corner",
      prod.MakeInt32Array(c_dim, MakeSeq<int32_t>(kCorners, 0, 1))));

  if (num_layers > 0) {
    const long l_dim[] = {num_layers};
    ASSERT_TRUE(prod.AddField(
        g.product, "layer",
        prod.MakeInt32Array(l_dim, MakeSeq<int32_t>(num_layers, 0, 1))));
  }
  if (num_sif > 0) {
    const long sif_dim[] = {num_sif};
    ASSERT_TRUE(prod.AddField(
        g.product, "sif_wavelengths",
        prod.MakeInt32Array(sif_dim, MakeSeq<int32_t>(num_sif, 0, 1))));
  }
  if (num_wavelengths > 0) {
    const long w_dim[] = {num_wavelengths};
    ASSERT_TRUE(
        AddUniformFloat(prod, g.product, "wavelength", w_dim, 340.0f, 100.0f));
  }

  constexpr long kTimeDim[] = {1};
  constexpr double kTimeVal[] = {10.0};
  ASSERT_TRUE(AddDoubleDataset(prod, g.product, "time", kTimeDim, kTimeVal));
  ASSERT_TRUE(AddUniformDouble(prod, g.product, "delta_time", s_dim, 0.0, 2.5));

  const long n_sp = num_scanlines * num_pixels;
  if (uint_qa) {
    ASSERT_TRUE(prod.AddField(
        g.product, "qa_value",
        prod.MakeUint32Array(sp_dim, MakeSeq<uint32_t>(n_sp, 80, 5))));
  } else {
    ASSERT_TRUE(prod.AddField(
        g.product, "qa_value",
        prod.MakeInt32Array(sp_dim, MakeSeq<int32_t>(n_sp, 80, 5))));
  }
  ASSERT_TRUE(prod.AddField(
      g.product, "processing_quality_flags",
      prod.MakeUint64Array(sp_dim, MakeSeq<uint64_t>(n_sp, 0, 1))));
}

void PopulateGeolocation(CodaMemProduct& prod, const S5Groups& g,
                         long num_scanlines, long num_pixels) {
  const long s_dim[] = {num_scanlines};
  const long sp_dim[] = {num_scanlines * num_pixels};
  const long spc_dim[] = {num_scanlines * num_pixels * kCorners};

  ASSERT_TRUE(AddUniformFloat(prod, g.geoloc, "latitude", sp_dim, 10.0f, 1.0f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.geoloc, "longitude", sp_dim, 20.0f, 1.0f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.geoloc, "latitude_bounds", spc_dim, 9.5f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(prod, g.geoloc, "longitude_bounds", spc_dim,
                              19.5f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(prod, g.geoloc, "satellite_latitude", s_dim,
                              11.0f, 1.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.geoloc, "satellite_longitude", s_dim,
                              21.0f, 1.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.geoloc, "satellite_altitude", s_dim,
                              820000.0f, 10.0f));
  ASSERT_TRUE(AddUniformDouble(prod, g.geoloc, "satellite_orbit_phase", s_dim,
                               0.1, 0.2));
  ASSERT_TRUE(AddUniformFloat(prod, g.geoloc, "solar_zenith_angle", sp_dim,
                              30.0f, 1.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.geoloc, "solar_azimuth_angle", sp_dim,
                              40.0f, 1.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.geoloc, "viewing_zenith_angle", sp_dim,
                              15.0f, 1.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.geoloc, "viewing_azimuth_angle", sp_dim,
                              50.0f, 1.0f));
}

void PopulateSurfaceAndSnow(CodaMemProduct& prod, const S5Groups& g,
                            long num_scanlines, long num_pixels,
                            std::string_view alt_prec_name,
                            bool uint_surface = false, bool double_sp = false) {
  const long n_sp = num_scanlines * num_pixels;
  const long sp_dim[] = {n_sp};

  ASSERT_TRUE(
      AddUniformFloat(prod, g.input, "surface_altitude", sp_dim, 100.0f, 5.0f));
  if (!alt_prec_name.empty()) {
    ASSERT_TRUE(
        AddUniformFloat(prod, g.input, alt_prec_name, sp_dim, 2.0f, 0.5f));
  }
  if (double_sp) {
    ASSERT_TRUE(AddUniformDouble(prod, g.input, "surface_pressure", sp_dim,
                                 101325.0, 10.0));
  } else {
    ASSERT_TRUE(AddUniformFloat(prod, g.input, "surface_pressure", sp_dim,
                                101325.0f, 10.0f));
  }

  if (uint_surface) {
    ASSERT_TRUE(prod.AddField(
        g.input, "surface_classification",
        prod.MakeUint8Array(sp_dim, MakeSeq<uint8_t>(n_sp, 1, 1))));
    // Populate snow_ice_flag as uint8 with FillValue attribute.
    std::vector<uint8_t> flags(static_cast<size_t>(n_sp), 0);
    if (n_sp >= 4) {
      flags[0] = 0;
      flags[1] = 50;
      flags[2] = 101;
      flags[3] = 255;
    }
    CodaMemNode* flag_node = prod.MakeUint8Array(sp_dim, flags);
    constexpr long kOneDim[] = {1};
    constexpr float kFill[] = {254.0f};
    ASSERT_TRUE(prod.AddAttribute(flag_node, "FillValue",
                                  prod.MakeFloatArray(kOneDim, kFill)));
    ASSERT_TRUE(prod.AddField(g.input, "snow_ice_flag", flag_node));
  } else {
    ASSERT_TRUE(
        prod.AddField(g.input, "surface_classification",
                      prod.MakeInt8Array(sp_dim, MakeSeq<int8_t>(n_sp, 0, 1))));
    std::vector<int8_t> flags(static_cast<size_t>(n_sp), 0);
    if (n_sp >= 8) {
      flags[0] = 0;    // snow_free_land (0)
      flags[1] = 50;   // sea_ice (1), frac 0.5
      flags[2] = 100;  // sea_ice (1), frac 1.0
      flags[3] = 101;  // permanent_ice (2)
      flags[4] = 102;  // invalid (-1)
      flags[5] = 103;  // snow (3)
      flags[6] = -1;   // ocean (4) (255)
      flags[7] = -2;   // invalid (-1)
    }
    CodaMemNode* flag_node = prod.MakeInt8Array(sp_dim, flags);
    constexpr long kOneDim[] = {1};
    constexpr float kFill[] = {127.0f};
    ASSERT_TRUE(prod.AddAttribute(flag_node, "FillValue",
                                  prod.MakeFloatArray(kOneDim, kFill)));
    ASSERT_TRUE(prod.AddField(g.input, "snow_ice_flag", flag_node));
  }
}

void PopulatePressureCoeffs(CodaMemProduct& prod, const S5Groups& g,
                            long num_layers, bool clamp_toa) {
  const long lvl_dim[] = {num_layers + 1};
  std::vector<double> coef_a(static_cast<size_t>(num_layers + 1), 100.0);
  std::vector<double> coef_b(static_cast<size_t>(num_layers + 1), 0.1);
  if (clamp_toa) {
    // Force toa_idx ((num_layers - 1) * 2 -> coef_a[num_layers - 1],
    // coef_b[num_layers - 1]) below 1e-3 to cover TOA clamping.
    coef_a[static_cast<size_t>(num_layers - 1)] = 0.0;
    coef_b[static_cast<size_t>(num_layers - 1)] = 0.0;
  }
  ASSERT_TRUE(AddDoubleDataset(prod, g.input, "pressure_coefficient_a", lvl_dim,
                               coef_a));
  ASSERT_TRUE(AddDoubleDataset(prod, g.input, "pressure_coefficient_b", lvl_dim,
                               coef_b));
}

void BuildAuiProduct(CodaMemProduct& prod, std::string_view lo,
                     std::string_view hi) {
  ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_AUI", 1));
  ASSERT_TRUE(
      prod.AddAttribute(prod.root(), "orbit_start", prod.MakeUint32(1234)));

  S5Groups g = CreateStandardGroups(prod);
  // Use 2x4 pixels so all snow_ice_flag branches are covered in one product.
  constexpr long kAuiPixels = 4;
  PopulateDimensionsAndCore(prod, g, kScanlines, kAuiPixels, 0, 0, 0);
  PopulateGeolocation(prod, g, kScanlines, kAuiPixels);
  PopulateSurfaceAndSnow(prod, g, kScanlines, kAuiPixels,
                         "surface_altitude_precision");

  const long sp_dim[] = {kScanlines * kAuiPixels};
  const std::string ai_name =
      "aerosol_index_" + std::string(lo) + "_" + std::string(hi);
  const std::string ai_prec = ai_name + "_precision";
  const std::string refl_lo = "reflectance_" + std::string(lo) + "_measured";
  const std::string refl_hi = "reflectance_" + std::string(hi) + "_measured";
  const std::string rprec_lo =
      "reflectance_precision_" + std::string(lo) + "_measured";
  const std::string rprec_hi =
      "reflectance_precision_" + std::string(hi) + "_measured";
  const std::string albedo = "scene_albedo_" + std::string(hi);

  ASSERT_TRUE(AddUniformFloat(prod, g.product, ai_name, sp_dim, 0.5f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product, ai_prec, sp_dim, 0.05f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, refl_lo, sp_dim, 0.2f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, refl_hi, sp_dim, 0.3f, 0.01f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.detailed, rprec_lo, sp_dim, 0.01f, 0.001f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.detailed, rprec_hi, sp_dim, 0.02f, 0.001f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, albedo, sp_dim, 0.4f, 0.02f));
}

TEST(HarpIngestS5L2Test, IngestAuiAllWavelengthRatiosAndSnowFlags) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5_L2_AUI");
  ASSERT_NE(nullptr, mod);

  struct RatioCase {
    const char* option;
    std::string_view lo;
    std::string_view hi;
  };
  constexpr RatioCase kCases[] = {
      {.option = nullptr, .lo = "354", .hi = "388"},
      {.option = "wavelength_ratio=354_388nm", .lo = "354", .hi = "388"},
      {.option = "wavelength_ratio=340_380nm", .lo = "340", .hi = "380"},
      {.option = "wavelength_ratio=335_367nm", .lo = "335", .hi = "367"},
  };

  for (const auto& tc : kCases) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    BuildAuiProduct(prod, tc.lo, tc.hi);

    IngestExerciseResult res = ExerciseS5Module(mod, prod.product(), tc.option);
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_EQ(8L, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(2L, res.dimensions[harp_dimension_spectral]);
    EXPECT_EQ(1234, res.variables["orbit_index"].int32_values[0]);
    EXPECT_THAT(res.variables["scan_subindex"].int16_values,
                ElementsAre(0, 1, 2, 3, 0, 1, 2, 3));
    EXPECT_NEAR(10.0 * 86400.0, res.variables["datetime"].double_values[0],
                1e-6);
    // Upstream read_datetime() loops j <= num_pixels, so scanline 0 overwrites
    // index num_pixels (4) with scanline 0's timestamp; index 5 holds
    // scanline 1.
    EXPECT_NEAR(10.0 * 86400.0 + 2.5,
                res.variables["datetime"].double_values[5], 1e-6);
    EXPECT_NEAR(2.5, res.variables["datetime_length"].double_values[0], 1e-6);
    // Upstream snow_ice_type is registered as harp_type_int32, while
    // read_snow_ice_type_from_flag() writes int8_t bytes into data.int8_data.
    const int8_t* raw_snow_bytes = reinterpret_cast<const int8_t*>(
        res.variables["snow_ice_type"].int32_values.data());
    EXPECT_THAT(std::vector<int8_t>(raw_snow_bytes, raw_snow_bytes + 8),
                ElementsAre(0, 1, 1, 2, -1, 3, 4, -1));
    EXPECT_THAT(res.variables["sea_ice_fraction"].float_values,
                ElementsAre(0.0f, 0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f));
    EXPECT_FLOAT_EQ(0.2f, res.variables["reflectance"].float_values[0]);
    EXPECT_FLOAT_EQ(0.3f, res.variables["reflectance"].float_values[8]);
  }
}

void BuildCh4Product(CodaMemProduct& prod, std::string_view ch4_suffix,
                     std::string_view albedo_field) {
  ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_CH4", 1));
  constexpr long kOrbitDim[] = {1};
  constexpr int32_t kOrbitVal[] = {4321};
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "orbit",
                                prod.MakeInt32Array(kOrbitDim, kOrbitVal)));

  S5Groups g = CreateStandardGroups(prod);
  PopulateDimensionsAndCore(prod, g, kScanlines, kPixels, kLayers,
                            kSifWavelengths, 0);
  PopulateGeolocation(prod, g, kScanlines, kPixels);
  PopulateSurfaceAndSnow(prod, g, kScanlines, kPixels,
                         "surface_altitude_precision");

  const long sp_dim[] = {kScanlines * kPixels};
  const long spl_dim[] = {kScanlines * kPixels * kLayers};
  const long spsif_dim[] = {kScanlines * kPixels * kSifWavelengths};

  ASSERT_TRUE(AddUniformFloat(
      prod, g.product,
      "methane_dry_air_column_mixing_ratio_" + std::string(ch4_suffix), sp_dim,
      1800.0f, 10.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product,
                              "methane_dry_air_column_mixing_ratio_precision_" +
                                  std::string(ch4_suffix),
                              sp_dim, 5.0f, 0.5f));

  ASSERT_TRUE(
      AddUniformFloat(prod, g.detailed, "pressure", spl_dim, 100.0f, 100.0f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.detailed, "altitude", spl_dim, 1000.0f, 500.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "dry_air_column", sp_dim,
                              3.5e5f, 100.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "methane_profile_apriori",
                              spl_dim, 1.0f, 1.0f));
  ASSERT_TRUE(AddUniformFloat(
      prod, g.detailed, "carbon_dioxide_profile_apriori", spl_dim, 2.0f, 1.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "oxygen_total_column_apriori",
                              sp_dim, 70000.0f, 10.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "water_total_column_apriori",
                              sp_dim, 500.0f, 10.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "solar_induced_fluorescence",
                              spsif_dim, 0.1f, 0.05f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "methane_total_column_averaging_kernel", spl_dim,
                              0.5f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "water_total_column", sp_dim,
                              600.0f, 5.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "carbon_dioxide_total_column",
                              sp_dim, 1.4e4f, 10.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "carbon_monoxide_total_column",
                              sp_dim, 0.03f, 0.001f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.detailed, "aerosol_size", sp_dim, 1e-6f, 1e-7f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "aerosol_particle_column",
                              sp_dim, 1e8f, 1e6f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "aerosol_layer_height", sp_dim,
                              2000.0f, 50.0f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.detailed, albedo_field, sp_dim, 0.25f, 0.01f));
}

TEST(HarpIngestS5L2Test, IngestCh4OptionsAndProfiles) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5_L2_CH4");
  ASSERT_NE(nullptr, mod);

  struct Ch4Case {
    const char* option;
    std::string_view ch4_suffix;
    std::string_view albedo_field;
  };
  constexpr Ch4Case kCases[] = {
      {.option = nullptr,
       .ch4_suffix = "physics",
       .albedo_field = "surface_albedo_swir_1"},
      {.option = "ch4=physics;band=SWIR-1",
       .ch4_suffix = "physics",
       .albedo_field = "surface_albedo_swir_1"},
      {.option = "ch4=proxy;band=SWIR-3",
       .ch4_suffix = "proxy",
       .albedo_field = "surface_albedo_swir_3"},
      {.option = "band=NIR-2",
       .ch4_suffix = "physics",
       .albedo_field = "surface_albedo_nir_2"},
  };

  for (const auto& tc : kCases) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    BuildCh4Product(prod, tc.ch4_suffix, tc.albedo_field);

    IngestExerciseResult res = ExerciseS5Module(mod, prod.product(), tc.option);
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_EQ(4L, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(kLayers, res.dimensions[harp_dimension_vertical]);
    EXPECT_EQ(kSifWavelengths, res.dimensions[harp_dimension_spectral]);
    EXPECT_EQ(4321, res.variables["orbit_index"].int32_values[0]);
    // Verify vertical inversion on pressure (first pixel: [100, 200, 300] ->
    // [300, 200, 100]).
    EXPECT_THAT(
        std::vector<float>(res.variables["pressure"].float_values.begin(),
                           res.variables["pressure"].float_values.begin() + 3),
        ElementsAre(300.0f, 200.0f, 100.0f));
  }
}

void BuildNo2Product(CodaMemProduct& prod, std::string_view col_field,
                     std::string_view col_unc_field, bool clamp_toa) {
  ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_NO2", 1));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "orbit", prod.MakeInt32(77)));

  S5Groups g = CreateStandardGroups(prod);
  PopulateDimensionsAndCore(prod, g, kScanlines, kPixels, kLayers, 0, 0,
                            /*uint_qa=*/true);
  PopulateGeolocation(prod, g, kScanlines, kPixels);
  PopulateSurfaceAndSnow(prod, g, kScanlines, kPixels,
                         "surface_altitude_uncertainty",
                         /*uint_surface=*/true, /*double_sp=*/true);
  PopulatePressureCoeffs(prod, g, kLayers, clamp_toa);

  const long sp_dim[] = {kScanlines * kPixels};
  const long spl_dim[] = {kScanlines * kPixels * kLayers};

  ASSERT_TRUE(AddUniformFloat(prod, g.product,
                              "nitrogen_dioxide_tropospheric_column", sp_dim,
                              1e-4f, 1e-5f));
  ASSERT_TRUE(AddUniformFloat(
      prod, g.product, "nitrogen_dioxide_tropospheric_column_uncertainty",
      sp_dim, 1e-5f, 1e-6f));
  ASSERT_TRUE(AddUniformFloat(
      prod, g.product, "nitrogen_dioxide_tropospheric_column_air_mass_factor",
      sp_dim, 1.5f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product,
                              "nitrogen_dioxide_total_column_air_mass_factor",
                              sp_dim, 1.8f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product,
                              "nitrogen_dioxide_total_column_averaging_kernel",
                              spl_dim, 0.2f, 0.1f));

  ASSERT_TRUE(AddUniformFloat(
      prod, g.detailed, "nitrogen_dioxide_stratospheric_column_air_mass_factor",
      sp_dim, 2.1f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "cloud_radiance_fraction",
                              sp_dim, 0.3f, 0.05f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "nitrogen_dioxide_slant_column",
                              sp_dim, 2e-4f, 1e-5f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "nitrogen_dioxide_slant_column_uncertainty",
                              sp_dim, 2e-5f, 1e-6f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "ozone_slant_column", sp_dim,
                              0.2f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(
      prod, g.detailed, "ozone_slant_column_uncertainty", sp_dim, 0.01f, 0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "water_vapor_slant_column",
                              sp_dim, 100.0f, 1.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "water_vapor_slant_column_uncertainty", sp_dim,
                              5.0f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "water_liquid_slant_column",
                              sp_dim, 1.0f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "nitrogen_dioxide_stratospheric_column", sp_dim,
                              5e-5f, 1e-6f));
  ASSERT_TRUE(AddUniformFloat(
      prod, g.detailed, "nitrogen_dioxide_stratospheric_column_uncertainty",
      sp_dim, 5e-6f, 1e-7f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.detailed, col_field, sp_dim, 1.5e-4f, 1e-5f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.detailed, col_unc_field, sp_dim, 1.5e-5f, 1e-6f));

  ASSERT_TRUE(
      AddUniformFloat(prod, g.input, "surface_albedo", sp_dim, 0.1f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "aerosol_index_354_388", sp_dim,
                              0.4f, 0.1f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.input, "cloud_albedo", sp_dim, 0.8f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "cloud_albedo_uncertainty", sp_dim,
                              0.05f, 0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "cloud_pressure", sp_dim, 80000.0f,
                              100.0f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.input, "scene_albedo", sp_dim, 0.3f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "scene_albedo_uncertainty", sp_dim,
                              0.02f, 0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "scene_pressure", sp_dim, 90000.0f,
                              100.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "scene_pressure_uncertainty",
                              sp_dim, 500.0f, 10.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "tropopause_pressure", sp_dim,
                              20000.0f, 100.0f));
}

TEST(HarpIngestS5L2Test, IngestNo2TotalAndSummedOptionsAndPressureBounds) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5_L2_NO2");
  ASSERT_NE(nullptr, mod);

  struct No2Case {
    const char* option;
    std::string_view col_field;
    std::string_view col_unc_field;
    bool clamp_toa;
  };
  constexpr No2Case kCases[] = {
      {.option = nullptr,
       .col_field = "nitrogen_dioxide_total_column",
       .col_unc_field = "nitrogen_dioxide_total_column_uncertainty",
       .clamp_toa = true},
      {.option = "total_column=total",
       .col_field = "nitrogen_dioxide_total_column",
       .col_unc_field = "nitrogen_dioxide_total_column_uncertainty",
       .clamp_toa = false},
      {.option = "total_column=summed",
       .col_field = "nitrogen_dioxide_summed_total_column",
       .col_unc_field = "nitrogen_dioxide_summed_total_column_uncertainty",
       .clamp_toa = true},
  };

  for (const auto& tc : kCases) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    BuildNo2Product(prod, tc.col_field, tc.col_unc_field, tc.clamp_toa);

    IngestExerciseResult res = ExerciseS5Module(mod, prod.product(), tc.option);
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_EQ(77, res.variables["orbit_index"].int32_values[0]);
    const auto& pb = res.variables["pressure_bounds"].double_values;
    // TOA upper bound is at layer (kLayers - 1) * 2.
    const double toa_val = pb[static_cast<size_t>((kLayers - 1) * 2)];
    if (tc.clamp_toa) {
      EXPECT_DOUBLE_EQ(1e-3, toa_val);
    } else {
      EXPECT_GT(toa_val, 1.0);
    }
  }
}

TEST(HarpIngestS5L2Test, IngestO3Success) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5_L2_O3");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_O3_", 1));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "orbit_start", prod.MakeInt32(5)));

  S5Groups g = CreateStandardGroups(prod);
  PopulateDimensionsAndCore(prod, g, kScanlines, kPixels, kLayers, 0, 0);
  PopulateGeolocation(prod, g, kScanlines, kPixels);
  PopulateSurfaceAndSnow(prod, g, kScanlines, kPixels, "");

  const long sp_dim[] = {kScanlines * kPixels};
  const long spl_dim[] = {kScanlines * kPixels * kLayers};

  ASSERT_TRUE(AddUniformFloat(prod, g.product, "ozone_total_column", sp_dim,
                              0.13f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product, "ozone_total_column_precision",
                              sp_dim, 0.002f, 0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product, "ozone_total_column_trueness",
                              sp_dim, 0.003f, 0.0f));

  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "ozone_slant_column", sp_dim,
                              0.2f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "ozone_slant_column_precision",
                              sp_dim, 0.005f, 0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "effective_temperature", sp_dim,
                              230.0f, 1.0f));
  ASSERT_TRUE(AddUniformFloat(
      prod, g.detailed, "effective_scene_air_mass_factor", sp_dim, 2.0f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "effective_scene_albedo",
                              sp_dim, 0.25f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "ozone_total_column_averaging_kernel", spl_dim,
                              0.4f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "ozone_profile_apriori",
                              spl_dim, 0.01f, 0.005f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "pressure_grid", spl_dim,
                              500.0f, 100.0f));

  ASSERT_TRUE(AddUniformFloat(prod, g.input, "surface_albedo_335", sp_dim, 0.1f,
                              0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "effective_cloud_fraction", sp_dim,
                              0.3f, 0.05f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "cloud_pressure", sp_dim, 70000.0f,
                              100.0f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.input, "cloud_albedo", sp_dim, 0.8f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "scene_pressure", sp_dim, 85000.0f,
                              100.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "aerosol_index_340_380", sp_dim,
                              0.2f, 0.05f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "tropopause_pressure", sp_dim,
                              19000.0f, 100.0f));

  IngestExerciseResult res = ExerciseS5Module(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
  EXPECT_FLOAT_EQ(0.13f,
                  res.variables["O3_column_number_density"].float_values[0]);
}

void BuildSo2Product(CodaMemProduct& prod, bool put_amf_in_product = false) {
  ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_SO2", 1));
  ASSERT_TRUE(
      prod.AddAttribute(prod.root(), "orbit_start", prod.MakeUint32(9)));

  S5Groups g = CreateStandardGroups(prod);
  PopulateDimensionsAndCore(prod, g, kScanlines, kPixels, kLayers, 0, 0);
  PopulateGeolocation(prod, g, kScanlines, kPixels);
  PopulateSurfaceAndSnow(prod, g, kScanlines, kPixels,
                         "surface_altitude_precision",
                         /*uint_surface=*/false, /*double_sp=*/true);
  PopulatePressureCoeffs(prod, g, kLayers, /*clamp_toa=*/true);

  const long sp_dim[] = {kScanlines * kPixels};
  const long spl_dim[] = {kScanlines * kPixels * kLayers};
  const long spp_dim[] = {kScanlines * kPixels * 4};

  // Profile values per pixel: [0, 1, 2, 3] + 10*pixel.
  std::vector<float> prof_vals;
  for (long i = 0; i < kScanlines * kPixels; ++i) {
    for (long p = 0; p < 4; ++p) {
      prof_vals.push_back(static_cast<float>(i * 10 + p));
    }
  }
  ASSERT_TRUE(AddFloatDataset(prod, g.product, "sulfur_dioxide_total_column",
                              spp_dim, prof_vals));
  ASSERT_TRUE(AddUniformFloat(prod, g.product,
                              "sulfur_dioxide_total_column_precision", spp_dim,
                              0.1f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product,
                              "sulfur_dioxide_total_column_trueness", spp_dim,
                              0.2f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product, "sulfur_dioxide_layer_height",
                              sp_dim, 3000.0f, 100.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product,
                              "sulfur_dioxide_layer_height_uncertainty", sp_dim,
                              200.0f, 10.0f));
  ASSERT_TRUE(prod.AddField(
      g.product, "sulfur_dioxide_layer_height_flag",
      prod.MakeInt8Array(sp_dim, MakeSeq<int8_t>(kScanlines * kPixels, 1, 0))));

  CodaMemNode* amf_target = put_amf_in_product ? g.product : g.detailed;
  ASSERT_TRUE(AddFloatDataset(prod, amf_target,
                              "sulfur_dioxide_total_column_air_mass_factor",
                              spp_dim, prof_vals));
  ASSERT_TRUE(AddUniformFloat(
      prod, amf_target, "sulfur_dioxide_total_column_air_mass_factor_precision",
      spp_dim, 0.05f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(
      prod, amf_target, "sulfur_dioxide_total_column_air_mass_factor_trueness",
      spp_dim, 0.06f, 0.01f));

  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "sulfur_dioxide_slant_column",
                              sp_dim, 0.01f, 0.001f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "sulfur_dioxide_slant_column_precision", sp_dim,
                              0.001f, 0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "sulfur_dioxide_slant_column_trueness", sp_dim,
                              0.002f, 0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "cloud_radiance_fraction",
                              sp_dim, 0.2f, 0.05f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "sulfur_dioxide_total_column_averaging_kernel",
                              spl_dim, 0.3f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "sulfur_dioxide_layer_pressure",
                              sp_dim, 70000.0f, 100.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "sulfur_dioxide_layer_pressure_uncertainty",
                              sp_dim, 1000.0f, 10.0f));

  ASSERT_TRUE(
      AddUniformFloat(prod, g.input, "surface_albedo", sp_dim, 0.15f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "sulfur_dioxide_profile_apriori",
                              spl_dim, 0.001f, 0.001f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "cloud_pressure", sp_dim, 75000.0f,
                              50.0f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.input, "cloud_height", sp_dim, 2500.0f, 50.0f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.input, "cloud_albedo", sp_dim, 0.7f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "aerosol_index_340_380", sp_dim,
                              0.3f, 0.05f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "ozone_total_column", sp_dim,
                              0.14f, 0.01f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.input, "scene_albedo", sp_dim, 0.25f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "scene_pressure", sp_dim, 85000.0f,
                              50.0f));
}

TEST(HarpIngestS5L2Test, IngestSo2ProfileOptionsAndFallback) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5_L2_SO2");
  ASSERT_NE(nullptr, mod);

  struct So2Case {
    const char* option;
    float expected_first;
    bool amf_in_product;
  };
  constexpr So2Case kCases[] = {
      {.option = nullptr, .expected_first = 0.0f, .amf_in_product = false},
      {.option = "so2_column=1km",
       .expected_first = 1.0f,
       .amf_in_product = true},
      {.option = "so2_column=7km",
       .expected_first = 2.0f,
       .amf_in_product = false},
      {.option = "so2_column=15km",
       .expected_first = 3.0f,
       .amf_in_product = false},
  };

  for (const auto& tc : kCases) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    BuildSo2Product(prod, tc.amf_in_product);

    IngestExerciseResult res = ExerciseS5Module(mod, prod.product(), tc.option);
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_FLOAT_EQ(tc.expected_first,
                    res.variables["SO2_column_number_density"].float_values[0]);
    EXPECT_FLOAT_EQ(
        tc.expected_first + 10.0f,
        res.variables["SO2_column_number_density_amf"].float_values[1]);
  }
}

void PopulateCldBand(CodaMemProduct& prod, const S5Groups& g, float base_cf) {
  PopulateDimensionsAndCore(prod, g, kScanlines, kPixels, 0, 0, 0);
  PopulateGeolocation(prod, g, kScanlines, kPixels);
  PopulateSurfaceAndSnow(prod, g, kScanlines, kPixels,
                         "surface_altitude_precision");

  const long sp_dim[] = {kScanlines * kPixels};
  ASSERT_TRUE(AddUniformFloat(prod, g.product, "effective_cloud_fraction",
                              sp_dim, base_cf, 0.05f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product,
                              "effective_cloud_fraction_precision", sp_dim,
                              0.01f, 0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product, "cloud_pressure", sp_dim,
                              65000.0f, 100.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product, "cloud_pressure_precision",
                              sp_dim, 500.0f, 10.0f));
  // Upstream read_product_cloud_height reads "cloud_height_precision".
  ASSERT_TRUE(AddUniformFloat(prod, g.product, "cloud_height_precision", sp_dim,
                              3500.0f, 50.0f));

  ASSERT_TRUE(
      AddUniformFloat(prod, g.detailed, "scene_albedo", sp_dim, 0.3f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "scene_albedo_precision",
                              sp_dim, 0.01f, 0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "scene_pressure", sp_dim,
                              80000.0f, 50.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "scene_pressure_precision",
                              sp_dim, 400.0f, 10.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "scene_height", sp_dim, 1500.0f,
                              20.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "scene_height_precision",
                              sp_dim, 100.0f, 5.0f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.detailed, "cloud_albedo", sp_dim, 0.8f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "cloud_albedo_precision",
                              sp_dim, 0.02f, 0.0f));
}

TEST(HarpIngestS5L2Test, IngestCldBandsAndDataFallbackLayout) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5_L2_CLD");
  ASSERT_NE(nullptr, mod);

  // 1. Root-level PRODUCT_BAND3A / PRODUCT_BAND3C with default band (3a).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_CLD", 1));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "orbit_start", prod.MakeUint32(100)));
    S5Groups g3a = CreateBandGroup(prod, prod.root(), "PRODUCT_BAND3A", true);
    S5Groups g3c = CreateBandGroup(prod, prod.root(), "PRODUCT_BAND3C", true);
    PopulateCldBand(prod, g3a, 0.25f);
    PopulateCldBand(prod, g3c, 0.75f);

    IngestExerciseResult res =
        ExerciseS5Module(mod, prod.product(), "band=band3a");
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_FLOAT_EQ(0.25f, res.variables["cloud_fraction"].float_values[0]);
  }

  // 2. Fallback under data/PRODUCT_BAND3A / data/PRODUCT_BAND3C with band3c.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_CLD", 1));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "orbit_start", prod.MakeUint32(101)));
    CodaMemNode* data_rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "data", data_rec));
    S5Groups g3a = CreateBandGroup(prod, data_rec, "PRODUCT_BAND3A", true);
    S5Groups g3c = CreateBandGroup(prod, data_rec, "PRODUCT_BAND3C", true);
    PopulateCldBand(prod, g3a, 0.25f);
    PopulateCldBand(prod, g3c, 0.75f);

    IngestExerciseResult res =
        ExerciseS5Module(mod, prod.product(), "band=band3c");
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_FLOAT_EQ(0.75f, res.variables["cloud_fraction"].float_values[0]);
  }
}

TEST(HarpIngestS5L2Test, IngestCoSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5_L2_CO");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_CO_", 1));
  ASSERT_TRUE(
      prod.AddAttribute(prod.root(), "orbit_start", prod.MakeUint32(200)));

  S5Groups g = CreateStandardGroups(prod);
  PopulateDimensionsAndCore(prod, g, kScanlines, kPixels, kLayers, 0, 0);
  PopulateGeolocation(prod, g, kScanlines, kPixels);
  PopulateSurfaceAndSnow(prod, g, kScanlines, kPixels,
                         "surface_altitude_precision");

  const long sp_dim[] = {kScanlines * kPixels};
  const long spl_dim[] = {kScanlines * kPixels * kLayers};

  ASSERT_TRUE(AddUniformFloat(prod, g.product, "carbon_monoxide_total_column",
                              sp_dim, 0.03f, 0.001f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product,
                              "carbon_monoxide_total_column_precision", sp_dim,
                              0.002f, 0.0f));

  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "carbon_monoxide_total_column_averaging_kernel",
                              spl_dim, 0.5f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "water_total_column", sp_dim,
                              500.0f, 10.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "semiheavy_water_total_column",
                              sp_dim, 0.1f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "methane_total_column_prefit",
                              sp_dim, 0.6f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "cloud_centre_height", sp_dim,
                              2500.0f, 50.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "cloud_optical_depth", sp_dim,
                              5.0f, 0.5f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.detailed, "surface_albedo", sp_dim, 0.2f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "carbon_monoxide_profile_apriori", spl_dim,
                              0.001f, 0.001f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "methane_profile_apriori",
                              spl_dim, 0.01f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "dry_air_column", sp_dim,
                              3.5e5f, 100.0f));

  IngestExerciseResult res = ExerciseS5Module(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
  EXPECT_FLOAT_EQ(0.03f,
                  res.variables["CO_column_number_density"].float_values[0]);
}

void BuildFdyProduct(CodaMemProduct& prod) {
  ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_FDY", 1));
  ASSERT_TRUE(
      prod.AddAttribute(prod.root(), "orbit_start", prod.MakeUint32(300)));

  S5Groups g = CreateStandardGroups(prod);
  PopulateDimensionsAndCore(prod, g, kScanlines, kPixels, kLayers, 0, 0);
  PopulateGeolocation(prod, g, kScanlines, kPixels);
  PopulateSurfaceAndSnow(prod, g, kScanlines, kPixels,
                         "surface_altitude_precision",
                         /*uint_surface=*/false, /*double_sp=*/true);
  PopulatePressureCoeffs(prod, g, kLayers, /*clamp_toa=*/true);

  const long sp_dim[] = {kScanlines * kPixels};
  const long spl_dim[] = {kScanlines * kPixels * kLayers};

  ASSERT_TRUE(AddUniformFloat(
      prod, g.product, "formaldehyde_tropospheric_column", sp_dim, 2.0f, 0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product,
                              "formaldehyde_tropospheric_column_precision",
                              sp_dim, 0.4f, 0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product,
                              "formaldehyde_tropospheric_column_trueness",
                              sp_dim, 0.5f, 0.0f));

  ASSERT_TRUE(AddUniformFloat(
      prod, g.detailed, "formaldehyde_tropospheric_column_air_mass_factor",
      sp_dim, 3.0f, 0.0f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.detailed,
                      "formaldehyde_tropospheric_column_clear_air_mass_factor",
                      sp_dim, 1.5f, 0.0f));
  ASSERT_TRUE(AddUniformFloat(
      prod, g.detailed,
      "formaldehyde_tropospheric_column_air_mass_factor_trueness", sp_dim, 0.2f,
      0.0f));
  ASSERT_TRUE(AddUniformFloat(
      prod, g.detailed, "formaldehyde_tropospheric_column_averaging_kernel",
      spl_dim, 0.1f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "formaldehyde_corrected_slant_column", sp_dim,
                              1e-4f, 1e-5f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "formaldehyde_corrected_slant_column_trueness",
                              sp_dim, 1e-5f, 0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "cloud_radiance_fraction",
                              sp_dim, 0.25f, 0.05f));

  ASSERT_TRUE(AddUniformFloat(prod, g.input, "formaldehyde_profile_apriori",
                              spl_dim, 1e-9f, 1e-9f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "surface_albedo_342", sp_dim, 0.1f,
                              0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "aerosol_index_340_380", sp_dim,
                              0.2f, 0.05f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "effective_cloud_fraction", sp_dim,
                              0.3f, 0.05f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.input, "cloud_albedo", sp_dim, 0.8f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "cloud_pressure", sp_dim, 70000.0f,
                              50.0f));
}

TEST(HarpIngestS5L2Test, IngestFdyDefaultAndClearSkyAmf) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5_L2_FDY");
  ASSERT_NE(nullptr, mod);

  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    BuildFdyProduct(prod);
    IngestExerciseResult res = ExerciseS5Module(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_FLOAT_EQ(2.0f,
                    res.variables["tropospheric_HCHO_column_number_density"]
                        .float_values[0]);
    EXPECT_FLOAT_EQ(
        0.4f, res.variables
                  ["tropospheric_HCHO_column_number_density_uncertainty_random"]
                      .float_values[0]);
    EXPECT_FLOAT_EQ(3.0f,
                    res.variables["tropospheric_HCHO_column_number_density_amf"]
                        .float_values[0]);
  }

  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    BuildFdyProduct(prod);
    IngestExerciseResult res =
        ExerciseS5Module(mod, prod.product(), "amf=clear_sky");
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    // Scaled by amf (3.0) / amf_clear (1.5) == 2.0.
    EXPECT_THAT(res.variables["tropospheric_HCHO_column_number_density"]
                    .float_values[0],
                FloatNear(4.0f, 1e-5f));
    EXPECT_THAT(
        res.variables
            ["tropospheric_HCHO_column_number_density_uncertainty_random"]
                .float_values[0],
        FloatNear(0.8f, 1e-5f));
    EXPECT_FLOAT_EQ(1.5f,
                    res.variables["tropospheric_HCHO_column_number_density_amf"]
                        .float_values[0]);
  }
}

TEST(HarpIngestS5L2Test, IngestGlySuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5_L2_GLY");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_GLY", 1));
  ASSERT_TRUE(
      prod.AddAttribute(prod.root(), "orbit_start", prod.MakeUint32(400)));

  S5Groups g = CreateStandardGroups(prod);
  PopulateDimensionsAndCore(prod, g, kScanlines, kPixels, kLayers, 0, 0);
  PopulateGeolocation(prod, g, kScanlines, kPixels);
  PopulateSurfaceAndSnow(prod, g, kScanlines, kPixels,
                         "surface_altitude_precision");

  const long sp_dim[] = {kScanlines * kPixels};
  const long spl_dim[] = {kScanlines * kPixels * kLayers};

  ASSERT_TRUE(AddUniformFloat(prod, g.product, "glyoxal_tropospheric_column",
                              sp_dim, 1e-5f, 1e-6f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product,
                              "glyoxal_tropospheric_column_precision", sp_dim,
                              1e-6f, 0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product,
                              "glyoxal_tropospheric_column_trueness", sp_dim,
                              2e-6f, 0.0f));

  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "glyoxal_tropospheric_column_air_mass_factor",
                              sp_dim, 1.6f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(
      prod, g.detailed, "glyoxal_tropospheric_column_air_mass_factor_trueness",
      sp_dim, 0.1f, 0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "glyoxal_tropospheric_column_averaging_kernel",
                              spl_dim, 0.3f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "glyoxal_slant_column", sp_dim,
                              1.5e-5f, 1e-6f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "glyoxal_slant_column_precision", sp_dim, 1.5e-6f,
                              0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "glyoxal_slant_column_trueness",
                              sp_dim, 2.5e-6f, 0.0f));

  ASSERT_TRUE(AddUniformFloat(prod, g.input, "surface_albedo_452", sp_dim,
                              0.12f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "glyoxal_profile_apriori", spl_dim,
                              1e-10f, 1e-10f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "glyoxal_profile_apriori_pressure",
                              spl_dim, 1000.0f, 500.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "aerosol_index_340_380", sp_dim,
                              0.2f, 0.05f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "effective_cloud_fraction", sp_dim,
                              0.2f, 0.05f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "cloud_pressure", sp_dim, 72000.0f,
                              50.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "tropopause_pressure", sp_dim,
                              21000.0f, 50.0f));

  IngestExerciseResult res = ExerciseS5Module(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
  EXPECT_FLOAT_EQ(1e-5f,
                  res.variables["tropospheric_CHOCHO_column_number_density"]
                      .float_values[0]);
}

void BuildClaProduct(CodaMemProduct& prod, bool under_data_dir) {
  ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_CLA", 1));
  ASSERT_TRUE(
      prod.AddAttribute(prod.root(), "orbit_start", prod.MakeUint32(500)));

  CodaMemNode* parent = prod.root();
  if (under_data_dir) {
    parent = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "data", parent));
  }

  struct BandSpec {
    const char* name;
    float cf_base;
  };
  constexpr BandSpec kBands[] = {
      {.name = "PRODUCT_BAND3A", .cf_base = 0.1f},
      {.name = "PRODUCT_BAND1B", .cf_base = 0.2f},
      {.name = "PRODUCT_BAND2", .cf_base = 0.3f},
      {.name = "PRODUCT_BAND3B", .cf_base = 0.4f},
      {.name = "PRODUCT_BAND3C", .cf_base = 0.5f},
      {.name = "PRODUCT_BAND4", .cf_base = 0.6f},
      {.name = "PRODUCT_BAND5", .cf_base = 0.7f},
  };
  const long sp_dim[] = {kScanlines * kPixels};
  for (const auto& b : kBands) {
    S5Groups g = CreateBandGroup(prod, parent, b.name, false);
    PopulateDimensionsAndCore(prod, g, kScanlines, kPixels, 0, 0, 0);
    PopulateGeolocation(prod, g, kScanlines, kPixels);
    ASSERT_TRUE(AddUniformFloat(prod, g.product, "moxy_cfr_psf_mean", sp_dim,
                                b.cf_base, 0.01f));
    ASSERT_TRUE(AddUniformFloat(prod, g.product, "moxy_cot_psf_mean", sp_dim,
                                10.0f, 0.5f));
    ASSERT_TRUE(AddUniformFloat(prod, g.product, "moxy_ctp_psf_mean", sp_dim,
                                500.0f, 10.0f));
  }
}

TEST(HarpIngestS5L2Test, IngestClaAllSevenBandsAndDataFallback) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5_L2_CLA");
  ASSERT_NE(nullptr, mod);

  struct ClaCase {
    const char* option;
    float expected_cf;
    bool under_data;
  };
  constexpr ClaCase kCases[] = {
      {.option = nullptr, .expected_cf = 0.1f, .under_data = false},
      {.option = "band=band3a", .expected_cf = 0.1f, .under_data = true},
      {.option = "band=band1b", .expected_cf = 0.2f, .under_data = false},
      {.option = "band=band2", .expected_cf = 0.3f, .under_data = true},
      {.option = "band=band3b", .expected_cf = 0.4f, .under_data = false},
      {.option = "band=band3c", .expected_cf = 0.5f, .under_data = true},
      {.option = "band=band4", .expected_cf = 0.6f, .under_data = false},
      {.option = "band=band5", .expected_cf = 0.7f, .under_data = true},
  };

  for (const auto& tc : kCases) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    BuildClaProduct(prod, tc.under_data);

    IngestExerciseResult res = ExerciseS5Module(mod, prod.product(), tc.option);
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_FLOAT_EQ(tc.expected_cf,
                    res.variables["cloud_fraction"].float_values[0]);
  }
}

TEST(HarpIngestS5L2Test, IngestAodSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5_L2_AOD");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_AOD", 1));
  ASSERT_TRUE(
      prod.AddAttribute(prod.root(), "orbit_start", prod.MakeUint32(600)));

  S5Groups g = CreateStandardGroups(prod);
  PopulateDimensionsAndCore(prod, g, kScanlines, kPixels, 0, 0, kWavelengths);
  PopulateGeolocation(prod, g, kScanlines, kPixels);
  PopulateSurfaceAndSnow(prod, g, kScanlines, kPixels,
                         "surface_altitude_precision");

  const long sp_dim[] = {kScanlines * kPixels};
  const long spw_dim[] = {kScanlines * kPixels * kWavelengths};

  ASSERT_TRUE(AddUniformFloat(prod, g.product, "aerosol_optical_depth", spw_dim,
                              0.2f, 0.05f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product,
                              "aerosol_optical_depth_precision", spw_dim, 0.02f,
                              0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product,
                              "absorbing_aerosol_optical_depth", spw_dim, 0.05f,
                              0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.product,
                              "absorbing_aerosol_optical_depth_precision",
                              spw_dim, 0.005f, 0.0f));

  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "single_scattering_albedo",
                              spw_dim, 0.9f, 0.01f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                              "single_scattering_albedo_precision", spw_dim,
                              0.02f, 0.0f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "aerosol_mean_height", sp_dim,
                              2.5f, 0.1f));
  ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "diffuse_surface_reflectance",
                              spw_dim, 0.15f, 0.01f));

  ASSERT_TRUE(AddUniformFloat(prod, g.input, "effective_cloud_fraction", sp_dim,
                              0.1f, 0.02f));
  ASSERT_TRUE(AddUniformFloat(prod, g.input, "aerosol_index_354_388", sp_dim,
                              0.5f, 0.1f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.input, "wind_u_velocity", sp_dim, 3.0f, 0.5f));
  ASSERT_TRUE(
      AddUniformFloat(prod, g.input, "wind_v_velocity", sp_dim, -2.0f, 0.5f));

  IngestExerciseResult res = ExerciseS5Module(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
  EXPECT_EQ(kWavelengths, res.dimensions[harp_dimension_spectral]);
  EXPECT_THAT(res.variables["wavelength"].float_values,
              ElementsAre(340.0f, 440.0f));
}

TEST(HarpIngestS5L2Test, IngestAlhDefaultAndSurfaceAlbedo772Option) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("S5_L2_ALH");
  ASSERT_NE(nullptr, mod);

  for (const char* opt :
       {static_cast<const char*>(nullptr), "surface_albedo=772"}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_ALH", 1));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "orbit_start", prod.MakeUint32(700)));

    S5Groups g = CreateStandardGroups(prod);
    PopulateDimensionsAndCore(prod, g, kScanlines, kPixels, 0, 0, 0);
    PopulateGeolocation(prod, g, kScanlines, kPixels);
    PopulateSurfaceAndSnow(prod, g, kScanlines, kPixels,
                           "surface_altitude_precision");

    const long sp_dim[] = {kScanlines * kPixels};
    ASSERT_TRUE(AddUniformFloat(prod, g.product, "aerosol_mid_pressure", sp_dim,
                                75000.0f, 100.0f));
    ASSERT_TRUE(AddUniformFloat(prod, g.product,
                                "aerosol_mid_pressure_precision", sp_dim,
                                500.0f, 10.0f));
    ASSERT_TRUE(AddUniformFloat(prod, g.product, "aerosol_mid_altitude", sp_dim,
                                2500.0f, 50.0f));
    ASSERT_TRUE(AddUniformFloat(prod, g.product,
                                "aerosol_mid_altitude_precision", sp_dim,
                                100.0f, 5.0f));
    ASSERT_TRUE(AddUniformFloat(prod, g.product, "aerosol_optical_thickness",
                                sp_dim, 0.5f, 0.05f));
    ASSERT_TRUE(AddUniformFloat(prod, g.product,
                                "aerosol_optical_thickness_precision", sp_dim,
                                0.05f, 0.0f));

    ASSERT_TRUE(AddUniformFloat(prod, g.input, "scene_albedo_380", sp_dim,
                                0.22f, 0.01f));
    ASSERT_TRUE(AddUniformFloat(prod, g.input, "aerosol_index_354_388", sp_dim,
                                0.6f, 0.1f));

    IngestExerciseResult res = ExerciseS5Module(mod, prod.product(), opt);
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_FLOAT_EQ(75000.0f,
                    res.variables["aerosol_pressure"].float_values[0]);
  }
}

TEST(HarpIngestS5L2Test, InitProductTypeAndCursorErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* aui = FindIngestionModule("S5_L2_AUI");
  const harp_ingestion_module* cld = FindIngestionModule("S5_L2_CLD");
  const harp_ingestion_module* cla = FindIngestionModule("S5_L2_CLA");
  ASSERT_NE(nullptr, aui);
  ASSERT_NE(nullptr, cld);
  ASSERT_NE(nullptr, cla);

  // Unsupported product type (note: omitting SetProductInfo leaves
  // coda_get_product_type returning NULL, which upstream strcmp dereferences).
  {
    CodaMemProduct bad_type;
    ASSERT_TRUE(bad_type.ok());
    ASSERT_TRUE(bad_type.SetProductInfo("Sentinel5", "SN5_02_UNKNOWN", 1));
    IngestExerciseResult res = ExerciseS5Module(aui, bad_type.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 2. Standard cursor hierarchy missing data / PRODUCT / SUPPORT_DATA /
  // GEOLOCATIONS / DETAILED_RESULTS / INPUT_DATA.
  for (int stage = 0; stage <= 4; ++stage) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_AUI", 1));
    if (stage >= 1) {
      CodaMemNode* data = prod.MakeRecord();
      ASSERT_TRUE(prod.AddField(prod.root(), "data", data));
      CodaMemNode* p = prod.MakeRecord();
      ASSERT_TRUE(prod.AddField(data, "PRODUCT", p));
      if (stage >= 2) {
        CodaMemNode* sup = prod.MakeRecord();
        ASSERT_TRUE(prod.AddField(p, "SUPPORT_DATA", sup));
        if (stage >= 3) {
          ASSERT_TRUE(prod.AddField(sup, "GEOLOCATIONS", prod.MakeRecord()));
        }
        if (stage >= 4) {
          ASSERT_TRUE(
              prod.AddField(sup, "DETAILED_RESULTS", prod.MakeRecord()));
        }
      }
    }
    EXPECT_EQ(-1, ExerciseS5Module(aui, prod.product()).status);
  }

  // 3. CLD cursor error stages across PRODUCT_BAND3A and PRODUCT_BAND3C.
  for (int stage = 0; stage <= 8; ++stage) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_CLD", 1));
    if (stage >= 1) {
      CodaMemNode* b3a = prod.MakeRecord();
      ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT_BAND3A", b3a));
      if (stage >= 2) {
        CodaMemNode* sup = prod.MakeRecord();
        ASSERT_TRUE(prod.AddField(b3a, "SUPPORT_DATA", sup));
        if (stage >= 3) {
          ASSERT_TRUE(prod.AddField(sup, "GEOLOCATIONS", prod.MakeRecord()));
        }
        if (stage >= 4) {
          ASSERT_TRUE(
              prod.AddField(sup, "DETAILED_RESULTS", prod.MakeRecord()));
        }
        if (stage >= 5) {
          ASSERT_TRUE(prod.AddField(sup, "INPUT_DATA", prod.MakeRecord()));
        }
      }
    }
    if (stage >= 6) {
      CodaMemNode* b3c = prod.MakeRecord();
      ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT_BAND3C", b3c));
      if (stage >= 7) {
        CodaMemNode* sup = prod.MakeRecord();
        ASSERT_TRUE(prod.AddField(b3c, "SUPPORT_DATA", sup));
        if (stage >= 8) {
          ASSERT_TRUE(prod.AddField(sup, "GEOLOCATIONS", prod.MakeRecord()));
        }
        // Leave DETAILED_RESULTS / INPUT_DATA missing at stage 8.
      }
    }
    EXPECT_EQ(-1, ExerciseS5Module(cld, prod.product()).status);
  }
  {
    // Stage 9: missing INPUT_DATA under PRODUCT_BAND3C.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_CLD", 1));
    CreateBandGroup(prod, prod.root(), "PRODUCT_BAND3A", true);
    CodaMemNode* b3c = prod.MakeRecord();
    CodaMemNode* sup = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prod.root(), "PRODUCT_BAND3C", b3c));
    ASSERT_TRUE(prod.AddField(b3c, "SUPPORT_DATA", sup));
    ASSERT_TRUE(prod.AddField(sup, "GEOLOCATIONS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(sup, "DETAILED_RESULTS", prod.MakeRecord()));
    EXPECT_EQ(-1, ExerciseS5Module(cld, prod.product()).status);
  }

  // 4. CLA cursor error stages across all 7 bands (missing band, SUPPORT_DATA,
  // or GEOLOCATIONS).
  constexpr const char* kClaBands[] = {
      "PRODUCT_BAND3A", "PRODUCT_BAND1B", "PRODUCT_BAND2", "PRODUCT_BAND3B",
      "PRODUCT_BAND3C", "PRODUCT_BAND4",  "PRODUCT_BAND5",
  };
  for (int fail_band = 0; fail_band < 7; ++fail_band) {
    for (int sub = 0; sub < 3; ++sub) {
      CodaMemProduct prod;
      ASSERT_TRUE(prod.ok());
      ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_CLA", 1));
      for (int b = 0; b <= fail_band; ++b) {
        if (b < fail_band) {
          CreateBandGroup(prod, prod.root(), kClaBands[b], false);
        } else if (sub == 1) {
          ASSERT_TRUE(
              prod.AddField(prod.root(), kClaBands[b], prod.MakeRecord()));
        } else if (sub == 2) {
          CodaMemNode* br = prod.MakeRecord();
          ASSERT_TRUE(prod.AddField(prod.root(), kClaBands[b], br));
          ASSERT_TRUE(prod.AddField(br, "SUPPORT_DATA", prod.MakeRecord()));
        }
      }
      EXPECT_EQ(-1, ExerciseS5Module(cla, prod.product()).status);
    }
  }
}

TEST(HarpIngestS5L2Test, DimensionDiscoveryAndErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* ch4 = FindIngestionModule("S5_L2_CH4");
  const harp_ingestion_module* aod = FindIngestionModule("S5_L2_AOD");
  ASSERT_NE(nullptr, ch4);
  ASSERT_NE(nullptr, aod);

  // 1. Dimension found recursively inside an array of records (array branch in
  // find_dimension_length_recursive).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_AOD", 1));
    S5Groups g = CreateStandardGroups(prod);
    constexpr long kArrDim[] = {1};
    constexpr long kTwoDim[] = {2};
    constexpr long kFourDim[] = {4};
    CodaMemNode* inner = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(inner, "scanline",
                      prod.MakeInt32Array(kTwoDim, MakeSeq<int32_t>(2, 0, 1))));
    ASSERT_TRUE(
        prod.AddField(inner, "ground_pixel",
                      prod.MakeInt32Array(kTwoDim, MakeSeq<int32_t>(2, 0, 1))));
    ASSERT_TRUE(prod.AddField(
        inner, "corner",
        prod.MakeInt32Array(kFourDim, MakeSeq<int32_t>(4, 0, 1))));
    ASSERT_TRUE(
        prod.AddField(inner, "wavelength",
                      prod.MakeInt32Array(kTwoDim, MakeSeq<int32_t>(2, 0, 1))));
    CodaMemNode* elems[] = {inner};
    ASSERT_TRUE(
        prod.AddField(g.product, "nested", prod.MakeNodeArray(kArrDim, elems)));

    IngestExerciseResult res = ExerciseS5Module(
        aod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(4L, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(2L, res.dimensions[harp_dimension_spectral]);
  }

  // 2. Non-1D dimension field (2D) triggers "field ... is not a 1D array".
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_CH4", 1));
    S5Groups g = CreateStandardGroups(prod);
    constexpr long k2d[] = {2, 2};
    ASSERT_TRUE(
        prod.AddField(g.product, "scanline",
                      prod.MakeInt32Array(k2d, MakeSeq<int32_t>(4, 0, 1))));
    IngestExerciseResult res = ExerciseS5Module(ch4, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 3. Missing ground_pixel, missing corner, corner != 4, missing layer,
  // missing spectral.
  for (int stage = 0; stage < 5; ++stage) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_CH4", 1));
    S5Groups g = CreateStandardGroups(prod);
    constexpr long kTwo[] = {2};
    constexpr long kThree[] = {3};
    ASSERT_TRUE(
        prod.AddField(g.product, "scanline",
                      prod.MakeInt32Array(kTwo, MakeSeq<int32_t>(2, 0, 1))));
    if (stage >= 1) {
      ASSERT_TRUE(
          prod.AddField(g.product, "ground_pixel",
                        prod.MakeInt32Array(kTwo, MakeSeq<int32_t>(2, 0, 1))));
    }
    if (stage == 2) {
      // corner length 3 instead of 4.
      ASSERT_TRUE(prod.AddField(
          g.product, "corner",
          prod.MakeInt32Array(kThree, MakeSeq<int32_t>(3, 0, 1))));
    } else if (stage >= 3) {
      constexpr long kFour[] = {4};
      ASSERT_TRUE(
          prod.AddField(g.product, "corner",
                        prod.MakeInt32Array(kFour, MakeSeq<int32_t>(4, 0, 1))));
    }
    if (stage >= 4) {
      ASSERT_TRUE(prod.AddField(
          g.product, "layer",
          prod.MakeInt32Array(kThree, MakeSeq<int32_t>(3, 0, 1))));
    }
    IngestExerciseResult res = ExerciseS5Module(ch4, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
}

TEST(HarpIngestS5L2Test, VariableReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* aui = FindIngestionModule("S5_L2_AUI");
  const harp_ingestion_module* no2 = FindIngestionModule("S5_L2_NO2");
  const harp_ingestion_module* fdy = FindIngestionModule("S5_L2_FDY");
  const harp_ingestion_module* so2 = FindIngestionModule("S5_L2_SO2");
  ASSERT_NE(nullptr, aui);
  ASSERT_NE(nullptr, no2);
  ASSERT_NE(nullptr, fdy);
  ASSERT_NE(nullptr, so2);

  // 1. Empty product with dimensions only: missing datasets on all AUI readers.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_AUI", 1));
    S5Groups g = CreateStandardGroups(prod);
    constexpr long kTwo[] = {2};
    constexpr long kFour[] = {4};
    ASSERT_TRUE(
        prod.AddField(g.product, "scanline",
                      prod.MakeInt32Array(kTwo, MakeSeq<int32_t>(2, 0, 1))));
    ASSERT_TRUE(
        prod.AddField(g.product, "ground_pixel",
                      prod.MakeInt32Array(kTwo, MakeSeq<int32_t>(2, 0, 1))));
    ASSERT_TRUE(
        prod.AddField(g.product, "corner",
                      prod.MakeInt32Array(kFour, MakeSeq<int32_t>(4, 0, 1))));

    IngestExerciseResult res = ExerciseS5Module(
        aui, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
    EXPECT_EQ(-1, res.variables["reflectance"].read_status);
    EXPECT_EQ(-1, res.variables["reflectance_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["snow_ice_type"].read_status);
    EXPECT_EQ(-1, res.variables["sea_ice_fraction"].read_status);
  }

  // 2. Wrong types / missing FillValue / element count mismatches.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_AUI", 1));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "orbit_start", prod.MakeString("bad")));

    S5Groups g = CreateStandardGroups(prod);
    constexpr long kOne[] = {1};
    constexpr long kTwo[] = {2};
    constexpr long kFour[] = {4};
    ASSERT_TRUE(
        prod.AddField(g.product, "scanline",
                      prod.MakeInt32Array(kTwo, MakeSeq<int32_t>(2, 0, 1))));
    ASSERT_TRUE(
        prod.AddField(g.product, "ground_pixel",
                      prod.MakeInt32Array(kTwo, MakeSeq<int32_t>(2, 0, 1))));
    ASSERT_TRUE(
        prod.AddField(g.product, "corner",
                      prod.MakeInt32Array(kFour, MakeSeq<int32_t>(4, 0, 1))));

    // time present (1.0) so datetime proceeds to missing delta_time.
    constexpr double kTimeVal[] = {1.0};
    ASSERT_TRUE(AddDoubleDataset(prod, g.product, "time", kOne, kTimeVal));
    // qa_value has wrong element count (1 instead of 4).
    ASSERT_TRUE(
        prod.AddField(g.product, "qa_value",
                      prod.MakeInt32Array(kOne, MakeSeq<int32_t>(1, 1, 0))));
    // processing_quality_flags has wrong element count (1 instead of 4).
    ASSERT_TRUE(
        prod.AddField(g.product, "processing_quality_flags",
                      prod.MakeUint64Array(kOne, MakeSeq<uint64_t>(1, 1, 0))));
    // aerosol_index_354_388 is string array -> read_float_array fails.
    constexpr std::string_view kBadStrs[] = {"a", "b", "c", "d"};
    ASSERT_TRUE(prod.AddField(g.product, "aerosol_index_354_388",
                              prod.MakeStringArray(kFour, kBadStrs)));
    // aerosol_index_354_388_precision missing @FillValue.
    ASSERT_TRUE(prod.AddField(
        g.product, "aerosol_index_354_388_precision",
        prod.MakeFloatArray(kFour, MakeSeq<float>(4, 0.1f, 0.0f))));
    // surface_classification is string array -> read_int8_array fails.
    ASSERT_TRUE(prod.AddField(g.input, "surface_classification",
                              prod.MakeStringArray(kFour, kBadStrs)));
    // latitude has float data and bad string @FillValue[0].
    CodaMemNode* lat =
        prod.MakeFloatArray(kFour, MakeSeq<float>(4, 1.0f, 1.0f));
    constexpr std::string_view kBadFill[] = {"nan"};
    ASSERT_TRUE(prod.AddAttribute(lat, "FillValue",
                                  prod.MakeStringArray(kOne, kBadFill)));
    ASSERT_TRUE(prod.AddField(g.geoloc, "latitude", lat));
    // reflectance_354_measured present, reflectance_388_measured missing.
    ASSERT_TRUE(AddUniformFloat(prod, g.detailed, "reflectance_354_measured",
                                kFour, 0.2f, 0.01f));
    ASSERT_TRUE(AddUniformFloat(prod, g.detailed,
                                "reflectance_precision_354_measured", kFour,
                                0.01f, 0.0f));

    IngestExerciseResult res = ExerciseS5Module(
        aui, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
    EXPECT_EQ(-1, res.variables["absorbing_aerosol_index"].read_status);
    EXPECT_EQ(-1,
              res.variables["absorbing_aerosol_index_uncertainty"].read_status);
    EXPECT_EQ(-1,
              res.variables["absorbing_aerosol_index_validity"].read_status);
    EXPECT_EQ(-1, res.variables["surface_type"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["reflectance"].read_status);
    EXPECT_EQ(-1, res.variables["reflectance_uncertainty"].read_status);
  }

  // 3. Double @FillValue failure, datetime_length short delta_time,
  // processing_quality_flags wrong type, and int32 wrong type.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_AUI", 1));
    // Empty orbit_start array triggers goto_first_array_element failure.
    constexpr long kZero[] = {0};
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "orbit_start",
                                  prod.MakeInt32Array(kZero, {})));
    S5Groups g = CreateStandardGroups(prod);
    constexpr long kOne[] = {1};
    constexpr long kTwo[] = {2};
    constexpr long kFour[] = {4};
    ASSERT_TRUE(
        prod.AddField(g.product, "scanline",
                      prod.MakeInt32Array(kTwo, MakeSeq<int32_t>(2, 0, 1))));
    ASSERT_TRUE(
        prod.AddField(g.product, "ground_pixel",
                      prod.MakeInt32Array(kTwo, MakeSeq<int32_t>(2, 0, 1))));
    ASSERT_TRUE(
        prod.AddField(g.product, "corner",
                      prod.MakeInt32Array(kFour, MakeSeq<int32_t>(4, 0, 1))));
    // delta_time has 1 element -> datetime_length fails at
    // goto_next_array_element.
    ASSERT_TRUE(
        AddUniformDouble(prod, g.product, "delta_time", kOne, 1.0, 0.0));
    // time is double array without @FillValue -> read_dataset double fails.
    constexpr double kTime1[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(g.product, "time", prod.MakeDoubleArray(kOne, kTime1)));
    // satellite_orbit_phase is string array -> read_double_array fails.
    constexpr std::string_view kBad2[] = {"x", "y"};
    ASSERT_TRUE(prod.AddField(g.geoloc, "satellite_orbit_phase",
                              prod.MakeStringArray(kTwo, kBad2)));
    // qa_value is string array -> read_int32_array fails.
    constexpr std::string_view kBad4[] = {"a", "b", "c", "d"};
    ASSERT_TRUE(prod.AddField(g.product, "qa_value",
                              prod.MakeStringArray(kFour, kBad4)));
    // processing_quality_flags is string array -> read_uint64_array fails.
    ASSERT_TRUE(prod.AddField(g.product, "processing_quality_flags",
                              prod.MakeStringArray(kFour, kBad4)));

    IngestExerciseResult res = ExerciseS5Module(
        aui, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_orbit_phase"].read_status);
    EXPECT_EQ(-1,
              res.variables["absorbing_aerosol_index_validity"].read_status);
  }

  // 4. Missing datasets for FDY clear-sky scaling, NO2 pressure_bounds, SO2
  // scalar, and 2D vertical inversions.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_FDY", 1));
    S5Groups g = CreateStandardGroups(prod);
    PopulateDimensionsAndCore(prod, g, kScanlines, kPixels, kLayers, 0, 0);
    const long sp_dim[] = {kScanlines * kPixels};
    // Provide column + precision + amf, omit clear_air_mass_factor.
    ASSERT_TRUE(AddUniformFloat(prod, g.product,
                                "formaldehyde_tropospheric_column", sp_dim,
                                2.0f, 0.0f));
    ASSERT_TRUE(AddUniformFloat(prod, g.product,
                                "formaldehyde_tropospheric_column_precision",
                                sp_dim, 0.4f, 0.0f));
    IngestExerciseResult res1 = ExerciseS5Module(
        fdy, prod.product(), "amf=clear_sky", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res1.status);
    EXPECT_EQ(
        -1,
        res1.variables["tropospheric_HCHO_column_number_density"].read_status);
    EXPECT_EQ(-1, res1.variables["tropospheric_HCHO_column_number_density_avk"]
                      .read_status);
    EXPECT_EQ(-1, res1.variables["HCHO_mass_mixing_ratio_apriori"].read_status);
    EXPECT_EQ(-1, res1.variables["pressure_bounds"].read_status);

    // Now add amf (but keep clear_amf missing) to cover second read_dataset
    // failure in scale_amf_hcho.
    ASSERT_TRUE(AddUniformFloat(
        prod, g.detailed, "formaldehyde_tropospheric_column_air_mass_factor",
        sp_dim, 3.0f, 0.0f));
    IngestExerciseResult res2 = ExerciseS5Module(
        fdy, prod.product(), "amf=clear_sky", /*stop_on_error=*/false);
    EXPECT_EQ(
        -1,
        res2.variables["tropospheric_HCHO_column_number_density"].read_status);
  }

  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_NO2", 1));
    S5Groups g = CreateStandardGroups(prod);
    PopulateDimensionsAndCore(prod, g, kScanlines, kPixels, kLayers, 0, 0);
    IngestExerciseResult res = ExerciseS5Module(
        no2, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["NO2_column_number_density_avk"].read_status);
    EXPECT_EQ(-1, res.variables["pressure_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_latitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_longitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_altitude"].read_status);
  }

  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5", "SN5_02_SO2", 1));
    S5Groups g = CreateStandardGroups(prod);
    PopulateDimensionsAndCore(prod, g, kScanlines, kPixels, kLayers, 0, 0);
    IngestExerciseResult res = ExerciseS5Module(
        so2, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["SO2_column_number_density"].read_status);
    EXPECT_EQ(-1, res.variables["SO2_column_number_density_avk"].read_status);
    EXPECT_EQ(-1,
              res.variables["SO2_column_number_density_apriori"].read_status);
  }
}

TEST(HarpIngestS5L2Test, VerticalInversionErrorPathsForCh4O3CoGly) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* ch4 = FindIngestionModule("S5_L2_CH4");
  const harp_ingestion_module* o3 = FindIngestionModule("S5_L2_O3");
  const harp_ingestion_module* co = FindIngestionModule("S5_L2_CO");
  const harp_ingestion_module* gly = FindIngestionModule("S5_L2_GLY");
  ASSERT_NE(nullptr, ch4);
  ASSERT_NE(nullptr, o3);
  ASSERT_NE(nullptr, co);
  ASSERT_NE(nullptr, gly);

  for (const auto* mod : {ch4, o3, co, gly}) {
    const char* ptype =
        (mod == ch4) ? "SN5_02_CH4"
                     : (mod == o3 ? "SN5_02_O3_"
                                  : (mod == co ? "SN5_02_CO_" : "SN5_02_GLY"));
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("Sentinel5", ptype, 1));
    S5Groups g = CreateStandardGroups(prod);
    PopulateDimensionsAndCore(prod, g, kScanlines, kPixels, kLayers,
                              mod == ch4 ? kSifWavelengths : 0, 0);
    IngestExerciseResult res = ExerciseS5Module(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
  }
}

}  // namespace
}  // namespace stcorp_harp
