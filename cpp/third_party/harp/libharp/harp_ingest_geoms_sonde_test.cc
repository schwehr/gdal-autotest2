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
// Unit tests for the GEOMS-TE-SONDE ingestion module
// (libharp/harp-ingest-geoms-sonde.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-geoms-sonde.c

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <string>
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

using ::testing::ElementsAre;
using ::testing::IsNan;

struct GeomsSondeConfig {
  int version = 2;
  std::string_view data_template;
  long num_time = 2;
  bool scalar_latlon = false;
  bool include_optional_v2 = true;
  bool invert_time = false;
  double dt_fill = -9999.0;
  float fill_value = -9999.0f;
  std::map<std::string, std::function<CodaMemNode*(CodaMemProduct&)>> overrides;
};

bool AddFloatVar(CodaMemProduct& prod, std::string_view name,
                 std::span<const long> dims, std::span<const float> vals,
                 float fill_value) {
  CodaMemNode* arr = prod.MakeFloatArray(dims, vals);
  if (arr == nullptr) {
    return false;
  }
  if (!prod.AddAttribute(arr, "VAR_FILL_VALUE", prod.MakeFloat(fill_value))) {
    return false;
  }
  return prod.AddField(prod.root(), name, arr);
}

bool AddFloatFieldWithOverrides(CodaMemProduct& prod,
                                const GeomsSondeConfig& cfg,
                                std::string_view name,
                                std::span<const long> dims,
                                std::span<const float> vals) {
  if (auto it = cfg.overrides.find(std::string(name));
      it != cfg.overrides.end()) {
    CodaMemNode* custom = it->second(prod);
    if (custom == nullptr) {
      return true;
    }
    return prod.AddField(prod.root(), name, custom);
  }
  return AddFloatVar(prod, name, dims, vals, cfg.fill_value);
}

bool AddCustomFieldOrDefault(
    CodaMemProduct& prod, const GeomsSondeConfig& cfg, std::string_view name,
    const std::function<CodaMemNode*(CodaMemProduct&)>& make_default) {
  if (auto it = cfg.overrides.find(std::string(name));
      it != cfg.overrides.end()) {
    CodaMemNode* custom = it->second(prod);
    if (custom == nullptr) {
      return true;
    }
    return prod.AddField(prod.root(), name, custom);
  }
  CodaMemNode* node = make_default(prod);
  if (node == nullptr) {
    return false;
  }
  return prod.AddField(prod.root(), name, node);
}

void AddRootAttrWithOverrides(CodaMemProduct& prod, const GeomsSondeConfig& cfg,
                              std::string_view name,
                              std::string_view default_val) {
  const std::string key = "@" + std::string(name);
  if (auto it = cfg.overrides.find(key); it != cfg.overrides.end()) {
    CodaMemNode* custom = it->second(prod);
    if (custom != nullptr) {
      ASSERT_TRUE(prod.AddAttribute(prod.root(), name, custom));
    }
    return;
  }
  ASSERT_TRUE(
      prod.AddAttribute(prod.root(), name, prod.MakeString(default_val)));
}

