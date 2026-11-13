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
// Unit tests for NPP Suomi VIIRS L2 ingestion
// (libharp/harp-ingest-npp-suomi-viirs_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-npp-suomi-viirs_l2.c

#include <cmath>
#include <cstddef>
#include <format>
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

constexpr double kMicrosecondsPerSecond = 1000000.0;
constexpr double kSecondsFrom1958To2000 = 1325376000.0;
constexpr int kAodWavelengths[] = {412, 445, 488,  550,  555, 672,
                                   746, 865, 1240, 1610, 2250};

void AddGeoSwath(CodaMemProduct& prod, CodaMemNode* all_data,
                 std::string_view swath_name, bool with_fill_values) {
  CodaMemNode* geo = prod.MakeRecord();
  const long time_dims[] = {2};
  const long grid_dims[] = {2, 2};
  const double base_us =
      (kSecondsFrom1958To2000 + 400.0) * kMicrosecondsPerSecond;
  const std::vector<double> start_times = {base_us,
                                           with_fill_values ? -995.0 : base_us};
  const std::vector<double> mid_times = {
      base_us + 2.0 * kMicrosecondsPerSecond,
      base_us + 4.0 * kMicrosecondsPerSecond};
  ASSERT_TRUE(prod.AddField(geo, "StartTime",
                            prod.MakeDoubleArray(time_dims, start_times)));
  ASSERT_TRUE(prod.AddField(geo, "MidTime",
                            prod.MakeDoubleArray(time_dims, mid_times)));

  const std::vector<double> lats = {10.0, with_fill_values ? -999.5 : 20.0,
                                    30.0, 40.0};
  const std::vector<double> lons = {100.0, 110.0, 120.0, 130.0};
  const std::vector<double> saz = {15.0, 25.0, 35.0, 45.0};
  const std::vector<double> sza = {5.0, 15.0, 25.0, 35.0};
  const std::vector<double> solaz = {55.0, 65.0, 75.0, 85.0};
  const std::vector<double> solza = {30.0, 40.0, 50.0, 60.0};

  ASSERT_TRUE(
      prod.AddField(geo, "Latitude", prod.MakeDoubleArray(grid_dims, lats)));
  ASSERT_TRUE(
      prod.AddField(geo, "Longitude", prod.MakeDoubleArray(grid_dims, lons)));
  ASSERT_TRUE(prod.AddField(geo, "SatelliteAzimuthAngle",
                            prod.MakeDoubleArray(grid_dims, saz)));
  ASSERT_TRUE(prod.AddField(geo, "SatelliteZenithAngle",
                            prod.MakeDoubleArray(grid_dims, sza)));
  ASSERT_TRUE(prod.AddField(geo, "SolarAzimuthAngle",
                            prod.MakeDoubleArray(grid_dims, solaz)));
  ASSERT_TRUE(prod.AddField(geo, "SolarZenithAngle",
                            prod.MakeDoubleArray(grid_dims, solza)));

  ASSERT_TRUE(prod.AddField(all_data, swath_name, geo));
}

