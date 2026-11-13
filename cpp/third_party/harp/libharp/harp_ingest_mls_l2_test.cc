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
// Unit tests for Aura MLS L2 ingestion modules (libharp/harp-ingest-mls_l2.c).
//
// Google3 build notes: HDF4 is disabled (#undef HAVE_HDF4); CODA products are
// synthesized in memory via CodaMemProduct without .codadef files.
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-mls_l2.c

#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::DoubleNear;
using ::testing::ElementsAre;

constexpr double kSecondsFrom1993To2000 = 220838400.0 + 5.0;
constexpr double kMissingDouble = -999.99;

struct MlsSwathBuilder {
  CodaMemProduct prod;
  CodaMemNode* geo = nullptr;
  CodaMemNode* data = nullptr;

  explicit MlsSwathBuilder(std::string_view swath_name) {
    if (!prod.ok()) return;
    CodaMemNode* hdfeos = prod.MakeRecord();
    CodaMemNode* swaths = prod.MakeRecord();
    CodaMemNode* swath = prod.MakeRecord();
    geo = prod.MakeRecord();
    data = prod.MakeRecord();
    if (!prod.AddField(swath, "Geolocation_Fields", geo) ||
        !prod.AddField(swath, "Data_Fields", data) ||
        !prod.AddField(swaths, swath_name, swath) ||
        !prod.AddField(hdfeos, "SWATHS", swaths) ||
        !prod.AddField(prod.root(), "HDFEOS", hdfeos)) {
      geo = nullptr;
      data = nullptr;
    }
  }

  bool ok() const { return prod.ok() && geo != nullptr && data != nullptr; }

  bool AddDoubleField(CodaMemNode* parent, std::string_view name,
                      std::span<const long> dims,
                      std::span<const double> values,
                      double missing_value = kMissingDouble,
                      bool include_scale = false, double scale_factor = 1.0,
                      bool include_offset = false, double offset = 0.0) {
    CodaMemNode* node = prod.MakeDoubleArray(dims, values);
    if (node == nullptr) return false;
    constexpr long kAttrDims[] = {1};
    const double missing_arr[] = {missing_value};
    if (!prod.AddAttribute(node, "MissingValue",
                           prod.MakeDoubleArray(kAttrDims, missing_arr))) {
      return false;
    }
    if (include_scale) {
      const double scale_arr[] = {scale_factor};
      if (!prod.AddAttribute(node, "ScaleFactor",
                             prod.MakeDoubleArray(kAttrDims, scale_arr))) {
        return false;
      }
    }
    if (include_offset) {
      const double offset_arr[] = {offset};
      if (!prod.AddAttribute(node, "Offset",
                             prod.MakeDoubleArray(kAttrDims, offset_arr))) {
        return false;
      }
    }
    return prod.AddField(parent, name, node);
  }

  bool AddInt32Field(CodaMemNode* parent, std::string_view name,
                     std::span<const long> dims,
                     std::span<const int32_t> values) {
    return prod.AddField(parent, name, prod.MakeInt32Array(dims, values));
  }
};

struct MlsModuleSpec {
  const char* module_name;
  std::string_view swath_name;
  std::string_view value_var;
  std::string_view uncertainty_var;
  std::string_view validity_var;
  double valid_pressure;
  double valid_quality;
  double valid_convergence;
};

