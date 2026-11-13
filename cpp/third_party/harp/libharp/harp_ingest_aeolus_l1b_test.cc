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
// Unit tests for AEOLUS L1B ingestion module
// (libharp/harp-ingest-aeolus_l1b.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-aeolus_l1b.c

#include <cstdint>
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

constexpr int kNumGeoBins = 25;
constexpr int kNumWindBins = 24;

CodaMemNode* MakeGeoBinArray(CodaMemProduct& prod, double lat_base,
                             double lon_base, double alt_base,
                             bool include_angles) {
  std::vector<CodaMemNode*> bins;
  bins.reserve(kNumGeoBins);
  for (int i = 0; i < kNumGeoBins; ++i) {
    CodaMemNode* bin = prod.MakeRecord();
    // Stored top-to-bottom in product.
    const double val_offset = static_cast<double>(kNumGeoBins - 1 - i);
    prod.AddField(bin, "latitude_of_height_bin",
                  prod.MakeDouble(lat_base + val_offset));
    prod.AddField(bin, "longitude_of_height_bin",
                  prod.MakeDouble(lon_base + val_offset));
    prod.AddField(bin, "altitude_of_height_bin",
                  prod.MakeDouble(alt_base + val_offset * 1000.0));
    if (include_angles) {
      prod.AddField(bin, "topocentric_azimuth_of_height_bin",
                    prod.MakeDouble(100.0 + val_offset));
      prod.AddField(bin, "topocentric_elevation_of_height_bin",
                    prod.MakeDouble(45.0 + val_offset));
    }
    bins.push_back(bin);
  }
  const long dims[] = {kNumGeoBins};
  return prod.MakeNodeArray(dims, bins);
}

CodaMemNode* MakeWindBinArray(CodaMemProduct& prod, double vel_base,
                              int32_t flag_base) {
  std::vector<CodaMemNode*> bins;
  bins.reserve(kNumWindBins);
  for (int i = 0; i < kNumWindBins; ++i) {
    CodaMemNode* bin = prod.MakeRecord();
    // Stored top-to-bottom in product.
    const int rev = kNumWindBins - 1 - i;
    prod.AddField(bin, "wind_velocity",
                  prod.MakeDouble(vel_base + static_cast<double>(rev)));
    prod.AddField(bin, "bin_quality_flag", prod.MakeInt32(flag_base + rev));
    bins.push_back(bin);
  }
  const long dims[] = {kNumWindBins};
  return prod.MakeNodeArray(dims, bins);
}