void AddCloudSwath(CodaMemProduct& prod, CodaMemNode* all_data,
                   std::string_view swath_name, std::string_view data_field,
                   std::string_view factors_field, double base_val,
                   bool include_factors = true) {
  CodaMemNode* swath = prod.MakeRecord();
  const long grid_dims[] = {2, 2};
  const long factor_dims[] = {8};
  // Element 0 is 65530.0 (in [65527.5, 65535.5] -> mapped to NaN).
  const std::vector<double> vals = {65530.0, base_val + 1.0, base_val + 2.0,
                                    base_val + 3.0};
  const std::vector<double> factors = {2.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  ASSERT_TRUE(
      prod.AddField(swath, data_field, prod.MakeDoubleArray(grid_dims, vals)));
  if (include_factors) {
    ASSERT_TRUE(prod.AddField(swath, factors_field,
                              prod.MakeDoubleArray(factor_dims, factors)));
  }
  ASSERT_TRUE(prod.AddField(all_data, swath_name, swath));
}

TEST(HarpIngestNppSuomiViirsL2Test, IngestAerosolOpticalDepthSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("NPP_SUOMI_L2_VIIRS_EDR_VAOO");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  CodaMemNode* all_data = prod.MakeRecord();
  AddGeoSwath(prod, all_data, "VIIRS_Aeros_EDR_GEO_All",
              /*with_fill_values=*/true);

  CodaMemNode* aeros = prod.MakeRecord();
  const long grid_dims[] = {2, 2};
  for (size_t i = 0; i < std::size(kAodWavelengths); ++i) {
    const std::string field =
        std::format("AerosolOpticalDepth_at_{}nm", kAodWavelengths[i]);
    const double base = static_cast<double>(i) * 0.1;
    const std::vector<double> vals = {i == 0 ? 65530.0 : base + 0.01,
                                      base + 0.02, base + 0.03, base + 0.04};
    ASSERT_TRUE(
        prod.AddField(aeros, field, prod.MakeDoubleArray(grid_dims, vals)));
  }
  ASSERT_TRUE(prod.AddField(all_data, "VIIRS_Aeros_EDR_All", aeros));
  ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(4, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(11, res.dimensions[harp_dimension_spectral]);

  ASSERT_EQ(4u, res.variables["datetime"].double_values.size());
  EXPECT_THAT(res.variables["datetime"].double_values[0],
              DoubleNear(400.0, 1e-6));
  EXPECT_THAT(res.variables["datetime"].double_values[1],
              DoubleNear(402.0, 1e-6));
  EXPECT_TRUE(std::isnan(res.variables["datetime"].double_values[2]));
  EXPECT_TRUE(std::isnan(res.variables["datetime"].double_values[3]));

  ASSERT_EQ(4u, res.variables["latitude"].double_values.size());
  EXPECT_DOUBLE_EQ(10.0, res.variables["latitude"].double_values[0]);
  EXPECT_TRUE(std::isnan(res.variables["latitude"].double_values[1]));
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
  EXPECT_THAT(res.variables["wavelength"].double_values,
              ElementsAre(412.0, 445.0, 488.0, 550.0, 555.0, 672.0, 746.0,
                          865.0, 1240.0, 1610.0, 2250.0));

  ASSERT_EQ(44u, res.variables["aerosol_optical_depth"].double_values.size());
  EXPECT_TRUE(
      std::isnan(res.variables["aerosol_optical_depth"].double_values[0]));
  EXPECT_THAT(res.variables["aerosol_optical_depth"].double_values[1],
              DoubleNear(0.11, 1e-9));
  EXPECT_THAT(res.variables["aerosol_optical_depth"].double_values[11],
              DoubleNear(0.02, 1e-9));
}

TEST(HarpIngestNppSuomiViirsL2Test, IngestAllCloudProductsTogetherSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("NPP_SUOMI_L2_VIIRS_EDR_VCBH");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  CodaMemNode* all_data = prod.MakeRecord();
  // Use GEO_TC_All suffix to exercise the second geo swath suffix branch.
  AddGeoSwath(prod, all_data, "VIIRS_CLD_AGG_GEO_TC_All",
              /*with_fill_values=*/false);
  AddCloudSwath(prod, all_data, "VIIRS_CBH_EDR_All", "AverageCloudBaseHeight",
                "CBHFactors", 1.0);
  AddCloudSwath(prod, all_data, "VIIRS_CTH_EDR_All", "AverageCloudTopHeight",
                "CTHFactors", 10.0);
  AddCloudSwath(prod, all_data, "VIIRS_CTP_EDR_All", "AverageCloudTopPressure",
                "CTPFactors", 100.0);
  AddCloudSwath(prod, all_data, "VIIRS_CTT_EDR_All",
                "AverageCloudTopTemperature", "CTTFactors", 200.0);
  AddCloudSwath(prod, all_data, "VIIRS_CCL_EDR_All", "SummedCloudCover",
                "CCLFactors", 0.0);
  AddCloudSwath(prod, all_data, "VIIRS_CEPS_EDR_All",
                "AverageCloudEffectiveParticleSize", "CEPSFactors", 5.0);
  AddCloudSwath(prod, all_data, "VIIRS_COT_EDR_All",
                "AverageCloudOpticalThickness", "COTFactors", 20.0);
  ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);

  auto expect_scaled = [&](std::string_view var_name, double base) {
    ASSERT_TRUE(res.variables.contains(std::string(var_name))) << var_name;
    EXPECT_TRUE(res.variables[std::string(var_name)].included) << var_name;
    const auto& vals = res.variables[std::string(var_name)].double_values;
    ASSERT_EQ(4u, vals.size()) << var_name;
    EXPECT_TRUE(std::isnan(vals[0])) << var_name;
    EXPECT_DOUBLE_EQ((base + 1.0) * 2.0 + 1.0, vals[1]) << var_name;
    EXPECT_DOUBLE_EQ((base + 2.0) * 2.0 + 1.0, vals[2]) << var_name;
    EXPECT_DOUBLE_EQ((base + 3.0) * 2.0 + 1.0, vals[3]) << var_name;
  };

  expect_scaled("cloud_base_height", 1.0);
  expect_scaled("cloud_top_height", 10.0);
  expect_scaled("cloud_top_pressure", 100.0);
  expect_scaled("cloud_top_temperature", 200.0);
  expect_scaled("cloud_fraction", 0.0);
  expect_scaled("cloud_effective_particle_size", 5.0);
  expect_scaled("cloud_optical_depth", 20.0);
}

struct CloudModuleCase {
  std::string_view module_name;
  std::string_view swath_name;
  std::string_view data_field;
  std::string_view factors_field;
  std::string_view harp_var;
};

constexpr CloudModuleCase kCloudModules[] = {
    {.module_name = "NPP_SUOMI_L2_VIIRS_EDR_VCCL",
     .swath_name = "VIIRS_CCL_EDR_All",
     .data_field = "SummedCloudCover",
     .factors_field = "CCLFactors",
     .harp_var = "cloud_fraction"},
    {.module_name = "NPP_SUOMI_L2_VIIRS_EDR_VCEP",
     .swath_name = "VIIRS_CEPS_EDR_All",
     .data_field = "AverageCloudEffectiveParticleSize",
     .factors_field = "CEPSFactors",
     .harp_var = "cloud_effective_particle_size"},
    {.module_name = "NPP_SUOMI_L2_VIIRS_EDR_VCDT",
     .swath_name = "VIIRS_COT_EDR_All",
     .data_field = "AverageCloudOpticalThickness",
     .factors_field = "COTFactors",
     .harp_var = "cloud_optical_depth"},
    {.module_name = "NPP_SUOMI_L2_VIIRS_EDR_VCTH",
     .swath_name = "VIIRS_CTH_EDR_All",
     .data_field = "AverageCloudTopHeight",
     .factors_field = "CTHFactors",
     .harp_var = "cloud_top_height"},
    {.module_name = "NPP_SUOMI_L2_VIIRS_EDR_VCTP",
     .swath_name = "VIIRS_CTP_EDR_All",
     .data_field = "AverageCloudTopPressure",
     .factors_field = "CTPFactors",
     .harp_var = "cloud_top_pressure"},
    {.module_name = "NPP_SUOMI_L2_VIIRS_EDR_VCTT",
     .swath_name = "VIIRS_CTT_EDR_All",
     .data_field = "AverageCloudTopTemperature",
     .factors_field = "CTTFactors",
     .harp_var = "cloud_top_temperature"},
};

TEST(HarpIngestNppSuomiViirsL2Test, IngestEachCloudModuleIndividually) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  for (const auto& tc : kCloudModules) {
    const harp_ingestion_module* mod = FindIngestionModule(tc.module_name);
    ASSERT_NE(nullptr, mod) << tc.module_name;

    // 1. Success with only the primary swath present (other cloud vars
    // excluded).
    {
      CodaMemProduct prod;
      ASSERT_TRUE(prod.ok());
      CodaMemNode* all_data = prod.MakeRecord();
      AddGeoSwath(prod, all_data, "VIIRS_CLD_AGG_GEO_All",
                  /*with_fill_values=*/false);
      AddCloudSwath(prod, all_data, tc.swath_name, tc.data_field,
                    tc.factors_field, 2.0);
      ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));

      IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
      ASSERT_EQ(HARP_SUCCESS, res.status) << tc.module_name;
      EXPECT_TRUE(res.variables[std::string(tc.harp_var)].included);
      if (tc.harp_var != "cloud_base_height") {
        EXPECT_FALSE(res.variables["cloud_base_height"].included);
      }
    }

    // 2. Error when data field is missing.
    {
      CodaMemProduct prod;
      ASSERT_TRUE(prod.ok());
      CodaMemNode* all_data = prod.MakeRecord();
      AddGeoSwath(prod, all_data, "VIIRS_CLD_AGG_GEO_All",
                  /*with_fill_values=*/false);
      ASSERT_TRUE(prod.AddField(all_data, tc.swath_name, prod.MakeRecord()));
      ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));

      IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
      EXPECT_EQ(-1, res.status) << tc.module_name;
      EXPECT_EQ(tc.harp_var, res.failed_variable);
    }

    // 3. Error when *Factors field is missing.
    {
      CodaMemProduct prod;
      ASSERT_TRUE(prod.ok());
      CodaMemNode* all_data = prod.MakeRecord();
      AddGeoSwath(prod, all_data, "VIIRS_CLD_AGG_GEO_All",
                  /*with_fill_values=*/false);
      AddCloudSwath(prod, all_data, tc.swath_name, tc.data_field,
                    tc.factors_field, 2.0, /*include_factors=*/false);
      ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));

      IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
      EXPECT_EQ(-1, res.status) << tc.module_name;
      EXPECT_EQ(tc.harp_var, res.failed_variable);
    }
  }
}

