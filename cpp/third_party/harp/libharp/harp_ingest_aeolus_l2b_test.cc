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
// Unit tests for AEOLUS L2B ingestion module
// (libharp/harp-ingest-aeolus_l2b.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-aeolus_l2b.c

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

using ::testing::ElementsAre;

constexpr int kNumResultIds = 24;

void PopulateAeolusL2bProduct(CodaMemProduct& prod, bool rayleigh) {
  CodaMemNode* mph = prod.MakeRecord();
  ASSERT_TRUE(prod.AddField(mph, "abs_orbit", prod.MakeUint32(5555u)));
  ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));

  // Build 2 profiles. Profile 0 references wind results 1 and 2 (stored at
  // top-to-bottom indices 23 and 22 so bottom-up order is 1, 2, then 22 zeros).
  // Profile 1 references wind results 3 and 2.
  std::vector<CodaMemNode*> profiles;
  for (int p = 0; p < 2; ++p) {
    CodaMemNode* prof = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prof, "profile_datetime_average",
                              prod.MakeDouble(3000.0 + p * 12.0)));
    ASSERT_TRUE(prod.AddField(prof, "profile_datetime_start",
                              prod.MakeDouble(2994.0 + p * 12.0)));
    ASSERT_TRUE(prod.AddField(prof, "profile_datetime_stop",
                              prod.MakeDouble(3006.0 + p * 12.0)));
    ASSERT_TRUE(
        prod.AddField(prof, "profile_lat_average", prod.MakeDouble(40.0 + p)));
    ASSERT_TRUE(
        prod.AddField(prof, "profile_lon_average", prod.MakeDouble(-10.0 + p)));

    uint32_t ids[kNumResultIds] = {0};
    ids[23] = (p == 0) ? 1u : 3u;
    ids[22] = 2u;
    constexpr long id_dims[] = {kNumResultIds};
    CodaMemNode* l2b_wp = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(l2b_wp, "wind_result_id_number",
                              prod.MakeUint32Array(id_dims, ids)));
    ASSERT_TRUE(prod.AddField(prof, "l2b_wind_profiles", l2b_wp));
    profiles.push_back(prof);
  }
  const long prof_dims[] = {2};
  ASSERT_TRUE(prod.AddField(prod.root(),
                            rayleigh ? "rayleigh_profile" : "mie_profile",
                            prod.MakeNodeArray(prof_dims, profiles)));

  // Build 3 hloswind and 3 geolocation records (1-based IDs 1..3).
  std::vector<CodaMemNode*> hloswinds;
  std::vector<CodaMemNode*> geos;
  for (int r = 0; r < 3; ++r) {
    CodaMemNode* hw = prod.MakeRecord();
    CodaMemNode* wr = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(
        wr, rayleigh ? "rayleigh_wind_velocity" : "mie_wind_velocity",
        prod.MakeDouble(100.0 + r * 50.0)));
    ASSERT_TRUE(prod.AddField(wr, "validity_flag",
                              prod.MakeUint8(static_cast<uint8_t>(r + 1))));
    ASSERT_TRUE(prod.AddField(hw, "windresult", wr));
    hloswinds.push_back(hw);

    CodaMemNode* g = prod.MakeRecord();
    CodaMemNode* wg = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(wg, "altitude_vcog",
                              prod.MakeDouble(1500.0 + r * 500.0)));
    ASSERT_TRUE(prod.AddField(wg, "altitude_bottom",
                              prod.MakeDouble(1000.0 + r * 500.0)));
    ASSERT_TRUE(
        prod.AddField(wg, "altitude_top", prod.MakeDouble(2000.0 + r * 500.0)));
    ASSERT_TRUE(
        prod.AddField(wg, "los_azimuth", prod.MakeDouble(90.0 + r * 5.0)));
    ASSERT_TRUE(prod.AddField(wg, "los_elevation_vcog",
                              prod.MakeDouble(35.0 + r * 2.0)));
    ASSERT_TRUE(prod.AddField(g, "windresult_geolocation", wg));
    geos.push_back(g);
  }
  const long res_dims[] = {3};
  ASSERT_TRUE(prod.AddField(prod.root(),
                            rayleigh ? "rayleigh_hloswind" : "mie_hloswind",
                            prod.MakeNodeArray(res_dims, hloswinds)));
  ASSERT_TRUE(prod.AddField(
      prod.root(), rayleigh ? "rayleigh_geolocation" : "mie_geolocation",
      prod.MakeNodeArray(res_dims, geos)));
}

