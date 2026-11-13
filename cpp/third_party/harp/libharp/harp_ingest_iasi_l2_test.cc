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
// Unit tests for IASI Level 2 ingestion module
// (libharp/harp-ingest-iasi_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-iasi_l2.c

#include <cmath>
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

using ::testing::DoubleNear;

constexpr long kScansPerMdr = 30;
constexpr long kPixelsPerScan = 4;
constexpr long kMeasPerMdr = kScansPerMdr * kPixelsPerScan;

struct MdrL2Options {
  bool dummy_record = false;
  bool not_a_record = false;
  bool include_record_header = true;
  bool include_start_time = true;
  bool start_time_wrong_type = false;
  double start_time = 1000.0;

  bool include_earth_location = true;
  bool earth_location_short = false;
  bool earth_location_wrong_type = false;

  bool include_angular_relation = true;
  bool angular_relation_short = false;
  bool angular_relation_wrong_type = false;

  bool include_species = true;
  bool species_short = false;
  bool species_wrong_type = false;
};

struct IasiL2Config {
  bool include_mphr = true;
  bool include_orbit_start = true;
  bool orbit_start_wrong_type = false;
  uint32_t orbit_start = 42424;

  bool include_mdr = true;
  bool mdr_scalar = false;
  bool mdr_empty = false;
  std::vector<MdrL2Options> mdrs;
};

void AddSpeciesArray(CodaMemProduct& prod, CodaMemNode* rec,
                     std::string_view name, const MdrL2Options& opt,
                     double base) {
  if (!opt.include_species) {
    return;
  }
  if (opt.species_short) {
    const long dims[] = {1};
    const double vals[] = {base};
    prod.AddField(rec, name, prod.MakeDoubleArray(dims, vals));
    return;
  }
  if (opt.species_wrong_type) {
    const long dims[] = {kMeasPerMdr};
    std::vector<std::string_view> bad(kMeasPerMdr, "bad");
    prod.AddField(rec, name, prod.MakeStringArray(dims, bad));
    return;
  }
  const long dims[] = {kMeasPerMdr};
  std::vector<double> vals(kMeasPerMdr);
  for (long i = 0; i < kMeasPerMdr; ++i) {
    vals[i] = base + 0.01 * i;
  }
  prod.AddField(rec, name, prod.MakeDoubleArray(dims, vals));
}

