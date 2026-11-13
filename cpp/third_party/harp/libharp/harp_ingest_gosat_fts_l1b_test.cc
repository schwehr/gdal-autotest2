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
// Unit tests for GOSAT FTS L1b ingestion module
// (libharp/harp-ingest-gosat_fts_l1b.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-gosat_fts_l1b.c

#include <cstdint>
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

constexpr long kBandNumWavenumbers[] = {6565, 6565, 8080, 8080,
                                        6565, 6565, 7575};

CodaMemNode* MakeTimeRecord(CodaMemProduct& prod, int32_t year, int32_t month,
                            int32_t day, int32_t hour, int32_t minute,
                            double sec) {
  CodaMemNode* rec = prod.MakeRecord();
  if (rec == nullptr) {
    return nullptr;
  }
  if (!prod.AddField(rec, "year", prod.MakeInt32(year)) ||
      !prod.AddField(rec, "month", prod.MakeInt32(month)) ||
      !prod.AddField(rec, "day", prod.MakeInt32(day)) ||
      !prod.AddField(rec, "hour", prod.MakeInt32(hour)) ||
      !prod.AddField(rec, "min", prod.MakeInt32(minute)) ||
      !prod.AddField(rec, "sec", prod.MakeDouble(sec))) {
    return nullptr;
  }
  return rec;
}