TEST(HarpIngestAeolusL2bTest, IngestRayleighDefaultAndExplicit) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("AEOLUS_L2B");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateAeolusL2bProduct(prod, /*rayleigh=*/true);

  for (const char* opt : {static_cast<const char*>(nullptr), "data=rayleigh"}) {
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), opt);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(24, res.dimensions[harp_dimension_vertical]);
    EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(5555));
    EXPECT_THAT(res.variables["datetime"].double_values,
                ElementsAre(3000.0, 3012.0));
    EXPECT_THAT(res.variables["datetime_start"].double_values,
                ElementsAre(2994.0, 3006.0));
    EXPECT_THAT(res.variables["datetime_stop"].double_values,
                ElementsAre(3006.0, 3018.0));
    EXPECT_THAT(res.variables["latitude"].double_values,
                ElementsAre(40.0, 41.0));
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(-10.0, -9.0));

    // Profile 0: first two vertical levels correspond to IDs 1 and 2; rest
    // are NaN (double) or 0 (int8).
    EXPECT_DOUBLE_EQ(1500.0, res.variables["altitude"].double_values[0]);
    EXPECT_DOUBLE_EQ(2000.0, res.variables["altitude"].double_values[1]);
    EXPECT_TRUE(std::isnan(res.variables["altitude"].double_values[2]));
    EXPECT_DOUBLE_EQ(1000.0, res.variables["altitude_bounds"].double_values[0]);
    EXPECT_DOUBLE_EQ(2000.0, res.variables["altitude_bounds"].double_values[1]);
    EXPECT_TRUE(std::isnan(res.variables["altitude_bounds"].double_values[4]));
    EXPECT_DOUBLE_EQ(90.0,
                     res.variables["sensor_azimuth_angle"].double_values[0]);
    EXPECT_DOUBLE_EQ(35.0,
                     res.variables["sensor_elevation_angle"].double_values[0]);
    EXPECT_DOUBLE_EQ(100.0,
                     res.variables["hlos_wind_velocity"].double_values[0]);
    EXPECT_DOUBLE_EQ(150.0,
                     res.variables["hlos_wind_velocity"].double_values[1]);
    EXPECT_TRUE(
        std::isnan(res.variables["hlos_wind_velocity"].double_values[2]));
    EXPECT_EQ(1, res.variables["hlos_wind_velocity_validity"].int8_values[0]);
    EXPECT_EQ(2, res.variables["hlos_wind_velocity_validity"].int8_values[1]);
    EXPECT_EQ(0, res.variables["hlos_wind_velocity_validity"].int8_values[2]);

    // Profile 1 level 0 corresponds to ID 3.
    EXPECT_DOUBLE_EQ(200.0,
                     res.variables["hlos_wind_velocity"].double_values[24]);
    EXPECT_EQ(3, res.variables["hlos_wind_velocity_validity"].int8_values[24]);
  }
}

TEST(HarpIngestAeolusL2bTest, IngestMieSuccessAndUnavailableProfile) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("AEOLUS_L2B");
  ASSERT_NE(nullptr, mod);

  // 1. Mie profile ingestion.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateAeolusL2bProduct(prod, /*rayleigh=*/false);
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "data=mie");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_DOUBLE_EQ(100.0,
                     res.variables["hlos_wind_velocity"].double_values[0]);
  }

  // 2. Unavailable rayleigh_profile yields 0 profiles.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddUnavailableField(prod.root(), "rayleigh_profile"));
    CodaMemNode* mph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mph, "abs_orbit", prod.MakeUint32(1u)));
    ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(0, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(24, res.dimensions[harp_dimension_vertical]);
  }
}