CodaMemNode* MakeMdrL2Wrapper(CodaMemProduct& prod, const MdrL2Options& opt,
                              int mdr_index) {
  if (opt.not_a_record) {
    return prod.MakeInt32(0);
  }
  CodaMemNode* wrapper = prod.MakeRecord();
  if (opt.dummy_record) {
    prod.AddUnavailableField(wrapper, "MDR");
    return wrapper;
  }

  CodaMemNode* inner = prod.MakeRecord();
  if (opt.include_record_header) {
    CodaMemNode* hdr = prod.MakeRecord();
    if (opt.include_start_time) {
      prod.AddField(hdr, "RECORD_START_TIME",
                    opt.start_time_wrong_type
                        ? prod.MakeString("bad")
                        : prod.MakeDouble(opt.start_time));
    }
    prod.AddField(inner, "RECORD_HEADER", hdr);
  }

  if (opt.include_earth_location) {
    if (opt.earth_location_short) {
      const long dims[] = {1, 2};
      const double vals[] = {10.0, 20.0};
      prod.AddField(inner, "EARTH_LOCATION", prod.MakeDoubleArray(dims, vals));
    } else if (opt.earth_location_wrong_type) {
      const long dims[] = {kMeasPerMdr, 2};
      std::vector<std::string_view> bad(kMeasPerMdr * 2, "bad");
      prod.AddField(inner, "EARTH_LOCATION", prod.MakeStringArray(dims, bad));
    } else {
      const long dims[] = {kMeasPerMdr, 2};
      std::vector<double> locs(kMeasPerMdr * 2);
      for (long s = 0; s < kScansPerMdr; ++s) {
        const double clat = 10.0 + 0.2 * s + mdr_index;
        const double clon = 20.0 + 0.2 * s + mdr_index;
        // Order: bottom-right, top-right, top-left, bottom-left.
        const double dlat[4] = {-0.1, 0.1, 0.1, -0.1};
        const double dlon[4] = {0.1, 0.1, -0.1, -0.1};
        for (long p = 0; p < kPixelsPerScan; ++p) {
          const long idx = (s * kPixelsPerScan + p) * 2;
          locs[idx] = clat + dlat[p];
          locs[idx + 1] = clon + dlon[p];
        }
      }
      prod.AddField(inner, "EARTH_LOCATION", prod.MakeDoubleArray(dims, locs));
    }
  }

  if (opt.include_angular_relation) {
    if (opt.angular_relation_short) {
      const long dims[] = {1, 4};
      const double vals[] = {30.0, 5.0, 120.0, 200.0};
      prod.AddField(inner, "ANGULAR_RELATION",
                    prod.MakeDoubleArray(dims, vals));
    } else if (opt.angular_relation_wrong_type) {
      const long dims[] = {kMeasPerMdr, 4};
      std::vector<std::string_view> bad(kMeasPerMdr * 4, "bad");
      prod.AddField(inner, "ANGULAR_RELATION", prod.MakeStringArray(dims, bad));
    } else {
      const long dims[] = {kMeasPerMdr, 4};
      std::vector<double> angs(kMeasPerMdr * 4);
      for (long i = 0; i < kMeasPerMdr; ++i) {
        angs[i * 4 + 0] = 30.0 + 0.1 * i;   // solar zenith
        angs[i * 4 + 1] = 5.0 + 0.1 * i;    // sensor zenith
        angs[i * 4 + 2] = 120.0 + 0.1 * i;  // solar azimuth
        angs[i * 4 + 3] = 200.0 + 0.1 * i;  // sensor azimuth
      }
      prod.AddField(inner, "ANGULAR_RELATION",
                    prod.MakeDoubleArray(dims, angs));
    }
  }

  AddSpeciesArray(prod, inner, "INTEGRATED_OZONE", opt, 0.005 + mdr_index);
  AddSpeciesArray(prod, inner, "INTEGRATED_N2O", opt, 0.004 + mdr_index);
  AddSpeciesArray(prod, inner, "INTEGRATED_CO", opt, 0.003 + mdr_index);
  AddSpeciesArray(prod, inner, "INTEGRATED_CH4", opt, 0.002 + mdr_index);
  AddSpeciesArray(prod, inner, "INTEGRATED_CO2", opt, 0.006 + mdr_index);

  prod.AddField(wrapper, "MDR", inner);
  return wrapper;
}

void PopulateIasiL2Product(CodaMemProduct& prod, const IasiL2Config& cfg) {
  if (cfg.include_mphr) {
    CodaMemNode* mphr = prod.MakeRecord();
    if (cfg.include_orbit_start) {
      ASSERT_TRUE(prod.AddField(mphr, "ORBIT_START",
                                cfg.orbit_start_wrong_type
                                    ? prod.MakeString("bad")
                                    : prod.MakeUint32(cfg.orbit_start)));
    }
    ASSERT_TRUE(prod.AddField(prod.root(), "MPHR", mphr));
  }

  if (cfg.include_mdr) {
    if (cfg.mdr_scalar) {
      ASSERT_TRUE(prod.AddField(prod.root(), "MDR", prod.MakeInt32(1)));
    } else if (cfg.mdr_empty) {
      CodaMemNode* sample = MakeMdrL2Wrapper(prod, MdrL2Options{}, 0);
      const long dims[] = {0};
      ASSERT_TRUE(prod.AddField(prod.root(), "MDR",
                                prod.MakeNodeArray(dims, {}, sample)));
    } else {
      std::vector<CodaMemNode*> elems;
      elems.reserve(cfg.mdrs.size());
      for (size_t i = 0; i < cfg.mdrs.size(); ++i) {
        elems.push_back(
            MakeMdrL2Wrapper(prod, cfg.mdrs[i], static_cast<int>(i)));
      }
      const long dims[] = {static_cast<long>(elems.size())};
      ASSERT_TRUE(
          prod.AddField(prod.root(), "MDR", prod.MakeNodeArray(dims, elems)));
    }
  }
}

