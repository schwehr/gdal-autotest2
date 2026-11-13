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
// Unit tests for GEOMS Pandora ingestion modules
// (libharp/harp-ingest-geoms-pandora.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-geoms-pandora.c

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

struct GeomsPandoraConfig {
  bool offaxis = false;
  int version = 3;
  std::string_view geoms_gas = "O3";
  std::string_view data_template;
  std::string_view data_source;
  long num_time = 2;
  long num_vertical = 2;
  bool invert_time = false;
  bool set_product_info = true;
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
                                 const GeomsPandoraConfig& cfg,
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

bool AddCustomFieldOrDefault(
    CodaMemProduct& prod, const GeomsPandoraConfig& cfg, std::string_view name,
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

void AddRootAttrWithOverrides(CodaMemProduct& prod,
                              const GeomsPandoraConfig& cfg,
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

void PopulateGeomsPandoraProduct(CodaMemProduct& prod,
                                 const GeomsPandoraConfig& cfg) {
  if (cfg.set_product_info) {
    ASSERT_TRUE(prod.SetProductInfo(
        "GEOMS", cfg.offaxis ? "PANDORA_OFFAXIS_GAS" : "PANDORA_DIRECTSUN_GAS",
        1000 + cfg.version));
  }

  const std::string default_tmpl =
      (cfg.offaxis ? "GEOMS-TE-PANDORA-OFFAXIS-GAS-00"
                   : "GEOMS-TE-PANDORA-DIRECTSUN-GAS-00") +
      std::to_string(cfg.version);
  const std::string default_src =
      (cfg.offaxis ? "UVVIS.DOAS.OFFAXIS." : "UVVIS.DOAS.DIRECTSUN.") +
      std::string(cfg.geoms_gas) + "_TEST";
  AddRootAttrWithOverrides(
      prod, cfg, "DATA_TEMPLATE",
      cfg.data_template.empty() ? default_tmpl : cfg.data_template);
  AddRootAttrWithOverrides(
      prod, cfg, "DATA_SOURCE",
      cfg.data_source.empty() ? default_src : cfg.data_source);
  AddRootAttrWithOverrides(prod, cfg, "DATA_LOCATION", "INNSBRUCK");

  const long scalar_dims[] = {1};
  const double inst_lat[] = {47.26};
  const double inst_lon[] = {11.39};
  const double inst_alt[] = {616.0};
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
  std::vector<double> integ(static_cast<size_t>(cfg.num_time));
  std::vector<double> sza(static_cast<size_t>(cfg.num_time));
  std::vector<double> saa(static_cast<size_t>(cfg.num_time));
  for (long t = 0; t < cfg.num_time; ++t) {
    const long tt = cfg.invert_time ? (cfg.num_time - 1 - t) : t;
    dt[static_cast<size_t>(t)] = 7500.0 + static_cast<double>(tt);
    dt_start[static_cast<size_t>(t)] = 7499.9 + static_cast<double>(tt);
    dt_stop[static_cast<size_t>(t)] = 7500.1 + static_cast<double>(tt);
    integ[static_cast<size_t>(t)] = 30.0 + static_cast<double>(t);
    sza[static_cast<size_t>(t)] = 45.0 + static_cast<double>(t);
    saa[static_cast<size_t>(t)] = 150.0 + static_cast<double>(t);
  }
  ASSERT_TRUE(
      AddDoubleFieldWithOverrides(prod, cfg, "DATETIME", time_dims, dt));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "DATETIME_START",
                                          time_dims, dt_start));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "DATETIME_STOP", time_dims,
                                          dt_stop));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "INTEGRATION_TIME",
                                          time_dims, integ));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(
      prod, cfg, "ANGLE_SOLAR_ZENITH_ASTRONOMICAL", time_dims, sza));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "ANGLE_SOLAR_AZIMUTH",
                                          time_dims, saa));

  const std::string gas(cfg.geoms_gas);
  if (!cfg.offaxis) {
    std::vector<double> col(static_cast<size_t>(cfg.num_time));
    std::vector<double> unc_rand(static_cast<size_t>(cfg.num_time), 0.5);
    std::vector<double> unc_comb(static_cast<size_t>(cfg.num_time), 0.7);
    std::vector<double> unc_sys(static_cast<size_t>(cfg.num_time), 0.4);
    std::vector<double> amf(static_cast<size_t>(cfg.num_time), 1.8);
    std::vector<int32_t> flags(static_cast<size_t>(cfg.num_time), 0);
    std::vector<double> eff_t(static_cast<size_t>(cfg.num_time), 260.0);
    for (long t = 0; t < cfg.num_time; ++t) {
      col[static_cast<size_t>(t)] =
          (t == 0) ? cfg.fill_value : (300.0 + static_cast<double>(t));
      flags[static_cast<size_t>(t)] = static_cast<int32_t>(t);
    }
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, gas + "_COLUMN_ABSORPTION_SOLAR", time_dims, col));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, gas + "_COLUMN_ABSORPTION_SOLAR_UNCERTAINTY_RANDOM_STANDARD",
        time_dims, unc_rand));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, gas + "_COLUMN_ABSORPTION_SOLAR_AMF", time_dims, amf));
    ASSERT_TRUE(AddCustomFieldOrDefault(
        prod, cfg, gas + "_COLUMN_ABSORPTION_SOLAR_FLAG",
        [&](CodaMemProduct& p) { return p.MakeInt32Array(time_dims, flags); }));
    if (cfg.version >= 3) {
      ASSERT_TRUE(AddDoubleFieldWithOverrides(
          prod, cfg,
          gas + "_COLUMN_ABSORPTION_SOLAR_UNCERTAINTY_COMBINED_STANDARD",
          time_dims, unc_comb));
      ASSERT_TRUE(AddDoubleFieldWithOverrides(
          prod, cfg,
          gas + "_COLUMN_ABSORPTION_SOLAR_UNCERTAINTY_SYSTEMATIC_STANDARD",
          time_dims, unc_sys));
      ASSERT_TRUE(AddDoubleFieldWithOverrides(
          prod, cfg, "TEMPERATURE_EFFECTIVE_" + gas, time_dims, eff_t));
      ASSERT_TRUE(AddDoubleFieldWithOverrides(
          prod, cfg,
          "TEMPERATURE_EFFECTIVE_" + gas + "_UNCERTAINTY_COMBINED_STANDARD",
          time_dims, unc_comb));
      ASSERT_TRUE(AddDoubleFieldWithOverrides(
          prod, cfg,
          "TEMPERATURE_EFFECTIVE_" + gas + "_UNCERTAINTY_RANDOM_STANDARD",
          time_dims, unc_rand));
      ASSERT_TRUE(AddDoubleFieldWithOverrides(
          prod, cfg,
          "TEMPERATURE_EFFECTIVE_" + gas + "_UNCERTAINTY_SYSTEMATIC_STANDARD",
          time_dims, unc_sys));
    }
  } else {
    const long tv_dims[] = {cfg.num_time, cfg.num_vertical};
    const long tv2_dims[] = {cfg.num_time, cfg.num_vertical, 2};
    const size_t tv_len = static_cast<size_t>(cfg.num_time * cfg.num_vertical);
    std::vector<double> alt(tv_len);
    std::vector<double> bnds(tv_len * 2);
    std::vector<double> partial(tv_len);
    for (long t = 0; t < cfg.num_time; ++t) {
      for (long v = 0; v < cfg.num_vertical; ++v) {
        const size_t idx = static_cast<size_t>(t * cfg.num_vertical + v);
        alt[idx] = 0.5 + static_cast<double>(v);
        bnds[idx * 2] = static_cast<double>(v);
        bnds[idx * 2 + 1] = static_cast<double>(v) + 1.0;
        partial[idx] = 1.0e-4 * static_cast<double>(idx + 1);
      }
    }
    ASSERT_TRUE(
        AddDoubleFieldWithOverrides(prod, cfg, "ALTITUDE", tv_dims, alt));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "ALTITUDE_BOUNDARIES",
                                            tv2_dims, bnds));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, gas + "_COLUMN_PARTIAL_SCATTER_SOLAR_OFFAXIS", tv_dims,
        partial));

    std::vector<double> trop(static_cast<size_t>(cfg.num_time), 0.02);
    std::vector<double> trop_unc(static_cast<size_t>(cfg.num_time), 0.002);
    std::vector<double> surf(static_cast<size_t>(cfg.num_time), 1.0e-6);
    std::vector<double> surf_unc(static_cast<size_t>(cfg.num_time), 1.0e-7);
    std::vector<uint8_t> qual(static_cast<size_t>(cfg.num_time), 1);
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, gas + "_COLUMN_TROPOSPHERIC_SCATTER_SOLAR_OFFAXIS",
        time_dims, trop));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg,
        gas + "_COLUMN_TROPOSPHERIC_SCATTER_SOLAR_OFFAXIS_UNCERTAINTY_RANDOM_"
              "STANDARD",
        time_dims, trop_unc));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, gas + "_NUMBER_DENSITY_SURFACE_SCATTER_SOLAR_OFFAXIS",
        time_dims, surf));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg,
        gas +
            "_NUMBER_DENSITY_SURFACE_SCATTER_SOLAR_OFFAXIS_UNCERTAINTY_RANDOM_"
            "STANDARD",
        time_dims, surf_unc));
    ASSERT_TRUE(AddCustomFieldOrDefault(
        prod, cfg, "FLAG_MEASUREMENT_QUALITY",
        [&](CodaMemProduct& p) { return p.MakeUint8Array(time_dims, qual); }));
  }
}