void PopulateGeomsSondeProduct(CodaMemProduct& prod,
                               const GeomsSondeConfig& cfg) {
  ASSERT_TRUE(prod.SetProductInfo("GEOMS", "SONDE", 100 + cfg.version));

  const std::string default_tmpl =
      (cfg.version < 3) ? "GEOMS-TE-SONDE-002" : "GEOMS-TE-SONDE-O3-003";
  AddRootAttrWithOverrides(
      prod, cfg, "DATA_TEMPLATE",
      cfg.data_template.empty() ? default_tmpl : cfg.data_template);
  AddRootAttrWithOverrides(prod, cfg, "DATA_SOURCE", "SONDE.ECC_TEST");
  AddRootAttrWithOverrides(prod, cfg, "DATA_LOCATION", "DE.BILT");

  const long time_dims[] = {cfg.num_time};
  const long scalar_dims[] = {1};
  std::vector<double> dt(static_cast<size_t>(cfg.num_time));
  for (long t = 0; t < cfg.num_time; ++t) {
    const long tt = cfg.invert_time ? (cfg.num_time - 1 - t) : t;
    dt[static_cast<size_t>(t)] = 7000.0 + static_cast<double>(tt);
  }
  ASSERT_TRUE(
      AddCustomFieldOrDefault(prod, cfg, "DATETIME", [&](CodaMemProduct& p) {
        CodaMemNode* arr = p.MakeDoubleArray(time_dims, dt);
        p.AddAttribute(arr, "VAR_FILL_VALUE", p.MakeDouble(cfg.dt_fill));
        return arr;
      }));

  if (cfg.scalar_latlon && cfg.version < 3) {
    const float lat[] = {52.1f};
    const float lon[] = {5.18f};
    ASSERT_TRUE(
        AddFloatFieldWithOverrides(prod, cfg, "LATITUDE", scalar_dims, lat));
    ASSERT_TRUE(
        AddFloatFieldWithOverrides(prod, cfg, "LONGITUDE", scalar_dims, lon));
  } else {
    std::vector<float> lat(static_cast<size_t>(cfg.num_time), 52.1f);
    std::vector<float> lon(static_cast<size_t>(cfg.num_time), 5.18f);
    ASSERT_TRUE(
        AddFloatFieldWithOverrides(prod, cfg, "LATITUDE", time_dims, lat));
    ASSERT_TRUE(
        AddFloatFieldWithOverrides(prod, cfg, "LONGITUDE", time_dims, lon));
  }

  std::vector<float> gph(static_cast<size_t>(cfg.num_time));
  std::vector<float> gph_unc(static_cast<size_t>(cfg.num_time), 5.0f);
  std::vector<float> press(static_cast<size_t>(cfg.num_time));
  std::vector<float> press_unc(static_cast<size_t>(cfg.num_time), 0.5f);
  std::vector<float> temp(static_cast<size_t>(cfg.num_time));
  std::vector<float> temp_unc(static_cast<size_t>(cfg.num_time), 0.3f);
  std::vector<float> rh(static_cast<size_t>(cfg.num_time));
  std::vector<float> rh_unc(static_cast<size_t>(cfg.num_time), 2.0f);
  std::vector<float> o3_pp(static_cast<size_t>(cfg.num_time));
  std::vector<float> o3_pp_unc(static_cast<size_t>(cfg.num_time), 0.1f);
  std::vector<float> wspd(static_cast<size_t>(cfg.num_time), 10.0f);
  std::vector<float> wdir(static_cast<size_t>(cfg.num_time), 270.0f);
  for (long t = 0; t < cfg.num_time; ++t) {
    gph[static_cast<size_t>(t)] = 100.0f + 500.0f * static_cast<float>(t);
    press[static_cast<size_t>(t)] =
        (t == 0) ? cfg.fill_value : (1000.0f - 100.0f * static_cast<float>(t));
    temp[static_cast<size_t>(t)] = 280.0f - 5.0f * static_cast<float>(t);
    rh[static_cast<size_t>(t)] = 75.0f - 5.0f * static_cast<float>(t);
    o3_pp[static_cast<size_t>(t)] = 3.5f + 0.5f * static_cast<float>(t);
  }
  ASSERT_TRUE(
      AddFloatFieldWithOverrides(prod, cfg, "ALTITUDE_GPH", time_dims, gph));

  if (cfg.version < 3) {
    ASSERT_TRUE(AddFloatFieldWithOverrides(prod, cfg, "PRESSURE_INSITU",
                                           time_dims, press));
    ASSERT_TRUE(AddFloatFieldWithOverrides(
        prod, cfg, "PRESSURE_INSITU_UNCERTAINTY_COMBINED_STANDARD", time_dims,
        press_unc));
    ASSERT_TRUE(AddFloatFieldWithOverrides(
        prod, cfg, "ALTITUDE_GPH_UNCERTAINTY_COMBINED_STANDARD", time_dims,
        gph_unc));
    ASSERT_TRUE(AddFloatFieldWithOverrides(prod, cfg, "TEMPERATURE_INSITU",
                                           time_dims, temp));
    ASSERT_TRUE(AddFloatFieldWithOverrides(
        prod, cfg, "TEMPERATURE_INSITU_UNCERTAINTY_COMBINED_STANDARD",
        time_dims, temp_unc));
    ASSERT_TRUE(AddFloatFieldWithOverrides(
        prod, cfg, "HUMIDITY_RELATIVE_INSITU", time_dims, rh));
    ASSERT_TRUE(AddFloatFieldWithOverrides(
        prod, cfg, "HUMIDITY_RELATIVE_INSITU_UNCERTAINTY_COMBINED_STANDARD",
        time_dims, rh_unc));
    if (cfg.include_optional_v2) {
      std::vector<float> o3_vmr(static_cast<size_t>(cfg.num_time), 0.04f);
      std::vector<float> o3_nd(static_cast<size_t>(cfg.num_time), 1.0e18f);
      std::vector<float> pot_t(static_cast<size_t>(cfg.num_time), 295.0f);
      std::vector<float> h2o_vmr(static_cast<size_t>(cfg.num_time), 5000.0f);
      ASSERT_TRUE(AddFloatFieldWithOverrides(
          prod, cfg, "O3_PARTIAL_PRESSURE_INSITU", time_dims, o3_pp));
      ASSERT_TRUE(AddFloatFieldWithOverrides(
          prod, cfg, "O3_PARTIAL_PRESSURE_INSITU_UNCERTAINTY_COMBINED_STANDARD",
          time_dims, o3_pp_unc));
      ASSERT_TRUE(AddFloatFieldWithOverrides(
          prod, cfg, "O3_MIXING_RATIO_VOLUME_INSITU", time_dims, o3_vmr));
      ASSERT_TRUE(AddFloatFieldWithOverrides(
          prod, cfg, "O3_NUMBER_DENSITY_INSITU", time_dims, o3_nd));
      ASSERT_TRUE(AddFloatFieldWithOverrides(prod, cfg, "WIND_SPEED_INSITU",
                                             time_dims, wspd));
      ASSERT_TRUE(AddFloatFieldWithOverrides(prod, cfg, "WIND_DIRECTION_INSITU",
                                             time_dims, wdir));
      ASSERT_TRUE(AddFloatFieldWithOverrides(
          prod, cfg, "POTENTIAL_TEMPERATURE_INSITU", time_dims, pot_t));
      ASSERT_TRUE(AddFloatFieldWithOverrides(
          prod, cfg, "H2O_MIXING_RATIO_VOLUME_INSITU", time_dims, h2o_vmr));
    }
  } else {
    std::vector<uint8_t> flag(static_cast<size_t>(cfg.num_time), 2);
    const float o3_col[] = {310.0f};
    ASSERT_TRUE(
        AddFloatFieldWithOverrides(prod, cfg, "PRESSURE", time_dims, press));
    ASSERT_TRUE(
        AddFloatFieldWithOverrides(prod, cfg, "TEMPERATURE", time_dims, temp));
    ASSERT_TRUE(AddFloatFieldWithOverrides(prod, cfg, "HUMIDITY_RELATIVE",
                                           time_dims, rh));
    ASSERT_TRUE(AddFloatFieldWithOverrides(prod, cfg, "O3_PARTIAL_PRESSURE",
                                           time_dims, o3_pp));
    ASSERT_TRUE(AddFloatFieldWithOverrides(
        prod, cfg, "O3_PARTIAL_PRESSURE_UNCERTAINTY_COMBINED_STANDARD",
        time_dims, o3_pp_unc));
    ASSERT_TRUE(AddCustomFieldOrDefault(
        prod, cfg, "O3_PARTIAL_PRESSURE_FLAG",
        [&](CodaMemProduct& p) { return p.MakeUint8Array(time_dims, flag); }));
    ASSERT_TRUE(AddFloatFieldWithOverrides(prod, cfg, "O3_COLUMN", scalar_dims,
                                           o3_col));
    ASSERT_TRUE(
        AddFloatFieldWithOverrides(prod, cfg, "WIND_SPEED", time_dims, wspd));
    ASSERT_TRUE(AddFloatFieldWithOverrides(prod, cfg, "WIND_DIRECTION",
                                           time_dims, wdir));
  }
}