void PopulateGosatL1bProduct(CodaMemProduct& prod, long num_main,
                             bool include_swir, bool include_tir) {
  CodaMemNode* exp_attr = prod.MakeRecord();
  ASSERT_NE(nullptr, exp_attr);
  CodaMemNode* pt_attr = prod.MakeRecord();
  ASSERT_NE(nullptr, pt_attr);
  CodaMemNode* rad_info = prod.MakeRecord();
  ASSERT_NE(nullptr, rad_info);

  if (include_swir) {
    std::vector<double> swir_range(static_cast<size_t>(num_main) * 6 * 2);
    for (long t = 0; t < num_main; ++t) {
      for (int b = 0; b < 6; ++b) {
        const size_t idx = static_cast<size_t>(t * 6 + b) * 2;
        swir_range[idx] = 0.2 + 0.01 * b;
        swir_range[idx + 1] = 1000.0 + 100.0 * b + 10.0 * t;
      }
    }
    const long dims[] = {num_main, 6, 2};
    ASSERT_TRUE(prod.AddField(rad_info, "spectrumObsWavelengthRange_SWIR",
                              prod.MakeDoubleArray(dims, swir_range)));
  }
  if (include_tir) {
    std::vector<double> tir_range(static_cast<size_t>(num_main) * 2);
    for (long t = 0; t < num_main; ++t) {
      tir_range[static_cast<size_t>(t) * 2] = 0.5;
      tir_range[static_cast<size_t>(t) * 2 + 1] = 600.0 + 10.0 * t;
    }
    const long dims[] = {num_main, 2};
    ASSERT_TRUE(prod.AddField(rad_info, "spectrumObsWavelengthRange_TIR",
                              prod.MakeDoubleArray(dims, tir_range)));
  }
  ASSERT_TRUE(prod.AddField(pt_attr, "RadiometricCorrectionInfo", rad_info));

  std::vector<CodaMemNode*> time_elems;
  time_elems.reserve(static_cast<size_t>(num_main));
  for (long t = 0; t < num_main; ++t) {
    CodaMemNode* rec = MakeTimeRecord(prod, 2015, 6, 15, 12, 30, 10.5 + t);
    ASSERT_NE(nullptr, rec);
    time_elems.push_back(rec);
  }
  const long time_dims[] = {num_main};
  ASSERT_TRUE(prod.AddField(pt_attr, "Time",
                            prod.MakeNodeArray(time_dims, time_elems)));

  CodaMemNode* geo = prod.MakeRecord();
  ASSERT_NE(nullptr, geo);
  std::vector<double> lats(static_cast<size_t>(num_main));
  std::vector<double> lons(static_cast<size_t>(num_main));
  for (long t = 0; t < num_main; ++t) {
    lats[static_cast<size_t>(t)] = 35.0 + t;
    lons[static_cast<size_t>(t)] = 135.0 + t;
  }
  ASSERT_TRUE(
      prod.AddField(geo, "centerLat", prod.MakeDoubleArray(time_dims, lats)));
  ASSERT_TRUE(
      prod.AddField(geo, "centerLon", prod.MakeDoubleArray(time_dims, lons)));
  ASSERT_TRUE(prod.AddField(pt_attr, "geometricInfo", geo));

  ASSERT_TRUE(prod.AddField(exp_attr, "pointAttribute", pt_attr));
  ASSERT_TRUE(prod.AddField(prod.root(), "exposureAttribute", exp_attr));

  CodaMemNode* spectrum = prod.MakeRecord();
  ASSERT_NE(nullptr, spectrum);
  if (include_swir) {
    CodaMemNode* swir = prod.MakeRecord();
    ASSERT_NE(nullptr, swir);
    const char* band_names[] = {"band1", "band2", "band3"};
    const long band_lens[] = {6565, 8080, 6565};
    for (int i = 0; i < 3; ++i) {
      CodaMemNode* band = prod.MakeRecord();
      ASSERT_NE(nullptr, band);
      const long n = band_lens[i];
      std::vector<float> obs(static_cast<size_t>(num_main) * 2 * n * 2, 0.0f);
      for (long t = 0; t < num_main; ++t) {
        const size_t p_off = static_cast<size_t>(t * 2 * n * 2);
        const size_t s_off = static_cast<size_t>(t * 2 * n * 2 + n * 2);
        obs[p_off] = 3.0f * (t + 1);
        obs[p_off + 1] = 4.0f * (t + 1);
        obs[s_off] = 6.0f * (t + 1);
        obs[s_off + 1] = 8.0f * (t + 1);
      }
      const long obs_dims[] = {num_main, 2, n, 2};
      ASSERT_TRUE(prod.AddField(band, "obsWavelength",
                                prod.MakeFloatArray(obs_dims, obs)));
      ASSERT_TRUE(prod.AddField(swir, band_names[i], band));
    }
    ASSERT_TRUE(prod.AddField(spectrum, "SWIR", swir));
  }
  if (include_tir) {
    CodaMemNode* tir = prod.MakeRecord();
    ASSERT_NE(nullptr, tir);
    CodaMemNode* band4 = prod.MakeRecord();
    ASSERT_NE(nullptr, band4);
    const long n = 7575;
    std::vector<float> obs(static_cast<size_t>(num_main) * n * 2, 0.0f);
    for (long t = 0; t < num_main; ++t) {
      const size_t off = static_cast<size_t>(t * n * 2);
      obs[off] = 5.0f * (t + 1);
      obs[off + 1] = 12.0f * (t + 1);
    }
    const long obs_dims[] = {num_main, n, 2};
    ASSERT_TRUE(prod.AddField(band4, "obsWavelength",
                              prod.MakeFloatArray(obs_dims, obs)));
    ASSERT_TRUE(prod.AddField(tir, "band4", band4));
    ASSERT_TRUE(prod.AddField(spectrum, "TIR", tir));
  }
  ASSERT_TRUE(prod.AddField(prod.root(), "Spectrum", spectrum));
}

