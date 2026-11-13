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
// Unit tests for NPP Suomi CrIMSS L2 ingestion
// (libharp/harp-ingest-npp-suomi-crimss_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-npp-suomi-crimss_l2.c

#include <cmath>
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

constexpr std::string_view kModuleName = "NPP_SUOMI_L2_CRIMSS_EDR_REDR";
constexpr double kMicrosecondsPerSecond = 1000000.0;
constexpr double kSecondsFrom1958To2000 = 1325376000.0;

struct CrimssConfig {
  long num_granules = 1;
  long num_scans = 2;
  long num_retrievals_per_scan = 2;
  long num_vertical = 3;
  std::string_view data_field = "H2O";
  std::string_view axis_field = "PressureLevels_H2O";
};

void PopulateCrimssProduct(CodaMemProduct& prod, const CrimssConfig& cfg) {
  CodaMemNode* all_data = prod.MakeRecord();
  CodaMemNode* geo = prod.MakeRecord();
  CodaMemNode* edr = prod.MakeRecord();

  const long num_obs = cfg.num_scans * cfg.num_retrievals_per_scan;
  const long scan_dims[] = {cfg.num_scans};
  const long obs_dims[] = {num_obs};
  const long gran_dims[] = {cfg.num_granules};
  const long data_dims[] = {num_obs, cfg.num_vertical};
  const long axis_dims[] = {cfg.num_granules * cfg.num_vertical};

  // Scan 0 is valid; Scan 1 has a fill StartTime (-995.0 in [-999.5, -992.5]).
  const double base_us =
      (kSecondsFrom1958To2000 + 100.0) * kMicrosecondsPerSecond;
  const std::vector<double> start_times = {base_us, -995.0};
  const std::vector<double> mid_times = {
      base_us + 2.0 * kMicrosecondsPerSecond,
      base_us + 10.0 * kMicrosecondsPerSecond};
  ASSERT_TRUE(prod.AddField(geo, "StartTime",
                            prod.MakeDoubleArray(scan_dims, start_times)));
  ASSERT_TRUE(prod.AddField(geo, "MidTime",
                            prod.MakeDoubleArray(scan_dims, mid_times)));

  // Geo fields; include one fill value (-999.5 in [-1000.0, -999.0]).
  const std::vector<double> lats = {10.0, 20.0, -999.5, 40.0};
  const std::vector<double> lons = {100.0, 110.0, 120.0, 130.0};
  const std::vector<double> saz = {15.0, 25.0, 35.0, 45.0};
  const std::vector<double> sza = {5.0, 15.0, 25.0, 35.0};
  const std::vector<double> solaz = {55.0, 65.0, 75.0, 85.0};
  const std::vector<double> solza = {30.0, 40.0, 50.0, 60.0};

  ASSERT_TRUE(
      prod.AddField(geo, "Latitude", prod.MakeDoubleArray(obs_dims, lats)));
  ASSERT_TRUE(
      prod.AddField(geo, "Longitude", prod.MakeDoubleArray(obs_dims, lons)));
  ASSERT_TRUE(prod.AddField(geo, "SatelliteAzimuthAngle",
                            prod.MakeDoubleArray(obs_dims, saz)));
  ASSERT_TRUE(prod.AddField(geo, "SatelliteZenithAngle",
                            prod.MakeDoubleArray(obs_dims, sza)));
  ASSERT_TRUE(prod.AddField(geo, "SolarAzimuthAngle",
                            prod.MakeDoubleArray(obs_dims, solaz)));
  ASSERT_TRUE(prod.AddField(geo, "SolarZenithAngle",
                            prod.MakeDoubleArray(obs_dims, solza)));

  const std::vector<double> retrievals(static_cast<size_t>(cfg.num_granules),
                                       2.0);
  ASSERT_TRUE(prod.AddField(edr, "NumRetrievals",
                            prod.MakeDoubleArray(gran_dims, retrievals)));

  std::vector<double> data_vals(static_cast<size_t>(num_obs * cfg.num_vertical),
                                1.0);
  for (size_t i = 0; i < data_vals.size(); ++i) {
    data_vals[i] = static_cast<double>(i + 1) * 1.5;
  }
  data_vals[0] = -999.5;  // In [-999.95, -999.25] -> mapped to NaN.
  ASSERT_TRUE(prod.AddField(edr, cfg.data_field,
                            prod.MakeDoubleArray(data_dims, data_vals)));

  std::vector<double> axis_vals(
      static_cast<size_t>(cfg.num_granules * cfg.num_vertical), 0.0);
  for (size_t i = 0; i < axis_vals.size(); ++i) {
    axis_vals[i] = 1000.0 - static_cast<double>(i) * 100.0;
  }
  ASSERT_TRUE(prod.AddField(edr, cfg.axis_field,
                            prod.MakeDoubleArray(axis_dims, axis_vals)));

  ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_GEO_TC_All", geo));
  ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_All", edr));
  ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
}

