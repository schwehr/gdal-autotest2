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
// Unit tests for the GEOMS-TE-FTIR ingestion module
// (libharp/harp-ingest-geoms-ftir.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-geoms-ftir.c

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

using ::testing::DoubleNear;
using ::testing::ElementsAre;

struct GeomsFtirConfig {
  int version = 3;
  std::string_view gas = "O3";
  bool lunar = false;
  bool time_dep_altitude = false;
  bool time_dep_altitude_bounds = false;
  bool invert_vertical = false;
  bool include_vmr = true;
  double fill_value = -9999.0;
  std::string_view column_unit = "molec/m2";
  std::string_view vmr_unit = "ppmv";
  std::string_view vmr_cov_unit = "(ppmv)2";
  std::string_view h2o_column_unit = "molec/m2";
  std::string_view h2o_vmr_unit = "ppmv";
  // Optional overrides: if a field/root-attr name is present in `overrides`,
  // its callback is invoked instead of adding the default node (or returning
  // nullptr to omit the node).
  std::map<std::string, std::function<CodaMemNode*(CodaMemProduct&)>> overrides;
};

bool AddDoubleVar(CodaMemProduct& prod, std::string_view name,
                  std::span<const long> dims, std::span<const double> vals,
                  double fill_value, std::string_view unit = "") {
  CodaMemNode* arr = prod.MakeDoubleArray(dims, vals);
  if (arr == nullptr) {
    return false;
  }
  if (!prod.AddAttribute(arr, "VAR_FILL_VALUE", prod.MakeDouble(fill_value))) {
    return false;
  }
  if (!unit.empty() &&
      !prod.AddAttribute(arr, "VAR_UNITS", prod.MakeString(unit))) {
    return false;
  }
  return prod.AddField(prod.root(), name, arr);
}

bool AddDoubleFieldWithOverrides(CodaMemProduct& prod,
                                 const GeomsFtirConfig& cfg,
                                 std::string_view name,
                                 std::span<const long> dims,
                                 std::span<const double> vals,
                                 std::string_view unit = "") {
  auto it = cfg.overrides.find(std::string(name));
  if (it != cfg.overrides.end()) {
    CodaMemNode* custom = it->second(prod);
    if (custom == nullptr) {
      return true;
    }
    return prod.AddField(prod.root(), name, custom);
  }
  return AddDoubleVar(prod, name, dims, vals, cfg.fill_value, unit);
}