TEST(HarpIngestGeomsSondeTest, V2ProfileAndPointsWithAllOptionalVars) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-SONDE");
  ASSERT_NE(nullptr, mod);

  // 1. Default profile mode with scalar lat/lon and all optional vars.
  {
    GeomsSondeConfig cfg;
    cfg.version = 2;
    cfg.scalar_latlon = true;
    cfg.include_optional_v2 = true;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsSondeProduct(prod, cfg);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_EQ(1, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(2, res.dimensions[harp_dimension_vertical]);
    EXPECT_THAT(res.variables["datetime_start"].double_values,
                ElementsAre(7000.0));
    EXPECT_THAT(res.variables["datetime_stop"].double_values,
                ElementsAre(7001.0));
    EXPECT_THAT(res.variables["latitude"].float_values,
                ElementsAre(52.1f, 52.1f));
    EXPECT_THAT(res.variables["longitude"].float_values,
                ElementsAre(5.18f, 5.18f));
    EXPECT_THAT(res.variables["pressure"].float_values,
                ElementsAre(IsNan(), 900.0f));
    EXPECT_TRUE(res.variables["O3_partial_pressure"].included);
    EXPECT_TRUE(res.variables["wind_speed"].included);
    EXPECT_TRUE(res.variables["wind_direction"].included);
    EXPECT_TRUE(res.variables["potential_temperature"].included);
    EXPECT_TRUE(res.variables["h2o_volume_mixing_ratio"].included);
  }

  // 2. Points mode (profile=false) with vector lat/lon and optional vars
  // absent.
  {
    GeomsSondeConfig cfg;
    cfg.version = 2;
    cfg.scalar_latlon = false;
    cfg.include_optional_v2 = false;
    cfg.fill_value = std::numeric_limits<float>::quiet_NaN();
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsSondeProduct(prod, cfg);

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "profile=false");
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_THAT(res.variables["datetime"].double_values,
                ElementsAre(7000.0, 7001.0));
    EXPECT_FALSE(res.variables["O3_partial_pressure"].included);
    EXPECT_FALSE(res.variables["wind_speed"].included);
    EXPECT_FALSE(res.variables["wind_direction"].included);
    EXPECT_FALSE(res.variables["potential_temperature"].included);
    EXPECT_FALSE(res.variables["h2o_volume_mixing_ratio"].included);
  }
}