constexpr MlsModuleSpec kAllModules[] = {
    {"MLS_L2_BRO", "BrO", "BrO_volume_mixing_ratio",
     "BrO_volume_mixing_ratio_uncertainty", "BrO_volume_mixing_ratio_validity",
     5.0, 1.5, 1.01},
    {"MLS_L2_CH3Cl", "CH3Cl", "CH3Cl_volume_mixing_ratio",
     "CH3Cl_volume_mixing_ratio_uncertainty",
     "CH3Cl_volume_mixing_ratio_validity", 50.0, 1.5, 1.01},
    {"MLS_L2_CH3CN", "CH3CN", "CH3CN_volume_mixing_ratio",
     "CH3CN_volume_mixing_ratio_uncertainty",
     "CH3CN_volume_mixing_ratio_validity", 10.0, 1.5, 1.01},
    {"MLS_L2_CH3OH", "CH3OH", "CH3OH_volume_mixing_ratio",
     "CH3OH_volume_mixing_ratio_uncertainty",
     "CH3OH_volume_mixing_ratio_validity", 10.0, 0.0, 5.0},
    {"MLS_L2_CLO", "ClO", "ClO_volume_mixing_ratio",
     "ClO_volume_mixing_ratio_uncertainty", "ClO_volume_mixing_ratio_validity",
     10.0, 1.5, 1.01},
    {"MLS_L2_CO", "CO", "CO_volume_mixing_ratio",
     "CO_volume_mixing_ratio_uncertainty", "CO_volume_mixing_ratio_validity",
     180.0, 1.6, 1.01},
    {"MLS_L2_GPH", "GPH", "geopotential_height",
     "geopotential_height_uncertainty", "geopotential_height_validity", 150.0,
     1.0, 1.01},
    {"MLS_L2_H2O", "H2O", "H2O_volume_mixing_ratio",
     "H2O_volume_mixing_ratio_uncertainty", "H2O_volume_mixing_ratio_validity",
     200.0, 0.8, 1.5},
    {"MLS_L2_HCL", "HCl", "HCl_volume_mixing_ratio",
     "HCl_volume_mixing_ratio_uncertainty", "HCl_volume_mixing_ratio_validity",
     10.0, 1.5, 1.01},
    {"MLS_L2_HCN", "HCN", "HCN_volume_mixing_ratio",
     "HCN_volume_mixing_ratio_uncertainty", "HCN_volume_mixing_ratio_validity",
     5.0, 0.5, 1.5},
    {"MLS_L2_HNO3", "HNO3", "HNO3_volume_mixing_ratio",
     "HNO3_volume_mixing_ratio_uncertainty",
     "HNO3_volume_mixing_ratio_validity", 100.0, 1.0, 1.01},
    {"MLS_L2_HO2", "HO2", "HO2_volume_mixing_ratio",
     "HO2_volume_mixing_ratio_uncertainty", "HO2_volume_mixing_ratio_validity",
     5.0, 0.0, 1.05},
    {"MLS_L2_HOCL", "HOCl", "HOCl_volume_mixing_ratio",
     "HOCl_volume_mixing_ratio_uncertainty",
     "HOCl_volume_mixing_ratio_validity", 5.0, 1.5, 1.01},
    {"MLS_L2_IWC", "IWC", "ice_water_density", "ice_water_density_uncertainty",
     "ice_water_density_validity", 150.0, 0.0, 5.0},
    {"MLS_L2_N2O", "N2O", "N2O_volume_mixing_ratio",
     "N2O_volume_mixing_ratio_uncertainty", "N2O_volume_mixing_ratio_validity",
     10.0, 1.2, 1.5},
    {"MLS_L2_O3", "O3", "O3_volume_mixing_ratio",
     "O3_volume_mixing_ratio_uncertainty", "O3_volume_mixing_ratio_validity",
     150.0, 1.2, 1.01},
    {"MLS_L2_OH", "OH", "OH_volume_mixing_ratio",
     "OH_volume_mixing_ratio_uncertainty", "OH_volume_mixing_ratio_validity",
     5.0, 0.0, 1.05},
    {"MLS_L2_RHI", "RHI", "relative_humidity_ice",
     "relative_humidity_ice_uncertainty", "relative_humidity_ice_validity",
     90.0, 0.0, 1.5},
    {"MLS_L2_SO2", "SO2", "SO2_volume_mixing_ratio",
     "SO2_volume_mixing_ratio_uncertainty", "SO2_volume_mixing_ratio_validity",
     50.0, 1.0, 1.01},
    {"MLS_L2_T", "Temperature", "temperature", "temperature_uncertainty",
     "temperature_validity", 150.0, 1.0, 1.01},
};

