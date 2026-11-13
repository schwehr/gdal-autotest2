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
// Unit tests for the GEOMS-TE-MWR ingestion module
// (libharp/harp-ingest-geoms-mwr.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-geoms-mwr.c

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

using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::IsNan;

struct GeomsMwrConfig {
  int version = 3;
  std::string_view gas = "O3";
  std::string_view data_template;
  std::string_view data_source;
  long num_time = 2;
  long num_vertical = 2;
  bool scalar_replicated = false;
  bool include_h2o_column = true;
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
                                 const GeomsMwrConfig& cfg,
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

void AddRootAttrWithOverrides(CodaMemProduct& prod, const GeomsMwrConfig& cfg,
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

void PopulateGeomsMwrProduct(CodaMemProduct& prod, const GeomsMwrConfig& cfg) {
  const std::string tmpl =
      cfg.data_template.empty()
          ? ("GEOMS-TE-MWR-00" + std::to_string(cfg.version))
          : std::string(cfg.data_template);
  const std::string src = cfg.data_source.empty()
                              ? ("MWR." + std::string(cfg.gas) + "_INST")
                              : std::string(cfg.data_source);
  AddRootAttrWithOverrides(prod, cfg, "DATA_TEMPLATE", tmpl);
  AddRootAttrWithOverrides(prod, cfg, "DATA_SOURCE", src);
  AddRootAttrWithOverrides(prod, cfg, "DATA_LOCATION", "NY.ALESUND");

  const long scalar_dims[] = {1};
  const double inst_alt[] = {20.0};
  const double inst_lat[] = {78.92};
  const double inst_lon[] = {11.92};
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "ALTITUDE_INSTRUMENT",
                                          scalar_dims, inst_alt));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "LATITUDE_INSTRUMENT",
                                          scalar_dims, inst_lat));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "LONGITUDE_INSTRUMENT",
                                          scalar_dims, inst_lon));

  const long time_dims[] = {cfg.num_time};
  std::vector<double> dt(static_cast<size_t>(cfg.num_time));
  std::vector<double> dt_start(static_cast<size_t>(cfg.num_time));
  std::vector<double> dt_stop(static_cast<size_t>(cfg.num_time));
  std::vector<double> vza(static_cast<size_t>(cfg.num_time));
  std::vector<double> sza(static_cast<size_t>(cfg.num_time));
  std::vector<double> vaa(static_cast<size_t>(cfg.num_time));
  std::vector<double> h2o_col(static_cast<size_t>(cfg.num_time));
  for (long t = 0; t < cfg.num_time; ++t) {
    const long tt = cfg.invert_time ? (cfg.num_time - 1 - t) : t;
    dt[static_cast<size_t>(t)] = 7000.0 + static_cast<double>(tt);
    dt_start[static_cast<size_t>(t)] = 6999.9 + static_cast<double>(tt);
    dt_stop[static_cast<size_t>(t)] = 7000.1 + static_cast<double>(tt);
    vza[static_cast<size_t>(t)] = 20.0 + static_cast<double>(t);
    sza[static_cast<size_t>(t)] = 50.0 + static_cast<double>(t);
    vaa[static_cast<size_t>(t)] = 180.0 + static_cast<double>(t);
    h2o_col[static_cast<size_t>(t)] = 1.5e22 + 1.0e21 * static_cast<double>(t);
  }
  ASSERT_TRUE(
      AddDoubleFieldWithOverrides(prod, cfg, "DATETIME", time_dims, dt));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "DATETIME_START",
                                          time_dims, dt_start));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "DATETIME_STOP", time_dims,
                                          dt_stop));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "ANGLE_VIEW_ZENITH_MEAN",
                                          time_dims, vza));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "ANGLE_SOLAR_ZENITH_MEAN",
                                          time_dims, sza));
  if (cfg.scalar_replicated) {
    const double vaa_scalar[] = {180.0};
    ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "ANGLE_VIEW_AZIMUTH",
                                            scalar_dims, vaa_scalar));
  } else {
    ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "ANGLE_VIEW_AZIMUTH",
                                            time_dims, vaa));
  }

  const long vert_dims[] = {cfg.num_vertical};
  std::vector<double> alt(static_cast<size_t>(cfg.num_vertical));
  for (long v = 0; v < cfg.num_vertical; ++v) {
    const long vv = cfg.invert_vertical ? (cfg.num_vertical - 1 - v) : v;
    alt[static_cast<size_t>(v)] = 10000.0 + 5000.0 * static_cast<double>(vv);
  }
  ASSERT_TRUE(
      AddDoubleFieldWithOverrides(prod, cfg, "ALTITUDE", vert_dims, alt));

  const long tv_dims[] = {cfg.num_time, cfg.num_vertical};
  const size_t tv_len = static_cast<size_t>(cfg.num_time * cfg.num_vertical);
  if (cfg.scalar_replicated) {
    std::vector<double> press(static_cast<size_t>(cfg.num_vertical));
    std::vector<double> temp(static_cast<size_t>(cfg.num_vertical));
    for (long v = 0; v < cfg.num_vertical; ++v) {
      press[static_cast<size_t>(v)] = (v == 0) ? cfg.fill_value : 200.0;
      temp[static_cast<size_t>(v)] = 220.0 - 5.0 * static_cast<double>(v);
    }
    ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "PRESSURE_INDEPENDENT",
                                            vert_dims, press));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, "TEMPERATURE_INDEPENDENT", vert_dims, temp));
  } else {
    std::vector<double> press(tv_len, 200.0);
    std::vector<double> temp(tv_len, 220.0);
    ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "PRESSURE_INDEPENDENT",
                                            tv_dims, press));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, "TEMPERATURE_INDEPENDENT", tv_dims, temp));
  }

  std::vector<double> vmr(tv_len);
  std::vector<double> vmr_rand(tv_len);
  std::vector<double> vmr_sys(tv_len);
  std::vector<double> vmr_ap(tv_len);
  for (size_t i = 0; i < tv_len; ++i) {
    vmr[i] = 5.0 + static_cast<double>(i);
    vmr_rand[i] =
        (cfg.version == 1) ? 10.0 : (0.25 + 0.25 * static_cast<double>(i));
    vmr_sys[i] =
        (cfg.version == 1) ? 20.0 : (0.5 + 0.25 * static_cast<double>(i));
    vmr_ap[i] = 4.0 + static_cast<double>(i);
  }
  const long avk_dims[] = {cfg.num_time, cfg.num_vertical, cfg.num_vertical};
  std::vector<double> avk(
      static_cast<size_t>(cfg.num_time * cfg.num_vertical * cfg.num_vertical),
      0.5);

  const std::string gas(cfg.gas);
  const std::string vol = (cfg.version == 1) ? "" : "_VOLUME";
  const std::string unc_suffix = (cfg.version == 1) ? "" : "_STANDARD";
  ASSERT_TRUE(AddDoubleFieldWithOverrides(
      prod, cfg, gas + "_MIXING_RATIO" + vol + "_EMISSION", tv_dims, vmr));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(
      prod, cfg,
      gas + "_MIXING_RATIO" + vol + "_EMISSION_UNCERTAINTY_RANDOM" + unc_suffix,
      tv_dims, vmr_rand));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(
      prod, cfg,
      gas + "_MIXING_RATIO" + vol + "_EMISSION_UNCERTAINTY_SYSTEMATIC" +
          unc_suffix,
      tv_dims, vmr_sys));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(
      prod, cfg, gas + "_MIXING_RATIO" + vol + "_EMISSION_APRIORI", tv_dims,
      vmr_ap));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(
      prod, cfg, gas + "_MIXING_RATIO" + vol + "_EMISSION_AVK", avk_dims, avk));

  if (cfg.include_h2o_column) {
    ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "H2O_COLUMN_DERIVED",
                                            time_dims, h2o_col));
  }
}