TEST(HarpIngestGeomsSondeTest, V3ProfileAndPointsSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-SONDE");
  ASSERT_NE(nullptr, mod);

  for (const char* opts :
       {static_cast<const char*>(nullptr), "profile=false"}) {
    GeomsSondeConfig cfg;
    cfg.version = 3;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsSondeProduct(prod, cfg);

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), opts);
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_THAT(res.variables["O3_partial_pressure"].float_values,
                ElementsAre(3.5f, 4.0f));
    EXPECT_THAT(res.variables["O3_partial_pressure_validity"].int8_values,
                ElementsAre(2, 2));
    EXPECT_THAT(res.variables["O3_column_number_density"].float_values,
                ElementsAre(310.0f));
    EXPECT_THAT(res.variables["wind_speed"].float_values,
                ElementsAre(10.0f, 10.0f));
    EXPECT_THAT(res.variables["wind_direction"].float_values,
                ElementsAre(270.0f, 270.0f));
  }
}

TEST(HarpIngestGeomsSondeTest, InitAndDimensionErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-SONDE");
  ASSERT_NE(nullptr, mod);

  // 1. Invalid option value rejected.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    GeomsSondeConfig cfg;
    PopulateGeomsSondeProduct(prod, cfg);
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "profile=true");
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INVALID_INGESTION_OPTION_VALUE, res.failed_errno);
  }

  // 2. Missing @DATA_TEMPLATE.
  {
    GeomsSondeConfig cfg;
    cfg.overrides["@DATA_TEMPLATE"] = [](CodaMemProduct&) { return nullptr; };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsSondeProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 3. Non-string @DATA_TEMPLATE.
  {
    GeomsSondeConfig cfg;
    cfg.overrides["@DATA_TEMPLATE"] = [](CodaMemProduct& p) {
      return p.MakeInt32(1);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsSondeProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 4. Unsupported @DATA_TEMPLATE string.
  {
    GeomsSondeConfig cfg;
    cfg.data_template = "GEOMS-TE-SONDE-999";
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsSondeProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 5. Missing DATETIME, non-double DATETIME (>1 elems), decreasing DATETIME.
  {
    GeomsSondeConfig cfg;
    cfg.overrides["DATETIME"] = [](CodaMemProduct&) { return nullptr; };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsSondeProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    GeomsSondeConfig cfg;
    cfg.overrides["DATETIME"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {2};
      constexpr std::string_view kStrs[] = {"a", "b"};
      return p.MakeStringArray(kDims, kStrs);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsSondeProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    GeomsSondeConfig cfg;
    cfg.invert_time = true;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsSondeProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 6. Missing /LATITUDE in v2 fails get_latlon_length_v2.
  {
    GeomsSondeConfig cfg;
    cfg.version = 2;
    cfg.overrides["LATITUDE"] = [](CodaMemProduct&) { return nullptr; };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsSondeProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

TEST(HarpIngestGeomsSondeTest, VariableReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-SONDE");
  ASSERT_NE(nullptr, mod);

  // 1. 1-element string DATETIME passes get_dimensions but fails
  // read_datetime_start, read_datetime_stop, and read_datetime.
  for (const char* opts :
       {static_cast<const char*>(nullptr), "profile=false"}) {
    GeomsSondeConfig cfg;
    cfg.version = 2;
    cfg.num_time = 1;
    cfg.overrides["DATETIME"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {1};
      constexpr std::string_view kStrs[] = {"bad"};
      return p.MakeStringArray(kDims, kStrs);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsSondeProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), opts, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    if (opts == nullptr) {
      EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
      EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    } else {
      EXPECT_EQ(-1, res.variables["datetime"].read_status);
    }
  }

  // 2. Scalar lat/lon failure paths and float/attribute error branches.
  {
    GeomsSondeConfig cfg;
    cfg.version = 2;
    cfg.scalar_latlon = true;
    cfg.overrides["@DATA_SOURCE"] = [](CodaMemProduct&) { return nullptr; };
    cfg.overrides["@DATA_LOCATION"] = [](CodaMemProduct& p) {
      return p.MakeInt32(1);
    };
    cfg.overrides["LATITUDE"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {1};
      constexpr float kVals[] = {52.1f};
      return p.MakeFloatArray(kDims, kVals);  // Missing @VAR_FILL_VALUE.
    };
    cfg.overrides["LONGITUDE"] = [](CodaMemProduct& p) {
      // 2 elements: get_latlon_length_v2 was 1 from LATITUDE, so scalar path
      // expects 1 element for LONGITUDE and fails.
      constexpr long kDims[] = {2};
      constexpr float kVals[] = {5.0f, 6.0f};
      return p.MakeFloatArray(kDims, kVals);
    };
    cfg.overrides["PRESSURE_INSITU"] = [](CodaMemProduct&) { return nullptr; };
    cfg.overrides["ALTITUDE_GPH"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {1};
      constexpr float kVals[] = {100.0f};
      return p.MakeFloatArray(kDims, kVals);
    };
    cfg.overrides["TEMPERATURE_INSITU"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {2};
      constexpr std::string_view kVals[] = {"a", "b"};
      return p.MakeStringArray(kDims, kVals);
    };
    cfg.overrides["HUMIDITY_RELATIVE_INSITU"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {2};
      constexpr float kVals[] = {50.0f, 60.0f};
      CodaMemNode* n = p.MakeFloatArray(kDims, kVals);
      p.AddAttribute(n, "VAR_FILL_VALUE", p.MakeString("bad"));
      return n;
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsSondeProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["sensor_name"].read_status);
    EXPECT_EQ(-1, res.variables["location_name"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(-1, res.variables["geopotential_height"].read_status);
    EXPECT_EQ(-1, res.variables["temperature"].read_status);
    EXPECT_EQ(-1, res.variables["relative_humidity"].read_status);
  }

  // 3. V3 uint8 flag error branches (missing, wrong count, wrong type).
  {
    GeomsSondeConfig cfg;
    cfg.version = 3;
    cfg.overrides["O3_PARTIAL_PRESSURE_FLAG"] = [](CodaMemProduct&) {
      return nullptr;
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsSondeProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["O3_partial_pressure_validity"].read_status);
  }
  {
    GeomsSondeConfig cfg;
    cfg.version = 3;
    cfg.overrides["O3_PARTIAL_PRESSURE_FLAG"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {1};
      constexpr uint8_t kVals[] = {1};
      return p.MakeUint8Array(kDims, kVals);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsSondeProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["O3_partial_pressure_validity"].read_status);
  }
  {
    GeomsSondeConfig cfg;
    cfg.version = 3;
    cfg.overrides["O3_PARTIAL_PRESSURE_FLAG"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {2};
      constexpr std::string_view kVals[] = {"a", "b"};
      return p.MakeStringArray(kDims, kVals);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsSondeProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["O3_partial_pressure_validity"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