TEST(HarpIngestNppSuomiViirsL2Test, InitAndVariableErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod =
      FindIngestionModule("NPP_SUOMI_L2_VIIRS_EDR_VAOO");
  ASSERT_NE(nullptr, mod);

  // 1. Missing All_Data.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Missing Latitude, scalar Latitude, missing StartTime, scalar StartTime.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(all_data, "VIIRS_Aeros_EDR_GEO_All", prod.MakeRecord()));
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
    ASSERT_TRUE(prod.AddField(geo, "Latitude", prod.MakeDouble(1.0)));
    ASSERT_TRUE(prod.AddField(all_data, "VIIRS_Aeros_EDR_GEO_All", geo));
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
    const long dims[] = {2, 2};
    const double vals[] = {1.0, 2.0, 3.0, 4.0};
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(all_data, "VIIRS_Aeros_EDR_GEO_All", geo));
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
    const long dims[] = {2, 2};
    const double vals[] = {1.0, 2.0, 3.0, 4.0};
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(geo, "StartTime", prod.MakeDouble(1.0)));
    ASSERT_TRUE(prod.AddField(all_data, "VIIRS_Aeros_EDR_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. StartTime wrong type (string array) -> read_datetime fails on StartTime.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    const long dims[] = {2, 2};
    const long tdims[] = {2};
    const double vals[] = {1.0, 2.0, 3.0, 4.0};
    const std::string_view bad_t[] = {"a", "b"};
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(
        prod.AddField(geo, "StartTime", prod.MakeStringArray(tdims, bad_t)));
    ASSERT_TRUE(prod.AddField(all_data, "VIIRS_Aeros_EDR_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    EXPECT_EQ("datetime", res.failed_variable);
  }

  // 4. Missing MidTime -> read_datetime fails on MidTime.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    const long dims[] = {2, 2};
    const long tdims[] = {2};
    const double vals[] = {1.0, 2.0, 3.0, 4.0};
    const double tvals[] = {1.0, 2.0};
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(
        prod.AddField(geo, "StartTime", prod.MakeDoubleArray(tdims, tvals)));
    ASSERT_TRUE(prod.AddField(all_data, "VIIRS_Aeros_EDR_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    EXPECT_EQ("datetime", res.failed_variable);
  }

  // 5. Longitude 1D -> num_coda_dimensions != num_dimensions; wrong dim0 on
  // SatelliteZenithAngle; wrong dim1 on SatelliteAzimuthAngle; missing AOD
  // field. (Note: Latitude must remain 2D because get_dimensions() reads
  // coda_dimension[1] unconditionally.)
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    const long dims1[] = {2};
    const long dims2[] = {2, 2};
    const double vals1[] = {1.0, 2.0};
    const double vals2[] = {1.0, 2.0, 3.0, 4.0};
    ASSERT_TRUE(
        prod.AddField(geo, "StartTime", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(
        prod.AddField(geo, "MidTime", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims2, vals2)));
    ASSERT_TRUE(
        prod.AddField(geo, "Longitude", prod.MakeDoubleArray(dims1, vals1)));
    ASSERT_TRUE(prod.AddField(all_data, "VIIRS_Aeros_EDR_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
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
    const long tdims[] = {2};
    const long dims[] = {2, 2};
    const long bad_dims[] = {1, 2};
    const double tvals[] = {1.0, 2.0};
    const double vals[] = {1.0, 2.0, 3.0, 4.0};
    ASSERT_TRUE(
        prod.AddField(geo, "StartTime", prod.MakeDoubleArray(tdims, tvals)));
    ASSERT_TRUE(
        prod.AddField(geo, "MidTime", prod.MakeDoubleArray(tdims, tvals)));
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(
        prod.AddField(geo, "Longitude", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(geo, "SatelliteAzimuthAngle",
                              prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(geo, "SatelliteZenithAngle",
                              prod.MakeDoubleArray(bad_dims, tvals)));
    ASSERT_TRUE(prod.AddField(all_data, "VIIRS_Aeros_EDR_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    EXPECT_EQ("sensor_zenith_angle", res.failed_variable);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    CodaMemNode* geo = prod.MakeRecord();
    const long tdims[] = {2};
    const long dims[] = {2, 2};
    const long bad_dims[] = {2, 1};
    const double tvals[] = {1.0, 2.0};
    const double vals[] = {1.0, 2.0, 3.0, 4.0};
    ASSERT_TRUE(
        prod.AddField(geo, "StartTime", prod.MakeDoubleArray(tdims, tvals)));
    ASSERT_TRUE(
        prod.AddField(geo, "MidTime", prod.MakeDoubleArray(tdims, tvals)));
    ASSERT_TRUE(
        prod.AddField(geo, "Latitude", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(
        prod.AddField(geo, "Longitude", prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(geo, "SatelliteAzimuthAngle",
                              prod.MakeDoubleArray(bad_dims, tvals)));
    ASSERT_TRUE(prod.AddField(all_data, "VIIRS_Aeros_EDR_GEO_All", geo));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
    EXPECT_EQ("sensor_azimuth_angle", res.failed_variable);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    AddGeoSwath(prod, all_data, "VIIRS_Aeros_EDR_GEO_All",
                /*with_fill_values=*/false);
    ASSERT_TRUE(
        prod.AddField(all_data, "VIIRS_Aeros_EDR_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
    EXPECT_EQ("aerosol_optical_depth", res.failed_variable);
  }

  // 6. VCBH missing AverageCloudBaseHeight and missing CBHFactors.
  const harp_ingestion_module* vcbh_mod =
      FindIngestionModule("NPP_SUOMI_L2_VIIRS_EDR_VCBH");
  ASSERT_NE(nullptr, vcbh_mod);
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    AddGeoSwath(prod, all_data, "VIIRS_CLD_AGG_GEO_All",
                /*with_fill_values=*/false);
    ASSERT_TRUE(
        prod.AddField(all_data, "VIIRS_CBH_EDR_All", prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res =
        ExerciseIngestionModule(vcbh_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ("cloud_base_height", res.failed_variable);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* all_data = prod.MakeRecord();
    AddGeoSwath(prod, all_data, "VIIRS_CLD_AGG_GEO_All",
                /*with_fill_values=*/false);
    AddCloudSwath(prod, all_data, "VIIRS_CBH_EDR_All", "AverageCloudBaseHeight",
                  "CBHFactors", 1.0, /*include_factors=*/false);
    ASSERT_TRUE(prod.AddField(prod.root(), "All_Data", all_data));
    IngestExerciseResult res =
        ExerciseIngestionModule(vcbh_mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ("cloud_base_height", res.failed_variable);
  }
}

}  // namespace
}  // namespace stcorp_harp