void PopulateGeomsFtirProduct(CodaMemProduct& prod, const GeomsFtirConfig& cfg,
                              long num_time = 2, long num_vertical = 2) {
  const std::string tmpl = "GEOMS-TE-FTIR-00" + std::to_string(cfg.version);
  const std::string data_src = "FTIR." + std::string(cfg.gas) + "_TEST";
  ASSERT_TRUE(
      prod.AddAttribute(prod.root(), "DATA_TEMPLATE", prod.MakeString(tmpl)));
  ASSERT_TRUE(
      prod.AddAttribute(prod.root(), "DATA_SOURCE", prod.MakeString(data_src)));
  if (auto it = cfg.overrides.find("@DATA_LOCATION");
      it != cfg.overrides.end()) {
    CodaMemNode* custom = it->second(prod);
    if (custom != nullptr) {
      ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_LOCATION", custom));
    }
  } else {
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_LOCATION",
                                  prod.MakeString("KIRUNA")));
  }

  const std::string mode = cfg.lunar ? "LUNAR" : "SOLAR";
  const long scalar_dims[] = {1};
  const double inst_alt[] = {0.42};
  const double inst_lat[] = {67.84};
  const double inst_lon[] = {20.41};
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "ALTITUDE_INSTRUMENT",
                                          scalar_dims, inst_alt));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "LATITUDE_INSTRUMENT",
                                          scalar_dims, inst_lat));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "LONGITUDE_INSTRUMENT",
                                          scalar_dims, inst_lon));

  const long time_dims[] = {num_time};
  std::vector<double> dt(num_time);
  std::vector<double> integ(num_time);
  std::vector<double> azim(num_time);
  std::vector<double> sza(num_time);
  std::vector<double> sp(num_time);
  std::vector<double> st(num_time);
  std::vector<double> col(num_time);
  std::vector<double> col_ap(num_time);
  std::vector<double> col_rand(num_time);
  std::vector<double> col_sys(num_time);
  for (long t = 0; t < num_time; ++t) {
    dt[t] = 7000.0 + static_cast<double>(t);
    integ[t] = 300.0 + 10.0 * static_cast<double>(t);
    azim[t] = 180.0 + static_cast<double>(t);
    sza[t] = 60.0 + static_cast<double>(t);
    sp[t] = 960.0 + static_cast<double>(t);
    st[t] = 270.0 + static_cast<double>(t);
    col[t] = 1.0e22 * static_cast<double>(t + 1);
    col_ap[t] = 0.9e22 * static_cast<double>(t + 1);
    col_rand[t] = 0.05e22 * static_cast<double>(t + 1);
    col_sys[t] = 0.08e22 * static_cast<double>(t + 1);
  }
  ASSERT_TRUE(
      AddDoubleFieldWithOverrides(prod, cfg, "DATETIME", time_dims, dt));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "INTEGRATION_TIME",
                                          time_dims, integ));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(
      prod, cfg, cfg.lunar ? "ANGLE_LUNAR_AZIMUTH" : "ANGLE_SOLAR_AZIMUTH",
      time_dims, azim));
  ASSERT_TRUE(
      AddDoubleFieldWithOverrides(prod, cfg,
                                  cfg.lunar ? "ANGLE_LUNAR_ZENITH_ASTRONOMICAL"
                                            : "ANGLE_SOLAR_ZENITH_ASTRONOMICAL",
                                  time_dims, sza));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(
      prod, cfg, "SURFACE_PRESSURE_INDEPENDENT", time_dims, sp));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(
      prod, cfg, "SURFACE_TEMPERATURE_INDEPENDENT", time_dims, st));

  const std::string gas(cfg.gas);
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg,
                                          gas + "_COLUMN_ABSORPTION_" + mode,
                                          time_dims, col, cfg.column_unit));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(
      prod, cfg,
      cfg.version < 3 ? gas + "_COLUMN_ABSORPTION_" + mode + "_APRIORI"
                      : gas + "_COLUMN_APRIORI",
      time_dims, col_ap));
  const std::string std_suffix = cfg.version == 1 ? "" : "_STANDARD";
  ASSERT_TRUE(AddDoubleFieldWithOverrides(
      prod, cfg,
      gas + "_COLUMN_ABSORPTION_" + mode + "_UNCERTAINTY_RANDOM" + std_suffix,
      time_dims, col_rand));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg,
                                          gas + "_COLUMN_ABSORPTION_" + mode +
                                              "_UNCERTAINTY_SYSTEMATIC" +
                                              std_suffix,
                                          time_dims, col_sys));

  if (cfg.time_dep_altitude) {
    const long alt_dims[] = {num_time, num_vertical};
    std::vector<double> alt(static_cast<size_t>(num_time * num_vertical));
    for (long t = 0; t < num_time; ++t) {
      for (long v = 0; v < num_vertical; ++v) {
        const long vv = cfg.invert_vertical ? (num_vertical - 1 - v) : v;
        alt[static_cast<size_t>(t * num_vertical + v)] =
            10.0 * static_cast<double>(vv + 1) + static_cast<double>(t);
      }
    }
    ASSERT_TRUE(
        AddDoubleFieldWithOverrides(prod, cfg, "ALTITUDE", alt_dims, alt));
  } else {
    const long alt_dims[] = {num_vertical};
    std::vector<double> alt(static_cast<size_t>(num_vertical));
    for (long v = 0; v < num_vertical; ++v) {
      const long vv = cfg.invert_vertical ? (num_vertical - 1 - v) : v;
      alt[static_cast<size_t>(v)] = 10.0 * static_cast<double>(vv + 1);
    }
    ASSERT_TRUE(
        AddDoubleFieldWithOverrides(prod, cfg, "ALTITUDE", alt_dims, alt));
  }

  if (cfg.time_dep_altitude_bounds) {
    const long bnds_dims[] = {num_time, 2, num_vertical};
    std::vector<double> bnds(static_cast<size_t>(num_time * 2 * num_vertical));
    for (long t = 0; t < num_time; ++t) {
      for (long b = 0; b < 2; ++b) {
        for (long v = 0; v < num_vertical; ++v) {
          const long vv = cfg.invert_vertical ? (num_vertical - 1 - v) : v;
          bnds[static_cast<size_t>((t * 2 + b) * num_vertical + v)] =
              10.0 * static_cast<double>(vv) + 10.0 * static_cast<double>(b) +
              static_cast<double>(t);
        }
      }
    }
    ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "ALTITUDE_BOUNDARIES",
                                            bnds_dims, bnds));
  } else {
    const long bnds_dims[] = {2, num_vertical};
    std::vector<double> bnds(static_cast<size_t>(2 * num_vertical));
    for (long b = 0; b < 2; ++b) {
      for (long v = 0; v < num_vertical; ++v) {
        const long vv = cfg.invert_vertical ? (num_vertical - 1 - v) : v;
        bnds[static_cast<size_t>(b * num_vertical + v)] =
            10.0 * static_cast<double>(vv) + 10.0 * static_cast<double>(b);
      }
    }
    ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "ALTITUDE_BOUNDARIES",
                                            bnds_dims, bnds));
  }

  const long tv_dims[] = {num_time, num_vertical};
  std::vector<double> press(static_cast<size_t>(num_time * num_vertical));
  std::vector<double> temp(static_cast<size_t>(num_time * num_vertical));
  std::vector<double> col_avk(static_cast<size_t>(num_time * num_vertical));
  for (long t = 0; t < num_time; ++t) {
    for (long v = 0; v < num_vertical; ++v) {
      const size_t idx = static_cast<size_t>(t * num_vertical + v);
      press[idx] = 1000.0 - 200.0 * static_cast<double>(v);
      temp[idx] = 280.0 - 20.0 * static_cast<double>(v);
      col_avk[idx] = 0.8 + 0.1 * static_cast<double>(v);
    }
  }
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "PRESSURE_INDEPENDENT",
                                          tv_dims, press));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, "TEMPERATURE_INDEPENDENT",
                                          tv_dims, temp));
  ASSERT_TRUE(AddDoubleFieldWithOverrides(
      prod, cfg, gas + "_COLUMN_ABSORPTION_" + mode + "_AVK", tv_dims,
      col_avk));

  const std::string vol_part = cfg.version == 1 ? "" : "_VOLUME";
  const std::string dry_part = cfg.version < 3 ? "" : "_DRY";
  const std::string cov_part = cfg.version == 1 ? "" : "_COVARIANCE";

  if (cfg.gas != "H2O") {
    std::vector<double> h2o_col(num_time, 1.5e24);
    std::vector<double> h2o_vmr(static_cast<size_t>(num_time * num_vertical),
                                5000.0);
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg,
        cfg.version < 3 ? "H2O_COLUMN_ABSORPTION_" + mode
                        : "H2O_COLUMN_APRIORI",
        time_dims, h2o_col, cfg.h2o_column_unit));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg,
        cfg.version < 3 ? "H2O_MIXING_RATIO" + vol_part + "_ABSORPTION_" + mode
                        : "H2O_MIXING_RATIO_VOLUME_DRY_APRIORI",
        tv_dims, h2o_vmr, cfg.h2o_vmr_unit));
  }

  if (cfg.include_vmr) {
    std::vector<double> vmr(static_cast<size_t>(num_time * num_vertical));
    std::vector<double> vmr_ap(static_cast<size_t>(num_time * num_vertical));
    for (long t = 0; t < num_time; ++t) {
      for (long v = 0; v < num_vertical; ++v) {
        const size_t idx = static_cast<size_t>(t * num_vertical + v);
        vmr[idx] = 2.0 + static_cast<double>(v) + 10.0 * static_cast<double>(t);
        vmr_ap[idx] = 1.5 + static_cast<double>(v);
      }
    }
    const std::string vmr_base =
        gas + "_MIXING_RATIO" + vol_part + dry_part + "_ABSORPTION_" + mode;
    ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, vmr_base, tv_dims, vmr,
                                            cfg.vmr_unit));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg,
        cfg.version < 3 ? gas + "_MIXING_RATIO" + vol_part + "_ABSORPTION_" +
                              mode + "_APRIORI"
                        : gas + "_MIXING_RATIO_VOLUME_DRY_APRIORI",
        tv_dims, vmr_ap));

    const long tvv_dims[] = {num_time, num_vertical, num_vertical};
    const size_t tvv_count =
        static_cast<size_t>(num_time * num_vertical * num_vertical);
    std::vector<double> vmr_avk(tvv_count, 0.25);
    std::vector<double> cov_rand(tvv_count, 0.0);
    std::vector<double> cov_sys(tvv_count, 0.0);
    for (long t = 0; t < num_time; ++t) {
      for (long i = 0; i < num_vertical; ++i) {
        for (long j = 0; j < num_vertical; ++j) {
          const size_t idx =
              static_cast<size_t>((t * num_vertical + i) * num_vertical + j);
          vmr_avk[idx] = (i == j) ? static_cast<double>(i + 1) * 0.5 : 0.1;
          cov_rand[idx] =
              (i == j) ? static_cast<double>((i + 1) * (i + 1)) : 0.5;
          cov_sys[idx] =
              (i == j) ? static_cast<double>((i + 2) * (i + 2)) : 0.25;
        }
      }
    }
    ASSERT_TRUE(AddDoubleFieldWithOverrides(prod, cfg, vmr_base + "_AVK",
                                            tvv_dims, vmr_avk));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, vmr_base + "_UNCERTAINTY_RANDOM" + cov_part, tvv_dims,
        cov_rand, cfg.vmr_cov_unit));
    ASSERT_TRUE(AddDoubleFieldWithOverrides(
        prod, cfg, vmr_base + "_UNCERTAINTY_SYSTEMATIC" + cov_part, tvv_dims,
        cov_sys));
  }
}