TEST(HarpIngestMlsL2Test, IngestAllTwentyProductsSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr long kTimes = 2;
  constexpr long kLevels = 2;
  const long t_dims[] = {kTimes};
  const long l_dims[] = {kLevels};
  const long tl_dims[] = {kTimes, kLevels};

  for (const auto& spec : kAllModules) {
    const harp_ingestion_module* mod = FindIngestionModule(spec.module_name);
    ASSERT_NE(nullptr, mod) << spec.module_name;

    MlsSwathBuilder b(spec.swath_name);
    ASSERT_TRUE(b.ok());

    const double times[] = {kSecondsFrom1993To2000 + 10.0,
                            kSecondsFrom1993To2000 + 20.0};
    const double lons[] = {-10.0, 25.0};
    const double lats[] = {45.0, kMissingDouble};
    const double pressures[] = {spec.valid_pressure, spec.valid_pressure};
    const double values[] = {1.0, kMissingDouble, 3.0, 4.0};
    const double prec[] = {0.1, 0.2, 0.3, 0.4};
    const int32_t status[] = {0, 0};
    const double quality[] = {spec.valid_quality, spec.valid_quality};
    const double convergence[] = {spec.valid_convergence,
                                  spec.valid_convergence};

    ASSERT_TRUE(b.AddDoubleField(b.geo, "Time", t_dims, times));
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Longitude", t_dims, lons,
                                 kMissingDouble, /*include_scale=*/true, 2.0,
                                 /*include_offset=*/true, 1.0));
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Latitude", t_dims, lats));
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Pressure", l_dims, pressures));
    ASSERT_TRUE(b.AddDoubleField(b.data, "L2gpValue", tl_dims, values));
    ASSERT_TRUE(b.AddDoubleField(b.data, "L2gpPrecision", tl_dims, prec));
    ASSERT_TRUE(b.AddInt32Field(b.data, "Status", t_dims, status));
    ASSERT_TRUE(b.AddDoubleField(b.data, "Quality", t_dims, quality));
    ASSERT_TRUE(b.AddDoubleField(b.data, "Convergence", t_dims, convergence));

    IngestExerciseResult res = ExerciseIngestionModule(mod, b.prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status) << spec.module_name;
    EXPECT_EQ(kTimes, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(kLevels, res.dimensions[harp_dimension_vertical]);

    EXPECT_THAT(res.variables["datetime"].double_values,
                ElementsAre(DoubleNear(10.0, 1e-6), DoubleNear(20.0, 1e-6)));
    // Longitude scaled by 2.0 + 1.0 -> -19.0, 51.0.
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(-19.0, 51.0));
    EXPECT_EQ(45.0, res.variables["latitude"].double_values[0]);
    EXPECT_TRUE(std::isnan(res.variables["latitude"].double_values[1]));
    EXPECT_THAT(res.variables["pressure"].double_values,
                ElementsAre(spec.valid_pressure, spec.valid_pressure));
    EXPECT_EQ(1.0, res.variables[std::string(spec.value_var)].double_values[0]);
    EXPECT_TRUE(std::isnan(
        res.variables[std::string(spec.value_var)].double_values[1]));
    EXPECT_EQ(
        4u,
        res.variables[std::string(spec.uncertainty_var)].double_values.size());
    EXPECT_THAT(res.variables[std::string(spec.validity_var)].int32_values,
                ElementsAre(0, 0, 0, 0))
        << spec.module_name;
  }
}