struct MwrSuccessCase {
  int version;
  std::string_view gas;
  std::string_view var_prefix;
  bool scalar_replicated;
  bool include_h2o_column;
};

constexpr MwrSuccessCase kSuccessCases[] = {
    {.version = 1,
     .gas = "O3",
     .var_prefix = "O3",
     .scalar_replicated = false,
     .include_h2o_column = true},
    {.version = 2,
     .gas = "H2O",
     .var_prefix = "H2O",
     .scalar_replicated = true,
     .include_h2o_column = false},
    {.version = 3,
     .gas = "ClO",
     .var_prefix = "ClO",
     .scalar_replicated = false,
     .include_h2o_column = true},
    {.version = 3,
     .gas = "CO",
     .var_prefix = "CO",
     .scalar_replicated = false,
     .include_h2o_column = false},
    {.version = 3,
     .gas = "HCN",
     .var_prefix = "HCN",
     .scalar_replicated = false,
     .include_h2o_column = false},
    {.version = 3,
     .gas = "HNO3",
     .var_prefix = "HNO3",
     .scalar_replicated = false,
     .include_h2o_column = false},
    {.version = 3,
     .gas = "N2O",
     .var_prefix = "N2O",
     .scalar_replicated = false,
     .include_h2o_column = false},
};

