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
// Unit tests for AEOLUS L2A ingestion module
// (libharp/harp-ingest-aeolus_l2a.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-aeolus_l2a.c

#include <cmath>
#include <cstdint>
#include <cstdlib>
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

#if defined(__SANITIZE_ADDRESS__)
#define HARP_TEST_HAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HARP_TEST_HAS_ASAN 1
#endif
#endif

constexpr int kNumBins = 24;

bool PopulateAeolusL2aProduct(CodaMemProduct& prod, int num_props) {
  CodaMemNode* mph = prod.MakeRecord();
  if (!prod.AddField(mph, "abs_orbit", prod.MakeUint32(9876u)) ||
      !prod.AddField(prod.root(), "mph", mph)) {
    return false;
  }

  std::vector<CodaMemNode*> prop_elems;
  std::vector<CodaMemNode*> pcd_elems;
  for (int i = 0; i < num_props; ++i) {
    CodaMemNode* prop = prod.MakeRecord();
    if (!prod.AddField(prop, "starttime", prod.MakeDouble(2000.0 + i * 12.0))) {
      return false;
    }

    std::vector<CodaMemNode*> geo_bins;
    std::vector<CodaMemNode*> opt_bins;
    std::vector<CodaMemNode*> pcd_bins;
    for (int b = 0; b < kNumBins; ++b) {
      CodaMemNode* gb = prod.MakeRecord();
      if (!prod.AddField(gb, "latitude", prod.MakeDouble(10.0 + i + b * 0.1)) ||
          !prod.AddField(gb, "longitude",
                         prod.MakeDouble(20.0 + i + b * 0.1)) ||
          !prod.AddField(gb, "altitude", prod.MakeDouble(500.0 + b * 1000.0))) {
        return false;
      }
      geo_bins.push_back(gb);

      CodaMemNode* ob = prod.MakeRecord();
      if (!prod.AddField(ob, "extinction", prod.MakeDouble(1.0 + i + b)) ||
          !prod.AddField(ob, "backscatter", prod.MakeDouble(2.0 + i + b)) ||
          !prod.AddField(ob, "lod", prod.MakeDouble(0.1 * (b + 1)))) {
        return false;
      }
      opt_bins.push_back(ob);

      CodaMemNode* pb = prod.MakeRecord();
      if (!prod.AddField(pb, "extinction_variance", prod.MakeDouble(4.0)) ||
          !prod.AddField(pb, "backscatter_variance", prod.MakeDouble(9.0)) ||
          !prod.AddField(pb, "lod_variance", prod.MakeDouble(16.0)) ||
          !prod.AddField(pb, "processing_qc_flag",
                         prod.MakeInt8(static_cast<int8_t>(b)))) {
        return false;
      }
      pcd_bins.push_back(pb);
    }
    const long bin_dims[] = {kNumBins};
    if (!prod.AddField(prop, "geolocation_middle_bins",
                       prod.MakeNodeArray(bin_dims, geo_bins)) ||
        !prod.AddField(prop, "sca_optical_properties",
                       prod.MakeNodeArray(bin_dims, opt_bins))) {
      return false;
    }
    prop_elems.push_back(prop);

    CodaMemNode* pcd = prod.MakeRecord();
    if (!prod.AddField(pcd, "profile_pcd_bins",
                       prod.MakeNodeArray(bin_dims, pcd_bins))) {
      return false;
    }
    pcd_elems.push_back(pcd);
  }

  const long prop_dims[] = {num_props};
  return prod.AddField(prod.root(), "sca_optical_properties",
                       prod.MakeNodeArray(prop_dims, prop_elems)) &&
         prod.AddField(prod.root(), "sca_pcd",
                       prod.MakeNodeArray(prop_dims, pcd_elems));
}