void PopulateAeolusL1bProduct(CodaMemProduct& prod, int32_t num_obs,
                              int32_t n_max, int32_t n_max_actual,
                              bool rayleigh, bool observation) {
  CodaMemNode* mph = prod.MakeRecord();
  ASSERT_TRUE(prod.AddField(mph, "abs_orbit", prod.MakeUint32(4321u)));
  ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));

  CodaMemNode* sph = prod.MakeRecord();
  ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(n_max)));
  ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(n_max_actual)));
  ASSERT_TRUE(
      prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(num_obs)));
  ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));

  std::vector<CodaMemNode*> geo_elems;
  std::vector<CodaMemNode*> wv_elems;
  for (int i = 0; i < num_obs; ++i) {
    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(geo, "start_of_observation_time",
                              prod.MakeDouble(1000.0 + i * 12.0)));

    CodaMemNode* obs_geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(obs_geo, "geoid_separation",
                              prod.MakeDouble(100.0 + i * 10.0)));
    if (observation) {
      ASSERT_TRUE(
          prod.AddField(obs_geo,
                        rayleigh ? "observation_rayleigh_geolocation"
                                 : "observation_mie_geolocation",
                        MakeGeoBinArray(prod, 10.0 + i, 20.0 + i, 1000.0,
                                        /*include_angles=*/true)));
    }
    ASSERT_TRUE(prod.AddField(geo, "observation_geolocation", obs_geo));

    if (!observation) {
      std::vector<CodaMemNode*> meas_geos;
      for (int j = 0; j < n_max_actual; ++j) {
        CodaMemNode* mg = prod.MakeRecord();
        ASSERT_TRUE(prod.AddField(
            mg, rayleigh ? "rayleigh_geolocation" : "mie_geolocation",
            MakeGeoBinArray(prod, 10.0 + i * 10 + j, 20.0 + i * 10 + j, 1000.0,
                            /*include_angles=*/false)));
        meas_geos.push_back(mg);
      }
      const long mdims[] = {n_max_actual};
      ASSERT_TRUE(prod.AddField(geo, "measurement_geolocation",
                                prod.MakeNodeArray(mdims, meas_geos)));
    }
    geo_elems.push_back(geo);

    CodaMemNode* wv = prod.MakeRecord();
    if (observation) {
      CodaMemNode* obs_wp = prod.MakeRecord();
      ASSERT_TRUE(
          prod.AddField(obs_wp,
                        rayleigh ? "rayleigh_altitude_bin_wind_info"
                                 : "mie_altitude_bin_wind_info",
                        MakeWindBinArray(prod, 5.0 + i * 100.0, i * 10)));
      ASSERT_TRUE(prod.AddField(wv, "observation_wind_profile", obs_wp));
    } else {
      std::vector<CodaMemNode*> meas_wps;
      for (int j = 0; j < n_max_actual; ++j) {
        CodaMemNode* mw = prod.MakeRecord();
        ASSERT_TRUE(prod.AddField(
            mw,
            rayleigh ? "rayleigh_altitude_bin_wind_info"
                     : "mie_altitude_bin_wind_info",
            MakeWindBinArray(prod, 5.0 + (i * n_max_actual + j) * 100.0,
                             i * 10 + j)));
        meas_wps.push_back(mw);
      }
      const long mdims[] = {n_max_actual};
      ASSERT_TRUE(prod.AddField(wv, "measurement_wind_profile",
                                prod.MakeNodeArray(mdims, meas_wps)));
    }
    wv_elems.push_back(wv);
  }

  const long obs_dims[] = {num_obs};
  ASSERT_TRUE(prod.AddField(prod.root(), "geolocation",
                            prod.MakeNodeArray(obs_dims, geo_elems)));
  ASSERT_TRUE(prod.AddField(prod.root(), "wind_velocity",
                            prod.MakeNodeArray(obs_dims, wv_elems)));
}

TEST(HarpIngestAeolusL1bTest, IngestRayleighObservationDefaultAndExplicit) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("AEOLUS_L1B");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateAeolusL1bProduct(prod, /*num_obs=*/2, /*n_max=*/4,
                           /*n_max_actual=*/2, /*rayleigh=*/true,
                           /*observation=*/true);

  for (const char* opt :
       {static_cast<const char*>(nullptr), "data=rayleigh_observation"}) {
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), opt);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(24, res.dimensions[harp_dimension_vertical]);
    EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(4321));
    EXPECT_THAT(res.variables["datetime_start"].double_values,
                ElementsAre(1000.0, 1012.0));
    EXPECT_THAT(res.variables["datetime_length"].double_values,
                ElementsAre(12.0, 12.0));
    EXPECT_THAT(res.variables["geoid_height"].double_values,
                ElementsAre(100.0, 110.0));
    // Bottom bin (index 0) averages product bins [23] and [24] -> 10.5.
    EXPECT_DOUBLE_EQ(10.5, res.variables["latitude"].double_values[0]);
    EXPECT_DOUBLE_EQ(20.5, res.variables["longitude"].double_values[0]);
    EXPECT_DOUBLE_EQ(100.5,
                     res.variables["sensor_azimuth_angle"].double_values[0]);
    EXPECT_DOUBLE_EQ(45.5,
                     res.variables["sensor_elevation_angle"].double_values[0]);
    // altitude_bounds for bin 0: [1000 - 100, 2000 - 100] = [900, 1900].
    EXPECT_DOUBLE_EQ(900.0, res.variables["altitude_bounds"].double_values[0]);
    EXPECT_DOUBLE_EQ(1900.0, res.variables["altitude_bounds"].double_values[1]);
    EXPECT_DOUBLE_EQ(5.0, res.variables["hlos_wind_velocity"].double_values[0]);
    EXPECT_EQ(0, res.variables["hlos_wind_velocity_validity"].int32_values[0]);
  }
}

