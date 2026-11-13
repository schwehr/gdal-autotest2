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
// Unit tests for MIPAS Level 1 ingestion module
// (libharp/harp-ingest-mipas_l1.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-mipas_l1.c

#include <cstdint>
#include <span>
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
using ::testing::FloatEq;

constexpr int32_t kNumPoints[5] = {3, 2, 4, 2, 3};
constexpr double kFirstWn[5] = {685.0, 1020.0, 1215.0, 1570.0, 1820.0};
constexpr double kLastWn[5] = {970.0, 1170.0, 1500.0, 1750.0, 2410.0};
constexpr const char* kBandNames[5] = {"band_a", "band_ab", "band_b", "band_c",
                                       "band_d"};

CodaMemNode* MakeMdsRecord(CodaMemProduct& prod, int mds_idx) {
  CodaMemNode* rec = prod.MakeRecord();
  if (rec == nullptr) {
    return nullptr;
  }
  if (!prod.AddField(rec, "dsr_time", prod.MakeDouble(1000.0 + mds_idx * 5))) {
    return nullptr;
  }
  CodaMemNode* loc2 = prod.MakeRecord();
  if (loc2 == nullptr ||
      !prod.AddField(loc2, "latitude", prod.MakeDouble(-10.0 + mds_idx * 20)) ||
      !prod.AddField(loc2, "longitude", prod.MakeDouble(50.0 + mds_idx * 10)) ||
      !prod.AddField(rec, "loc_2", loc2)) {
    return nullptr;
  }
  const long loc1_dims[] = {1};
  const double loc1_vals[] = {18.5 + mds_idx * 3.0};
  if (!prod.AddField(rec, "loc_1",
                     prod.MakeDoubleArray(loc1_dims, loc1_vals))) {
    return nullptr;
  }
  for (int b = 0; b < 5; ++b) {
    const long n = kNumPoints[b];
    const long dims[] = {n};
    std::vector<float> rad(static_cast<size_t>(n));
    for (long j = 0; j < n; ++j) {
      rad[static_cast<size_t>(j)] =
          static_cast<float>((mds_idx + 1) * 10 + b * 100 + j);
    }
    if (!prod.AddField(rec, kBandNames[b], prod.MakeFloatArray(dims, rad))) {
      return nullptr;
    }
  }
  return rec;
}

void PopulateMipasL1Product(CodaMemProduct& prod, long num_mds) {
  ASSERT_TRUE(prod.SetProductInfo("ENVISAT_MIPAS", "MIP_NL__1P", 1));

  CodaMemNode* mph = prod.MakeRecord();
  ASSERT_TRUE(prod.AddField(mph, "abs_orbit", prod.MakeInt32(12345)));
  ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));

  CodaMemNode* sph = prod.MakeRecord();
  const long band_dims[] = {5};
  ASSERT_TRUE(prod.AddField(sph, "first_wavenum",
                            prod.MakeDoubleArray(band_dims, kFirstWn)));
  ASSERT_TRUE(prod.AddField(sph, "last_wavenum",
                            prod.MakeDoubleArray(band_dims, kLastWn)));
  ASSERT_TRUE(prod.AddField(sph, "num_points_per_band",
                            prod.MakeInt32Array(band_dims, kNumPoints)));
  ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));

  std::vector<CodaMemNode*> mds_elems;
  mds_elems.reserve(static_cast<size_t>(num_mds));
  for (long i = 0; i < num_mds; ++i) {
    CodaMemNode* rec = MakeMdsRecord(prod, static_cast<int>(i));
    ASSERT_NE(nullptr, rec);
    mds_elems.push_back(rec);
  }
  const long mds_dims[] = {num_mds};
  ASSERT_TRUE(prod.AddField(prod.root(), "mipas_level_1b_mds",
                            prod.MakeNodeArray(mds_dims, mds_elems)));
}

TEST(HarpIngestMipasL1Test, IngestAllBandsAndSingleBandOptionsSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("MIPAS_L1");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateMipasL1Product(prod, /*num_mds=*/2);

  // 1. All bands (no band option).
  {
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(14, res.dimensions[harp_dimension_spectral]);
    EXPECT_THAT(res.variables["datetime"].double_values,
                ElementsAre(1000.0, 1005.0));
    EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(12345));
    EXPECT_THAT(res.variables["latitude"].double_values,
                ElementsAre(-10.0, 10.0));
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(50.0, 60.0));
    EXPECT_THAT(res.variables["altitude"].double_values,
                ElementsAre(18.5, 21.5));

    const auto& wn = res.variables["wavenumber"].double_values;
    ASSERT_EQ(14u, wn.size());
    EXPECT_DOUBLE_EQ(685.0, wn[0]);
    EXPECT_DOUBLE_EQ(827.5, wn[1]);
    EXPECT_DOUBLE_EQ(970.0, wn[2]);
    EXPECT_DOUBLE_EQ(1020.0, wn[3]);
    EXPECT_DOUBLE_EQ(2410.0, wn[13]);

    const auto& rad = res.variables["wavenumber_radiance"].float_values;
    ASSERT_EQ(28u, rad.size());
    EXPECT_FLOAT_EQ(10.0f, rad[0]);
    EXPECT_FLOAT_EQ(110.0f, rad[3]);
    EXPECT_FLOAT_EQ(20.0f, rad[14]);
    EXPECT_FLOAT_EQ(422.0f, rad[27]);
  }

  // 2. Each individual band option ('A', 'AB', 'B', 'C', 'D').
  struct BandCase {
    const char* opt;
    int band_idx;
  };
  constexpr BandCase kBandCases[] = {
      {"band=A", 0}, {"band=AB", 1}, {"band=B", 2},
      {"band=C", 3}, {"band=D", 4},
  };
  for (const auto& tc : kBandCases) {
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), tc.opt);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(kNumPoints[tc.band_idx], res.dimensions[harp_dimension_spectral]);
    const auto& wn = res.variables["wavenumber"].double_values;
    EXPECT_THAT(wn.front(), DoubleNear(kFirstWn[tc.band_idx], 1e-9));
    EXPECT_THAT(wn.back(), DoubleNear(kLastWn[tc.band_idx], 1e-9));
    const auto& rad = res.variables["wavenumber_radiance"].float_values;
    EXPECT_THAT(rad[0], FloatEq(static_cast<float>(10 + tc.band_idx * 100)));
  }
}