TEST(HarpIngestAeolusL2bTest, InitAndVariableErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("AEOLUS_L2B");
  ASSERT_NE(nullptr, mod);

  // 1. Missing rayleigh_profile.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Empty rayleigh_profile (0 elements).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kZeroDims[] = {0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "rayleigh_profile",
                      prod.MakeNodeArray(kZeroDims, {}, prod.MakeRecord())));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Missing rayleigh_hloswind.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long dims[] = {1};
    CodaMemNode* elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_profile",
                              prod.MakeNodeArray(dims, elems)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. Empty rayleigh_hloswind.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long dims[] = {1};
    CodaMemNode* elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_profile",
                              prod.MakeNodeArray(dims, elems)));
    constexpr long kZeroDims[] = {0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "rayleigh_hloswind",
                      prod.MakeNodeArray(kZeroDims, {}, prod.MakeRecord())));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 5. Missing, scalar, or mismatched rayleigh_geolocation.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long dims1[] = {1};
    CodaMemNode* prof_elems[] = {prod.MakeRecord()};
    CodaMemNode* hw_elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_profile",
                              prod.MakeNodeArray(dims1, prof_elems)));
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_hloswind",
                              prod.MakeNodeArray(dims1, hw_elems)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long dims1[] = {1};
    CodaMemNode* prof_elems[] = {prod.MakeRecord()};
    CodaMemNode* hw_elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_profile",
                              prod.MakeNodeArray(dims1, prof_elems)));
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_hloswind",
                              prod.MakeNodeArray(dims1, hw_elems)));
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_geolocation",
                              prod.MakeString("scalar")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long dims2[] = {2};
    CodaMemNode* hw_elems[] = {prod.MakeRecord(), prod.MakeRecord()};
    const long dims1[] = {1};
    CodaMemNode* prof_elems[] = {prod.MakeRecord()};
    CodaMemNode* geo_elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_profile",
                              prod.MakeNodeArray(dims1, prof_elems)));
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_hloswind",
                              prod.MakeNodeArray(dims2, hw_elems)));
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_geolocation",
                              prod.MakeNodeArray(dims1, geo_elems)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 6. Variable read errors: missing/wrong-type wind_result_id_number and
  // scalar fields + wrong abs_orbit type.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* mph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mph, "abs_orbit", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));
    CodaMemNode* prof = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(prof, "profile_datetime_average",
                              prod.MakeString("bad")));
    CodaMemNode* l2b_wp = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(l2b_wp, "wind_result_id_number", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prof, "l2b_wind_profiles", l2b_wp));
    const long dims1[] = {1};
    CodaMemNode* prof_elems[] = {prof};
    CodaMemNode* hw_elems[] = {prod.MakeRecord()};
    CodaMemNode* geo_elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_profile",
                              prod.MakeNodeArray(dims1, prof_elems)));
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_hloswind",
                              prod.MakeNodeArray(dims1, hw_elems)));
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_geolocation",
                              prod.MakeNodeArray(dims1, geo_elems)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["altitude"].read_status);
    EXPECT_EQ(-1, res.variables["altitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["hlos_wind_velocity"].read_status);
    EXPECT_EQ(-1, res.variables["hlos_wind_velocity_validity"].read_status);
  }

  // 7. Variable read errors inside result_cursor[id - 1] (missing/wrong types
  // for altitude_vcog, altitude_top, validity_flag).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* prof = prod.MakeRecord();
    uint32_t ids[kNumResultIds] = {0};
    ids[23] = 1u;
    constexpr long id_dims[] = {kNumResultIds};
    CodaMemNode* l2b_wp = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(l2b_wp, "wind_result_id_number",
                              prod.MakeUint32Array(id_dims, ids)));
    ASSERT_TRUE(prod.AddField(prof, "l2b_wind_profiles", l2b_wp));
    const long dims1[] = {1};
    CodaMemNode* prof_elems[] = {prof};
    CodaMemNode* hw = prod.MakeRecord();
    CodaMemNode* wr = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(wr, "validity_flag", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(hw, "windresult", wr));
    CodaMemNode* hw_elems[] = {hw};
    CodaMemNode* g = prod.MakeRecord();
    CodaMemNode* wg = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(wg, "altitude_vcog", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(wg, "altitude_bottom", prod.MakeDouble(1000.0)));
    ASSERT_TRUE(prod.AddField(g, "windresult_geolocation", wg));
    CodaMemNode* geo_elems[] = {g};
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_profile",
                              prod.MakeNodeArray(dims1, prof_elems)));
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_hloswind",
                              prod.MakeNodeArray(dims1, hw_elems)));
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_geolocation",
                              prod.MakeNodeArray(dims1, geo_elems)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["altitude"].read_status);
    EXPECT_EQ(-1, res.variables["altitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["hlos_wind_velocity"].read_status);
    EXPECT_EQ(-1, res.variables["hlos_wind_velocity_validity"].read_status);
  }

  // 8. Missing l2b_wind_profiles/wind_result_id_number and missing
  // windresult/validity_flag.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long dims1[] = {1};
    CodaMemNode* prof_elems[] = {prod.MakeRecord()};
    CodaMemNode* hw_elems[] = {prod.MakeRecord()};
    CodaMemNode* geo_elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_profile",
                              prod.MakeNodeArray(dims1, prof_elems)));
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_hloswind",
                              prod.MakeNodeArray(dims1, hw_elems)));
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_geolocation",
                              prod.MakeNodeArray(dims1, geo_elems)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["altitude"].read_status);
    EXPECT_EQ(-1, res.variables["hlos_wind_velocity_validity"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* prof = prod.MakeRecord();
    uint32_t ids[kNumResultIds] = {0};
    ids[23] = 1u;
    constexpr long id_dims[] = {kNumResultIds};
    CodaMemNode* l2b_wp = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(l2b_wp, "wind_result_id_number",
                              prod.MakeUint32Array(id_dims, ids)));
    ASSERT_TRUE(prod.AddField(prof, "l2b_wind_profiles", l2b_wp));
    const long dims1[] = {1};
    CodaMemNode* prof_elems[] = {prof};
    CodaMemNode* hw_elems[] = {prod.MakeRecord()};
    CodaMemNode* geo_elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_profile",
                              prod.MakeNodeArray(dims1, prof_elems)));
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_hloswind",
                              prod.MakeNodeArray(dims1, hw_elems)));
    ASSERT_TRUE(prod.AddField(prod.root(), "rayleigh_geolocation",
                              prod.MakeNodeArray(dims1, geo_elems)));
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["hlos_wind_velocity_validity"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