TEST(HarpIngestGosatFtsL1bTest, IngestAllBandsSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("GOSAT_FTS_L1b");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateGosatL1bProduct(prod, /*num_main=*/2, /*include_swir=*/true,
                          /*include_tir=*/true);

  struct BandCase {
    int band_id;
    const char* option_str;
    double expected_rad0;
    double expected_rad1;
    double expected_wn_a;
    double expected_wn_b0;
  };
  constexpr BandCase kCases[] = {
      {.band_id = 0,
       .option_str = nullptr,
       .expected_rad0 = 5.0,
       .expected_rad1 = 10.0,
       .expected_wn_a = 0.20,
       .expected_wn_b0 = 1000.0},
      {.band_id = 0,
       .option_str = "band=1p",
       .expected_rad0 = 5.0,
       .expected_rad1 = 10.0,
       .expected_wn_a = 0.20,
       .expected_wn_b0 = 1000.0},
      {.band_id = 1,
       .option_str = "band=1s",
       .expected_rad0 = 10.0,
       .expected_rad1 = 20.0,
       .expected_wn_a = 0.21,
       .expected_wn_b0 = 1100.0},
      {.band_id = 2,
       .option_str = "band=2p",
       .expected_rad0 = 5.0,
       .expected_rad1 = 10.0,
       .expected_wn_a = 0.22,
       .expected_wn_b0 = 1200.0},
      {.band_id = 3,
       .option_str = "band=2s",
       .expected_rad0 = 10.0,
       .expected_rad1 = 20.0,
       .expected_wn_a = 0.23,
       .expected_wn_b0 = 1300.0},
      {.band_id = 4,
       .option_str = "band=3p",
       .expected_rad0 = 5.0,
       .expected_rad1 = 10.0,
       .expected_wn_a = 0.24,
       .expected_wn_b0 = 1400.0},
      {.band_id = 5,
       .option_str = "band=3s",
       .expected_rad0 = 10.0,
       .expected_rad1 = 20.0,
       .expected_wn_a = 0.25,
       .expected_wn_b0 = 1500.0},
      {.band_id = 6,
       .option_str = "band=4",
       .expected_rad0 = 13.0,
       .expected_rad1 = 26.0,
       .expected_wn_a = 0.50,
       .expected_wn_b0 = 600.0},
  };

  double expected_t0 = 0.0;
  double expected_t1 = 0.0;
  ASSERT_EQ(0, coda_datetime_to_double(2015, 6, 15, 12, 30, 10, 500000,
                                       &expected_t0));
  ASSERT_EQ(0, coda_datetime_to_double(2015, 6, 15, 12, 30, 11, 500000,
                                       &expected_t1));

  for (const auto& tc : kCases) {
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), tc.option_str);
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    const long n_wn = kBandNumWavenumbers[tc.band_id];
    EXPECT_EQ(n_wn, res.dimensions[harp_dimension_spectral]);

    EXPECT_THAT(res.variables["datetime"].double_values,
                ElementsAre(DoubleNear(expected_t0, 1e-6),
                            DoubleNear(expected_t1, 1e-6)));
    EXPECT_THAT(res.variables["latitude"].double_values,
                ElementsAre(35.0, 36.0));
    EXPECT_THAT(res.variables["longitude"].double_values,
                ElementsAre(135.0, 136.0));
    EXPECT_NEAR(tc.expected_rad0, res.variables["radiance"].double_values[0],
                1e-6);
    EXPECT_NEAR(tc.expected_rad1, res.variables["radiance"].double_values[n_wn],
                1e-6);
    EXPECT_NEAR(tc.expected_wn_b0, res.variables["wavenumber"].double_values[0],
                1e-9);
    EXPECT_NEAR(tc.expected_wn_a + tc.expected_wn_b0,
                res.variables["wavenumber"].double_values[1], 1e-9);
    EXPECT_NEAR(tc.expected_wn_b0 + 10.0,
                res.variables["wavenumber"].double_values[n_wn], 1e-9);
  }
}

TEST(HarpIngestGosatFtsL1bTest, NightTimeSwirReturnsEmptyProduct) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOSAT_FTS_L1b");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateGosatL1bProduct(prod, /*num_main=*/1, /*include_swir=*/false,
                          /*include_tir=*/true);

  IngestExerciseResult res =
      ExerciseIngestionModule(mod, prod.product(), "band=1p");
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(0, res.dimensions[harp_dimension_time]);
}

