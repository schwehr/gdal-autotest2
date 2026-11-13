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
// Unit tests for GOMOS Level 2 ingestion (libharp/harp-ingest-gomos_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-gomos_l2.c

#include <cmath>
#include <cstdint>
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

CodaMemNode* MakeSingleNodeArray(CodaMemProduct& prod, CodaMemNode* elem) {
  constexpr long kOne[] = {1};
  CodaMemNode* elems[] = {elem};
  return prod.MakeNodeArray(kOne, elems);
}

struct ProfilePoint {
  double dsr_time;
  double tangent_alt;
  double tangent_lat;
  double tangent_long;
  double tangent_atm_p;
  double local_temp;
  double tangent_temp;
  double local_temp_std;
  double tangent_density;
  double sensor_alt;
  double sensor_lat;
  double sensor_lon;
  double o3;
  double o3_std;
  double no2;
  double no2_std;
  double no3;
  double no3_std;
  double air;
  double air_std;
  double o2;
  double o2_std;
  double h2o;
  double h2o_std;
  double oclo;
  double oclo_std;
  int16_t pcd[7];
  double local_ext;
  double local_ext_std;
};

void PopulateGomosL2(CodaMemProduct& prod, int version, int32_t abs_orbit,
                     int32_t illum_cond,
                     const std::vector<ProfilePoint>& points) {
  ASSERT_TRUE(prod.SetProductInfo("ENVISAT_GOMOS", "GOM_NL__2P", version));

  CodaMemNode* mph = prod.MakeRecord();
  ASSERT_NE(nullptr, mph);
  ASSERT_TRUE(prod.AddField(mph, "abs_orbit", prod.MakeInt32(abs_orbit)));
  ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));

  std::vector<CodaMemNode*> geo_nodes;
  std::vector<CodaMemNode*> dens_nodes;
  std::vector<CodaMemNode*> aero_nodes;
  const long seven_dims[] = {7};
  for (const auto& pt : points) {
    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(geo, "dsr_time", prod.MakeDouble(pt.dsr_time)));
    ASSERT_TRUE(
        prod.AddField(geo, "tangent_alt", prod.MakeDouble(pt.tangent_alt)));
    ASSERT_TRUE(
        prod.AddField(geo, "tangent_lat", prod.MakeDouble(pt.tangent_lat)));
    ASSERT_TRUE(
        prod.AddField(geo, "tangent_long", prod.MakeDouble(pt.tangent_long)));
    ASSERT_TRUE(
        prod.AddField(geo, "tangent_atm_p", prod.MakeDouble(pt.tangent_atm_p)));
    ASSERT_TRUE(
        prod.AddField(geo, "local_temp", prod.MakeDouble(pt.local_temp)));
    ASSERT_TRUE(
        prod.AddField(geo, "tangent_temp", prod.MakeDouble(pt.tangent_temp)));
    ASSERT_TRUE(prod.AddField(geo, "local_temp_std",
                              prod.MakeDouble(pt.local_temp_std)));
    ASSERT_TRUE(prod.AddField(geo, "tangent_density",
                              prod.MakeDouble(pt.tangent_density)));
    ASSERT_TRUE(prod.AddField(geo, "alt", prod.MakeDouble(pt.sensor_alt)));
    ASSERT_TRUE(prod.AddField(geo, "lat", prod.MakeDouble(pt.sensor_lat)));
    ASSERT_TRUE(prod.AddField(geo, "longit", prod.MakeDouble(pt.sensor_lon)));
    geo_nodes.push_back(geo);

    CodaMemNode* dens = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(dens, "o3", prod.MakeDouble(pt.o3)));
    ASSERT_TRUE(prod.AddField(dens, "o3_std", prod.MakeDouble(pt.o3_std)));
    ASSERT_TRUE(prod.AddField(dens, "no2", prod.MakeDouble(pt.no2)));
    ASSERT_TRUE(prod.AddField(dens, "no2_std", prod.MakeDouble(pt.no2_std)));
    ASSERT_TRUE(prod.AddField(dens, "no3", prod.MakeDouble(pt.no3)));
    ASSERT_TRUE(prod.AddField(dens, "no3_std", prod.MakeDouble(pt.no3_std)));
    ASSERT_TRUE(prod.AddField(dens, "air", prod.MakeDouble(pt.air)));
    ASSERT_TRUE(prod.AddField(dens, "air_std", prod.MakeDouble(pt.air_std)));
    ASSERT_TRUE(prod.AddField(dens, "o2", prod.MakeDouble(pt.o2)));
    ASSERT_TRUE(prod.AddField(dens, "o2_std", prod.MakeDouble(pt.o2_std)));
    ASSERT_TRUE(prod.AddField(dens, "h2o", prod.MakeDouble(pt.h2o)));
    ASSERT_TRUE(prod.AddField(dens, "h2o_std", prod.MakeDouble(pt.h2o_std)));
    ASSERT_TRUE(prod.AddField(dens, "oclo", prod.MakeDouble(pt.oclo)));
    ASSERT_TRUE(prod.AddField(dens, "oclo_std", prod.MakeDouble(pt.oclo_std)));
    ASSERT_TRUE(
        prod.AddField(dens, "pcd", prod.MakeInt16Array(seven_dims, pt.pcd)));
    dens_nodes.push_back(dens);

    CodaMemNode* aero = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(aero, "local_ext", prod.MakeDouble(pt.local_ext)));
    ASSERT_TRUE(prod.AddField(aero, "local_ext_std",
                              prod.MakeDouble(pt.local_ext_std)));
    aero_nodes.push_back(aero);
  }
  const long n_dims[] = {static_cast<long>(points.size())};
  ASSERT_TRUE(prod.AddField(prod.root(), "nl_geolocation",
                            prod.MakeNodeArray(n_dims, geo_nodes)));
  ASSERT_TRUE(prod.AddField(prod.root(), "nl_local_species_density",
                            prod.MakeNodeArray(n_dims, dens_nodes)));
  ASSERT_TRUE(prod.AddField(prod.root(), "nl_aerosols",
                            prod.MakeNodeArray(n_dims, aero_nodes)));

  CodaMemNode* qual = prod.MakeRecord();
  ASSERT_TRUE(prod.AddField(qual, version == 0 ? "limb_flag" : "obs_illum_cond",
                            prod.MakeInt32(illum_cond)));
  ASSERT_TRUE(prod.AddField(prod.root(), "nl_summary_quality",
                            MakeSingleNodeArray(prod, qual)));
}