struct PandoraDirectsunCase {
  int version;
  std::string_view geoms_gas;
  std::string_view harp_gas;
};

constexpr PandoraDirectsunCase kDirectsunCases[] = {
    {.version = 2, .geoms_gas = "NO2", .harp_gas = "NO2"},
    {.version = 2, .geoms_gas = "O3", .harp_gas = "O3"},
    {.version = 3, .geoms_gas = "NO2", .harp_gas = "NO2"},
    {.version = 3, .geoms_gas = "O3", .harp_gas = "O3"},
    {.version = 3, .geoms_gas = "SO2", .harp_gas = "SO2"},
    {.version = 3, .geoms_gas = "H2CO", .harp_gas = "HCHO"},
};

TEST(HarpIngestGeomsPandoraTest, DirectsunAllVersionsAndGasesSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("GEOMS-TE-PANDORA-DIRECTSUN-GAS");
  ASSERT_NE(nullptr, mod);

  for (const auto& tc : kDirectsunCases) {
    SCOPED_TRACE(tc.geoms_gas);
    GeomsPandoraConfig cfg;
    cfg.version = tc.version;
    cfg.geoms_gas = tc.geoms_gas;

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_THAT(res.variables["location_name"].string_values,
                ElementsAre("INNSBRUCK"));
    EXPECT_THAT(res.variables["sensor_latitude"].double_values,
                ElementsAre(47.26));
    EXPECT_THAT(res.variables["sensor_longitude"].double_values,
                ElementsAre(11.39));
    EXPECT_THAT(res.variables["sensor_altitude"].double_values,
                ElementsAre(616.0));
    EXPECT_THAT(res.variables["datetime_duration"].double_values,
                ElementsAre(30.0, 31.0));
    EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
                ElementsAre(45.0, 46.0));
    EXPECT_THAT(res.variables["solar_azimuth_angle"].double_values,
                ElementsAre(150.0, 151.0));

    const std::string pfx(tc.harp_gas);
    EXPECT_THAT(res.variables[pfx + "_column_number_density"].double_values,
                ElementsAre(IsNan(), 301.0));
    EXPECT_THAT(res.variables[pfx + "_column_number_density_amf"].double_values,
                ElementsAre(1.8, 1.8));
    EXPECT_THAT(
        res.variables[pfx + "_column_number_density_validity"].int32_values,
        ElementsAre(0, 1));
    if (tc.version >= 3) {
      EXPECT_THAT(res.variables[pfx + "_effective_temperature"].double_values,
                  ElementsAre(260.0, 260.0));
      EXPECT_THAT(res.variables[pfx + "_effective_temperature_uncertainty"]
                      .double_values,
                  ElementsAre(0.7, 0.7));
      EXPECT_THAT(
          res.variables[pfx + "_effective_temperature_uncertainty_random"]
              .double_values,
          ElementsAre(0.5, 0.5));
      EXPECT_THAT(
          res.variables[pfx + "_effective_temperature_uncertainty_systematic"]
              .double_values,
          ElementsAre(0.4, 0.4));
    }
  }
}