CodaMemNode* MakeDoubleArrayWithFill(CodaMemProduct& prod,
                                     std::span<const long> dims,
                                     std::span<const double> vals,
                                     double fill_value,
                                     std::string_view unit = "") {
  CodaMemNode* arr = prod.MakeDoubleArray(dims, vals);
  if (arr == nullptr) {
    return nullptr;
  }
  if (!prod.AddAttribute(arr, "VAR_FILL_VALUE", prod.MakeDouble(fill_value))) {
    return nullptr;
  }
  if (!unit.empty() &&
      !prod.AddAttribute(arr, "VAR_UNITS", prod.MakeString(unit))) {
    return nullptr;
  }
  return arr;
}

TEST(HarpIngestGeomsFtirTest, IngestVersion3SolarWithInvertAndTimeDepAlt) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-FTIR");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  GeomsFtirConfig cfg;
  cfg.version = 3;
  cfg.gas = "O3";
  cfg.lunar = false;
  cfg.time_dep_altitude = true;
  cfg.time_dep_altitude_bounds = true;
  cfg.invert_vertical = true;
  cfg.include_vmr = true;
  cfg.column_unit = "molec/cm2";
  cfg.vmr_unit = "ppbv";
  cfg.vmr_cov_unit = "(ppbv)2";
  PopulateGeomsFtirProduct(prod, cfg, /*num_time=*/2, /*num_vertical=*/2);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(2, res.dimensions[harp_dimension_vertical]);

  EXPECT_THAT(res.variables["sensor_name"].string_values,
              ElementsAre("FTIR.O3_TEST"));
  EXPECT_THAT(res.variables["location_name"].string_values,
              ElementsAre("KIRUNA"));
  EXPECT_THAT(res.variables["measurement_mode"].string_values,
              ElementsAre("solar"));
  EXPECT_THAT(res.variables["sensor_altitude"].double_values,
              ElementsAre(0.42));
  EXPECT_THAT(res.variables["sensor_latitude"].double_values,
              ElementsAre(67.84));
  EXPECT_THAT(res.variables["sensor_longitude"].double_values,
              ElementsAre(20.41));

  // Inverted vertical axis: surface (10) first, top (20) second.
  EXPECT_THAT(res.variables["altitude"].double_values,
              ElementsAre(10.0, 20.0, 11.0, 21.0));
  EXPECT_THAT(res.variables["altitude_bounds"].double_values,
              ElementsAre(0.0, 10.0, 10.0, 20.0, 1.0, 11.0, 11.0, 21.0));

  // Unit conversions: molec/cm2 -> molec/m2 (*1e4), ppbv -> ppmv (*1e-3).
  EXPECT_NEAR(1.0e26,
              res.variables["O3_column_number_density"].double_values[0],
              1.0e20);
  // Inverted VMR: [t=0, v=1]=3 -> 0.003 ppmv, [t=0, v=0]=2 -> 0.002 ppmv.
  EXPECT_THAT(res.variables["O3_volume_mixing_ratio_dry_air"].double_values,
              ElementsAre(DoubleNear(0.003, 1e-9), DoubleNear(0.002, 1e-9),
                          DoubleNear(0.013, 1e-9), DoubleNear(0.012, 1e-9)));
  // Inverted sqrt trace of covariance [[1, 0.5], [0.5, 4]] -> [2, 1] * 1e-3.
  EXPECT_THAT(res.variables["O3_volume_mixing_ratio_dry_air_uncertainty_random"]
                  .double_values,
              ElementsAre(DoubleNear(0.002, 1e-9), DoubleNear(0.001, 1e-9),
                          DoubleNear(0.002, 1e-9), DoubleNear(0.001, 1e-9)));
  // Inverted systematic uncertainty sqrt([[4, 0.25], [0.25, 9]]) -> [3,
  // 2]*1e-3.
  EXPECT_THAT(
      res.variables["O3_volume_mixing_ratio_dry_air_uncertainty_systematic"]
          .double_values,
      ElementsAre(DoubleNear(0.003, 1e-9), DoubleNear(0.002, 1e-9),
                  DoubleNear(0.003, 1e-9), DoubleNear(0.002, 1e-9)));
  // Covariance converted (ppbv)2 -> (ppmv)2 (*1e-6) and inverted on both axes.
  EXPECT_NEAR(4.0e-6,
              res.variables["O3_volume_mixing_ratio_dry_air_covariance"]
                  .double_values[0],
              1e-12);
}

