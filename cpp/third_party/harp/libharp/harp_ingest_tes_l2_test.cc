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
// Unit tests for Aura TES Level 2 ingestion modules
// (libharp/harp-ingest-tes_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-tes_l2.c

#include <cmath>
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

constexpr double kSecondsFrom1993To2000 = 220838400.0 + 5.0;

struct TesModuleSpec {
  std::string_view module_name;
  std::string_view swath_name;
  std::string_view value_field;
  std::string_view error_field;
  std::string_view harp_value_var;
  std::string_view harp_error_var;
};

constexpr TesModuleSpec kAllTesModules[] = {
    {"TES_L2_CH3OH_Nadir", "CH3OHNadirSwath", "CH3OH", "CH3OHPrecision",
     "CH3OH_volume_mixing_ratio", "CH3OH_volume_mixing_ratio_uncertainty"},
    {"TES_L2_CH4_Nadir", "CH4NadirSwath", "CH4", "CH4Precision",
     "CH4_volume_mixing_ratio", "CH4_volume_mixing_ratio_uncertainty"},
    {"TES_L2_CO_Nadir", "CONadirSwath", "CO", "COPrecision",
     "CO_volume_mixing_ratio", "CO_volume_mixing_ratio_uncertainty"},
    {"TES_L2_CO2_Nadir", "CO2NadirSwath", "CO2", "CO2Precision",
     "CO2_volume_mixing_ratio", "CO2_volume_mixing_ratio_uncertainty"},
    {"TES_L2_H2O_Nadir", "H2ONadirSwath", "H2O", "H2OPrecision",
     "H2O_volume_mixing_ratio", "H2O_volume_mixing_ratio_uncertainty"},
    {"TES_L2_HCOOH_Nadir", "HCOOHNadirSwath", "HCOOH", "HCOOHPrecision",
     "HCOOH_volume_mixing_ratio", "HCOOH_volume_mixing_ratio_uncertainty"},
    {"TES_L2_HDO_Nadir", "HDONadirSwath", "HDO", "HDOPrecision",
     "HDO_volume_mixing_ratio", "HDO_volume_mixing_ratio_uncertainty"},
    {"TES_L2_N2O_Nadir", "N2ONadirSwath", "N2O", "N2OPrecision",
     "N2O_volume_mixing_ratio", "N2O_volume_mixing_ratio_uncertainty"},
    {"TES_L2_NH3_Nadir", "NH3NadirSwath", "NH3", "NH3Precision",
     "NH3_volume_mixing_ratio", "NH3_volume_mixing_ratio_uncertainty"},
    {"TES_L2_O3_Nadir", "O3NadirSwath", "O3", "O3Precision",
     "O3_volume_mixing_ratio", "O3_volume_mixing_ratio_uncertainty"},
    {"TES_L2_Temperature_Nadir", "TATMNadirSwath", "TATM", "TATMPrecision",
     "temperature", "temperature_uncertainty"},
    {"TES_L2_CH4_Limb", "CH4LimbSwath", "CH4", "CH4Precision",
     "CH4_volume_mixing_ratio", "CH4_volume_mixing_ratio_uncertainty"},
    {"TES_L2_H2O_Limb", "H2OLimbSwath", "H2O", "H2OPrecision",
     "H2O_volume_mixing_ratio", "H2O_volume_mixing_ratio_uncertainty"},
    {"TES_L2_HDO_Limb", "HDOLimbSwath", "HDO", "HDOPrecision",
     "HDO_volume_mixing_ratio", "HDO_volume_mixing_ratio_uncertainty"},
    {"TES_L2_HNO3_Limb", "HNO3LimbSwath", "HNO3", "HNO3Precision",
     "HNO3_volume_mixing_ratio", "HNO3_volume_mixing_ratio_uncertainty"},
    {"TES_L2_NO2_Limb", "O3LimbSwath", "O3", "O3Precision",
     "NO2_volume_mixing_ratio", "NO2_volume_mixing_ratio_uncertainty"},
    {"TES_L2_O3_Limb", "O3LimbSwath", "O3", "O3Precision",
     "O3_volume_mixing_ratio", "O3_volume_mixing_ratio_uncertainty"},
    {"TES_L2_Temperature_Limb", "TATMLimbSwath", "TATM", "TATMPrecision",
     "temperature", "temperature_uncertainty"},
};

struct AttrConfig {
  bool include_missing = true;
  bool missing_empty = false;
  bool missing_wrong_type = false;
  double missing_val = -999.0;

  bool include_scale = true;
  bool scale_empty = false;
  bool scale_wrong_type = false;
  double scale_val = 2.0;