TEST(HarpIngestNppSuomiCrimssL2Test, IngestMoistureProfileSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateCrimssProduct(prod, CrimssConfig{});

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(4, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(3, res.dimensions[harp_dimension_vertical]);

  ASSERT_EQ(4u, res.variables["datetime"].double_values.size());
  EXPECT_THAT(res.variables["datetime"].double_values[0],
              DoubleNear(100.0, 1e-6));
  EXPECT_THAT(res.variables["datetime"].double_values[1],
              DoubleNear(102.0, 1e-6));
  EXPECT_TRUE(std::isnan(res.variables["datetime"].double_values[2]));
  EXPECT_TRUE(std::isnan(res.variables["datetime"].double_values[3]));

  ASSERT_EQ(4u, res.variables["latitude"].double_values.size());
  EXPECT_DOUBLE_EQ(10.0, res.variables["latitude"].double_values[0]);
  EXPECT_DOUBLE_EQ(20.0, res.variables["latitude"].double_values[1]);
  EXPECT_TRUE(std::isnan(res.variables["latitude"].double_values[2]));
  EXPECT_DOUBLE_EQ(40.0, res.variables["latitude"].double_values[3]);

  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(100.0, 110.0, 120.0, 130.0));
  EXPECT_THAT(res.variables["sensor_azimuth_angle"].double_values,
              ElementsAre(15.0, 25.0, 35.0, 45.0));
  EXPECT_THAT(res.variables["sensor_zenith_angle"].double_values,
              ElementsAre(5.0, 15.0, 25.0, 35.0));
  EXPECT_THAT(res.variables["solar_azimuth_angle"].double_values,
              ElementsAre(55.0, 65.0, 75.0, 85.0));
  EXPECT_THAT(res.variables["solar_zenith_angle"].double_values,
              ElementsAre(30.0, 40.0, 50.0, 60.0));

  ASSERT_EQ(12u,
            res.variables["H2O_column_mass_mixing_ratio"].double_values.size());
  EXPECT_TRUE(std::isnan(
      res.variables["H2O_column_mass_mixing_ratio"].double_values[0]));
  EXPECT_DOUBLE_EQ(
      3.0, res.variables["H2O_column_mass_mixing_ratio"].double_values[1]);
  EXPECT_THAT(res.variables["pressure"].double_values,
              ElementsAre(1000.0, 900.0, 800.0));
}

TEST(HarpIngestNppSuomiCrimssL2Test, IngestTemperatureProfileSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  CrimssConfig cfg;
  cfg.data_field = "Temperature";
  cfg.axis_field = "PressureLevels_Temperature";
  PopulateCrimssProduct(prod, cfg);

  IngestExerciseResult res =
      ExerciseIngestionModule(mod, prod.product(), "dataset=temp");
  ASSERT_EQ(HARP_SUCCESS, res.status);
  ASSERT_EQ(12u, res.variables["temperature"].double_values.size());
  EXPECT_TRUE(std::isnan(res.variables["temperature"].double_values[0]));
  EXPECT_DOUBLE_EQ(3.0, res.variables["temperature"].double_values[1]);
  EXPECT_THAT(res.variables["pressure"].double_values,
              ElementsAre(1000.0, 900.0, 800.0));
}

