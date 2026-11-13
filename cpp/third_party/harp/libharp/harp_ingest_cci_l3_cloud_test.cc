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
// Unit tests for ESA CCI L3 cloud ingestion modules
// (libharp/harp-ingest-cci_l3_cloud.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-cci_l3_cloud.c

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

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

void AddCommonCloudFields(CodaMemProduct& prod) {
  constexpr long kDim[] = {2};
  constexpr double kLat[] = {-30.0, 30.0};
  constexpr double kLon[] = {-60.0, 60.0};
  ASSERT_TRUE(
      prod.AddField(prod.root(), "lat", prod.MakeDoubleArray(kDim, kLat)));
  ASSERT_TRUE(
      prod.AddField(prod.root(), "lon", prod.MakeDoubleArray(kDim, kLon)));

  // 10957 days since 1970-01-01 = 2000-01-01T00:00:00Z = 0.0 s since 2000.
  constexpr long kTimeDim[] = {1};
  constexpr double kTime[] = {10957.0};
  ASSERT_TRUE(prod.AddField(prod.root(), "time",
                            prod.MakeDoubleArray(kTimeDim, kTime)));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_start",
                                prod.MakeString("20000101T000000Z")));
  ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_end",
                                prod.MakeString("20000101T235959Z")));
}

void AddCloudDataFields(CodaMemProduct& prod, std::string_view suffix,
                        bool corrected, bool include_l3u_extras,
                        bool include_optional) {
  constexpr long kGridDims[] = {2, 2};
  const std::string corr = corrected ? "_corrected" : "";
  const std::string sfx(suffix);

  constexpr double kCot[] = {1.0, -999.0, 3.0, 4.0};
  constexpr double kCth[] = {1000.0, -32767.0, 3000.0, 4000.0};
  constexpr double kCtp[] = {800.0, 810.0, 820.0, 830.0};
  constexpr double kCtt[] = {250.0, 255.0, 260.0, 265.0};
  constexpr double kUnc[] = {0.1, 0.2, 0.3, 0.4};

  // Upstream read_cloud_optical_depth* always passes corrected=0, so cot and
  // cot_unc never have "_corrected" in their CODA path.
  ASSERT_TRUE(prod.AddField(prod.root(), "cot" + sfx,
                            prod.MakeDoubleArray(kGridDims, kCot)));
  ASSERT_TRUE(prod.AddField(prod.root(), "cot" + sfx + "_unc",
                            prod.MakeDoubleArray(kGridDims, kUnc)));
  ASSERT_TRUE(prod.AddField(prod.root(), "cth" + corr + sfx,
                            prod.MakeDoubleArray(kGridDims, kCth)));
  ASSERT_TRUE(prod.AddField(prod.root(), "cth" + corr + sfx + "_unc",
                            prod.MakeDoubleArray(kGridDims, kUnc)));
  ASSERT_TRUE(prod.AddField(prod.root(), "ctp" + corr + sfx,
                            prod.MakeDoubleArray(kGridDims, kCtp)));
  ASSERT_TRUE(prod.AddField(prod.root(), "ctp" + corr + sfx + "_unc",
                            prod.MakeDoubleArray(kGridDims, kUnc)));
  ASSERT_TRUE(prod.AddField(prod.root(), "ctt" + corr + sfx,
                            prod.MakeDoubleArray(kGridDims, kCtt)));
  ASSERT_TRUE(prod.AddField(prod.root(), "ctt" + corr + sfx + "_unc",
                            prod.MakeDoubleArray(kGridDims, kUnc)));

  if (include_l3u_extras) {
    constexpr double kAng[] = {10.0, -32767.0, 30.0, 40.0};
    constexpr double kZen[] = {15.0, -999.0, 25.0, 35.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "relazi" + sfx,
                              prod.MakeDoubleArray(kGridDims, kAng)));
    ASSERT_TRUE(prod.AddField(prod.root(), "satzen" + sfx,
                              prod.MakeDoubleArray(kGridDims, kZen)));
    ASSERT_TRUE(prod.AddField(prod.root(), "solzen" + sfx,
                              prod.MakeDoubleArray(kGridDims, kZen)));
  }

  if (include_optional) {
    constexpr double kStemp[] = {290.0, -32767.0, 295.0, 300.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "stemp" + sfx,
                              prod.MakeDoubleArray(kGridDims, kStemp)));
    ASSERT_TRUE(prod.AddField(prod.root(), "stemp" + sfx + "_unc",
                              prod.MakeDoubleArray(kGridDims, kUnc)));
    if (include_l3u_extras) {
      constexpr int16_t kQc[] = {0, -999, 1, 2};
      ASSERT_TRUE(prod.AddField(prod.root(), "qcflag" + sfx,
                                prod.MakeInt16Array(kGridDims, kQc)));
    }
  }
}