TEST(HarpIngestAeolusL1bTest, IngestMieObservationAndMeasurementModes) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("AEOLUS_L1B");
  ASSERT_NE(nullptr, mod);

  // 1. Mie observation.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateAeolusL1bProduct(prod, /*num_obs=*/2, /*n_max=*/4,
                             /*n_max_actual=*/2, /*rayleigh=*/false,
                             /*observation=*/true);
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "data=mie_observation");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_DOUBLE_EQ(105.0,
                     res.variables["hlos_wind_velocity"].double_values[24]);
  }

  // 2. Rayleigh measurement.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateAeolusL1bProduct(prod, /*num_obs=*/2, /*n_max=*/4,
                             /*n_max_actual=*/2, /*rayleigh=*/true,
                             /*observation=*/false);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "data=rayleigh_measurement");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(4, res.dimensions[harp_dimension_time]);
    EXPECT_THAT(res.variables["datetime_start"].double_values,
                ElementsAre(1000.0, 1006.0, 1012.0, 1018.0));
    EXPECT_THAT(res.variables["datetime_length"].double_values,
                ElementsAre(6.0, 6.0, 6.0, 6.0));
    EXPECT_THAT(res.variables["geoid_height"].double_values,
                ElementsAre(100.0, 100.0, 110.0, 110.0));
    EXPECT_DOUBLE_EQ(105.0,
                     res.variables["hlos_wind_velocity"].double_values[24]);
  }

  // 3. Mie measurement.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateAeolusL1bProduct(prod, /*num_obs=*/1, /*n_max=*/3,
                             /*n_max_actual=*/3, /*rayleigh=*/false,
                             /*observation=*/false);
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "data=mie_measurement");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(3, res.dimensions[harp_dimension_time]);
    EXPECT_THAT(res.variables["datetime_length"].double_values,
                ElementsAre(4.0, 4.0, 4.0));
  }
}

TEST(HarpIngestAeolusL1bTest, ZeroObservationsSkipsCursors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("AEOLUS_L1B");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  CodaMemNode* sph = prod.MakeRecord();
  ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(4)));
  ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(2)));
  ASSERT_TRUE(
      prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(0)));
  ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
  CodaMemNode* mph = prod.MakeRecord();
  ASSERT_TRUE(prod.AddField(mph, "abs_orbit", prod.MakeUint32(1u)));
  ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(0, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(24, res.dimensions[harp_dimension_vertical]);
}