  bool include_offset = true;
  bool offset_empty = false;
  bool offset_wrong_type = false;
  double offset_val = 1.0;
};

struct TesL2Config {
  std::string_view swath_name = "O3NadirSwath";
  std::string_view value_field = "O3";
  std::string_view error_field = "O3Precision";
  long num_times = 2;
  long num_levels = 3;

  bool include_hdfeos = true;
  bool include_swath = true;
  bool include_data_fields = true;
  bool include_geo_fields = true;

  bool include_altitude = true;
  bool altitude_scalar = false;
  bool include_pressure = true;
  bool pressure_1d = false;
  bool pressure_bad_dim0 = false;
  bool pressure_bad_dim1 = false;

  bool include_time = true;
  bool time_wrong_type = false;
  AttrConfig value_attrs;
};

void AttachAttrs(CodaMemProduct& prod, CodaMemNode* node,
                 const AttrConfig& attrs) {
  constexpr long kOne[] = {1};
  constexpr long kZero[] = {0};
  if (attrs.include_missing) {
    if (attrs.missing_empty) {
      ASSERT_TRUE(prod.AddAttribute(node, "MissingValue",
                                    prod.MakeDoubleArray(kZero, {})));
    } else if (attrs.missing_wrong_type) {
      const std::string_view bad[] = {"bad"};
      ASSERT_TRUE(prod.AddAttribute(node, "MissingValue",
                                    prod.MakeStringArray(kOne, bad)));
    } else {
      const double vals[] = {attrs.missing_val};
      ASSERT_TRUE(prod.AddAttribute(node, "MissingValue",
                                    prod.MakeDoubleArray(kOne, vals)));
    }
  }
  if (attrs.include_scale) {
    if (attrs.scale_empty) {
      ASSERT_TRUE(prod.AddAttribute(node, "ScaleFactor",
                                    prod.MakeDoubleArray(kZero, {})));
    } else if (attrs.scale_wrong_type) {
      const std::string_view bad[] = {"bad"};
      ASSERT_TRUE(prod.AddAttribute(node, "ScaleFactor",
                                    prod.MakeStringArray(kOne, bad)));
    } else {
      const double vals[] = {attrs.scale_val};
      ASSERT_TRUE(prod.AddAttribute(node, "ScaleFactor",
                                    prod.MakeDoubleArray(kOne, vals)));
    }
  }
  if (attrs.include_offset) {
    if (attrs.offset_empty) {
      ASSERT_TRUE(
          prod.AddAttribute(node, "Offset", prod.MakeDoubleArray(kZero, {})));
    } else if (attrs.offset_wrong_type) {
      const std::string_view bad[] = {"bad"};
      ASSERT_TRUE(
          prod.AddAttribute(node, "Offset", prod.MakeStringArray(kOne, bad)));
    } else {
      const double vals[] = {attrs.offset_val};
      ASSERT_TRUE(
          prod.AddAttribute(node, "Offset", prod.MakeDoubleArray(kOne, vals)));
    }
  }
}

