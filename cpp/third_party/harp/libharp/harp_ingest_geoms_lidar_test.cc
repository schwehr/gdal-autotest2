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
// Unit tests for GEOMS LIDAR ingestion modules
// (libharp/harp-ingest-geoms-lidar.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-geoms-lidar.c

#include <cmath>
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

struct GeomsLidarConfig {
  std::string_view module_name = "GEOMS-TE-LIDAR-O3";
  std::string_view data_template = "GEOMS-TE-LIDAR-O3-005";
  long num_time = 1;
  long num_vertical = 3;
  bool invert_time = false;
  bool invert_vertical = false;
  double fill_value = -9999.0;
  std::map<std::string, std::function<CodaMemNode*(CodaMemProduct&)>> overrides;
};

bool AddDoubleVar(CodaMemProduct& prod, std::string_view name,
                  std::span<const long> dims, std::span<const double> vals,
                  double fill_value) {
  CodaMemNode* arr = prod.MakeDoubleArray(dims, vals);
  if (arr == nullptr) {
    return false;
  }
  if (!prod.AddAttribute(arr, "VAR_FILL_VALUE", prod.MakeDouble(fill_value))) {
    return false;
  }
  return prod.AddField(prod.root(), name, arr);
}

bool AddDoubleFieldWithOverrides(CodaMemProduct& prod,
                                 const GeomsLidarConfig& cfg,
                                 std::string_view name,
                                 std::span<const long> dims,
                                 std::span<const double> vals) {
  if (auto it = cfg.overrides.find(std::string(name));
      it != cfg.overrides.end()) {
    CodaMemNode* custom = it->second(prod);
    if (custom == nullptr) {
      return true;
    }
    return prod.AddField(prod.root(), name, custom);
  }
  return AddDoubleVar(prod, name, dims, vals, cfg.fill_value);
}