TEST(HarpIngestCciL3CloudTest, DailyL3uAscendingAndDescendingVariants) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_CLOUD_L3U");
  ASSERT_NE(nullptr, mod);

  // 1. Default options (orbit=ascending, corrected=1), with optional qcflag
  // and stemp. Upstream checks /ctt_corrected_desc, /qcflag_desc, and
  // /stemp_desc even for ascending orbits.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddCommonCloudFields(prod);
    AddCloudDataFields(prod, "_asc", /*corrected=*/true,
                       /*include_l3u_extras=*/true, /*include_optional=*/true);
    // Presence indicators checked by ingestion_daily_l3u_init:
    constexpr long kGridDims[] = {2, 2};
    constexpr double kDummy[] = {1.0, 2.0, 3.0, 4.0};
    constexpr int16_t kDummyQc[] = {0, 0, 0, 0};
    ASSERT_TRUE(prod.AddField(prod.root(), "ctt_corrected_desc",
                              prod.MakeDoubleArray(kGridDims, kDummy)));
    ASSERT_TRUE(prod.AddField(prod.root(), "qcflag_desc",
                              prod.MakeInt16Array(kGridDims, kDummyQc)));
    ASSERT_TRUE(prod.AddField(prod.root(), "stemp_desc",
                              prod.MakeDoubleArray(kGridDims, kDummy)));

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(1L, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(2L, res.dimensions[harp_dimension_latitude]);
    EXPECT_EQ(2L, res.dimensions[harp_dimension_longitude]);
    EXPECT_THAT(res.variables["latitude"].double_values,
                ElementsAre(-30.0, 30.0));
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(-60.0, 60.0));
    EXPECT_TRUE(
        std::isnan(res.variables["cloud_optical_depth"].double_values[1]));
    EXPECT_TRUE(std::isnan(res.variables["cloud_top_height"].double_values[1]));
    EXPECT_DOUBLE_EQ(800.0,
                     res.variables["cloud_top_pressure"].double_values[0]);
    EXPECT_DOUBLE_EQ(250.0,
                     res.variables["cloud_top_temperature"].double_values[0]);
    EXPECT_TRUE(res.variables["validity"].included);
    EXPECT_THAT(
        res.variables["validity"].int16_values,
        ElementsAre(0, harp_get_fill_value_for_type(harp_type_int16).int16_data,
                    1, 2));
    EXPECT_TRUE(res.variables["surface_temperature"].included);
    EXPECT_TRUE(
        std::isnan(res.variables["surface_temperature"].double_values[1]));
    EXPECT_DOUBLE_EQ(0.0, res.variables["datetime"].double_values[0]);
    EXPECT_DOUBLE_EQ(0.0, res.variables["datetime_start"].double_values[0]);
    EXPECT_DOUBLE_EQ(86399.0, res.variables["datetime_stop"].double_values[0]);
  }

  // 2. orbit=descending;corrected=false without optional qcflag/stemp.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddCommonCloudFields(prod);
    AddCloudDataFields(prod, "_desc", /*corrected=*/false,
                       /*include_l3u_extras=*/true, /*include_optional=*/false);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "orbit=descending;corrected=false");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(res.variables["validity"].included);
    EXPECT_FALSE(res.variables["surface_temperature"].included);
    EXPECT_FALSE(res.variables["surface_temperature_uncertainty"].included);
    EXPECT_DOUBLE_EQ(1.0,
                     res.variables["cloud_optical_depth"].double_values[0]);
    EXPECT_DOUBLE_EQ(10.0,
                     res.variables["relative_azimuth_angle"].double_values[0]);
    EXPECT_TRUE(
        std::isnan(res.variables["sensor_zenith_angle"].double_values[1]));
  }
}