void PopulateTesL2Product(CodaMemProduct& prod, const TesL2Config& cfg) {
  if (!cfg.include_hdfeos) {
    return;
  }
  CodaMemNode* hdfeos = prod.MakeRecord();
  CodaMemNode* swaths = prod.MakeRecord();
  if (cfg.include_swath) {
    CodaMemNode* swath = prod.MakeRecord();

    if (cfg.include_geo_fields) {
      CodaMemNode* geo = prod.MakeRecord();
      const long t_dims[] = {cfg.num_times};
      AttrConfig default_attrs;
      default_attrs.include_scale = false;
      default_attrs.include_offset = false;

      if (cfg.include_time) {
        CodaMemNode* time_node = nullptr;
        if (cfg.time_wrong_type) {
          std::vector<std::string_view> bad(cfg.num_times, "bad");
          time_node = prod.MakeStringArray(t_dims, bad);
        } else {
          std::vector<double> times(cfg.num_times);
          for (long i = 0; i < cfg.num_times; ++i) {
            times[i] = kSecondsFrom1993To2000 + 50.0 + i * 10.0;
          }
          time_node = prod.MakeDoubleArray(t_dims, times);
        }
        AttachAttrs(prod, time_node, default_attrs);
        ASSERT_TRUE(prod.AddField(geo, "Time", time_node));
      }

      std::vector<double> lons(cfg.num_times, 20.0);
      CodaMemNode* lon_node = prod.MakeDoubleArray(t_dims, lons);
      AttachAttrs(prod, lon_node, default_attrs);
      ASSERT_TRUE(prod.AddField(geo, "Longitude", lon_node));

      std::vector<double> lats(cfg.num_times, 10.0);
      CodaMemNode* lat_node = prod.MakeDoubleArray(t_dims, lats);
      AttachAttrs(prod, lat_node, default_attrs);
      ASSERT_TRUE(prod.AddField(geo, "Latitude", lat_node));

      ASSERT_TRUE(prod.AddField(swath, "Geolocation_Fields", geo));
    }

    if (cfg.include_data_fields) {
      CodaMemNode* data = prod.MakeRecord();
      const long grid_dims[] = {cfg.num_times, cfg.num_levels};
      const long total = cfg.num_times * cfg.num_levels;
      AttrConfig default_attrs;
      default_attrs.include_scale = false;
      default_attrs.include_offset = false;

      if (cfg.include_altitude) {
        CodaMemNode* alt_node = nullptr;
        if (cfg.altitude_scalar) {
          alt_node = prod.MakeDouble(1000.0);
        } else {
          std::vector<double> alts(total, 1000.0);
          alts[0] = -999.0;  // Triggers MissingValue -> NaN branch.
          alt_node = prod.MakeDoubleArray(grid_dims, alts);
          AttachAttrs(prod, alt_node, default_attrs);
        }
        ASSERT_TRUE(prod.AddField(data, "Altitude", alt_node));
      }

      if (cfg.include_pressure) {
        CodaMemNode* pres_node = nullptr;
        if (cfg.pressure_1d) {
          const long d[] = {cfg.num_times};
          std::vector<double> vals(cfg.num_times, 500.0);
          pres_node = prod.MakeDoubleArray(d, vals);
        } else if (cfg.pressure_bad_dim0) {
          const long d[] = {cfg.num_times + 1, cfg.num_levels};
          std::vector<double> vals((cfg.num_times + 1) * cfg.num_levels, 500.0);
          pres_node = prod.MakeDoubleArray(d, vals);
        } else if (cfg.pressure_bad_dim1) {
          const long d[] = {cfg.num_times, cfg.num_levels + 1};
          std::vector<double> vals(cfg.num_times * (cfg.num_levels + 1), 500.0);
          pres_node = prod.MakeDoubleArray(d, vals);
        } else {
          std::vector<double> vals(total, 500.0);
          pres_node = prod.MakeDoubleArray(grid_dims, vals);
        }
        AttachAttrs(prod, pres_node, default_attrs);
        ASSERT_TRUE(prod.AddField(data, "Pressure", pres_node));
      }

      std::vector<double> vals(total);
      std::vector<double> errs(total);
      for (long i = 0; i < total; ++i) {
        vals[i] = 10.0 + i;
        errs[i] = 0.5 + 0.1 * i;
      }
      CodaMemNode* val_node = prod.MakeDoubleArray(grid_dims, vals);
      AttachAttrs(prod, val_node, cfg.value_attrs);
      ASSERT_TRUE(prod.AddField(data, cfg.value_field, val_node));

      CodaMemNode* err_node = prod.MakeDoubleArray(grid_dims, errs);
      AttachAttrs(prod, err_node, default_attrs);
      ASSERT_TRUE(prod.AddField(data, cfg.error_field, err_node));

      ASSERT_TRUE(prod.AddField(swath, "Data_Fields", data));
    }

    ASSERT_TRUE(prod.AddField(swaths, cfg.swath_name, swath));
  }
  ASSERT_TRUE(prod.AddField(hdfeos, "SWATHS", swaths));
  ASSERT_TRUE(prod.AddField(prod.root(), "HDFEOS", hdfeos));
}

TEST(HarpIngestTesL2Test, AllEighteenNadirAndLimbModulesSucceed) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  for (const auto& spec : kAllTesModules) {
    SCOPED_TRACE(std::string(spec.module_name));
    const harp_ingestion_module* mod = FindIngestionModule(spec.module_name);
    ASSERT_NE(nullptr, mod);

    TesL2Config cfg;
    cfg.swath_name = spec.swath_name;
    cfg.value_field = spec.value_field;
    cfg.error_field = spec.error_field;

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateTesL2Product(prod, cfg);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(3, res.dimensions[harp_dimension_vertical]);

    EXPECT_DOUBLE_EQ(50.0, res.variables["datetime"].double_values[0]);
    EXPECT_DOUBLE_EQ(60.0, res.variables["datetime"].double_values[1]);
    EXPECT_DOUBLE_EQ(20.0, res.variables["longitude"].double_values[0]);
    EXPECT_DOUBLE_EQ(10.0, res.variables["latitude"].double_values[0]);
    EXPECT_TRUE(std::isnan(res.variables["altitude"].double_values[0]));
    EXPECT_DOUBLE_EQ(1000.0, res.variables["altitude"].double_values[1]);
    EXPECT_DOUBLE_EQ(500.0, res.variables["pressure"].double_values[0]);

    // ScaleFactor=2.0, Offset=1.0 -> 1.0 + 2.0 * 10.0 = 21.0.
    EXPECT_THAT(
        res.variables[std::string(spec.harp_value_var)].double_values[0],
        DoubleNear(21.0, 1e-9));
    EXPECT_THAT(
        res.variables[std::string(spec.harp_error_var)].double_values[0],
        DoubleNear(0.5, 1e-9));
  }
}