void AddRootAttrWithOverrides(CodaMemProduct& prod, const GeomsLidarConfig& cfg,
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

void PopulateGeomsLidarProduct(CodaMemProduct& prod,
                               const GeomsLidarConfig& cfg) {
  AddRootAttrWithOverrides(prod, cfg, "DATA_TEMPLATE", cfg.data_template);
  AddRootAttrWithOverrides(prod, cfg, "DATA_SOURCE", "LIDAR.TEST_001");
  AddRootAttrWithOverrides(prod, cfg, "DATA_LOCATION", "OHP");

  const long scalar_dims[] = {1};
  const double inst_lat[] = {43.93};
  const double inst_lon[] = {5.71};
  const double inst_alt[] = {670.0};
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "LATITUDE_INSTRUMENT",
                                          scalar_dims, inst_lat));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "LONGITUDE_INSTRUMENT",
                                          scalar_dims, inst_lon));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "ALTITUDE_INSTRUMENT",
                                          scalar_dims, inst_alt));

  const long time_dims[] = {cfg.num_time};
  std::vector<double> dt(static_cast<size_t>(cfg.num_time));
  std::vector<double> dt_start(static_cast<size_t>(cfg.num_time));
  std::vector<double> dt_stop(static_cast<size_t>(cfg.num_time));
  for (long t = 0; t < cfg.num_time; ++t) {
    const long tt = cfg.invert_time ? (cfg.num_time - 1 - t) : t;
    dt[static_cast<size_t>(t)] = 7000.0 + static_cast<double>(tt);
    dt_start[static_cast<size_t>(t)] = 6999.9 + static_cast<double>(tt);
    dt_stop[static_cast<size_t>(t)] = 7000.1 + static_cast<double>(tt);
  }
  ASSERT_TRUE(
      AddDoubleFieldWithOverrides(prod, cfg, "DATETIME", time_dims, dt));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "DATETIME_START",
                                          time_dims, dt_start));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "DATETIME_STOP", time_dims,
                                          dt_stop));

  const long vert_dims[] = {cfg.num_vertical};
  std::vector<double> alt(static_cast<size_t>(cfg.num_vertical));
  std::vector<double> press(static_cast<size_t>(cfg.num_vertical));
  std::vector<double> temp_ind(static_cast<size_t>(cfg.num_vertical));
  for (long v = 0; v < cfg.num_vertical; ++v) {
    const long vv = cfg.invert_vertical ? (cfg.num_vertical - 1 - v) : v;
    alt[static_cast<size_t>(v)] = 10000.0 + 1000.0 * static_cast<double>(vv);
    press[static_cast<size_t>(v)] = 250.0 - 20.0 * static_cast<double>(v);
    temp_ind[static_cast<size_t>(v)] = 220.0 - 2.0 * static_cast<double>(v);
  }
  ASSERT_TRUE(
      AddDoubleFieldWithOverrides(prod, cfg, "ALTITUDE", vert_dims, alt));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "PRESSURE_INDEPENDENT",
                                          vert_dims, press));
  if (cfg.module_name != "GEOMS-TE-LIDAR-TEMPERATURE") {
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, "TEMPERATURE_INDEPENDENT", vert_dims, temp_ind));
  }

  if (cfg.module_name == "GEOMS-TE-LIDAR-O3") {
    std::vector<double> o3(static_cast<size_t>(cfg.num_vertical));
    std::vector<double> o3_unc(static_cast<size_t>(cfg.num_vertical));
    for (long v = 0; v < cfg.num_vertical; ++v) {
      o3[static_cast<size_t>(v)] =
          (v == 0) ? cfg.fill_value
                   : (4.0e18 + static_cast<double>(v) * 1.0e17);
      o3_unc[static_cast<size_t>(v)] = 1.0e17 + static_cast<double>(v) * 1.0e16;
    }
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, "O3_NUMBER_DENSITY_ABSORPTION_DIFFERENTIAL", vert_dims, o3));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg,
        "O3_NUMBER_DENSITY_ABSORPTION_DIFFERENTIAL_UNCERTAINTY_COMBINED_"
        "STANDARD",
        vert_dims, o3_unc));
  } else if (cfg.module_name == "GEOMS-TE-LIDAR-H2O") {
    const long tv_dims[] = {cfg.num_time, cfg.num_vertical};
    const size_t n = static_cast<size_t>(cfg.num_time * cfg.num_vertical);
    std::vector<double> h2o(n);
    std::vector<double> h2o_unc(n);
    std::vector<double> rh(n);
    std::vector<double> rh_unc(n);
    for (size_t i = 0; i < n; ++i) {
      h2o[i] = 100.0 + static_cast<double>(i);
      h2o_unc[i] = 5.0 + static_cast<double>(i);
      rh[i] = 50.0 + static_cast<double>(i);
      rh_unc[i] = 2.0 + static_cast<double>(i);
    }
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, "H2O_MIXING_RATIO_VOLUME_BACKSCATTER", tv_dims, h2o));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg,
        "H2O_MIXING_RATIO_VOLUME_BACKSCATTER_UNCERTAINTY_COMBINED_STANDARD",
        tv_dims, h2o_unc));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, "HUMIDITY_RELATIVE_DERIVED", tv_dims, rh));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, "HUMIDITY_RELATIVE_DERIVED_UNCERTAINTY_COMBINED_STANDARD",
        tv_dims, rh_unc));
  } else if (cfg.module_name == "GEOMS-TE-LIDAR-TEMPERATURE") {
    std::vector<double> temp(static_cast<size_t>(cfg.num_vertical));
    std::vector<double> temp_unc(static_cast<size_t>(cfg.num_vertical));
    std::vector<double> nd(static_cast<size_t>(cfg.num_vertical));
    std::vector<double> nd_unc(static_cast<size_t>(cfg.num_vertical));
    for (long v = 0; v < cfg.num_vertical; ++v) {
      temp[static_cast<size_t>(v)] = 215.0 + static_cast<double>(v);
      temp_unc[static_cast<size_t>(v)] = 1.5 + 0.1 * static_cast<double>(v);
      nd[static_cast<size_t>(v)] = 1.0e23 - 1.0e22 * static_cast<double>(v);
      nd_unc[static_cast<size_t>(v)] = 1.0e21 + 1.0e20 * static_cast<double>(v);
    }
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, "TEMPERATURE_BACKSCATTER", vert_dims, temp));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, "TEMPERATURE_BACKSCATTER_UNCERTAINTY_COMBINED_STANDARD",
        vert_dims, temp_unc));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, "NUMBER_DENSITY_BACKSCATTER", vert_dims, nd));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, "NUMBER_DENSITY_BACKSCATTER_UNCERTAINTY_COMBINED_STANDARD",
        vert_dims, nd_unc));
  }
}