TEST(HarpIngestCciL3CloudTest, MonthlyL3cCorrectedAndUncorrected) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("ESACCI_CLOUD_L3C");
  ASSERT_NE(nullptr, mod);

  // 1. Default corrected=1 with stemp present.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddCommonCloudFields(prod);
    AddCloudDataFields(prod, "", /*corrected=*/true,
                       /*include_l3u_extras=*/false, /*include_optional=*/true);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_TRUE(res.variables["surface_temperature"].included);
    EXPECT_DOUBLE_EQ(290.0,
                     res.variables["surface_temperature"].double_values[0]);
    EXPECT_DOUBLE_EQ(
        0.1, res.variables["surface_temperature_uncertainty"].double_values[0]);
  }

  // 2. corrected=false without stemp.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    AddCommonCloudFields(prod);
    AddCloudDataFields(prod, "", /*corrected=*/false,
                       /*include_l3u_extras=*/false,
                       /*include_optional=*/false);

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "corrected=false");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(res.variables["surface_temperature"].included);
    EXPECT_DOUBLE_EQ(265.0,
                     res.variables["cloud_top_temperature"].double_values[3]);
  }
}

TEST(HarpIngestCciL3CloudTest, InitAndReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* l3u = FindIngestionModule("ESACCI_CLOUD_L3U");
  const harp_ingestion_module* l3c = FindIngestionModule("ESACCI_CLOUD_L3C");
  ASSERT_NE(nullptr, l3u);
  ASSERT_NE(nullptr, l3c);

  // 1. Missing ascending / descending / corrected datasets.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(l3u, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);

    IngestExerciseResult res_desc =
        ExerciseIngestionModule(l3u, prod.product(), "orbit=descending");
    EXPECT_EQ(-1, res_desc.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res_desc.failed_errno);

    constexpr long kDim[] = {1};
    constexpr double kOne[] = {1.0};
    ASSERT_TRUE(prod.AddField(prod.root(), "cot_asc",
                              prod.MakeDoubleArray(kDim, kOne)));
    IngestExerciseResult res_corr =
        ExerciseIngestionModule(l3u, prod.product());
    EXPECT_EQ(-1, res_corr.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res_corr.failed_errno);

    IngestExerciseResult res_l3c = ExerciseIngestionModule(l3c, prod.product());
    EXPECT_EQ(-1, res_l3c.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res_l3c.failed_errno);
  }

  // 2. init_dimensions error paths (missing /lat, scalar /lat, missing /lon,
  // 2D /lon).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult r1 =
        ExerciseIngestionModule(l3c, prod.product(), "corrected=false");
    EXPECT_EQ(-1, r1.status);

    ASSERT_TRUE(prod.AddField(prod.root(), "lat", prod.MakeDouble(0.0)));
    IngestExerciseResult r2 =
        ExerciseIngestionModule(l3c, prod.product(), "corrected=false");
    EXPECT_EQ(-1, r2.status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDim2D[] = {1, 1};
    constexpr double kOne[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeDoubleArray(kDim2D, kOne)));
    IngestExerciseResult r3 =
        ExerciseIngestionModule(l3c, prod.product(), "corrected=false");
    EXPECT_EQ(-1, r3.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, r3.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDim1D[] = {1};
    constexpr long kDim2D[] = {1, 1};
    constexpr double kOne[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeDoubleArray(kDim1D, kOne)));
    IngestExerciseResult r4 =
        ExerciseIngestionModule(l3c, prod.product(), "corrected=false");
    EXPECT_EQ(-1, r4.status);

    ASSERT_TRUE(
        prod.AddField(prod.root(), "lon", prod.MakeDoubleArray(kDim2D, kOne)));
    IngestExerciseResult r5 =
        ExerciseIngestionModule(l3c, prod.product(), "corrected=false");
    EXPECT_EQ(-1, r5.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, r5.failed_errno);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDim1D[] = {1};
    constexpr double kOne[] = {1.0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "lat", prod.MakeDoubleArray(kDim1D, kOne)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lon", prod.MakeDouble(0.0)));
    IngestExerciseResult r6 =
        ExerciseIngestionModule(l3c, prod.product(), "corrected=false");
    EXPECT_EQ(-1, r6.status);
  }

  // 3. Read errors: qcflag size/type, datetime attribute errors, dataset
  // errors.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDim1D[] = {2};
    constexpr double kLatLon[] = {1.0, 2.0};
    constexpr long kOneDim[] = {1};
    constexpr double kOne[] = {1.0};
    constexpr int16_t kOneInt[] = {1};
    constexpr long kGridDims[] = {2, 2};
    constexpr std::string_view kBadStrs[] = {"a", "b", "c", "d"};

    ASSERT_TRUE(prod.AddField(prod.root(), "lat",
                              prod.MakeDoubleArray(kDim1D, kLatLon)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lon",
                              prod.MakeDoubleArray(kDim1D, kLatLon)));
    ASSERT_TRUE(prod.AddField(prod.root(), "cot_desc",
                              prod.MakeDoubleArray(kOneDim, kOne)));
    ASSERT_TRUE(prod.AddField(prod.root(), "qcflag_desc",
                              prod.MakeInt16Array(kOneDim, kOneInt)));
    ASSERT_TRUE(prod.AddField(prod.root(), "cth_desc",
                              prod.MakeStringArray(kGridDims, kBadStrs)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_start",
                                  prod.MakeString("short")));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_end",
                                  prod.MakeString("99999999T999999Z")));

    IngestExerciseResult res = ExerciseIngestionModule(
        l3u, prod.product(), "orbit=descending;corrected=false",
        /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["cloud_optical_depth"].read_status);
    EXPECT_EQ(-1, res.variables["cloud_top_height"].read_status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
  }
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kDim1D[] = {2};
    constexpr double kLatLon[] = {1.0, 2.0};
    constexpr long kGridDims[] = {2, 2};
    constexpr std::string_view kBadStrs[] = {"a", "b", "c", "d"};
    ASSERT_TRUE(prod.AddField(prod.root(), "lat",
                              prod.MakeDoubleArray(kDim1D, kLatLon)));
    ASSERT_TRUE(prod.AddField(prod.root(), "lon",
                              prod.MakeDoubleArray(kDim1D, kLatLon)));
    ASSERT_TRUE(prod.AddField(prod.root(), "cot_desc",
                              prod.MakeStringArray(kGridDims, kBadStrs)));
    ASSERT_TRUE(prod.AddField(prod.root(), "qcflag_desc",
                              prod.MakeStringArray(kGridDims, kBadStrs)));
    ASSERT_TRUE(prod.AddAttribute(prod.root(), "time_coverage_start",
                                  prod.MakeInt32(123)));
    IngestExerciseResult res = ExerciseIngestionModule(
        l3u, prod.product(), "orbit=descending;corrected=false",
        /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["validity"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_start"].read_status);
    EXPECT_EQ(-1, res.variables["datetime_stop"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