TEST(HarpIngestTesL2Test, InitAndReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("TES_L2_O3_Nadir");
  ASSERT_NE(nullptr, mod);

  auto run_with = [&](const TesL2Config& cfg, bool stop_on_error = true) {
    CodaMemProduct prod;
    if (!prod.ok()) return IngestExerciseResult{};
    PopulateTesL2Product(prod, cfg);
    return ExerciseIngestionModule(mod, prod.product(),
                                   /*options_str=*/nullptr, stop_on_error);
  };

  // Init cursor & dimension errors.
  {
    TesL2Config cfg;
    cfg.include_hdfeos = false;
    EXPECT_EQ(-1, run_with(cfg).status);
  }
  {
    TesL2Config cfg;
    cfg.include_swath = false;
    EXPECT_EQ(-1, run_with(cfg).status);
  }
  {
    TesL2Config cfg;
    cfg.include_data_fields = false;
    EXPECT_EQ(-1, run_with(cfg).status);
  }
  {
    TesL2Config cfg;
    cfg.include_geo_fields = false;
    EXPECT_EQ(-1, run_with(cfg).status);
  }
  {
    TesL2Config cfg;
    cfg.include_altitude = false;
    EXPECT_EQ(-1, run_with(cfg).status);
  }
  {
    TesL2Config cfg;
    cfg.altitude_scalar = true;
    EXPECT_EQ(-1, run_with(cfg).status);
  }

  // read_variable shape/type/attribute errors.
  {
    TesL2Config cfg;
    cfg.include_pressure = false;
    EXPECT_EQ(-1, run_with(cfg, false).variables["pressure"].read_status);
  }
  {
    TesL2Config cfg;
    cfg.pressure_1d = true;
    IngestExerciseResult res = run_with(cfg, false);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    TesL2Config cfg;
    cfg.pressure_bad_dim0 = true;
    IngestExerciseResult res = run_with(cfg, false);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    TesL2Config cfg;
    cfg.pressure_bad_dim1 = true;
    IngestExerciseResult res = run_with(cfg, false);
    EXPECT_EQ(-1, res.variables["pressure"].read_status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    TesL2Config cfg;
    cfg.include_time = false;
    EXPECT_EQ(-1, run_with(cfg, false).variables["datetime"].read_status);
  }
  {
    TesL2Config cfg;
    cfg.time_wrong_type = true;
    EXPECT_EQ(-1, run_with(cfg, false).variables["datetime"].read_status);
  }
  {
    TesL2Config cfg;
    cfg.value_attrs.include_missing = false;
    EXPECT_EQ(
        -1,
        run_with(cfg, false).variables["O3_volume_mixing_ratio"].read_status);
  }
  {
    TesL2Config cfg;
    cfg.value_attrs.missing_empty = true;
    EXPECT_EQ(
        -1,
        run_with(cfg, false).variables["O3_volume_mixing_ratio"].read_status);
  }
  {
    TesL2Config cfg;
    cfg.value_attrs.missing_wrong_type = true;
    EXPECT_EQ(
        -1,
        run_with(cfg, false).variables["O3_volume_mixing_ratio"].read_status);
  }
  {
    TesL2Config cfg;
    cfg.value_attrs.scale_empty = true;
    EXPECT_EQ(
        -1,
        run_with(cfg, false).variables["O3_volume_mixing_ratio"].read_status);
  }
  {
    TesL2Config cfg;
    cfg.value_attrs.scale_wrong_type = true;
    EXPECT_EQ(
        -1,
        run_with(cfg, false).variables["O3_volume_mixing_ratio"].read_status);
  }
  {
    TesL2Config cfg;
    cfg.value_attrs.offset_empty = true;
    EXPECT_EQ(
        -1,
        run_with(cfg, false).variables["O3_volume_mixing_ratio"].read_status);
  }
  {
    TesL2Config cfg;
    cfg.value_attrs.offset_wrong_type = true;
    EXPECT_EQ(
        -1,
        run_with(cfg, false).variables["O3_volume_mixing_ratio"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