TEST(HarpIngestGeomsFtirTest, IngestVersions1And2LunarH2OAndNoVmr) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-FTIR");
  ASSERT_NE(nullptr, mod);

  // Version 1, lunar, PAN gas (mapped to C2H3NO5), 1D altitude, 2D bounds, no
  // vertical inversion.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    GeomsFtirConfig cfg;
    cfg.version = 1;
    cfg.gas = "PAN";
    cfg.lunar = true;
    cfg.time_dep_altitude = false;
    cfg.time_dep_altitude_bounds = false;
    cfg.invert_vertical = false;
    cfg.include_vmr = true;
    PopulateGeomsFtirProduct(prod, cfg, /*num_time=*/2, /*num_vertical=*/2);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["measurement_mode"].string_values,
                ElementsAre("lunar"));
    EXPECT_THAT(res.variables["altitude"].double_values,
                ElementsAre(10.0, 20.0, 10.0, 20.0));
    EXPECT_THAT(res.variables["altitude_bounds"].double_values,
                ElementsAre(0.0, 10.0, 10.0, 20.0, 0.0, 10.0, 10.0, 20.0));
    EXPECT_TRUE(res.variables["C2H3NO5_column_number_density"].included);
    EXPECT_TRUE(res.variables["C2H3NO5_volume_mixing_ratio_dry_air"].included);
  }

  // Version 2, solar, H2O gas, no VMR absorption, NaN fill_value, inverted 1D
  // altitude & 2D bounds.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    GeomsFtirConfig cfg;
    cfg.version = 2;
    cfg.gas = "H2O";
    cfg.lunar = false;
    cfg.time_dep_altitude = false;
    cfg.time_dep_altitude_bounds = false;
    cfg.invert_vertical = true;
    cfg.include_vmr = false;
    cfg.fill_value = std::numeric_limits<double>::quiet_NaN();
    PopulateGeomsFtirProduct(prod, cfg, /*num_time=*/2, /*num_vertical=*/2);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(res.variables["datetime_length"].included);
    EXPECT_FALSE(res.variables["H2O_volume_mixing_ratio_dry_air"].included);
    EXPECT_THAT(res.variables["altitude"].double_values,
                ElementsAre(10.0, 20.0, 10.0, 20.0));
    EXPECT_THAT(res.variables["altitude_bounds"].double_values,
                ElementsAre(0.0, 10.0, 10.0, 20.0, 0.0, 10.0, 10.0, 20.0));
  }
}