TEST(HarpIngestGeomsMwrTest, AllVersionsAndGasesSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-MWR");
  ASSERT_NE(nullptr, mod);

  for (const auto& tc : kSuccessCases) {
    SCOPED_TRACE(tc.gas);
    GeomsMwrConfig cfg;
    cfg.version = tc.version;
    cfg.gas = tc.gas;
    cfg.scalar_replicated = tc.scalar_replicated;
    cfg.include_h2o_column = tc.include_h2o_column;

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsMwrProduct(prod, cfg);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(2, res.dimensions[harp_dimension_vertical]);
    EXPECT_THAT(res.variables["location_name"].string_values,
                ElementsAre("NY.ALESUND"));
    EXPECT_THAT(res.variables["sensor_altitude"].double_values,
                ElementsAre(20.0));
    EXPECT_THAT(res.variables["sensor_latitude"].double_values,
                ElementsAre(78.92));
    EXPECT_THAT(res.variables["sensor_longitude"].double_values,
                ElementsAre(11.92));

    const std::string pfx(tc.var_prefix);
    EXPECT_THAT(res.variables[pfx + "_volume_mixing_ratio"].double_values,
                ElementsAre(5.0, 6.0, 7.0, 8.0));
    EXPECT_THAT(
        res.variables[pfx + "_volume_mixing_ratio_apriori"].double_values,
        ElementsAre(4.0, 5.0, 6.0, 7.0));
    EXPECT_EQ(
        8u,
        res.variables[pfx + "_volume_mixing_ratio_avk"].double_values.size());

    if (tc.version == 1) {
      // 10% and 20% of {5, 6, 7, 8}.
      EXPECT_THAT(res.variables[pfx + "_volume_mixing_ratio_uncertainty_random"]
                      .double_values,
                  ElementsAre(DoubleNear(0.5, 1e-12), DoubleNear(0.6, 1e-12),
                              DoubleNear(0.7, 1e-12), DoubleNear(0.8, 1e-12)));
      EXPECT_THAT(
          res.variables[pfx + "_volume_mixing_ratio_uncertainty_systematic"]
              .double_values,
          ElementsAre(DoubleNear(1.0, 1e-12), DoubleNear(1.2, 1e-12),
                      DoubleNear(1.4, 1e-12), DoubleNear(1.6, 1e-12)));
    } else {
      EXPECT_THAT(res.variables[pfx + "_volume_mixing_ratio_uncertainty_random"]
                      .double_values,
                  ElementsAre(0.25, 0.5, 0.75, 1.0));
      EXPECT_THAT(
          res.variables[pfx + "_volume_mixing_ratio_uncertainty_systematic"]
              .double_values,
          ElementsAre(0.5, 0.75, 1.0, 1.25));
    }

    if (tc.scalar_replicated) {
      EXPECT_THAT(res.variables["viewing_azimuth_angle"].double_values,
                  ElementsAre(180.0, 180.0));
      EXPECT_THAT(res.variables["pressure"].double_values,
                  ElementsAre(IsNan(), 200.0, IsNan(), 200.0));
      EXPECT_THAT(res.variables["temperature"].double_values,
                  ElementsAre(220.0, 215.0, 220.0, 215.0));
    }
    EXPECT_EQ(tc.include_h2o_column,
              res.variables["H2O_column_number_density"].included);
  }
}

TEST(HarpIngestGeomsMwrTest, NanFillValueInStandardAndReplicatedVars) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-MWR");
  ASSERT_NE(nullptr, mod);

  GeomsMwrConfig cfg;
  cfg.fill_value = std::numeric_limits<double>::quiet_NaN();
  cfg.scalar_replicated = true;
  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateGeomsMwrProduct(prod, cfg);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
  EXPECT_THAT(res.variables["pressure"].double_values,
              ElementsAre(IsNan(), 200.0, IsNan(), 200.0));
}