TEST(HarpIngestGomosL2Test, IngestVersion0DefaultAndAirModelExcluded) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOMOS_L2");
  ASSERT_NE(nullptr, mod);

  std::vector<ProfilePoint> pts = {
      {
          .dsr_time = 100.0,
          .tangent_alt = 50000.0,
          .tangent_lat = 10.0,
          .tangent_long = 20.0,
          .tangent_atm_p = 80.0,
          .local_temp = 260.0,
          .tangent_temp = 265.0,
          .local_temp_std = 10.0,
          .tangent_density = 1e16,
          .sensor_alt = 800000.0,
          .sensor_lat = 12.0,
          .sensor_lon = 22.0,
          .o3 = 1e12,
          .o3_std = 6553.5,
          .no2 = 2e9,
          .no2_std = 50.0,
          .no3 = 3e7,
          .no3_std = 10.0,
          .air = 4e16,
          .air_std = 5.0,
          .o2 = 8e15,
          .o2_std = 20.0,
          .h2o = 5e11,
          .h2o_std = 10.0,
          .oclo = 6e6,
          .oclo_std = 25.0,
          .pcd = {0, 1, 2, 3, 4, 5, 6},
          .local_ext = 0.02,
          .local_ext_std = 6553.5,
      },
      {
          .dsr_time = 102.0,
          .tangent_alt = 40000.0,
          .tangent_lat = 10.5,
          .tangent_long = 20.5,
          .tangent_atm_p = 200.0,
          .local_temp = 250.0,
          .tangent_temp = 255.0,
          .local_temp_std = 6553.5,
          .tangent_density = 2e16,
          .sensor_alt = 800100.0,
          .sensor_lat = 12.5,
          .sensor_lon = 22.5,
          .o3 = 2e12,
          .o3_std = 10.0,
          .no2 = 4e9,
          .no2_std = 25.0,
          .no3 = 6e7,
          .no3_std = 10.0,
          .air = 8e16,
          .air_std = 10.0,
          .o2 = 1.6e16,
          .o2_std = 10.0,
          .h2o = 1e12,
          .h2o_std = 20.0,
          .oclo = 1.2e7,
          .oclo_std = 50.0,
          .pcd = {10, 11, 12, 13, 14, 15, 16},
          .local_ext = 0.04,
          .local_ext_std = 50.0,
      },
  };

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateGomosL2(prod, /*version=*/0, /*abs_orbit=*/12345, /*illum_cond=*/3,
                  pts);

  // Default options on version 0.
  {
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(1, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(2, res.dimensions[harp_dimension_vertical]);
    EXPECT_THAT(res.variables["datetime_start"].double_values,
                ElementsAre(100.0));
    EXPECT_THAT(res.variables["datetime"].double_values, ElementsAre(102.0));
    EXPECT_THAT(res.variables["datetime_stop"].double_values,
                ElementsAre(102.0));
    EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(12345));
    // Vertical profile is reversed (index 1 then index 0).
    EXPECT_THAT(res.variables["altitude"].double_values,
                ElementsAre(40000.0, 50000.0));
    EXPECT_THAT(res.variables["latitude"].double_values,
                ElementsAre(10.5, 10.0));
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(20.5, 20.0));
    EXPECT_THAT(res.variables["O3_number_density"].double_values,
                ElementsAre(2e12, 1e12));
    EXPECT_THAT(res.variables["O3_number_density_uncertainty"].double_values[0],
                DoubleNear(2e11, 1e3));
    EXPECT_TRUE(std::isnan(
        res.variables["O3_number_density_uncertainty"].double_values[1]));
    EXPECT_THAT(res.variables["O3_number_density_validity"].int16_values,
                ElementsAre(10, 0));
    EXPECT_THAT(res.variables["NO2_number_density"].double_values,
                ElementsAre(4e9, 2e9));
    EXPECT_THAT(res.variables["NO2_number_density_validity"].int16_values,
                ElementsAre(11, 1));
    EXPECT_THAT(res.variables["NO3_number_density"].double_values,
                ElementsAre(6e7, 3e7));
    EXPECT_THAT(res.variables["NO3_number_density_validity"].int16_values,
                ElementsAre(12, 2));
    EXPECT_THAT(res.variables["O2_number_density"].double_values,
                ElementsAre(1.6e16, 8e15));
    EXPECT_THAT(res.variables["O2_number_density_validity"].int16_values,
                ElementsAre(14, 4));
    EXPECT_THAT(res.variables["H2O_number_density"].double_values,
                ElementsAre(1e12, 5e11));
    EXPECT_THAT(res.variables["H2O_number_density_validity"].int16_values,
                ElementsAre(15, 5));
    EXPECT_THAT(res.variables["OClO_number_density"].double_values,
                ElementsAre(1.2e7, 6e6));
    EXPECT_THAT(res.variables["OClO_number_density_validity"].int16_values,
                ElementsAre(16, 6));
    EXPECT_THAT(res.variables["aerosol_extinction_coefficient"].double_values,
                ElementsAre(0.04, 0.02));
    EXPECT_THAT(res.variables["aerosol_extinction_coefficient_uncertainty"]
                    .double_values[0],
                DoubleNear(0.02, 1e-12));
    EXPECT_TRUE(
        std::isnan(res.variables["aerosol_extinction_coefficient_uncertainty"]
                       .double_values[1]));
    EXPECT_THAT(res.variables["pressure"].double_values,
                ElementsAre(200.0, 80.0));
    EXPECT_THAT(res.variables["temperature"].double_values,
                ElementsAre(250.0, 260.0));
    EXPECT_TRUE(
        std::isnan(res.variables["temperature_uncertainty"].double_values[0]));
    EXPECT_THAT(res.variables["temperature_uncertainty"].double_values[1],
                DoubleNear(26.0, 1e-12));
    EXPECT_THAT(res.variables["number_density"].double_values,
                ElementsAre(8e16, 4e16));
    EXPECT_THAT(res.variables["number_density_validity"].int16_values,
                ElementsAre(13, 3));
    EXPECT_THAT(res.variables["sensor_altitude"].double_values,
                ElementsAre(800100.0, 800000.0));
    EXPECT_THAT(res.variables["sensor_latitude"].double_values,
                ElementsAre(12.5, 12.0));
    EXPECT_THAT(res.variables["sensor_longitude"].double_values,
                ElementsAre(22.5, 22.0));
    EXPECT_THAT(res.variables["scene_type"].int8_values, ElementsAre(3));
  }

  // On version 0, air=model excludes number_density, uncertainty, validity.
  {
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "air=model");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(res.variables["number_density"].included);
    EXPECT_FALSE(res.variables["number_density_uncertainty"].included);
    EXPECT_FALSE(res.variables["number_density_validity"].included);
  }
}