TEST(HarpIngestGeomsFtirTest, FillValueReplacementAndMultiElementScalar) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-FTIR");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  GeomsFtirConfig cfg;
  cfg.version = 3;
  cfg.gas = "O3";
  cfg.include_vmr = false;
  cfg.overrides["ALTITUDE_INSTRUMENT"] = [](CodaMemProduct& p) {
    const long two_dims[] = {2};
    const double vals[] = {-9999.0, -9999.0};
    return MakeDoubleArrayWithFill(p, two_dims, vals, -9999.0);
  };
  cfg.overrides["LATITUDE_INSTRUMENT"] = [](CodaMemProduct& p) {
    const long two_dims[] = {2};
    const double vals[] = {67.84, 67.84};
    return MakeDoubleArrayWithFill(p, two_dims, vals, -9999.0);
  };
  cfg.overrides["SURFACE_PRESSURE_INDEPENDENT"] = [](CodaMemProduct& p) {
    const long one_dim[] = {1};
    const double vals[] = {-9999.0};
    return MakeDoubleArrayWithFill(p, one_dim, vals, -9999.0);
  };
  PopulateGeomsFtirProduct(prod, cfg, /*num_time=*/1, /*num_vertical=*/1);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_TRUE(std::isnan(res.variables["sensor_altitude"].double_values[0]));
  EXPECT_DOUBLE_EQ(67.84, res.variables["sensor_latitude"].double_values[0]);
  EXPECT_TRUE(std::isnan(res.variables["surface_pressure"].double_values[0]));
}