TEST(HarpIngestAeolusL1bTest, InitErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("AEOLUS_L1B");
  ASSERT_NE(nullptr, mod);

  // 1. Missing /sph/n_max, wrong n_max type, missing/wrong n_max_actual,
  // missing/wrong total_num_of_observations.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(1)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeString("x")));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }

  // 2. n_max_actual > n_max -> HARP_ERROR_INGESTION.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(5)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 3. Missing geolocation, geolocation length mismatch, missing wind_velocity,
  // wind_velocity length mismatch, and non-array scalar geolocation/wind.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(1)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(1)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    CodaMemNode* geo = prod.MakeRecord();
    const long dims[] = {1};
    CodaMemNode* elems[] = {geo};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation",
                              prod.MakeNodeArray(dims, elems)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 4. wind_velocity missing and length mismatch -> HARP_ERROR_INGESTION.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(1)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    const long geo_dims[] = {2};
    CodaMemNode* geo_elems[] = {prod.MakeRecord(), prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation",
                              prod.MakeNodeArray(geo_dims, geo_elems)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
    const long wv_dims[] = {1};
    CodaMemNode* wv_elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "wind_velocity",
                              prod.MakeNodeArray(wv_dims, wv_elems)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }
  {
    // Scalar geolocation (num_elements == 1 == num_obs) fails
    // goto_first_array_element.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(1)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "geolocation", prod.MakeString("scalar")));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "wind_velocity", prod.MakeString("scalar")));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    // Array geolocation[1], scalar wind_velocity -> wind_velocity
    // goto_first_array_element fails.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(1)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    const long dims[] = {1};
    CodaMemNode* geo_elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation",
                              prod.MakeNodeArray(dims, geo_elems)));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "wind_velocity", prod.MakeString("scalar")));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }

  // 5. Missing/wrong start_of_observation_time, observation/measurement inner
  // fields, and short measurement arrays.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(2)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    const long dims[] = {1};
    CodaMemNode* geo_elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation",
                              prod.MakeNodeArray(dims, geo_elems)));
    CodaMemNode* wv_elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "wind_velocity",
                              prod.MakeNodeArray(dims, wv_elems)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(2)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(geo, "start_of_observation_time",
                              prod.MakeString("bad")));
    const long dims[] = {1};
    CodaMemNode* geo_elems[] = {geo};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation",
                              prod.MakeNodeArray(dims, geo_elems)));
    CodaMemNode* wv_elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "wind_velocity",
                              prod.MakeNodeArray(dims, wv_elems)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(2)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(geo, "start_of_observation_time", prod.MakeDouble(1.0)));
    ASSERT_TRUE(
        prod.AddField(geo, "observation_geolocation", prod.MakeRecord()));
    ASSERT_TRUE(
        prod.AddField(geo, "measurement_geolocation", prod.MakeString("x")));
    const long dims[] = {1};
    CodaMemNode* geo_elems[] = {geo};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation",
                              prod.MakeNodeArray(dims, geo_elems)));
    CodaMemNode* wv_elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "wind_velocity",
                              prod.MakeNodeArray(dims, wv_elems)));

    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product(),
                                          "data=rayleigh_measurement")
                      .status);
  }
  {
    // Observation mode: missing observation_wind_profile and missing
    // rayleigh_altitude_bin_wind_info.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(1)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(geo, "start_of_observation_time", prod.MakeDouble(1.0)));
    CodaMemNode* obs_geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(obs_geo, "observation_rayleigh_geolocation",
                              prod.MakeRecord()));
    ASSERT_TRUE(prod.AddField(geo, "observation_geolocation", obs_geo));
    const long dims[] = {1};
    CodaMemNode* geo_elems[] = {geo};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation",
                              prod.MakeNodeArray(dims, geo_elems)));
    CodaMemNode* wv = prod.MakeRecord();
    CodaMemNode* wv_elems[] = {wv};
    ASSERT_TRUE(prod.AddField(prod.root(), "wind_velocity",
                              prod.MakeNodeArray(dims, wv_elems)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);

    ASSERT_TRUE(
        prod.AddField(wv, "observation_wind_profile", prod.MakeRecord()));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }
  {
    // Measurement mode: missing measurement_wind_profile, scalar
    // measurement_wind_profile, missing rayleigh_geolocation, missing
    // rayleigh_altitude_bin_wind_info, and short measurement arrays (<
    // n_max_actual).
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(2)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(geo, "start_of_observation_time", prod.MakeDouble(1.0)));
    const long dims1[] = {1};
    CodaMemNode* mg0 = prod.MakeRecord();
    CodaMemNode* mg_elems[] = {mg0};
    ASSERT_TRUE(prod.AddField(geo, "measurement_geolocation",
                              prod.MakeNodeArray(dims1, mg_elems)));
    CodaMemNode* geo_elems[] = {geo};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation",
                              prod.MakeNodeArray(dims1, geo_elems)));
    CodaMemNode* wv = prod.MakeRecord();
    CodaMemNode* wv_elems[] = {wv};
    ASSERT_TRUE(prod.AddField(prod.root(), "wind_velocity",
                              prod.MakeNodeArray(dims1, wv_elems)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product(),
                                          "data=rayleigh_measurement")
                      .status);

    ASSERT_TRUE(
        prod.AddField(wv, "measurement_wind_profile", prod.MakeString("bad")));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product(),
                                          "data=rayleigh_measurement")
                      .status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(2)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(geo, "start_of_observation_time", prod.MakeDouble(1.0)));
    const long dims1[] = {1};
    CodaMemNode* mg0 = prod.MakeRecord();
    CodaMemNode* mg_elems[] = {mg0};
    ASSERT_TRUE(prod.AddField(geo, "measurement_geolocation",
                              prod.MakeNodeArray(dims1, mg_elems)));
    CodaMemNode* geo_elems[] = {geo};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation",
                              prod.MakeNodeArray(dims1, geo_elems)));
    CodaMemNode* wv = prod.MakeRecord();
    CodaMemNode* mw0 = prod.MakeRecord();
    CodaMemNode* mw_elems[] = {mw0};
    ASSERT_TRUE(prod.AddField(wv, "measurement_wind_profile",
                              prod.MakeNodeArray(dims1, mw_elems)));
    CodaMemNode* wv_elems[] = {wv};
    ASSERT_TRUE(prod.AddField(prod.root(), "wind_velocity",
                              prod.MakeNodeArray(dims1, wv_elems)));
    // Missing rayleigh_geolocation.
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product(),
                                          "data=rayleigh_measurement")
                      .status);

    // Add rayleigh_geolocation, missing rayleigh_altitude_bin_wind_info.
    ASSERT_TRUE(prod.AddField(mg0, "rayleigh_geolocation", prod.MakeRecord()));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product(),
                                          "data=rayleigh_measurement")
                      .status);

    // Add rayleigh_altitude_bin_wind_info, triggers measurement_geolocation
    // goto_next_array_element failure (len 1 < n_max_actual 2).
    ASSERT_TRUE(prod.AddField(mw0, "rayleigh_altitude_bin_wind_info",
                              prod.MakeRecord()));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product(),
                                          "data=rayleigh_measurement")
                      .status);
  }
  {
    // measurement_geolocation has 2 elements, measurement_wind_profile has 1.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(2)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(2)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(geo, "start_of_observation_time", prod.MakeDouble(1.0)));
    CodaMemNode* mg0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mg0, "rayleigh_geolocation", prod.MakeRecord()));
    CodaMemNode* mg1 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mg1, "rayleigh_geolocation", prod.MakeRecord()));
    const long dims2[] = {2};
    CodaMemNode* mg_elems[] = {mg0, mg1};
    ASSERT_TRUE(prod.AddField(geo, "measurement_geolocation",
                              prod.MakeNodeArray(dims2, mg_elems)));
    const long dims1[] = {1};
    CodaMemNode* geo_elems[] = {geo};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation",
                              prod.MakeNodeArray(dims1, geo_elems)));
    CodaMemNode* wv = prod.MakeRecord();
    CodaMemNode* mw0 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mw0, "rayleigh_altitude_bin_wind_info",
                              prod.MakeRecord()));
    CodaMemNode* mw_elems[] = {mw0};
    ASSERT_TRUE(prod.AddField(wv, "measurement_wind_profile",
                              prod.MakeNodeArray(dims1, mw_elems)));
    CodaMemNode* wv_elems[] = {wv};
    ASSERT_TRUE(prod.AddField(prod.root(), "wind_velocity",
                              prod.MakeNodeArray(dims1, wv_elems)));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product(),
                                          "data=rayleigh_measurement")
                      .status);
  }
}