TEST(HarpIngestIasiL2Test, IngestSuccessWithDummyAndMultiMdr) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  const harp_ingestion_module* mod = FindIngestionModule("IASI_L2");
  ASSERT_NE(nullptr, mod);

  MdrL2Options dummy;
  dummy.dummy_record = true;

  MdrL2Options mdr1;
  mdr1.start_time = 1000.0;

  MdrL2Options mdr2;
  mdr2.start_time = 2000.0;

  IasiL2Config cfg;
  cfg.mdrs = {dummy, mdr1, mdr2};

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateIasiL2Product(prod, cfg);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(2 * kMeasPerMdr, res.dimensions[harp_dimension_time]);

  EXPECT_EQ(42424, res.variables["orbit_index"].int32_values[0]);
  // Note integer division ((index % 120) / 4) * 8 / 37 in upstream.
  EXPECT_DOUBLE_EQ(1000.0, res.variables["datetime"].double_values[0]);
  EXPECT_DOUBLE_EQ(1001.0, res.variables["datetime"].double_values[20]);
  EXPECT_DOUBLE_EQ(2000.0,
                   res.variables["datetime"].double_values[kMeasPerMdr]);

  EXPECT_DOUBLE_EQ(10.9, res.variables["latitude"].double_values[0]);
  EXPECT_DOUBLE_EQ(21.1, res.variables["longitude"].double_values[0]);
  EXPECT_EQ(4 * 2 * kMeasPerMdr,
            res.variables["latitude_bounds"].double_values.size());
  EXPECT_EQ(4 * 2 * kMeasPerMdr,
            res.variables["longitude_bounds"].double_values.size());
  for (double v : res.variables["latitude_bounds"].double_values) {
    EXPECT_FALSE(std::isnan(v));
  }
  for (double v : res.variables["longitude_bounds"].double_values) {
    EXPECT_FALSE(std::isnan(v));
  }

  EXPECT_DOUBLE_EQ(30.0, res.variables["solar_zenith_angle"].double_values[0]);
  EXPECT_DOUBLE_EQ(5.0, res.variables["sensor_zenith_angle"].double_values[0]);
  EXPECT_DOUBLE_EQ(120.0,
                   res.variables["solar_azimuth_angle"].double_values[0]);
  EXPECT_DOUBLE_EQ(200.0,
                   res.variables["sensor_azimuth_angle"].double_values[0]);

  EXPECT_THAT(res.variables["O3_column_density"].double_values[0],
              DoubleNear(1.005, 1e-9));
  EXPECT_THAT(res.variables["N2O_column_density"].double_values[0],
              DoubleNear(1.004, 1e-9));
  EXPECT_THAT(res.variables["CO_column_density"].double_values[0],
              DoubleNear(1.003, 1e-9));
  EXPECT_THAT(res.variables["CH4_column_density"].double_values[0],
              DoubleNear(1.002, 1e-9));
  EXPECT_THAT(res.variables["CO2_column_density"].double_values[0],
              DoubleNear(1.006, 1e-9));

  EXPECT_EQ(0, res.variables["scan_subindex"].int8_values[0]);
  EXPECT_EQ(119, res.variables["scan_subindex"].int8_values[119]);
  EXPECT_EQ(0, res.variables["scan_subindex"].int8_values[120]);
}