TEST(HarpIngestGeomsFtirTest, ProductDefinitionValidationErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-FTIR");
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
        prod.AddAttribute(prod.root(), "DATA_TEMPLATE", prod.MakeInt32(123)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Invalid @DATA_TEMPLATE string length != 17.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("SHORT")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 4. Missing @DATA_SOURCE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-003")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 5. @DATA_SOURCE too long (>= 20 chars) triggers coda_cursor_read_string
  // failure.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-003")));
    ASSERT_TRUE(
        prod.AddAttribute(prod.root(), "DATA_SOURCE",
                          prod.MakeString("FTIR.O3_VERY_LONG_SITE_NAME_HERE")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 6. @DATA_SOURCE does not start with "FTIR.".
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-003")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                  prod.MakeString("LIDAR.O3_TEST")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }

  // 7. Unsupported template/gas combination.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-999")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                  prod.MakeString("FTIR.O3_TEST")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_UNSUPPORTED_PRODUCT, res.failed_errno);
  }
}

TEST(HarpIngestGeomsFtirTest, InitDimensionsLunarAndDynamicUnitsErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-FTIR");
  ASSERT_NE(nullptr, mod);

  // 1. Missing ANGLE_LUNAR_AZIMUTH and ANGLE_SOLAR_AZIMUTH.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-003")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                  prod.MakeString("FTIR.O3_TEST")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Missing DATETIME.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-003")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                  prod.MakeString("FTIR.O3_TEST")));
    const long d1[] = {1};
    const double az[] = {180.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "ANGLE_SOLAR_AZIMUTH",
                              prod.MakeDoubleArray(d1, az)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Non-chronological DATETIME.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    GeomsFtirConfig cfg;
    cfg.overrides["DATETIME"] = [](CodaMemProduct& p) {
      const long d2[] = {2};
      const double bad_dt[] = {7005.0, 7000.0};
      return MakeDoubleArrayWithFill(p, d2, bad_dt, -9999.0);
    };
    PopulateGeomsFtirProduct(prod, cfg, /*num_time=*/2, /*num_vertical=*/2);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 4. Wrong type for DATETIME (string array).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    GeomsFtirConfig cfg;
    cfg.overrides["DATETIME"] = [](CodaMemProduct& p) {
      const long d2[] = {2};
      const std::string_view strs[] = {"a", "b"};
      return p.MakeStringArray(d2, strs);
    };
    PopulateGeomsFtirProduct(prod, cfg, /*num_time=*/2, /*num_vertical=*/2);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 5. Missing ALTITUDE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-003")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                  prod.MakeString("FTIR.O3_TEST")));
    const long d1[] = {1};
    const double v[] = {1.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "ANGLE_SOLAR_AZIMUTH",
                              prod.MakeDoubleArray(d1, v)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "DATETIME", prod.MakeDoubleArray(d1, v)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 6. 3D ALTITUDE (invalid rank != 1 and != 2) and non-double ALTITUDE.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    GeomsFtirConfig cfg;
    cfg.overrides["ALTITUDE"] = [](CodaMemProduct& p) {
      const long d3[] = {1, 2, 2};
      const double vals[] = {1.0, 2.0, 3.0, 4.0};
      return MakeDoubleArrayWithFill(p, d3, vals, -9999.0);
    };
    PopulateGeomsFtirProduct(prod, cfg, /*num_time=*/2, /*num_vertical=*/2);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    GeomsFtirConfig cfg;
    cfg.overrides["ALTITUDE"] = [](CodaMemProduct& p) {
      const long d2[] = {2};
      const std::string_view vals[] = {"a", "b"};
      return p.MakeStringArray(d2, vals);
    };
    PopulateGeomsFtirProduct(prod, cfg, /*num_time=*/2, /*num_vertical=*/2);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 7. Missing ALTITUDE_BOUNDARIES.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_TEMPLATE",
                                  prod.MakeString("GEOMS-TE-FTIR-003")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "DATA_SOURCE",
                                  prod.MakeString("FTIR.O3_TEST")));
    const long d1[] = {1};
    const double v[] = {1.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "ANGLE_SOLAR_AZIMUTH",
                              prod.MakeDoubleArray(d1, v)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "DATETIME", prod.MakeDoubleArray(d1, v)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "ALTITUDE", prod.MakeDoubleArray(d1, v)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 8. Missing @VAR_UNITS on VMR, VMR covariance, column, H2O VMR, H2O column.
  for (const std::string_view bad_var :
       {"O3_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR",
        "O3_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR_UNCERTAINTY_RANDOM_"
        "COVARIANCE",
        "O3_COLUMN_ABSORPTION_SOLAR", "H2O_MIXING_RATIO_VOLUME_DRY_APRIORI",
        "H2O_COLUMN_APRIORI"}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    GeomsFtirConfig cfg;
    cfg.version = 3;
    cfg.gas = "O3";
    cfg.include_vmr = true;
    cfg.overrides[std::string(bad_var)] = [](CodaMemProduct& p) {
      const long d1[] = {1};
      const double val[] = {1.0};
      return MakeDoubleArrayWithFill(p, d1, val, -9999.0);  // no VAR_UNITS
    };
    PopulateGeomsFtirProduct(prod, cfg, /*num_time=*/1, /*num_vertical=*/1);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

TEST(HarpIngestGeomsFtirTest, VariableReadAndUnitConversionErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GEOMS-TE-FTIR");
  ASSERT_NE(nullptr, mod);

  // 1. Invalid units trigger harp_convert_unit failures across column/VMR
  // readers, and mismatched lengths / missing fill values / differing scalar
  // values trigger variable reader failures.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    GeomsFtirConfig cfg;
    cfg.version = 3;
    cfg.gas = "O3";
    cfg.include_vmr = true;
    cfg.column_unit = "bad_col_unit";
    cfg.vmr_unit = "bad_vmr_unit";
    cfg.vmr_cov_unit = "bad_cov_unit";
    cfg.h2o_column_unit = "bad_h2o_col";
    cfg.h2o_vmr_unit = "bad_h2o_vmr";

    // Differing values across time for ALTITUDE_INSTRUMENT.
    cfg.overrides["ALTITUDE_INSTRUMENT"] = [](CodaMemProduct& p) {
      const long d2[] = {2};
      const double vals[] = {0.4, 0.5};
      return MakeDoubleArrayWithFill(p, d2, vals, -9999.0);
    };
    // Empty LATITUDE_INSTRUMENT (0 elements).
    cfg.overrides["LATITUDE_INSTRUMENT"] = [](CodaMemProduct& p) {
      const long d0[] = {0};
      return MakeDoubleArrayWithFill(p, d0, {}, -9999.0);
    };
    // Missing @VAR_FILL_VALUE on LONGITUDE_INSTRUMENT.
    cfg.overrides["LONGITUDE_INSTRUMENT"] = [](CodaMemProduct& p) {
      const long d1[] = {1};
      const double vals[] = {20.0};
      return p.MakeDoubleArray(d1, vals);
    };
    // Non-string @DATA_LOCATION.
    cfg.overrides["@DATA_LOCATION"] = [](CodaMemProduct& p) {
      return p.MakeInt32(42);
    };
    // Wrong element count on SURFACE_PRESSURE_INDEPENDENT.
    cfg.overrides["SURFACE_PRESSURE_INDEPENDENT"] = [](CodaMemProduct& p) {
      const long d1[] = {1};
      const double vals[] = {960.0};
      return MakeDoubleArrayWithFill(p, d1, vals, -9999.0);
    };
    // Wrong type on SURFACE_TEMPERATURE_INDEPENDENT.
    cfg.overrides["SURFACE_TEMPERATURE_INDEPENDENT"] = [](CodaMemProduct& p) {
      const long d2[] = {2};
      const std::string_view strs[] = {"x", "y"};
      return p.MakeStringArray(d2, strs);
    };
    // Missing @VAR_FILL_VALUE on INTEGRATION_TIME.
    cfg.overrides["INTEGRATION_TIME"] = [](CodaMemProduct& p) {
      const long d2[] = {2};
      const double vals[] = {10.0, 20.0};
      return p.MakeDoubleArray(d2, vals);
    };
    // Wrong type @VAR_FILL_VALUE on ANGLE_SOLAR_AZIMUTH.
    cfg.overrides["ANGLE_SOLAR_AZIMUTH"] = [](CodaMemProduct& p) {
      const long d2[] = {2};
      const double vals[] = {10.0, 20.0};
      CodaMemNode* az = p.MakeDoubleArray(d2, vals);
      p.AddAttribute(az, "VAR_FILL_VALUE", p.MakeString("bad"));
      return az;
    };
    // Missing ANGLE_SOLAR_ZENITH_ASTRONOMICAL.
    cfg.overrides["ANGLE_SOLAR_ZENITH_ASTRONOMICAL"] = [](CodaMemProduct&) {
      return nullptr;
    };
    // Wrong size on ALTITUDE_BOUNDARIES and PRESSURE_INDEPENDENT.
    cfg.overrides["ALTITUDE_BOUNDARIES"] = [](CodaMemProduct& p) {
      const long d1[] = {1};
      const double vals[] = {1.0};
      return MakeDoubleArrayWithFill(p, d1, vals, -9999.0);
    };
    cfg.overrides["PRESSURE_INDEPENDENT"] = [](CodaMemProduct& p) {
      const long d1[] = {1};
      const double vals[] = {1.0};
      return MakeDoubleArrayWithFill(p, d1, vals, -9999.0);
    };
    // Wrong size on AVK, systematic covariance, VMR apriori, column apriori,
    // column uncertainties.
    cfg.overrides["O3_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR_AVK"] =
        [](CodaMemProduct& p) {
          const long d1[] = {1};
          const double vals[] = {1.0};
          return MakeDoubleArrayWithFill(p, d1, vals, -9999.0);
        };
    cfg.overrides
        ["O3_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR_UNCERTAINTY_SYSTEMATIC_"
         "COVARIANCE"] = [](CodaMemProduct& p) {
      const long d1[] = {1};
      const double vals[] = {1.0};
      return MakeDoubleArrayWithFill(p, d1, vals, -9999.0);
    };
    cfg.overrides["O3_MIXING_RATIO_VOLUME_DRY_APRIORI"] =
        [](CodaMemProduct& p) {
          const long d1[] = {1};
          const double vals[] = {1.0};
          return MakeDoubleArrayWithFill(p, d1, vals, -9999.0);
        };
    cfg.overrides["O3_COLUMN_APRIORI"] = [](CodaMemProduct& p) {
      const long d1[] = {1};
      const double vals[] = {1.0};
      return MakeDoubleArrayWithFill(p, d1, vals, -9999.0);
    };
    cfg.overrides["O3_COLUMN_ABSORPTION_SOLAR_UNCERTAINTY_RANDOM_STANDARD"] =
        [](CodaMemProduct& p) {
          const long d1[] = {1};
          const double vals[] = {1.0};
          return MakeDoubleArrayWithFill(p, d1, vals, -9999.0);
        };
    cfg.overrides
        ["O3_COLUMN_ABSORPTION_SOLAR_UNCERTAINTY_SYSTEMATIC_STANDARD"] =
        [](CodaMemProduct& p) {
          const long d1[] = {1};
          const double vals[] = {1.0};
          return MakeDoubleArrayWithFill(p, d1, vals, -9999.0);
        };

    PopulateGeomsFtirProduct(prod, cfg, /*num_time=*/2, /*num_vertical=*/2);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["location_name"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_altitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_latitude"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_longitude"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_length"].read_status);
    EXPECT_EQ(-1, res.variables["O3_column_number_density"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_column_number_density_apriori"].read_status);
    EXPECT_EQ(-1, res.variables["O3_column_number_density_uncertainty_random"]
                      .read_status);
    EXPECT_EQ(-1,
              res.variables["O3_column_number_density_uncertainty_systematic"]
                  .read_status);
    EXPECT_EQ(-1,
              res.variables["H2O_column_number_density_apriori"].read_status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio_dry_air"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["O3_volume_mixing_ratio_dry_air_apriori"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_volume_mixing_ratio_dry_air_avk"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["O3_volume_mixing_ratio_dry_air_covariance"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_volume_mixing_ratio_dry_air_uncertainty_random"]
                  .read_status);
    EXPECT_EQ(
        -1,
        res.variables["O3_volume_mixing_ratio_dry_air_uncertainty_systematic"]
            .read_status);
    EXPECT_EQ(-1, res.variables["H2O_volume_mixing_ratio_dry_air"].read_status);
    EXPECT_EQ(-1, res.variables["altitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(-1, res.variables["surface_pressure"].read_status);
    EXPECT_EQ(-1, res.variables["surface_temperature"].read_status);
    EXPECT_EQ(-1, res.variables["solar_azimuth_angle"].read_status);
    EXPECT_EQ(-1, res.variables["solar_zenith_angle"].read_status);
  }

  // 2. Also test invalid unit conversion failures on:
  // - O3_column_number_density_apriori
  // - O3_column_number_density_uncertainty_random / systematic
  // - H2O_column_number_density (v2)
  // - O3_volume_mixing_ratio_dry_air_apriori
  // - O3_volume_mixing_ratio_dry_air_uncertainty_systematic
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    GeomsFtirConfig cfg;
    cfg.version = 2;
    cfg.gas = "O3";
    cfg.include_vmr = true;
    cfg.column_unit = "bad_col_unit";
    cfg.vmr_unit = "bad_vmr_unit";
    cfg.h2o_column_unit = "bad_h2o_col";
    PopulateGeomsFtirProduct(prod, cfg, /*num_time=*/1, /*num_vertical=*/1);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1,
              res.variables["O3_column_number_density_apriori"].read_status);
    EXPECT_EQ(-1, res.variables["O3_column_number_density_uncertainty_random"]
                      .read_status);
    EXPECT_EQ(-1,
              res.variables["O3_column_number_density_uncertainty_systematic"]
                  .read_status);
    EXPECT_EQ(-1, res.variables["H2O_column_number_density"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["O3_volume_mixing_ratio_dry_air_apriori"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["O3_volume_mixing_ratio_dry_air_uncertainty_systematic"]
            .read_status);
  }

  // 3. Mismatched element counts on variables that also have @VAR_UNITS
  // (read_variable_double failure before harp_convert_unit) + 2D ALTITUDE &
  // 3D ALTITUDE_BOUNDARIES size mismatches.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    GeomsFtirConfig cfg;
    cfg.version = 3;
    cfg.gas = "O3";
    cfg.time_dep_altitude = true;
    cfg.time_dep_altitude_bounds = true;
    cfg.include_vmr = true;
    auto make_bad_size_with_unit = [](CodaMemProduct& p,
                                      std::string_view unit) {
      const long d1[] = {1};
      const double val[] = {1.0};
      return MakeDoubleArrayWithFill(p, d1, val, -9999.0, unit);
    };
    cfg.overrides["O3_COLUMN_ABSORPTION_SOLAR"] = [&](CodaMemProduct& p) {
      return make_bad_size_with_unit(p, "molec/m2");
    };
    cfg.overrides["H2O_COLUMN_APRIORI"] = [&](CodaMemProduct& p) {
      return make_bad_size_with_unit(p, "molec/m2");
    };
    cfg.overrides["O3_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR"] =
        [&](CodaMemProduct& p) { return make_bad_size_with_unit(p, "ppmv"); };
    cfg.overrides["H2O_MIXING_RATIO_VOLUME_DRY_APRIORI"] =
        [&](CodaMemProduct& p) { return make_bad_size_with_unit(p, "ppmv"); };
    cfg.overrides
        ["O3_MIXING_RATIO_VOLUME_DRY_ABSORPTION_SOLAR_UNCERTAINTY_RANDOM_"
         "COVARIANCE"] = [&](CodaMemProduct& p) {
      return make_bad_size_with_unit(p, "(ppmv)2");
    };
    cfg.overrides["ALTITUDE"] = [](CodaMemProduct& p) {
      const long d2[] = {1, 2};
      const double vals[] = {10.0, 20.0};
      return MakeDoubleArrayWithFill(p, d2, vals, -9999.0);
    };
    cfg.overrides["ALTITUDE_BOUNDARIES"] = [](CodaMemProduct& p) {
      const long d3[] = {1, 2, 2};
      const double vals[] = {0.0, 10.0, 10.0, 20.0};
      return MakeDoubleArrayWithFill(p, d3, vals, -9999.0);
    };
    PopulateGeomsFtirProduct(prod, cfg, /*num_time=*/2, /*num_vertical=*/2);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["O3_column_number_density"].read_status);
    EXPECT_EQ(-1,
              res.variables["H2O_column_number_density_apriori"].read_status);
    EXPECT_EQ(-1, res.variables["O3_volume_mixing_ratio_dry_air"].read_status);
    EXPECT_EQ(-1, res.variables["H2O_volume_mixing_ratio_dry_air"].read_status);
    EXPECT_EQ(
        -1,
        res.variables["O3_volume_mixing_ratio_dry_air_covariance"].read_status);
    EXPECT_EQ(-1,
              res.variables["O3_volume_mixing_ratio_dry_air_uncertainty_random"]
                  .read_status);
    EXPECT_EQ(-1, res.variables["altitude"].read_status);
    EXPECT_EQ(-1, res.variables["altitude_bounds"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