struct TemplateCase {
  std::string_view module_name;
  std::string_view template_name;
  long num_time;
};

constexpr TemplateCase kTemplateCases[] = {
    {.module_name = "GEOMS-TE-LIDAR-O3",
     .template_name = "GEOMS-TE-LIDAR-O3-003",
     .num_time = 1},
    {.module_name = "GEOMS-TE-LIDAR-O3",
     .template_name = "GEOMS-TE-LIDAR-O3-004",
     .num_time = 1},
    {.module_name = "GEOMS-TE-LIDAR-O3",
     .template_name = "GEOMS-TE-LIDAR-O3-005",
     .num_time = 1},
    {.module_name = "GEOMS-TE-LIDAR-H2O",
     .template_name = "GEOMS-TE-LIDAR-H2O-004",
     .num_time = 2},
    {.module_name = "GEOMS-TE-LIDAR-H2O",
     .template_name = "GEOMS-TE-LIDAR-H2O-005",
     .num_time = 2},
    {.module_name = "GEOMS-TE-LIDAR-TEMPERATURE",
     .template_name = "GEOMS-TE-LIDAR-TEMPERATURE-003",
     .num_time = 1},
    {.module_name = "GEOMS-TE-LIDAR-TEMPERATURE",
     .template_name = "GEOMS-TE-LIDAR-TEMPERATURE-004",
     .num_time = 1},
    {.module_name = "GEOMS-TE-LIDAR-TEMPERATURE",
     .template_name = "GEOMS-TE-LIDAR-TEMPERATURE-005",
     .num_time = 1},
};

TEST(HarpIngestGeomsLidarTest, AllTemplatesAndVersionsSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  for (const auto& tc : kTemplateCases) {
    SCOPED_TRACE(tc.template_name);
    const harp_ingestion_module* mod = FindIngestionModule(tc.module_name);
    ASSERT_NE(nullptr, mod);

    GeomsLidarConfig cfg;
    cfg.module_name = tc.module_name;
    cfg.data_template = tc.template_name;
    cfg.num_time = tc.num_time;

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsLidarProduct(prod, cfg);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_EQ(tc.num_time, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(cfg.num_vertical, res.dimensions[harp_dimension_vertical]);
    EXPECT_THAT(res.variables["sensor_name"].string_values,
                ElementsAre("LIDAR.TEST_001"));
    EXPECT_THAT(res.variables["location_name"].string_values,
                ElementsAre("OHP"));
    EXPECT_THAT(res.variables["sensor_latitude"].double_values,
                ElementsAre(43.93));
    EXPECT_THAT(res.variables["sensor_longitude"].double_values,
                ElementsAre(5.71));
    EXPECT_THAT(res.variables["sensor_altitude"].double_values,
                ElementsAre(670.0));
    EXPECT_THAT(res.variables["altitude"].double_values,
                ElementsAre(10000.0, 11000.0, 12000.0));
    EXPECT_THAT(res.variables["pressure"].double_values,
                ElementsAre(250.0, 230.0, 210.0));

    if (tc.module_name == "GEOMS-TE-LIDAR-O3") {
      EXPECT_THAT(res.variables["O3_number_density"].double_values,
                  ElementsAre(IsNan(), 4.1e18, 4.2e18));
      EXPECT_THAT(res.variables["temperature"].double_values,
                  ElementsAre(220.0, 218.0, 216.0));
    } else if (tc.module_name == "GEOMS-TE-LIDAR-H2O") {
      EXPECT_EQ(6u,
                res.variables["H2O_volume_mixing_ratio"].double_values.size());
      EXPECT_EQ(6u, res.variables["relative_humidity"].double_values.size());
      EXPECT_EQ(
          6u,
          res.variables["relative_humidity_uncertainty"].double_values.size());
    } else if (tc.module_name == "GEOMS-TE-LIDAR-TEMPERATURE") {
      EXPECT_THAT(res.variables["temperature"].double_values,
                  ElementsAre(215.0, 216.0, 217.0));
      EXPECT_THAT(res.variables["number_density"].double_values,
                  ElementsAre(1.0e23, 9.0e22, 8.0e22));
    }
  }
}

TEST(HarpIngestGeomsLidarTest, NanFillValueLeavesValuesUnchanged) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-LIDAR-O3");
  ASSERT_NE(nullptr, mod);

  GeomsLidarConfig cfg;
  cfg.fill_value = std::numeric_limits<double>::quiet_NaN();
  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateGeomsLidarProduct(prod, cfg);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
  EXPECT_TRUE(std::isnan(res.variables["O3_number_density"].double_values[0]));
  EXPECT_DOUBLE_EQ(4.1e18, res.variables["O3_number_density"].double_values[1]);
}