TEST(HarpIngestGeomsPandoraTest, OffaxisNo2AndH2coSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("GEOMS-TE-PANDORA-OFFAXIS-GAS");
  ASSERT_NE(nullptr, mod);

  struct OffaxisCase {
    std::string_view geoms_gas;
    std::string_view harp_gas;
  };
  constexpr OffaxisCase kOffaxisCases[] = {
      {.geoms_gas = "NO2", .harp_gas = "NO2"},
      {.geoms_gas = "H2CO", .harp_gas = "HCHO"},
  };

  for (const auto& tc : kOffaxisCases) {
    GeomsPandoraConfig cfg;
    cfg.offaxis = true;
    cfg.version = 1;
    cfg.geoms_gas = tc.geoms_gas;
    cfg.fill_value = std::numeric_limits<double>::quiet_NaN();

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status) << res.failed_variable;
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(2, res.dimensions[harp_dimension_vertical]);
    EXPECT_THAT(res.variables["altitude"].double_values,
                ElementsAre(0.5, 1.5, 0.5, 1.5));
    EXPECT_EQ(8u, res.variables["altitude_bounds"].double_values.size());
    EXPECT_THAT(res.variables["validity"].int8_values, ElementsAre(1, 1));

    const std::string pfx(tc.harp_gas);
    EXPECT_EQ(
        4u, res.variables[pfx + "_column_number_density"].double_values.size());
    EXPECT_THAT(res.variables["tropospheric_" + pfx + "_column_number_density"]
                    .double_values,
                ElementsAre(0.02, 0.02));
    EXPECT_THAT(res.variables["tropospheric_" + pfx +
                              "_column_number_density_uncertainty"]
                    .double_values,
                ElementsAre(0.002, 0.002));
    EXPECT_THAT(
        res.variables["surface_" + pfx + "_number_density"].double_values,
        ElementsAre(1.0e-6, 1.0e-6));
    EXPECT_THAT(res.variables["surface_" + pfx + "_number_density_uncertainty"]
                    .double_values,
                ElementsAre(1.0e-7, 1.0e-7));
  }
}