TEST(HarpIngestGosatFtsL1bTest, ErrorPathsInitCursorsAndNumMain) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOSAT_FTS_L1b");
  ASSERT_NE(nullptr, mod);

  // 1. Non-CODA_ERROR_INVALID_NAME on SWIR wavenumber path (scalar parent).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* exp_attr = prod.MakeRecord();
    CodaMemNode* pt_attr = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(pt_attr, "RadiometricCorrectionInfo", prod.MakeInt32(1)));
    ASSERT_TRUE(prod.AddField(exp_attr, "pointAttribute", pt_attr));
    ASSERT_TRUE(prod.AddField(prod.root(), "exposureAttribute", exp_attr));
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "band=1p");
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Missing TIR wavenumber path with band=4.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "band=4");
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Missing radiance path.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* exp_attr = prod.MakeRecord();
    CodaMemNode* pt_attr = prod.MakeRecord();
    CodaMemNode* rad_info = prod.MakeRecord();
    const long dims[] = {1, 6, 2};
    const std::vector<double> vals(12, 1.0);
    ASSERT_TRUE(prod.AddField(rad_info, "spectrumObsWavelengthRange_SWIR",
                              prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(pt_attr, "RadiometricCorrectionInfo", rad_info));
    ASSERT_TRUE(prod.AddField(exp_attr, "pointAttribute", pt_attr));
    ASSERT_TRUE(prod.AddField(prod.root(), "exposureAttribute", exp_attr));
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "band=1p");
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. Missing Time path.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* exp_attr = prod.MakeRecord();
    CodaMemNode* pt_attr = prod.MakeRecord();
    CodaMemNode* rad_info = prod.MakeRecord();
    const long dims[] = {1, 2};
    const double vals[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(rad_info, "spectrumObsWavelengthRange_TIR",
                              prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(pt_attr, "RadiometricCorrectionInfo", rad_info));
    ASSERT_TRUE(prod.AddField(exp_attr, "pointAttribute", pt_attr));
    ASSERT_TRUE(prod.AddField(prod.root(), "exposureAttribute", exp_attr));
    CodaMemNode* spectrum = prod.MakeRecord();
    CodaMemNode* tir = prod.MakeRecord();
    CodaMemNode* band4 = prod.MakeRecord();
    const long obs_dims[] = {1};
    const float obs_vals[] = {1.0f};
    ASSERT_TRUE(prod.AddField(band4, "obsWavelength",
                              prod.MakeFloatArray(obs_dims, obs_vals)));
    ASSERT_TRUE(prod.AddField(tir, "band4", band4));
    ASSERT_TRUE(prod.AddField(spectrum, "TIR", tir));
    ASSERT_TRUE(prod.AddField(prod.root(), "Spectrum", spectrum));
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "band=4");
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 5. Missing geometricInfo path and non-array Time.
  for (bool bad_time_array : {false, true}) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* exp_attr = prod.MakeRecord();
    CodaMemNode* pt_attr = prod.MakeRecord();
    CodaMemNode* rad_info = prod.MakeRecord();
    const long dims[] = {1, 2};
    const double vals[] = {1.0, 2.0};
    ASSERT_TRUE(prod.AddField(rad_info, "spectrumObsWavelengthRange_TIR",
                              prod.MakeDoubleArray(dims, vals)));
    ASSERT_TRUE(prod.AddField(pt_attr, "RadiometricCorrectionInfo", rad_info));
    if (bad_time_array) {
      ASSERT_TRUE(prod.AddField(pt_attr, "Time", prod.MakeInt32(1)));
      ASSERT_TRUE(prod.AddField(pt_attr, "geometricInfo", prod.MakeRecord()));
    } else {
      const long tdims[] = {1};
      CodaMemNode* elems[] = {MakeTimeRecord(prod, 2015, 6, 15, 12, 0, 0.0)};
      ASSERT_TRUE(
          prod.AddField(pt_attr, "Time", prod.MakeNodeArray(tdims, elems)));
    }
    ASSERT_TRUE(prod.AddField(exp_attr, "pointAttribute", pt_attr));
    ASSERT_TRUE(prod.AddField(prod.root(), "exposureAttribute", exp_attr));
    CodaMemNode* spectrum = prod.MakeRecord();
    CodaMemNode* tir = prod.MakeRecord();
    CodaMemNode* band4 = prod.MakeRecord();
    const long obs_dims[] = {1};
    const float obs_vals[] = {1.0f};
    ASSERT_TRUE(prod.AddField(band4, "obsWavelength",
                              prod.MakeFloatArray(obs_dims, obs_vals)));
    ASSERT_TRUE(prod.AddField(tir, "band4", band4));
    ASSERT_TRUE(prod.AddField(spectrum, "TIR", tir));
    ASSERT_TRUE(prod.AddField(prod.root(), "Spectrum", spectrum));
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "band=4");
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