TEST(HarpIngestGomosL2Test, IngestVersion1ModelOptionsAndVersion2AbsError) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOMOS_L2");
  ASSERT_NE(nullptr, mod);

  std::vector<ProfilePoint> pts = {
      {
          .dsr_time = 200.0,
          .tangent_alt = 30000.0,
          .tangent_lat = 5.0,
          .tangent_long = 6.0,
          .tangent_atm_p = 100.0,
          .local_temp = 240.0,
          .tangent_temp = 245.0,
          .local_temp_std = 5.0,
          .tangent_density = 5e16,
          .sensor_alt = 800000.0,
          .sensor_lat = 5.0,
          .sensor_lon = 6.0,
          .o3 = 1e12,
          .o3_std = 400.0,
          .no2 = 2e9,
          .no2_std = 6554.0,
          .no3 = 3e7,
          .no3_std = 200.0,
          .air = 4e16,
          .air_std = 600.0,
          .o2 = 8e15,
          .o2_std = 200.0,
          .h2o = 5e11,
          .h2o_std = 200.0,
          .oclo = 6e6,
          .oclo_std = 200.0,
          .pcd = {0, 0, 0, 0, 0, 0, 0},
          .local_ext = 0.05,
          .local_ext_std = 20.0,
      },
  };

  // Version 1 with temperature=model and air=model.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGomosL2(prod, /*version=*/1, /*abs_orbit=*/222, /*illum_cond=*/1,
                    pts);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "temperature=model;air=model");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_TRUE(res.variables["number_density"].included);
    EXPECT_FALSE(res.variables["number_density_uncertainty"].included);
    EXPECT_FALSE(res.variables["number_density_validity"].included);
    EXPECT_FALSE(res.variables["temperature_uncertainty"].included);
    EXPECT_THAT(res.variables["temperature"].double_values, ElementsAre(245.0));
    EXPECT_THAT(res.variables["number_density"].double_values,
                ElementsAre(5e16));
    EXPECT_THAT(res.variables["scene_type"].int8_values, ElementsAre(1));
  }

  // Version 2 uses exponent scaling for nl_local_species_density (6554 -> NaN)
  // while keeping relative scaling for nl_aerosols / nl_geolocation.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateGomosL2(prod, /*version=*/2, /*abs_orbit=*/333, /*illum_cond=*/4,
                    pts);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_THAT(res.variables["O3_number_density_uncertainty"].double_values[0],
                DoubleNear(100.0, 1e-12));
    EXPECT_TRUE(std::isnan(
        res.variables["NO2_number_density_uncertainty"].double_values[0]));
    EXPECT_THAT(res.variables["aerosol_extinction_coefficient_uncertainty"]
                    .double_values[0],
                DoubleNear(0.01, 1e-12));
    EXPECT_THAT(res.variables["scene_type"].int8_values, ElementsAre(4));
  }
}