TEST(HarpIngestGeomsLidarTest, InitAndDimensionErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-LIDAR-O3");
  ASSERT_NE(nullptr, mod);

  // 1. Missing @DATA_TEMPLATE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 2. Non-string @DATA_TEMPLATE causes coda_cursor_read_string failure.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "DATA_TEMPLATE", prod.MakeInt32(42)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Unsupported @DATA_TEMPLATE string.
  {
    GeomsLidarConfig cfg;
    cfg.data_template = "GEOMS-TE-LIDAR-O3-999";
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsLidarProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 4. Missing /DATETIME.
  {
    GeomsLidarConfig cfg;
    cfg.overrides["DATETIME"] = [](CodaMemProduct&) { return nullptr; };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsLidarProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 5. Non-double /DATETIME with > 1 elements fails partial array read.
  {
    GeomsLidarConfig cfg;
    cfg.num_time = 2;
    cfg.overrides["DATETIME"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {2};
      constexpr std::string_view kStrs[] = {"a", "b"};
      return p.MakeStringArray(kDims, kStrs);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsLidarProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 6. Decreasing DATETIME.
  {
    GeomsLidarConfig cfg;
    cfg.num_time = 2;
    cfg.invert_time = true;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsLidarProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 7. Missing /ALTITUDE.
  {
    GeomsLidarConfig cfg;
    cfg.overrides["ALTITUDE"] = [](CodaMemProduct&) { return nullptr; };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsLidarProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 8. Non-double /ALTITUDE with > 1 elements fails partial array read.
  {
    GeomsLidarConfig cfg;
    cfg.overrides["ALTITUDE"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {2};
      constexpr std::string_view kStrs[] = {"a", "b"};
      return p.MakeStringArray(kDims, kStrs);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsLidarProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 9. Decreasing ALTITUDE.
  {
    GeomsLidarConfig cfg;
    cfg.invert_vertical = true;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsLidarProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
}

TEST(HarpIngestGeomsLidarTest, VariableReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-LIDAR-O3");
  ASSERT_NE(nullptr, mod);

  // 1. Missing @DATA_SOURCE and non-string @DATA_LOCATION.
  {
    GeomsLidarConfig cfg;
    cfg.overrides["@DATA_SOURCE"] = [](CodaMemProduct&) { return nullptr; };
    cfg.overrides["@DATA_LOCATION"] = [](CodaMemProduct& p) {
      return p.MakeInt32(99);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsLidarProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["sensor_name"].read_status);
    EXPECT_EQ(-1, res.variables["location_name"].read_status);
  }

  // 2. Variable read failures: missing field, wrong element count, wrong type,
  // missing @VAR_FILL_VALUE, and wrong @VAR_FILL_VALUE type.
  {
    GeomsLidarConfig cfg;
    cfg.overrides["ALTITUDE_INSTRUMENT"] = [](CodaMemProduct&) {
      return nullptr;
    };
    cfg.overrides["LATITUDE_INSTRUMENT"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {2};
      constexpr double kVals[] = {1.0, 2.0};
      return p.MakeDoubleArray(kDims, kVals);
    };
    cfg.overrides["LONGITUDE_INSTRUMENT"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {1};
      constexpr std::string_view kVals[] = {"bad"};
      return p.MakeStringArray(kDims, kVals);
    };
    cfg.overrides["DATETIME_START"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {1};
      constexpr double kVals[] = {1.0};
      return p.MakeDoubleArray(kDims, kVals);  // No @VAR_FILL_VALUE.
    };
    cfg.overrides["DATETIME_STOP"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {1};
      constexpr double kVals[] = {1.0};
      CodaMemNode* n = p.MakeDoubleArray(kDims, kVals);
      p.AddAttribute(n, "VAR_FILL_VALUE", p.MakeString("bad"));
      return n;
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsLidarProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["sensor_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_latitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_longitude"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