TEST(HarpIngestMlsL2Test, ValidityChecksMissingConvergenceAndHno3Rules) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // 1. Missing Convergence falls back to 0.0; exercise all 4 check bits
  // (0x801 pressure out of range, 0x1001 quality too low, 0x2001 convergence
  // too high, 0x4001 precision <= 0) on O3.
  {
    const harp_ingestion_module* mod = FindIngestionModule("MLS_L2_O3");
    ASSERT_NE(nullptr, mod);

    MlsSwathBuilder b("O3");
    ASSERT_TRUE(b.ok());
    const long t_dims[] = {2};
    const long l_dims[] = {2};
    const long tl_dims[] = {2, 2};
    const double times[] = {kSecondsFrom1993To2000, kSecondsFrom1993To2000};
    const double lons[] = {0.0, 1.0};
    const double lats[] = {0.0, 1.0};
    // Level 0: 500 hPa (out of range for O3 [0.02..100, 121..261]).
    // Level 1: 50 hPa (in range).
    const double pressures[] = {500.0, 50.0};
    const double values[] = {1e-6, 2e-6, 3e-6, 4e-6};
    // (0,0): prec=0.1; (0,1): prec=-0.5 (bit 14 -> 0x4001).
    const double prec[] = {0.1, -0.5, 0.2, 0.3};
    const int32_t status[] = {0, 0};
    // Time 0: quality 0.5 < 1.0 (bit 12 -> 0x1001).
    // Time 1: quality 1.5 >= 1.0.
    const double quality[] = {0.5, 1.5};

    ASSERT_TRUE(b.AddDoubleField(b.geo, "Time", t_dims, times));
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Longitude", t_dims, lons));
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Latitude", t_dims, lats));
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Pressure", l_dims, pressures));
    ASSERT_TRUE(b.AddDoubleField(b.data, "L2gpValue", tl_dims, values));
    ASSERT_TRUE(b.AddDoubleField(b.data, "L2gpPrecision", tl_dims, prec));
    ASSERT_TRUE(b.AddInt32Field(b.data, "Status", t_dims, status));
    ASSERT_TRUE(b.AddDoubleField(b.data, "Quality", t_dims, quality));
    // Intentionally omit Convergence.

    IngestExerciseResult res = ExerciseIngestionModule(mod, b.prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    // (0,0): pressure out of range => 0x801 | 0x1001 | 0x2001 (since
    // pressure_is_within_limits=0 skips setting quality/convergence flags!)
    // = 0x3801.
    // (0,1): pressure in range, quality too low => 0x1001, convergence OK (0),
    // precision <= 0 => 0x4001 => 0x5001.
    // (1,0): pressure out of range => 0x3801.
    // (1,1): all ok => 0.
    EXPECT_THAT(res.variables["O3_volume_mixing_ratio_validity"].int32_values,
                ElementsAre(0x3801, 0x5001, 0x3801, 0));
  }

  // 2. Convergence too high (bit 13 -> 0x2001).
  {
    const harp_ingestion_module* mod = FindIngestionModule("MLS_L2_O3");
    ASSERT_NE(nullptr, mod);
    MlsSwathBuilder b("O3");
    ASSERT_TRUE(b.ok());
    const long t_dims[] = {1};
    const long l_dims[] = {1};
    const long tl_dims[] = {1, 1};
    const double times[] = {kSecondsFrom1993To2000};
    const double lons[] = {0.0};
    const double lats[] = {0.0};
    const double pressures[] = {50.0};
    const double values[] = {1e-6};
    const double prec[] = {0.1};
    const int32_t status[] = {0};
    const double quality[] = {1.5};
    const double convergence[] = {2.0};  // > 1.03

    ASSERT_TRUE(b.AddDoubleField(b.geo, "Time", t_dims, times));
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Longitude", t_dims, lons));
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Latitude", t_dims, lats));
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Pressure", l_dims, pressures));
    ASSERT_TRUE(b.AddDoubleField(b.data, "L2gpValue", tl_dims, values));
    ASSERT_TRUE(b.AddDoubleField(b.data, "L2gpPrecision", tl_dims, prec));
    ASSERT_TRUE(b.AddInt32Field(b.data, "Status", t_dims, status));
    ASSERT_TRUE(b.AddDoubleField(b.data, "Quality", t_dims, quality));
    ASSERT_TRUE(b.AddDoubleField(b.data, "Convergence", t_dims, convergence));

    IngestExerciseResult res = ExerciseIngestionModule(mod, b.prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["O3_volume_mixing_ratio_validity"].int32_values,
                ElementsAre(0x2001));
  }

  // 3. HNO3 special checks (cloud contamination bit 15 = 0x8001, and outlier
  // bit 16 = 0x10001 for >= 316 hPa with vmr < -2.0, and 68..215 hPa with
  // vmr < -1.6).
  {
    const harp_ingestion_module* mod = FindIngestionModule("MLS_L2_HNO3");
    ASSERT_NE(nullptr, mod);
    MlsSwathBuilder b("HNO3");
    ASSERT_TRUE(b.ok());
    const long t_dims[] = {1};
    const long l_dims[] = {3};
    const long tl_dims[] = {1, 3};
    const double times[] = {kSecondsFrom1993To2000};
    const double lons[] = {0.0};
    const double lats[] = {0.0};
    // Level 0: 50 hPa (<= 68, in HNO3 range [1.5..215]).
    // Level 1: 100 hPa (in [68..215]) with vmr = -1.8 (< -1.6).
    // Level 2: 316 hPa (out of HNO3 range [1.5..215] -> 0x3801, and >= 316
    //          with vmr = -2.5 < -2.0 -> 0x10001).
    const double pressures[] = {50.0, 100.0, 316.0};
    const double values[] = {1.0, -1.8, -2.5};
    const double prec[] = {0.1, 0.1, 0.1};
    const int32_t status[] = {2};  // non-zero status triggers bit 15 at <= 68
    const double quality[] = {1.0};
    const double convergence[] = {1.0};

    ASSERT_TRUE(b.AddDoubleField(b.geo, "Time", t_dims, times));
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Longitude", t_dims, lons));
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Latitude", t_dims, lats));
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Pressure", l_dims, pressures));
    ASSERT_TRUE(b.AddDoubleField(b.data, "L2gpValue", tl_dims, values));
    ASSERT_TRUE(b.AddDoubleField(b.data, "L2gpPrecision", tl_dims, prec));
    ASSERT_TRUE(b.AddInt32Field(b.data, "Status", t_dims, status));
    ASSERT_TRUE(b.AddDoubleField(b.data, "Quality", t_dims, quality));
    ASSERT_TRUE(b.AddDoubleField(b.data, "Convergence", t_dims, convergence));

    IngestExerciseResult res = ExerciseIngestionModule(mod, b.prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["HNO3_volume_mixing_ratio_validity"].int32_values,
                ElementsAre(2 | 0x8001, 2 | 0x10001, 2 | 0x3801 | 0x10001));
  }
}