TEST(HarpIngestNppSuomiCrimssL2Test, IngestPressureProfileSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  CrimssConfig cfg;
  cfg.data_field = "Pressure";
  cfg.axis_field = "AltitudeLevels_Pressure";
  PopulateCrimssProduct(prod, cfg);

  IngestExerciseResult res =
      ExerciseIngestionModule(mod, prod.product(), "dataset=press");
  ASSERT_EQ(HARP_SUCCESS, res.status);
  ASSERT_EQ(12u, res.variables["pressure"].double_values.size());
  EXPECT_TRUE(std::isnan(res.variables["pressure"].double_values[0]));
  EXPECT_DOUBLE_EQ(3.0, res.variables["pressure"].double_values[1]);
  EXPECT_THAT(res.variables["altitude"].double_values,
              ElementsAre(1000.0, 900.0, 800.0));
}

TEST(HarpIngestNppSuomiCrimssL2Test, InitErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  // 1. Invalid option value.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "dataset=invalid");
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INVALID_INGESTION_OPTION_VALUE, res.failed_errno);
  }

  // 2. Missing All_Data.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Missing CrIMSS_EDR_GEO_TC_All.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", prod.MakeRecord()));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. Missing CrIMSS_EDR_All.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(all_data, "CrIMSS_EDR_GEO_TC_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 5. Missing NumRetrievals.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(all_data, "CrIMSS_EDR_GEO_TC_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 6. Scalar NumRetrievals (not an array).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* edr = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(edr, "NumRetrievals", prod.MakeDouble(1.0)));
    ASSERT_TRUE(
        prod.AddField(all_data, "CrIMSS_EDR_GEO_TC_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_All", edr));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 7. Missing StartTime, then scalar StartTime.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* edr = prod.MakeRecord();
    const long dims[] = {1};
    const double vals[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(edr, "NumRetrievals", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(
        prod.AddField(all_data, "CrIMSS_EDR_GEO_TC_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_All", edr));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* edr = prod.MakeRecord();
    const long dims[] = {1};
    const double vals[] = {1.0};
    ASSERT_TRUE(prod.AddField(geo, "StartTime", prod.MakeDouble(1.0)));
    ASSERT_TRUE(
        prod.AddField(edr, "NumRetrievals", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_GEO_TC_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_All", edr));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 8. Missing Latitude, then scalar Latitude.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* edr = prod.MakeRecord();
    const long dims[] = {1};
    const double vals[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(geo, "StartTime", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(
        prod.AddField(edr, "NumRetrievals", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_GEO_TC_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_All", edr));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* edr = prod.MakeRecord();
    const long dims[] = {1};
    const double vals[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(geo, "StartTime", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(geo, "Latitude", prod.MakeDouble(1.0)));
    ASSERT_TRUE(
        prod.AddField(edr, "NumRetrievals", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_GEO_TC_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_All", edr));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 9. Missing PressureLevels_H2O, then scalar PressureLevels_H2O.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* edr = prod.MakeRecord();
    const long dims[] = {1};
    const double vals[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(geo, "StartTime", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(
        prod.AddField(edr, "NumRetrievals", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_GEO_TC_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_All", edr));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* edr = prod.MakeRecord();
    const long dims[] = {1};
    const double vals[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(geo, "StartTime", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(
        prod.AddField(edr, "NumRetrievals", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(
        prod.AddField(edr, "PressureLevels_H2O", prod.MakeDouble(1000.0)));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_GEO_TC_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_All", edr));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

TEST(HarpIngestNppSuomiCrimssL2Test, ReadVariableErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule(kModuleName);
  ASSERT_NE(nullptr, mod);

  // 1. StartTime is a string array -> read_datetime fails reading StartTime.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* edr = prod.MakeRecord();
    const long dims1[] = {1};
    const double vals1[] = {1.0};
    const std::string_view bad_str[] = {"bad"};
    ASSERT_TRUE(
        prod.AddField(geo, "StartTime", prod.MakeStringArray(dims1, bad_str)));
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(edr, "NumRetrievals",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(edr, "PressureLevels_H2O",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_GEO_TC_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_All", edr));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    EXPECT_EQ("datetime", res.failed_variable);
  }

  // 2. Missing MidTime -> read_datetime fails reading MidTime.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* edr = prod.MakeRecord();
    const long dims1[] = {1};
    const double vals1[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(geo, "StartTime", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(edr, "NumRetrievals",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(edr, "PressureLevels_H2O",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_GEO_TC_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_All", edr));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    EXPECT_EQ("datetime", res.failed_variable);
  }

  // 3. Latitude has 2 dims -> num_coda_dimensions != num_dimensions.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* edr = prod.MakeRecord();
    const long dims1[] = {1};
    const long dims2[] = {1, 1};
    const double vals1[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(geo, "StartTime", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(
        prod.AddField(geo, "MidTime", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims2, vals1)));
    ASSERT_TRUE(prod.AddField(edr, "NumRetrievals",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(edr, "PressureLevels_H2O",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_GEO_TC_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_All", edr));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    EXPECT_EQ("latitude", res.failed_variable);
  }

  // 4. Longitude has wrong dimension_0, H2O has wrong dimension_1, and
  // PressureLevels_H2O has wrong type (string array).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* edr = prod.MakeRecord();
    const long dims1[] = {1};
    const long bad_lon_dims[] = {2};
    const long bad_h2o_dims[] = {1, 2};
    const double vals1[] = {1.0};
    const double vals2[] = {1.0, 2.0};
    const std::string_view bad_axis[] = {"1000"};
    ASSERT_TRUE(
        prod.AddField(geo, "StartTime", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(
        prod.AddField(geo, "MidTime", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(geo, "Longitude",
                              prod.MakeDoubleArray(bad_lon_dims, vals2)));
    ASSERT_TRUE(prod.AddField(geo, "SatelliteAzimuthAngle",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(geo, "SatelliteZenithAngle",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(geo, "SolarAzimuthAngle",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(geo, "SolarZenithAngle",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(edr, "NumRetrievals",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(
        prod.AddField(edr, "H2O", prod.MakeDoubleArray(bad_h2o_dims, vals2)));
    ASSERT_TRUE(prod.AddField(edr, "PressureLevels_H2O",
                              prod.MakeStringArray(dims1, bad_axis)));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_GEO_TC_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_All", edr));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));

    // Note: use stop_on_error = true per failing variable because read_variable
    // leaves the cursor positioned inside the failed field on error.
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    EXPECT_EQ("longitude", res.failed_variable);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* edr = prod.MakeRecord();
    const long dims1[] = {1};
    const long bad_h2o_dims[] = {1, 2};
    const double vals1[] = {1.0};
    const double vals2[] = {1.0, 2.0};
    ASSERT_TRUE(
        prod.AddField(geo, "StartTime", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(
        prod.AddField(geo, "MidTime", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(
        prod.AddField(geo, "Longitude", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(geo, "SatelliteAzimuthAngle",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(geo, "SatelliteZenithAngle",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(geo, "SolarAzimuthAngle",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(geo, "SolarZenithAngle",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(edr, "NumRetrievals",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(
        prod.AddField(edr, "H2O", prod.MakeDoubleArray(bad_h2o_dims, vals2)));
    ASSERT_TRUE(prod.AddField(edr, "PressureLevels_H2O",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_GEO_TC_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_All", edr));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    EXPECT_EQ("H2O_column_mass_mixing_ratio", res.failed_variable);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    CodaMemNode* edr = prod.MakeRecord();
    const long dims1[] = {1};
    const long h2o_dims[] = {1, 1};
    const double vals1[] = {1.0};
    const std::string_view bad_axis[] = {"1000"};
    ASSERT_TRUE(
        prod.AddField(geo, "StartTime", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(
        prod.AddField(geo, "MidTime", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(
        prod.AddField(geo, "Longitude", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(geo, "SatelliteAzimuthAngle",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(geo, "SatelliteZenithAngle",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(geo, "SolarAzimuthAngle",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(geo, "SolarZenithAngle",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(edr, "NumRetrievals",
                              prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(
        prod.AddField(edr, "H2O", prod.MakeDoubleArray(h2o_dims, vals1)));
    ASSERT_TRUE(prod.AddField(edr, "PressureLevels_H2O",
                              prod.MakeStringArray(dims1, bad_axis)));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_GEO_TC_All", geo));
    ASSERT_TRUE(prod.AddField(all_data, "CrIMSS_EDR_All", edr));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    EXPECT_EQ("pressure", res.failed_variable);
  }
}

}  // namespace
}  // namespace stcorp_harp