TEST(HarpIngestGeomsPandoraTest, ProductDefinitionAndDimensionErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* ds_mod =
      FindIngestionModule("GEOMS-TE-PANDORA-DIRECTSUN-GAS");
  const harp_ingestion_module* oa_mod =
      FindIngestionModule("GEOMS-TE-PANDORA-OFFAXIS-GAS");
  ASSERT_NE(nullptr, ds_mod);
  ASSERT_NE(nullptr, oa_mod);

  // 1. Missing @DATA_TEMPLATE.
  {
    GeomsPandoraConfig cfg;
    cfg.overrides["@DATA_TEMPLATE"] = [](CodaMemProduct&) { return nullptr; };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 2. Non-string @DATA_TEMPLATE.
  {
    GeomsPandoraConfig cfg;
    cfg.overrides["@DATA_TEMPLATE"] = [](CodaMemProduct& p) {
      return p.MakeInt32(1);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Invalid @DATA_TEMPLATE length (!= 32 and != 34).
  {
    GeomsPandoraConfig cfg;
    cfg.data_template = "SHORT";
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 4. Length 34 with invalid prefix, missing @DATA_SOURCE, non-string
  // @DATA_SOURCE, and bad @DATA_SOURCE prefix.
  {
    GeomsPandoraConfig cfg;
    cfg.data_template = "GEOMS-TE-PANDORA-DIRECTSUN-BAD-003";
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }
  {
    GeomsPandoraConfig cfg;
    cfg.overrides["@DATA_SOURCE"] = [](CodaMemProduct&) { return nullptr; };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }
  {
    GeomsPandoraConfig cfg;
    cfg.overrides["@DATA_SOURCE"] = [](CodaMemProduct& p) {
      return p.MakeInt32(1);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    GeomsPandoraConfig cfg;
    cfg.data_source = "UVVIS.DOAS.BAD.O3_TEST";
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 5. Length 32 (offaxis) with invalid prefix, missing @DATA_SOURCE,
  // non-string @DATA_SOURCE, and bad @DATA_SOURCE prefix.
  {
    GeomsPandoraConfig cfg;
    cfg.offaxis = true;
    cfg.version = 1;
    cfg.data_template = "GEOMS-TE-PANDORA-OFFAXIS-BAD-001";
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(oa_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }
  {
    GeomsPandoraConfig cfg;
    cfg.offaxis = true;
    cfg.version = 1;
    cfg.overrides["@DATA_SOURCE"] = [](CodaMemProduct&) { return nullptr; };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(oa_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }
  {
    GeomsPandoraConfig cfg;
    cfg.offaxis = true;
    cfg.version = 1;
    cfg.overrides["@DATA_SOURCE"] = [](CodaMemProduct& p) {
      return p.MakeInt32(1);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(oa_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    GeomsPandoraConfig cfg;
    cfg.offaxis = true;
    cfg.version = 1;
    cfg.data_source = "UVVIS.DOAS.BAD.NO2_TEST";
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(oa_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 6. Unsupported template+gas combination.
  {
    GeomsPandoraConfig cfg;
    cfg.version = 2;
    cfg.geoms_gas = "SO2";
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 7. Missing DATETIME, non-double DATETIME, decreasing DATETIME, and offaxis
  // missing ALTITUDE.
  {
    GeomsPandoraConfig cfg;
    cfg.overrides["DATETIME"] = [](CodaMemProduct&) { return nullptr; };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    GeomsPandoraConfig cfg;
    cfg.overrides["DATETIME"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {2};
      constexpr std::string_view kStrs[] = {"a", "b"};
      return p.MakeStringArray(kDims, kStrs);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    GeomsPandoraConfig cfg;
    cfg.invert_time = true;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(ds_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    GeomsPandoraConfig cfg;
    cfg.offaxis = true;
    cfg.version = 1;
    cfg.geoms_gas = "NO2";
    cfg.overrides["ALTITUDE"] = [](CodaMemProduct&) { return nullptr; };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(oa_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

TEST(HarpIngestGeomsPandoraTest, VariableReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* ds_mod =
      FindIngestionModule("GEOMS-TE-PANDORA-DIRECTSUN-GAS");
  const harp_ingestion_module* oa_mod =
      FindIngestionModule("GEOMS-TE-PANDORA-OFFAXIS-GAS");
  ASSERT_NE(nullptr, ds_mod);
  ASSERT_NE(nullptr, oa_mod);

  // 1. Directsun: attribute, double, and int32 read errors.
  {
    GeomsPandoraConfig cfg;
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
    cfg.overrides["O3_COLUMN_ABSORPTION_SOLAR_FLAG"] = [](CodaMemProduct&) {
      return nullptr;
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res =
        ExerciseIngestionModule(ds_mod, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["location_name"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_latitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_longitude"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_column_number_density_validity"].read_status);
  }

  // 2. Non-string @DATA_LOCATION, int32 wrong element count, int32 wrong type.
  {
    GeomsPandoraConfig cfg;
    cfg.overrides["@DATA_LOCATION"] = [](CodaMemProduct& p) {
      return p.MakeInt32(9);
    };
    cfg.overrides["O3_COLUMN_ABSORPTION_SOLAR_FLAG"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {1};
      constexpr int32_t kVals[] = {0};
      return p.MakeInt32Array(kDims, kVals);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res =
        ExerciseIngestionModule(ds_mod, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["location_name"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_column_number_density_validity"].read_status);
  }
  {
    GeomsPandoraConfig cfg;
    cfg.overrides["O3_COLUMN_ABSORPTION_SOLAR_FLAG"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {2};
      constexpr std::string_view kVals[] = {"a", "b"};
      return p.MakeStringArray(kDims, kVals);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res =
        ExerciseIngestionModule(ds_mod, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1,
              res.variables["O3_column_number_density_validity"].read_status);
  }

  // 3. Offaxis: uint8 missing, wrong element count, and wrong type.
  {
    GeomsPandoraConfig cfg;
    cfg.offaxis = true;
    cfg.version = 1;
    cfg.geoms_gas = "NO2";
    cfg.overrides["FLAG_MEASUREMENT_QUALITY"] = [](CodaMemProduct&) {
      return nullptr;
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res =
        ExerciseIngestionModule(oa_mod, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
  }
  {
    GeomsPandoraConfig cfg;
    cfg.offaxis = true;
    cfg.version = 1;
    cfg.geoms_gas = "NO2";
    cfg.overrides["FLAG_MEASUREMENT_QUALITY"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {1};
      constexpr uint8_t kVals[] = {1};
      return p.MakeUint8Array(kDims, kVals);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res =
        ExerciseIngestionModule(oa_mod, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
  }
  {
    GeomsPandoraConfig cfg;
    cfg.offaxis = true;
    cfg.version = 1;
    cfg.geoms_gas = "NO2";
    cfg.overrides["FLAG_MEASUREMENT_QUALITY"] = [](CodaMemProduct& p) {
      constexpr long kDims[] = {2};
      constexpr std::string_view kVals[] = {"a", "b"};
      return p.MakeStringArray(kDims, kVals);
    };
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGeomsPandoraProduct(prod, cfg);
    IngestExerciseResult res =
        ExerciseIngestionModule(oa_mod, prod.product(), /*options_str=*/nullptr,
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