TEST(HarpIngestIasiL2Test, InitAndReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("IASI_L2");
  ASSERT_NE(nullptr, mod);

  auto run_with = [&](const IasiL2Config& cfg, bool stop_on_error = true) {
    CodaMemProduct prod;
    if (!prod.ok()) return IngestExerciseResult{};
    PopulateIasiL2Product(prod, cfg);
    return ExerciseIngestionModule(mod, prod.product(),
                                   /*options_str=*/nullptr, stop_on_error);
  };

  // Init errors.
  {
    IasiL2Config cfg;
    cfg.include_mdr = false;
    EXPECT_EQ(-1, run_with(cfg).status);
  }
  {
    IasiL2Config cfg;
    cfg.mdr_scalar = true;
    EXPECT_EQ(-1, run_with(cfg).status);
  }
  {
    IasiL2Config cfg;
    cfg.mdr_empty = true;
    IngestExerciseResult res = run_with(cfg);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_NO_DATA, res.failed_errno);
  }
  {
    IasiL2Config cfg;
    MdrL2Options m;
    m.not_a_record = true;
    cfg.mdrs = {m};
    EXPECT_EQ(-1, run_with(cfg).status);
  }
  {
    IasiL2Config cfg;
    MdrL2Options m;
    m.dummy_record = true;
    cfg.mdrs = {m};
    IngestExerciseResult res = run_with(cfg);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_NO_DATA, res.failed_errno);
  }

  // Variable read errors.
  {
    IasiL2Config cfg;
    MdrL2Options m;
    m.include_start_time = false;
    cfg.mdrs = {m};
    EXPECT_EQ(-1, run_with(cfg, false).variables["datetime"].read_status);
  }
  {
    IasiL2Config cfg;
    MdrL2Options m;
    m.start_time_wrong_type = true;
    cfg.mdrs = {m};
    EXPECT_EQ(-1, run_with(cfg, false).variables["datetime"].read_status);
  }
  {
    IasiL2Config cfg;
    cfg.mdrs = {MdrL2Options{}};
    cfg.include_orbit_start = false;
    EXPECT_EQ(-1, run_with(cfg, false).variables["orbit_index"].read_status);
  }
  {
    IasiL2Config cfg;
    cfg.mdrs = {MdrL2Options{}};
    cfg.orbit_start_wrong_type = true;
    EXPECT_EQ(-1, run_with(cfg, false).variables["orbit_index"].read_status);
  }
  {
    IasiL2Config cfg;
    MdrL2Options m;
    m.include_earth_location = false;
    cfg.mdrs = {m};
    IngestExerciseResult res = run_with(cfg, false);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
  }
  {
    IasiL2Config cfg;
    MdrL2Options m;
    m.earth_location_short = true;
    cfg.mdrs = {m};
    IngestExerciseResult res = run_with(cfg, false);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
  }
  {
    IasiL2Config cfg;
    MdrL2Options m;
    m.earth_location_wrong_type = true;
    cfg.mdrs = {m};
    IngestExerciseResult res = run_with(cfg, false);
    EXPECT_EQ(-1, res.variables["latitude"].read_status);
    EXPECT_EQ(-1, res.variables["longitude"].read_status);
    EXPECT_EQ(-1, res.variables["latitude_bounds"].read_status);
    EXPECT_EQ(-1, res.variables["longitude_bounds"].read_status);
  }
  {
    IasiL2Config cfg;
    MdrL2Options m;
    m.include_angular_relation = false;
    cfg.mdrs = {m};
    EXPECT_EQ(-1,
              run_with(cfg, false).variables["solar_zenith_angle"].read_status);
  }
  {
    IasiL2Config cfg;
    MdrL2Options m;
    m.angular_relation_short = true;
    cfg.mdrs = {m};
    EXPECT_EQ(-1,
              run_with(cfg, false).variables["solar_zenith_angle"].read_status);
  }
  {
    IasiL2Config cfg;
    MdrL2Options m;
    m.angular_relation_wrong_type = true;
    cfg.mdrs = {m};
    EXPECT_EQ(-1,
              run_with(cfg, false).variables["solar_zenith_angle"].read_status);
  }
  {
    IasiL2Config cfg;
    MdrL2Options m;
    m.include_species = false;
    cfg.mdrs = {m};
    EXPECT_EQ(-1,
              run_with(cfg, false).variables["O3_column_density"].read_status);
  }
  {
    IasiL2Config cfg;
    MdrL2Options m;
    m.species_short = true;
    cfg.mdrs = {m};
    EXPECT_EQ(-1,
              run_with(cfg, false).variables["O3_column_density"].read_status);
  }
  {
    IasiL2Config cfg;
    MdrL2Options m;
    m.species_wrong_type = true;
    cfg.mdrs = {m};
    EXPECT_EQ(-1,
              run_with(cfg, false).variables["O3_column_density"].read_status);
  }
}

}  // namespace
}  // namespace stcorp_harp