TEST(HarpIngestGosatFtsL1bTest, ErrorPathsTimeAndVariables) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOSAT_FTS_L1b");
  ASSERT_NE(nullptr, mod);

  // Helper to attach custom Time, geometricInfo, TIR obsWavelength, and
  // wavenumber range to `prod`.
  auto attach_custom = [](CodaMemProduct& prod, CodaMemNode* time_node,
                          CodaMemNode* geo_node, CodaMemNode* obs_node,
                          CodaMemNode* wn_node) {
    CodaMemNode* exp_attr = prod.MakeRecord();
    CodaMemNode* pt_attr = prod.MakeRecord();
    CodaMemNode* rad_info = prod.MakeRecord();
    ASSERT_TRUE(
        prod.AddField(rad_info, "spectrumObsWavelengthRange_TIR", wn_node));
    ASSERT_TRUE(prod.AddField(pt_attr, "RadiometricCorrectionInfo", rad_info));
    ASSERT_TRUE(prod.AddField(pt_attr, "Time", time_node));
    ASSERT_TRUE(prod.AddField(pt_attr, "geometricInfo", geo_node));
    ASSERT_TRUE(prod.AddField(exp_attr, "pointAttribute", pt_attr));
    ASSERT_TRUE(prod.AddField(prod.root(), "exposureAttribute", exp_attr));

    CodaMemNode* spectrum = prod.MakeRecord();
    CodaMemNode* tir = prod.MakeRecord();
    CodaMemNode* band4 = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(band4, "obsWavelength", obs_node));
    ASSERT_TRUE(prod.AddField(tir, "band4", band4));
    ASSERT_TRUE(prod.AddField(spectrum, "TIR", tir));
    ASSERT_TRUE(prod.AddField(prod.root(), "Spectrum", spectrum));
  };

  auto check_all_vars_fail = [&](CodaMemProduct& prod, std::string_view var) {
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "band=4",
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables[std::string(var)].read_status);
  };

  // Test missing or wrong-type Time fields and invalid datetime.
  for (int mode = 0; mode < 13; ++mode) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    CodaMemNode* exp_attr = prod.MakeRecord();
    CodaMemNode* pt_attr = prod.MakeRecord();
    CodaMemNode* rad_info = prod.MakeRecord();
    const long wn_dims[] = {1, 2};
    const double wn_vals[] = {0.5, 600.0};
    ASSERT_TRUE(prod.AddField(rad_info, "spectrumObsWavelengthRange_TIR",
                              prod.MakeDoubleArray(wn_dims, wn_vals)));
    ASSERT_TRUE(prod.AddField(pt_attr, "RadiometricCorrectionInfo", rad_info));

    CodaMemNode* trec = prod.MakeRecord();
    if (mode != 0) {
      ASSERT_TRUE(prod.AddField(
          trec, "year",
          mode == 1 ? prod.MakeString("x") : prod.MakeInt32(2015)));
    }
    if (mode != 2) {
      ASSERT_TRUE(prod.AddField(trec, "month",
                                mode == 3
                                    ? prod.MakeString("x")
                                    : prod.MakeInt32(mode == 12 ? 99 : 6)));
    }
    if (mode != 4) {
      ASSERT_TRUE(prod.AddField(
          trec, "day", mode == 5 ? prod.MakeString("x") : prod.MakeInt32(15)));
    }
    if (mode != 6) {
      ASSERT_TRUE(prod.AddField(
          trec, "hour", mode == 7 ? prod.MakeString("x") : prod.MakeInt32(12)));
    }
    if (mode != 8) {
      ASSERT_TRUE(prod.AddField(
          trec, "min", mode == 9 ? prod.MakeString("x") : prod.MakeInt32(30)));
    }
    if (mode != 10) {
      ASSERT_TRUE(prod.AddField(
          trec, "sec",
          mode == 11 ? prod.MakeString("x") : prod.MakeDouble(0.0)));
    }
    const long tdims[] = {1};
    CodaMemNode* telems[] = {trec};
    ASSERT_TRUE(
        prod.AddField(pt_attr, "Time", prod.MakeNodeArray(tdims, telems)));
    CodaMemNode* geo = prod.MakeRecord();
    ASSERT_TRUE(prod.AddField(pt_attr, "geometricInfo", geo));
    ASSERT_TRUE(prod.AddField(exp_attr, "pointAttribute", pt_attr));
    ASSERT_TRUE(prod.AddField(prod.root(), "exposureAttribute", exp_attr));

    CodaMemNode* spectrum = prod.MakeRecord();
    CodaMemNode* tir = prod.MakeRecord();
    CodaMemNode* band4 = prod.MakeRecord();
    const long obs_dims[] = {1};
    const float obs_vals[] = {1.0f};
    ASSERT_TRUE(prod.AddField(band4, "obsWavelength",
                              prod.MakeFloatArray(obs_dims, obs_vals)));
    ASSERT_TRUE(prod.AddField(tir, "band4", band4));
    ASSERT_TRUE(prod.AddField(spectrum, "TIR", tir));
    ASSERT_TRUE(prod.AddField(prod.root(), "Spectrum", spectrum));

    check_all_vars_fail(prod, "datetime");
  }

  // Missing / out-of-bounds / wrong-type centerLat, centerLon, obsWavelength,
  // and wavenumber range.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long tdims[] = {1};
    CodaMemNode* telems[] = {MakeTimeRecord(prod, 2015, 6, 15, 12, 0, 0.0)};
    CodaMemNode* time_arr = prod.MakeNodeArray(tdims, telems);
    CodaMemNode* geo = prod.MakeRecord();  // Missing centerLat & centerLon.
    const long bad_obs_dims[] = {1};
    constexpr std::string_view kBadStrs[] = {"bad"};
    CodaMemNode* bad_obs = prod.MakeStringArray(bad_obs_dims, kBadStrs);
    const long empty_dims[] = {0};
    CodaMemNode* empty_wn =
        prod.MakeDoubleArray(empty_dims, std::span<const double>{});
    attach_custom(prod, time_arr, geo, bad_obs, empty_wn);
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "band=4",
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["radiance"].read_status);
    EXPECT_EQ(-1, res.variables["wavenumber"].read_status);
  }

  // centerLat/centerLon empty arrays and 1-element wavenumber range.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long tdims[] = {1};
    CodaMemNode* telems[] = {MakeTimeRecord(prod, 2015, 6, 15, 12, 0, 0.0)};
    CodaMemNode* time_arr = prod.MakeNodeArray(tdims, telems);
    CodaMemNode* geo = prod.MakeRecord();
    const long empty_dims[] = {0};
    ASSERT_TRUE(prod.AddField(
        geo, "centerLat",
        prod.MakeDoubleArray(empty_dims, std::span<const double>{})));
    ASSERT_TRUE(prod.AddField(
        geo, "centerLon",
        prod.MakeDoubleArray(empty_dims, std::span<const double>{})));
    const long obs_dims[] = {1};
    const float obs_vals[] = {1.0f};
    CodaMemNode* obs = prod.MakeFloatArray(obs_dims, obs_vals);
    const long wn_dims[] = {1};
    const double wn_vals[] = {1.0};
    CodaMemNode* short_wn = prod.MakeDoubleArray(wn_dims, wn_vals);
    attach_custom(prod, time_arr, geo, obs, short_wn);
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "band=4",
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["wavenumber"].read_status);
  }

  // centerLat/centerLon/wavenumber wrong types (string arrays).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long tdims[] = {1};
    CodaMemNode* telems[] = {MakeTimeRecord(prod, 2015, 6, 15, 12, 0, 0.0)};
    CodaMemNode* time_arr = prod.MakeNodeArray(tdims, telems);
    CodaMemNode* geo = prod.MakeRecord();
    const long s1_dims[] = {1};
    constexpr std::string_view kS1[] = {"x"};
    ASSERT_TRUE(
        prod.AddField(geo, "centerLat", prod.MakeStringArray(s1_dims, kS1)));
    ASSERT_TRUE(
        prod.AddField(geo, "centerLon", prod.MakeStringArray(s1_dims, kS1)));
    const long obs_dims[] = {1};
    const float obs_vals[] = {1.0f};
    CodaMemNode* obs = prod.MakeFloatArray(obs_dims, obs_vals);
    const long wn_dims[] = {2};
    constexpr std::string_view kS2[] = {"a", "b"};
    CodaMemNode* bad_wn = prod.MakeStringArray(wn_dims, kS2);
    attach_custom(prod, time_arr, geo, obs, bad_wn);
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "band=4",
                                /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["wavenumber"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