TEST(HarpIngestAeolusL1bTest, VariableReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("AEOLUS_L1B");
  ASSERT_NE(nullptr, mod);

  // 1. Short arrays with valid element 0 fields (hits goto_next_array_element
  // failure in average/bounds/double/int32 helpers), wrong types for
  // abs_orbit and geoid_separation, and read_altitude_bounds failure after
  // geoid_separation succeeds.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* mph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mph, "abs_orbit", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));

    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(1)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));

    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(geo, "start_of_observation_time", prod.MakeDouble(10.0)));
    CodaMemNode* obs_geo = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(obs_geo, "geoid_separation", prod.MakeDouble(12.0)));
    const long short_dims[] = {1};
    CodaMemNode* gbin = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(gbin, "latitude_of_height_bin", prod.MakeDouble(1.0)));
    ASSERT_TRUE(
        prod.AddField(gbin, "longitude_of_height_bin", prod.MakeString("bad")));
    ASSERT_TRUE(
        prod.AddField(gbin, "altitude_of_height_bin", prod.MakeDouble(100.0)));
    CodaMemNode* short_geo_bin[] = {gbin};
    ASSERT_TRUE(prod.AddField(obs_geo, "observation_rayleigh_geolocation",
                              prod.MakeNodeArray(short_dims, short_geo_bin)));
    ASSERT_TRUE(prod.AddField(geo, "observation_geolocation", obs_geo));
    CodaMemNode* geo_elems[] = {geo};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation",
                              prod.MakeNodeArray(short_dims, geo_elems)));

    CodaMemNode* wv = prod.MakeRecord();
    CodaMemNode* obs_wp = prod.MakeRecord();
    CodaMemNode* wbin = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(wbin, "wind_velocity", prod.MakeDouble(5.0)));
    ASSERT_TRUE(prod.AddField(wbin, "bin_quality_flag", prod.MakeInt32(1)));
    CodaMemNode* short_wv_bin[] = {wbin};
    ASSERT_TRUE(prod.AddField(obs_wp, "rayleigh_altitude_bin_wind_info",
                              prod.MakeNodeArray(short_dims, short_wv_bin)));
    ASSERT_TRUE(prod.AddField(wv, "observation_wind_profile", obs_wp));
    CodaMemNode* wv_elems[] = {wv};
    ASSERT_TRUE(prod.AddField(prod.root(), "wind_velocity",
                              prod.MakeNodeArray(short_dims, wv_elems)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["altitude_bounds"].read_status);
    EXPECT_EQ(HARP_SUCCESS, res.variables["geoid_height"].read_status);
    EXPECT_EQ(-1, res.variables["sensor_azimuth_angle"].read_status);
    EXPECT_EQ(-1, res.variables["hlos_wind_velocity"].read_status);
    EXPECT_EQ(-1, res.variables["hlos_wind_velocity_validity"].read_status);
  }

  // 2. Non-array scalar geo/wind bin fields (hits goto_first_array_element
  // failure) and wrong types for altitude/wind/quality fields.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(1)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));

    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(geo, "start_of_observation_time", prod.MakeDouble(10.0)));
    CodaMemNode* obs_geo = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(obs_geo, "geoid_separation", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(obs_geo, "observation_rayleigh_geolocation",
                              prod.MakeString("not_array")));
    ASSERT_TRUE(prod.AddField(geo, "observation_geolocation", obs_geo));
    const long dims1[] = {1};
    CodaMemNode* geo_elems[] = {geo};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation",
                              prod.MakeNodeArray(dims1, geo_elems)));

    CodaMemNode* wv = prod.MakeRecord();
    CodaMemNode* obs_wp = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(obs_wp, "rayleigh_altitude_bin_wind_info",
                              prod.MakeString("not_array")));
    ASSERT_TRUE(prod.AddField(wv, "observation_wind_profile", obs_wp));
    CodaMemNode* wv_elems[] = {wv};
    ASSERT_TRUE(prod.AddField(prod.root(), "wind_velocity",
                              prod.MakeNodeArray(dims1, wv_elems)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["geoid_height"].read_status);
    EXPECT_EQ(-1, res.variables["hlos_wind_velocity"].read_status);
    EXPECT_EQ(-1, res.variables["hlos_wind_velocity_validity"].read_status);
  }

  // 3. Wrong types on scalar bin element 0 + missing observation_geolocation
  // in measurement mode + scalar observation_rayleigh_geolocation for bounds.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(1)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));

    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(geo, "start_of_observation_time", prod.MakeDouble(10.0)));
    CodaMemNode* obs_geo = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(obs_geo, "geoid_separation", prod.MakeDouble(5.0)));
    const long dims1[] = {1};
    CodaMemNode* gbin = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(gbin, "altitude_of_height_bin", prod.MakeString("bad")));
    CodaMemNode* gbins[] = {gbin};
    ASSERT_TRUE(prod.AddField(obs_geo, "observation_rayleigh_geolocation",
                              prod.MakeNodeArray(dims1, gbins)));
    ASSERT_TRUE(prod.AddField(geo, "observation_geolocation", obs_geo));
    CodaMemNode* geo_elems[] = {geo};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation",
                              prod.MakeNodeArray(dims1, geo_elems)));

    CodaMemNode* wv = prod.MakeRecord();
    CodaMemNode* obs_wp = prod.MakeRecord();
    CodaMemNode* wbin = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(wbin, "wind_velocity", prod.MakeString("bad")));
    ASSERT_TRUE(
        prod.AddField(wbin, "bin_quality_flag", prod.MakeString("bad")));
    CodaMemNode* wbins[] = {wbin};
    ASSERT_TRUE(prod.AddField(obs_wp, "rayleigh_altitude_bin_wind_info",
                              prod.MakeNodeArray(dims1, wbins)));
    ASSERT_TRUE(prod.AddField(wv, "observation_wind_profile", obs_wp));
    CodaMemNode* wv_elems[] = {wv};
    ASSERT_TRUE(prod.AddField(prod.root(), "wind_velocity",
                              prod.MakeNodeArray(dims1, wv_elems)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["altitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["hlos_wind_velocity"].read_status);
    EXPECT_EQ(-1, res.variables["hlos_wind_velocity_validity"].read_status);
  }
  {
    // Scalar bounds array after geoid_separation succeeds + measurement mode
    // missing observation_geolocation.
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(sph, "n_max", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(sph, "n_max_actual", prod.MakeInt32(1)));
    ASSERT_TRUE(
        prod.AddField(sph, "total_num_of_observations", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));

    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(geo, "start_of_observation_time", prod.MakeDouble(10.0)));
    CodaMemNode* mg = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(mg, "rayleigh_geolocation", prod.MakeString("scalar")));
    const long dims1[] = {1};
    CodaMemNode* mg_elems[] = {mg};
    ASSERT_TRUE(prod.AddField(geo, "measurement_geolocation",
                              prod.MakeNodeArray(dims1, mg_elems)));
    CodaMemNode* geo_elems[] = {geo};
    ASSERT_TRUE(prod.AddField(prod.root(), "geolocation",
                              prod.MakeNodeArray(dims1, geo_elems)));

    CodaMemNode* wv = prod.MakeRecord();
    CodaMemNode* mw = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mw, "rayleigh_altitude_bin_wind_info",
                              prod.MakeString("scalar")));
    CodaMemNode* mw_elems[] = {mw};
    ASSERT_TRUE(prod.AddField(wv, "measurement_wind_profile",
                              prod.MakeNodeArray(dims1, mw_elems)));
    CodaMemNode* wv_elems[] = {wv};
    ASSERT_TRUE(prod.AddField(prod.root(), "wind_velocity",
                              prod.MakeNodeArray(dims1, wv_elems)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "data=rayleigh_measurement",
        /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["geoid_height"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