TEST(HarpIngestGomosL2Test, ErrorPathsMissingAndInvalidFields) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOMOS_L2");
  ASSERT_NE(nullptr, mod);

  // 1. Missing nl_geolocation fails init_dimensions.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_GOMOS", "GOM_NL__2P", 2));
    EXPECT_EQ(-1, ExerciseIngestionModule(mod, prod.product()).status);
  }

  // 2. Empty nl_geolocation record fails profile/point readers.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_GOMOS", "GOM_NL__2P", 0));
    ASSERT_TRUE(prod.AddField(prod.root(), "nl_geolocation",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["altitude"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density_validity"].read_status);
    EXPECT_EQ(-1, res.variables["scene_type"].read_status);
  }

  // 3. Wrong types and empty arrays for geolocation, species, pcd, quality.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_GOMOS", "GOM_NL__2P", 1));
    const long one_dim[] = {1};
    const long zero_dims[] = {0};

    CodaMemNode* mph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mph, "abs_orbit", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));

    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(geo, "dsr_time", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(geo, "tangent_alt", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "nl_geolocation",
                              MakeSingleNodeArray(prod, geo)));

    CodaMemNode* dens = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(dens, "o3", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(dens, "o3_std", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(dens, "no2", prod.MakeDouble(1.0)));
    ASSERT_TRUE(prod.AddField(dens, "no2_std", prod.MakeDouble(10.0)));
    const std::string_view bad_pcd[] = {"bad"};
    ASSERT_TRUE(
        prod.AddField(dens, "pcd", prod.MakeStringArray(one_dim, bad_pcd)));
    ASSERT_TRUE(prod.AddField(prod.root(), "nl_local_species_density",
                              MakeSingleNodeArray(prod, dens)));

    ASSERT_TRUE(
        prod.AddField(prod.root(), "nl_summary_quality",
                      prod.MakeNodeArray(zero_dims, {}, prod.MakeRecord())));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["altitude"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["O3_number_density_validity"].read_status);
    EXPECT_EQ(-1, res.variables["NO2_number_density_validity"].read_status);
    EXPECT_EQ(-1, res.variables["scene_type"].read_status);
  }

  // 4. Version 0/1 get_std_profile fails when fieldname is wrong type after
  // valid std_fieldname, and obs_illum_cond wrong type fails read_illumination.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_GOMOS", "GOM_NL__2P", 1));
    ASSERT_TRUE(prod.AddField(prod.root(), "nl_geolocation",
                              MakeSingleNodeArray(prod, prod.MakeRecord())));
    CodaMemNode* dens = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(dens, "o3", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(dens, "o3_std", prod.MakeDouble(10.0)));
    ASSERT_TRUE(prod.AddField(prod.root(), "nl_local_species_density",
                              MakeSingleNodeArray(prod, dens)));
    CodaMemNode* qual = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(qual, "obs_illum_cond", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "nl_summary_quality",
                              MakeSingleNodeArray(prod, qual)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["O3_number_density_uncertainty"].read_status);
    EXPECT_EQ(-1, res.variables["scene_type"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