TEST(HarpIngestMlsL2Test, InitAndVariableReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* o3 = FindIngestionModule("MLS_L2_O3");
  const harp_ingestion_module* hno3 = FindIngestionModule("MLS_L2_HNO3");
  ASSERT_NE(nullptr, o3);
  ASSERT_NE(nullptr, hno3);

  // 1. init_cursors errors: missing /HDFEOS/SWATHS, missing swath, missing
  // Data_Fields, missing Geolocation_Fields.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(o3, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(hdfeos, "SWATHS", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
    EXPECT_EQ(-1, ExerciseIngestionModule(o3, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = prod.MakeRecord();
    CodaMemNode* swaths = prod.MakeRecord();
    CodaMemNode* swath = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(swaths, "O3", swath));
    ASSERT_TRUE(prod.AddField(hdfeos, "SWATHS", swaths));
    ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
    EXPECT_EQ(-1, ExerciseIngestionModule(o3, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* hdfeos = prod.MakeRecord();
    CodaMemNode* swaths = prod.MakeRecord();
    CodaMemNode* swath = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(swath, "Data_Fields", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(swaths, "O3", swath));
    ASSERT_TRUE(prod.AddField(hdfeos, "SWATHS", swaths));
    ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
    EXPECT_EQ(-1, ExerciseIngestionModule(o3, prod.product()).status);
  }

  // 2. get_dimensions errors: missing L2gpValue, scalar L2gpValue.
  {
    MlsSwathBuilder b("O3");
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(-1, ExerciseIngestionModule(o3, b.prod.product()).status);
  }
  {
    MlsSwathBuilder b("O3");
    ASSERT_TRUE(b.ok());
    ASSERT_TRUE(b.prod.AddField(b.data, "L2gpValue", b.prod.MakeDouble(1.0)));
    EXPECT_EQ(-1, ExerciseIngestionModule(o3, b.prod.product()).status);
  }

  // 3. Missing variables + wrong dims/lengths/types/attributes via
  // stop_on_error=false.
  {
    MlsSwathBuilder b("O3");
    ASSERT_TRUE(b.ok());
    const long tl_dims[] = {2, 2};
    const double vals[] = {1.0, 2.0, 3.0, 4.0};
    ASSERT_TRUE(b.AddDoubleField(b.data, "L2gpValue", tl_dims, vals));
    // Time: wrong num_dimensions (2D instead of 1D).
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Time", tl_dims, vals));
    // Longitude: wrong dimension_0 (1 instead of 2).
    const long one_dim[] = {1};
    const double one_val[] = {1.0};
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Longitude", one_dim, one_val));
    // Latitude: scalar (coda_cursor_get_array_dim fails).
    ASSERT_TRUE(b.prod.AddField(b.geo, "Latitude", b.prod.MakeDouble(1.0)));
    // Pressure: missing MissingValue attribute.
    const long two_dim[] = {2};
    const double two_val[] = {10.0, 20.0};
    ASSERT_TRUE(b.prod.AddField(b.geo, "Pressure",
                                b.prod.MakeDoubleArray(two_dim, two_val)));
    // L2gpPrecision: wrong dimension_1 (1 instead of 2).
    const long bad_2d[] = {2, 1};
    ASSERT_TRUE(b.AddDoubleField(b.data, "L2gpPrecision", bad_2d, two_val));
    // Status: scalar (coda_cursor_get_array_dim fails in read_int32_variable).
    ASSERT_TRUE(b.prod.AddField(b.data, "Status", b.prod.MakeInt32(0)));

    IngestExerciseResult res = ExerciseIngestionModule(
        o3, b.prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_volume_mixing_ratio_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio_validity"].read_status);
  }

  // 4. Attribute edge cases in get_variable_attributes + read_int32_variable
  // wrong dims/length/type + read_validity inner failures.
  {
    MlsSwathBuilder b("O3");
    ASSERT_TRUE(b.ok());
    const long tl_dims[] = {2, 2};
    const long two_dim[] = {2};
    const long zero_dim[] = {0};
    const double vals[] = {1.0, 2.0, 3.0, 4.0};
    const double two_val[] = {10.0, 20.0};
    ASSERT_TRUE(b.AddDoubleField(b.data, "L2gpValue", tl_dims, vals));

    // Time: empty MissingValue array -> goto_first_array_element fails.
    CodaMemNode* time_node = b.prod.MakeDoubleArray(two_dim, two_val);
    ASSERT_TRUE(b.prod.AddAttribute(time_node, "MissingValue",
                                    b.prod.MakeDoubleArray(zero_dim, {})));
    ASSERT_TRUE(b.prod.AddField(b.geo, "Time", time_node));

    // Longitude: non-numeric MissingValue -> read_double fails.
    CodaMemNode* lon_node = b.prod.MakeDoubleArray(two_dim, two_val);
    const long one_dim[] = {1};
    const std::string_view bad_str[] = {"bad"};
    ASSERT_TRUE(b.prod.AddAttribute(lon_node, "MissingValue",
                                    b.prod.MakeStringArray(one_dim, bad_str)));
    ASSERT_TRUE(b.prod.AddField(b.geo, "Longitude", lon_node));

    // Latitude: empty ScaleFactor array -> goto_first_array_element fails.
    CodaMemNode* lat_node = b.prod.MakeDoubleArray(two_dim, two_val);
    const double miss[] = {kMissingDouble};
    ASSERT_TRUE(b.prod.AddAttribute(lat_node, "MissingValue",
                                    b.prod.MakeDoubleArray(one_dim, miss)));
    ASSERT_TRUE(b.prod.AddAttribute(lat_node, "ScaleFactor",
                                    b.prod.MakeDoubleArray(zero_dim, {})));
    ASSERT_TRUE(b.prod.AddField(b.geo, "Latitude", lat_node));

    // Pressure: non-numeric ScaleFactor -> read_double fails.
    CodaMemNode* pres_node = b.prod.MakeDoubleArray(two_dim, two_val);
    ASSERT_TRUE(b.prod.AddAttribute(pres_node, "MissingValue",
                                    b.prod.MakeDoubleArray(one_dim, miss)));
    ASSERT_TRUE(b.prod.AddAttribute(pres_node, "ScaleFactor",
                                    b.prod.MakeStringArray(one_dim, bad_str)));
    ASSERT_TRUE(b.prod.AddField(b.geo, "Pressure", pres_node));

    // L2gpPrecision: empty Offset array -> goto_first_array_element fails.
    CodaMemNode* prec_node = b.prod.MakeDoubleArray(tl_dims, vals);
    ASSERT_TRUE(b.prod.AddAttribute(prec_node, "MissingValue",
                                    b.prod.MakeDoubleArray(one_dim, miss)));
    ASSERT_TRUE(b.prod.AddAttribute(prec_node, "Offset",
                                    b.prod.MakeDoubleArray(zero_dim, {})));
    ASSERT_TRUE(b.prod.AddField(b.data, "L2gpPrecision", prec_node));

    // Status: 2D -> wrong num_dimensions in read_int32_variable.
    const int32_t s2d[] = {0, 0, 0, 0};
    ASSERT_TRUE(b.AddInt32Field(b.data, "Status", tl_dims, s2d));

    IngestExerciseResult res = ExerciseIngestionModule(
        o3, b.prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_volume_mixing_ratio_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio_validity"].read_status);
  }

  // 5. Non-numeric Offset, non-numeric double array, Status wrong dim0 &
  // wrong type, missing Quality/L2gpPrecision in read_validity.
  {
    MlsSwathBuilder b("O3");
    ASSERT_TRUE(b.ok());
    const long tl_dims[] = {2, 2};
    const long two_dim[] = {2};
    const long one_dim[] = {1};
    const double vals[] = {1.0, 2.0, 3.0, 4.0};
    const double two_val[] = {10.0, 20.0};
    const double miss[] = {kMissingDouble};
    const std::string_view bad_str[] = {"bad"};
    const std::string_view bad_str2[] = {"a", "b"};

    ASSERT_TRUE(b.AddDoubleField(b.data, "L2gpValue", tl_dims, vals));
    // Time: non-numeric Offset -> read_double fails.
    CodaMemNode* time_node = b.prod.MakeDoubleArray(two_dim, two_val);
    ASSERT_TRUE(b.prod.AddAttribute(time_node, "MissingValue",
                                    b.prod.MakeDoubleArray(one_dim, miss)));
    ASSERT_TRUE(b.prod.AddAttribute(time_node, "Offset",
                                    b.prod.MakeStringArray(one_dim, bad_str)));
    ASSERT_TRUE(b.prod.AddField(b.geo, "Time", time_node));
    // Longitude: string array -> read_double_array fails.
    CodaMemNode* lon_node = b.prod.MakeStringArray(two_dim, bad_str2);
    ASSERT_TRUE(b.prod.AddAttribute(lon_node, "MissingValue",
                                    b.prod.MakeDoubleArray(one_dim, miss)));
    ASSERT_TRUE(b.prod.AddField(b.geo, "Longitude", lon_node));
    // Status: wrong dimension_0 (1 instead of 2).
    const int32_t s1[] = {0};
    ASSERT_TRUE(b.AddInt32Field(b.data, "Status", one_dim, s1));

    IngestExerciseResult res = ExerciseIngestionModule(
        o3, b.prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio_validity"].read_status);
  }

  // 6. Status string array (coda_cursor_read_int32_array fails), and missing
  // Quality / missing L2gpPrecision inside
  // perform_pressure_quality_convergence_precision_checks.
  {
    MlsSwathBuilder b("O3");
    ASSERT_TRUE(b.ok());
    const long tl_dims[] = {2, 2};
    const long two_dim[] = {2};
    const double vals[] = {1.0, 2.0, 3.0, 4.0};
    const std::string_view bad_str2[] = {"a", "b"};
    ASSERT_TRUE(b.AddDoubleField(b.data, "L2gpValue", tl_dims, vals));
    ASSERT_TRUE(b.prod.AddField(b.data, "Status",
                                b.prod.MakeStringArray(two_dim, bad_str2)));
    IngestExerciseResult res = ExerciseIngestionModule(
        o3, b.prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio_validity"].read_status);
  }
  {
    MlsSwathBuilder b("O3");
    ASSERT_TRUE(b.ok());
    const long tl_dims[] = {2, 2};
    const long two_dim[] = {2};
    const double vals[] = {1.0, 2.0, 3.0, 4.0};
    const double two_val[] = {50.0, 50.0};
    const int32_t s2[] = {0, 0};
    ASSERT_TRUE(b.AddDoubleField(b.data, "L2gpValue", tl_dims, vals));
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Pressure", two_dim, two_val));
    ASSERT_TRUE(b.AddInt32Field(b.data, "Status", two_dim, s2));
    // Missing Quality.
    IngestExerciseResult res = ExerciseIngestionModule(
        o3, b.prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio_validity"].read_status);
  }
  {
    MlsSwathBuilder b("O3");
    ASSERT_TRUE(b.ok());
    const long tl_dims[] = {2, 2};
    const long two_dim[] = {2};
    const double vals[] = {1.0, 2.0, 3.0, 4.0};
    const double two_val[] = {50.0, 50.0};
    const int32_t s2[] = {0, 0};
    ASSERT_TRUE(b.AddDoubleField(b.data, "L2gpValue", tl_dims, vals));
    ASSERT_TRUE(b.AddDoubleField(b.geo, "Pressure", two_dim, two_val));
    ASSERT_TRUE(b.AddInt32Field(b.data, "Status", two_dim, s2));
    ASSERT_TRUE(b.AddDoubleField(b.data, "Quality", two_dim, two_val));
    // Missing L2gpPrecision.
    IngestExerciseResult res = ExerciseIngestionModule(
        o3, b.prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio_validity"].read_status);
  }
  // Missing Pressure inside
  // perform_pressure_quality_convergence_precision_checks.
  {
    MlsSwathBuilder b("O3");
    ASSERT_TRUE(b.ok());
    const long tl_dims[] = {2, 2};
    const long two_dim[] = {2};
    const double vals[] = {1.0, 2.0, 3.0, 4.0};
    const int32_t s2[] = {0, 0};
    ASSERT_TRUE(b.AddDoubleField(b.data, "L2gpValue", tl_dims, vals));
    ASSERT_TRUE(b.AddInt32Field(b.data, "Status", two_dim, s2));
    IngestExerciseResult res = ExerciseIngestionModule(
        o3, b.prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio_validity"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