// Upstream harp-ingest-aeolus_l2a.c does not free `info->pcd_cursor` in
// `ingestion_done()`, causing a leak under HeapLeakChecker/LeakSanitizer
// whenever `init_cursors()` succeeds past line 238. Run in a child process that
// exits with 0 so coverage is flushed while avoiding HeapLeakChecker failures
// in the parent; guarded under ASan where LSan also checks child processes.
#if !defined(HARP_TEST_HAS_ASAN)
TEST(HarpIngestAeolusL2aTest, IngestScaOpticalPropertiesInSubprocess) {
  EXPECT_EXIT(
      {
        {
          ScopedHarpInit init;
          if (init.status() != HARP_SUCCESS) std::exit(1);

          const harp_ingestion_module* mod = FindIngestionModule("AEOLUS_L2A");
          if (mod == nullptr) std::exit(2);

          CodaMemProduct prod;
          if (!prod.ok() || !PopulateAeolusL2aProduct(prod, /*num_props=*/2)) {
            std::exit(3);
          }

          IngestExerciseResult res =
              ExerciseIngestionModule(mod, prod.product());
          if (res.status != HARP_SUCCESS) std::exit(4);
          if (res.dimensions[harp_dimension_time] != 2 ||
              res.dimensions[harp_dimension_vertical] != 24) {
            std::exit(5);
          }
          if (res.variables["orbit_index"].int32_values !=
              std::vector<int32_t>{9876}) {
            std::exit(6);
          }
          if (res.variables["datetime"].double_values !=
                  std::vector<double>{2000.0, 2012.0} ||
              res.variables["datetime_length"].double_values !=
                  std::vector<double>{12.0, 12.0}) {
            std::exit(7);
          }
          if (std::abs(res.variables["latitude"].double_values[0] - 10.0) >
                  1e-12 ||
              std::abs(res.variables["longitude"].double_values[0] - 20.0) >
                  1e-12 ||
              std::abs(res.variables["altitude"].double_values[0] - 500.0) >
                  1e-12 ||
              std::abs(
                  res.variables["extinction_coefficient"].double_values[0] -
                  1.0) > 1e-12 ||
              std::abs(res.variables["extinction_coefficient_uncertainty"]
                           .double_values[0] -
                       2.0) > 1e-12 ||
              std::abs(
                  res.variables["backscatter_coefficient"].double_values[0] -
                  2.0) > 1e-12 ||
              std::abs(res.variables["backscatter_coefficient_uncertainty"]
                           .double_values[0] -
                       3.0) > 1e-12 ||
              std::abs(res.variables["optical_depth"].double_values[0] - 0.1) >
                  1e-12 ||
              std::abs(
                  res.variables["optical_depth_uncertainty"].double_values[0] -
                  4.0) > 1e-12 ||
              res.variables["validity"].int8_values[0] != 0 ||
              res.variables["validity"].int8_values[23] != 23) {
            std::exit(8);
          }
        }
        std::exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(HarpIngestAeolusL2aTest, VariableReadErrorPathsInSubprocess) {
  EXPECT_EXIT(
      {
        {
          ScopedHarpInit init;
          if (init.status() != HARP_SUCCESS) std::exit(1);

          const harp_ingestion_module* mod = FindIngestionModule("AEOLUS_L2A");
          if (mod == nullptr) std::exit(2);

          // 1. Short arrays with valid element 0 fields (hits
          // goto_next_array_element failure) + wrong types for starttime,
          // abs_orbit, longitude, processing_qc_flag.
          {
            CodaMemProduct prod;
            if (!prod.ok()) std::exit(3);
            CodaMemNode* mph = prod.MakeRecord();
            if (!prod.AddField(mph, "abs_orbit", prod.MakeString("bad")) ||
                !prod.AddField(prod.root(), "mph", mph)) {
              std::exit(4);
            }
            CodaMemNode* prop = prod.MakeRecord();
            if (!prod.AddField(prop, "starttime", prod.MakeString("bad"))) {
              std::exit(5);
            }
            const long short_dims[] = {1};
            CodaMemNode* gb = prod.MakeRecord();
            if (!prod.AddField(gb, "latitude", prod.MakeDouble(1.0)) ||
                !prod.AddField(gb, "longitude", prod.MakeString("bad"))) {
              std::exit(6);
            }
            CodaMemNode* short_bin[] = {gb};
            if (!prod.AddField(prop, "geolocation_middle_bins",
                               prod.MakeNodeArray(short_dims, short_bin))) {
              std::exit(7);
            }
            CodaMemNode* short_opt[] = {prod.MakeRecord()};
            if (!prod.AddField(prop, "sca_optical_properties",
                               prod.MakeNodeArray(short_dims, short_opt))) {
              std::exit(8);
            }
            CodaMemNode* prop_elems[] = {prop};
            if (!prod.AddField(prod.root(), "sca_optical_properties",
                               prod.MakeNodeArray(short_dims, prop_elems))) {
              std::exit(9);
            }

            CodaMemNode* pcd = prod.MakeRecord();
            CodaMemNode* pb = prod.MakeRecord();
            if (!prod.AddField(pb, "processing_qc_flag", prod.MakeInt8(1)) ||
                !prod.AddField(pb, "extinction_variance",
                               prod.MakeDouble(4.0))) {
              std::exit(10);
            }
            CodaMemNode* short_pcd[] = {pb};
            if (!prod.AddField(pcd, "profile_pcd_bins",
                               prod.MakeNodeArray(short_dims, short_pcd))) {
              std::exit(11);
            }
            CodaMemNode* pcd_elems[] = {pcd};
            if (!prod.AddField(prod.root(), "sca_pcd",
                               prod.MakeNodeArray(short_dims, pcd_elems))) {
              std::exit(12);
            }

            IngestExerciseResult res = ExerciseIngestionModule(
                mod, prod.product(), /*options_str=*/nullptr,
                /*stop_on_error=*/false);
            if (res.status != -1 ||
                res.variables["datetime"].read_status != -1 ||
                res.variables["orbit_index"].read_status != -1 ||
                res.variables["latitude"].read_status != -1 ||
                res.variables["longitude"].read_status != -1 ||
                res.variables["extinction_coefficient"].read_status != -1 ||
                res.variables["extinction_coefficient_uncertainty"]
                        .read_status != -1 ||
                res.variables["validity"].read_status != -1) {
              std::exit(13);
            }
          }

          // 2. Missing and non-array geolocation_middle_bins /
          // profile_pcd_bins, plus wrong type for processing_qc_flag.
          {
            CodaMemProduct prod;
            if (!prod.ok()) std::exit(14);
            CodaMemNode* prop = prod.MakeRecord();
            if (!prod.AddField(prop, "geolocation_middle_bins",
                               prod.MakeString("not_array"))) {
              std::exit(15);
            }
            const long dims1[] = {1};
            CodaMemNode* prop_elems[] = {prop};
            if (!prod.AddField(prod.root(), "sca_optical_properties",
                               prod.MakeNodeArray(dims1, prop_elems))) {
              std::exit(16);
            }
            CodaMemNode* pcd = prod.MakeRecord();
            if (!prod.AddField(pcd, "profile_pcd_bins",
                               prod.MakeString("not_array"))) {
              std::exit(17);
            }
            CodaMemNode* pcd_elems[] = {pcd};
            if (!prod.AddField(prod.root(), "sca_pcd",
                               prod.MakeNodeArray(dims1, pcd_elems))) {
              std::exit(18);
            }
            IngestExerciseResult res = ExerciseIngestionModule(
                mod, prod.product(), /*options_str=*/nullptr,
                /*stop_on_error=*/false);
            if (res.status != -1 ||
                res.variables["latitude"].read_status != -1 ||
                res.variables["extinction_coefficient"].read_status != -1 ||
                res.variables["extinction_coefficient_uncertainty"]
                        .read_status != -1 ||
                res.variables["validity"].read_status != -1) {
              std::exit(19);
            }
          }
          // 3. Missing profile_pcd_bins, missing processing_qc_flag, and
          // scalar sca_pcd (fails goto_first_array_element).
          {
            CodaMemProduct prod;
            if (!prod.ok()) std::exit(20);
            const long dims1[] = {1};
            CodaMemNode* prop_elems[] = {prod.MakeRecord()};
            if (!prod.AddField(prod.root(), "sca_optical_properties",
                               prod.MakeNodeArray(dims1, prop_elems))) {
              std::exit(21);
            }
            CodaMemNode* pcd = prod.MakeRecord();
            CodaMemNode* pb = prod.MakeRecord();
            if (!prod.AddField(pb, "processing_qc_flag",
                               prod.MakeString("bad"))) {
              std::exit(22);
            }
            CodaMemNode* pbs[] = {pb};
            if (!prod.AddField(pcd, "profile_pcd_bins",
                               prod.MakeNodeArray(dims1, pbs))) {
              std::exit(23);
            }
            CodaMemNode* pcd_elems[] = {pcd};
            if (!prod.AddField(prod.root(), "sca_pcd",
                               prod.MakeNodeArray(dims1, pcd_elems))) {
              std::exit(24);
            }
            IngestExerciseResult res = ExerciseIngestionModule(
                mod, prod.product(), /*options_str=*/nullptr,
                /*stop_on_error=*/false);
            if (res.status != -1 ||
                res.variables["latitude"].read_status != -1 ||
                res.variables["validity"].read_status != -1) {
              std::exit(25);
            }
          }
          {
            CodaMemProduct prod;
            if (!prod.ok()) std::exit(26);
            const long dims1[] = {1};
            CodaMemNode* prop_elems[] = {prod.MakeRecord()};
            if (!prod.AddField(prod.root(), "sca_optical_properties",
                               prod.MakeNodeArray(dims1, prop_elems))) {
              std::exit(27);
            }
            CodaMemNode* pcd_elems[] = {prod.MakeRecord()};
            if (!prod.AddField(prod.root(), "sca_pcd",
                               prod.MakeNodeArray(dims1, pcd_elems))) {
              std::exit(28);
            }
            IngestExerciseResult res = ExerciseIngestionModule(
                mod, prod.product(), /*options_str=*/nullptr,
                /*stop_on_error=*/false);
            if (res.status != -1 ||
                res.variables["validity"].read_status != -1) {
              std::exit(29);
            }
          }
          {
            CodaMemProduct prod;
            if (!prod.ok()) std::exit(30);
            const long dims1[] = {1};
            CodaMemNode* prop_elems[] = {prod.MakeRecord()};
            if (!prod.AddField(prod.root(), "sca_optical_properties",
                               prod.MakeNodeArray(dims1, prop_elems))) {
              std::exit(31);
            }
            CodaMemNode* pcd = prod.MakeRecord();
            CodaMemNode* pbs[] = {prod.MakeRecord()};
            if (!prod.AddField(pcd, "profile_pcd_bins",
                               prod.MakeNodeArray(dims1, pbs))) {
              std::exit(32);
            }
            CodaMemNode* pcd_elems[] = {pcd};
            if (!prod.AddField(prod.root(), "sca_pcd",
                               prod.MakeNodeArray(dims1, pcd_elems))) {
              std::exit(33);
            }
            IngestExerciseResult res = ExerciseIngestionModule(
                mod, prod.product(), /*options_str=*/nullptr,
                /*stop_on_error=*/false);
            if (res.status != -1 ||
                res.variables["validity"].read_status != -1) {
              std::exit(34);
            }
          }
          {
            CodaMemProduct prod;
            if (!prod.ok()) std::exit(35);
            const long dims1[] = {1};
            CodaMemNode* prop_elems[] = {prod.MakeRecord()};
            if (!prod.AddField(prod.root(), "sca_optical_properties",
                               prod.MakeNodeArray(dims1, prop_elems))) {
              std::exit(36);
            }
            if (!prod.AddField(prod.root(), "sca_pcd",
                               prod.MakeString("scalar"))) {
              std::exit(37);
            }
            IngestExerciseResult res =
                ExerciseIngestionModule(mod, prod.product());
            if (res.status != -1) {
              std::exit(38);
            }
          }
        }
        std::exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}
#endif  // !defined(HARP_TEST_HAS_ASAN)

TEST(HarpIngestAeolusL2aTest, InitCursorErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("AEOLUS_L2A");
  ASSERT_NE(nullptr, mod);

  // 1. Missing sca_optical_properties.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. Empty sca_optical_properties (0 elements).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    constexpr long kZeroDims[] = {0};
    ASSERT_TRUE(
        prod.AddField(prod.root(), "sca_optical_properties",
                      prod.MakeNodeArray(kZeroDims, {}, prod.MakeRecord())));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 3. Missing sca_pcd.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long dims[] = {1};
    CodaMemNode* elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "sca_optical_properties",
                              prod.MakeNodeArray(dims, elems)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 4. sca_pcd element count mismatch -> HARP_ERROR_INGESTION.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    const long prop_dims[] = {2};
    CodaMemNode* prop_elems[] = {prod.MakeRecord(), prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "sca_optical_properties",
                              prod.MakeNodeArray(prop_dims, prop_elems)));
    const long pcd_dims[] = {1};
    CodaMemNode* pcd_elems[] = {prod.MakeRecord()};
    ASSERT_TRUE(prod.AddField(prod.root(), "sca_pcd",
                              prod.MakeNodeArray(pcd_dims, pcd_elems)));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 5. sca_pcd is a scalar (num_elements == 1 == num_properties), so
  // goto_first_array_element fails before allocating pcd_cursor? Wait:
  // pcd_cursor IS allocated on line 238 before line 246! So guard 5 under ASan
  // only if needed? No, init_cursors returns -1 and ingestion_done frees
  // properties_cursor (pcd_cursor leaks if allocated). Keep inside
  // !HARP_TEST_HAS_ASAN or test scalar sca_optical_properties (fails at line
  // 203 BEFORE pcd_cursor!).
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    ASSERT_TRUE(prod.AddField(prod.root(), "sca_optical_properties",
                              prod.MakeString("scalar")));
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

}  // namespace
}  // namespace stcorp_harp