TEST(HarpIngestMipasL1Test, ErrorPathsInitAndDimensions) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("MIPAS_L1");
  ASSERT_NE(nullptr, mod);

  // 1. Missing ProductInfo -> coda_get_product_version fails.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Missing mipas_level_1b_mds.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_MIPAS", "MIP_NL__1P", 1));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Non-array mipas_level_1b_mds.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_MIPAS", "MIP_NL__1P", 1));
    ASSERT_TRUE(
        prod.AddField(prod.root(), "mipas_level_1b_mds", prod.MakeInt32(1)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. Empty 0-element mipas_level_1b_mds (goto_first_array_element fails).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_MIPAS", "MIP_NL__1P", 1));
    const long empty_dims[] = {0};
    CodaMemNode* sample = MakeMdsRecord(prod, 0);
    ASSERT_TRUE(prod.AddField(
        prod.root(), "mipas_level_1b_mds",
        prod.MakeNodeArray(empty_dims, std::span<CodaMemNode* const>{},
                           sample)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 5. Missing / wrong-type /sph/* fields.
  for (int mode = 0; mode < 6; ++mode) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_MIPAS", "MIP_NL__1P", 1));
    const long mds_dims[] = {1};
    CodaMemNode* mds_elems[] = {MakeMdsRecord(prod, 0)};
    ASSERT_TRUE(prod.AddField(prod.root(), "mipas_level_1b_mds",
                              prod.MakeNodeArray(mds_dims, mds_elems)));

    CodaMemNode* sph = prod.MakeRecord();
    const long band_dims[] = {5};
    constexpr std::string_view kBadStrs[5] = {"a", "b", "c", "d", "e"};
    if (mode >= 1) {
      ASSERT_TRUE(
          prod.AddField(sph, "first_wavenum",
                        mode == 1 ? prod.MakeStringArray(band_dims, kBadStrs)
                                  : prod.MakeDoubleArray(band_dims, kFirstWn)));
    }
    if (mode >= 3) {
      ASSERT_TRUE(
          prod.AddField(sph, "last_wavenum",
                        mode == 3 ? prod.MakeStringArray(band_dims, kBadStrs)
                                  : prod.MakeDoubleArray(band_dims, kLastWn)));
    }
    if (mode == 5) {
      ASSERT_TRUE(prod.AddField(sph, "num_points_per_band",
                                prod.MakeStringArray(band_dims, kBadStrs)));
    }
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

TEST(HarpIngestMipasL1Test, ErrorPathsVariableReaders) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("MIPAS_L1");
  ASSERT_NE(nullptr, mod);

  // 1. Missing /mph/abs_orbit, missing dsr_time, missing band_a.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_MIPAS", "MIP_NL__1P", 1));
    CodaMemNode* sph = prod.MakeRecord();
    const long band_dims[] = {5};
    ASSERT_TRUE(prod.AddField(sph, "first_wavenum",
                              prod.MakeDoubleArray(band_dims, kFirstWn)));
    ASSERT_TRUE(prod.AddField(sph, "last_wavenum",
                              prod.MakeDoubleArray(band_dims, kLastWn)));
    ASSERT_TRUE(prod.AddField(sph, "num_points_per_band",
                              prod.MakeInt32Array(band_dims, kNumPoints)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));

    const long mds_dims[] = {1};
    CodaMemNode* empty_mds[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "mipas_level_1b_mds",
                              prod.MakeNodeArray(mds_dims, empty_mds)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["altitude"].read_status);
    EXPECT_EQ(-1, res.variables["wavenumber_radiance"].read_status);
  }

  // 2. Wrong types for /mph/abs_orbit, dsr_time, band_a.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_MIPAS", "MIP_NL__1P", 1));
    CodaMemNode* mph = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(mph, "abs_orbit", prod.MakeString("bad")));
    ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));

    CodaMemNode* sph = prod.MakeRecord();
    const long band_dims[] = {5};
    ASSERT_TRUE(prod.AddField(sph, "first_wavenum",
                              prod.MakeDoubleArray(band_dims, kFirstWn)));
    ASSERT_TRUE(prod.AddField(sph, "last_wavenum",
                              prod.MakeDoubleArray(band_dims, kLastWn)));
    ASSERT_TRUE(prod.AddField(sph, "num_points_per_band",
                              prod.MakeInt32Array(band_dims, kNumPoints)));
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));

    CodaMemNode* rec = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(rec, "dsr_time", prod.MakeString("bad")));
    const long bad_band_dims[] = {3};
    constexpr std::string_view kBadBand[] = {"a", "b", "c"};
    ASSERT_TRUE(prod.AddField(rec, "band_a",
                              prod.MakeStringArray(bad_band_dims, kBadBand)));
    const long mds_dims[] = {1};
    CodaMemNode* mds_elems[] = {rec};
    ASSERT_TRUE(prod.AddField(prod.root(), "mipas_level_1b_mds",
                              prod.MakeNodeArray(mds_dims, mds_elems)));

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "band=A", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["datetime"].read_status);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
    EXPECT_EQ(-1, res.variables["wavenumber_radiance"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