TEST(HarpIngestGeomsMwrTest, ProductDefinitionAndDimensionErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-MWR");
  ASSERT_NE(nullptr, mod);

  // 1. Missing @DATA_TEMPLATE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 2. Non-string @DATA_TEMPLATE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "DATA_TEMPLATE", prod.MakeInt32(1)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Invalid @DATA_TEMPLATE length (!= 16).
  {
    GeomsMwrConfig cfg;
    cfg.data_template = "SHORT";
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsMwrProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 4. Missing @DATA_SOURCE.
  {
    GeomsMwrConfig cfg;
    cfg.overrides["@DATA_SOURCE"] = [](CodaMemProduct&) { return nullptr; };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsMwrProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 5. Non-string @DATA_SOURCE.
  {
    GeomsMwrConfig cfg;
    cfg.overrides["@DATA_SOURCE"] = [](CodaMemProduct& p) {
      return p.MakeInt32(1);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsMwrProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 6. @DATA_SOURCE without "MWR." prefix.
  {
    GeomsMwrConfig cfg;
    cfg.data_source = "FTIR.O3_INST";
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsMwrProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 7. Unsupported gas in @DATA_SOURCE (and no underscore).
  {
    GeomsMwrConfig cfg;
    cfg.data_source = "MWR.CH4";
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsMwrProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 8. Missing DATETIME.
  {
    GeomsMwrConfig cfg;
    cfg.overrides["DATETIME"] = [](CodaMemProduct&) { return nullptr; };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsMwrProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 9. Non-double DATETIME with num_time > 1.
  {
    GeomsMwrConfig cfg;
    cfg.overrides["DATETIME"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {2};
      constexpr std::string_view kStrs[] = {"a", "b"};
      return p.MakeStringArray(kDims, kStrs);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsMwrProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 10. Decreasing DATETIME.
  {
    GeomsMwrConfig cfg;
    cfg.invert_time = true;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsMwrProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 11. Missing ALTITUDE.
  {
    GeomsMwrConfig cfg;
    cfg.overrides["ALTITUDE"] = [](CodaMemProduct&) { return nullptr; };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsMwrProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 12. Non-double ALTITUDE with num_vertical > 1.
  {
    GeomsMwrConfig cfg;
    cfg.overrides["ALTITUDE"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {2};
      constexpr std::string_view kStrs[] = {"a", "b"};
      return p.MakeStringArray(kDims, kStrs);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsMwrProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 13. Decreasing ALTITUDE.
  {
    GeomsMwrConfig cfg;
    cfg.invert_vertical = true;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsMwrProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
}

TEST(HarpIngestGeomsMwrTest, VariableAndScaledReadErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-MWR");
  ASSERT_NE(nullptr, mod);

  // 1. Standard, replicated, and attribute error branches on v1 product.
  {
    GeomsMwrConfig cfg;
    cfg.version = 1;
    cfg.overrides["@DATA_LOCATION"] = [](CodaMemProduct&) { return nullptr; };
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
      constexpr long kDims[] = {2};
      constexpr double kVals[] = {1.0, 2.0};
      return p.MakeDoubleArray(kDims, kVals);  // No @VAR_FILL_VALUE.
    };
    cfg.overrides["DATETIME_STOP"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {2};
      constexpr double kVals[] = {1.0, 2.0};
      CodaMemNode* n = p.MakeDoubleArray(kDims, kVals);
      p.AddAttribute(n, "VAR_FILL_VALUE", p.MakeString("bad"));
      return n;
    };
    cfg.overrides["ANGLE_VIEW_AZIMUTH"] = [](CodaMemProduct&) {
      return nullptr;
    };
    cfg.overrides["PRESSURE_INDEPENDENT"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {3};
      constexpr double kVals[] = {1.0, 2.0, 3.0};
      return p.MakeDoubleArray(kDims, kVals);  // Neither 2 nor 4 elements.
    };
    cfg.overrides["TEMPERATURE_INDEPENDENT"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {4};
      constexpr std::string_view kVals[] = {"a", "b", "c", "d"};
      return p.MakeStringArray(kDims, kVals);
    };
    // Scaled read failure in path1 (random unc missing) & path2 (VMR missing).
    cfg.overrides["O3_MIXING_RATIO_EMISSION"] = [](CodaMemProduct&) {
      return nullptr;
    };
    cfg.overrides["O3_MIXING_RATIO_EMISSION_UNCERTAINTY_RANDOM"] =
        [](CodaMemProduct&) { return nullptr; };

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsMwrProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["location_name"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_latitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_longitude"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1, res.variables["viewing_azimuth_angle"].read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(-1, res.variables["temperature"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["O3_volume_mixing_ratio_uncertainty_random"].read_status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio_uncertainty_systematic"]
                      .read_status);
  }

  // 2. Non-string @DATA_LOCATION and missing/bad @VAR_FILL_VALUE in replicated
  // variable.
  {
    GeomsMwrConfig cfg;
    cfg.overrides["@DATA_LOCATION"] = [](CodaMemProduct& p) {
      return p.MakeInt32(5);
    };
    cfg.overrides["PRESSURE_INDEPENDENT"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {4};
      constexpr double kVals[] = {1.0, 2.0, 3.0, 4.0};
      return p.MakeDoubleArray(kDims, kVals);  // No @VAR_FILL_VALUE.
    };
    cfg.overrides["TEMPERATURE_INDEPENDENT"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {4};
      constexpr double kVals[] = {1.0, 2.0, 3.0, 4.0};
      CodaMemNode* n = p.MakeDoubleArray(kDims, kVals);
      p.AddAttribute(n, "VAR_FILL_VALUE", p.MakeString("bad"));
      return n;
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsMwrProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["location_name"].read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(-1, res.variables["temperature"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
