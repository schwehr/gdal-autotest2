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
// Unit tests for ODIN SMR Level 2 ingestion module
// (libharp/harp-ingest-smr_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-smr_l2.c

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

#if defined(__SANITIZE_ADDRESS__)
#define HARP_TEST_HAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HARP_TEST_HAS_ASAN 1
#endif
#endif

constexpr double kSecondsFrom1993To2000 = 220838400.0 + 5.0;
constexpr long kSpeciesNameWidth = 12;

constexpr std::array<std::string_view, 18> kAllSpeciesPrefixes = {
    "ClO_",    "CO_",     "H2O_",    "H2O-161_", "H2O-162_", "H2O-181_",
    "HNO3_",   "HO2_",    "N2O_",    "NO_",      "O3_",      "O3-666_",
    "O3-667_", "O3-668_", "O3-686_", "OBrO_",    "TEMP_",    "PRES_",
};

struct SmrL2Config {
  bool set_product_info = true;
  long num_profiles = 2;
  std::vector<std::string> species = std::vector<std::string>(
      kAllSpeciesPrefixes.begin(), kAllSpeciesPrefixes.end());

  bool include_geolocation = true;
  bool include_latitude = true;
  bool latitude_scalar = false;
  bool latitude_wrong_type = false;
  bool include_longitude = true;
  bool include_time = true;
  bool time_wrong_type = false;
  bool include_sunzd = true;

  bool include_retrieval = true;
  bool include_naltitudes = true;
  bool naltitudes_scalar = false;
  bool naltitudes_wrong_type = false;
  bool include_species_names = true;
  bool species_names_scalar = false;
  bool species_names_wrong_type = false;
  bool species_names_no_trailing_row = false;

  bool include_data = true;
  bool include_altitudes = true;
  bool altitudes_short = false;
  bool altitudes_wrong_type = false;
  bool include_profiles = true;
  bool profiles_short = false;
  bool profiles_wrong_type = false;
  bool include_total_error = true;
  bool total_error_short = false;
  bool total_error_wrong_type = false;
};

void PopulateSmrL2Product(CodaMemProduct& prod, const SmrL2Config& cfg) {
  if (cfg.set_product_info) {
    ASSERT_TRUE(prod.SetProductInfo("ODIN_SMR", "L2", 1));
  }

  CodaMemNode* ghz = prod.MakeRecord();
  CodaMemNode* vgroup = prod.MakeRecord();

  if (cfg.include_geolocation) {
    CodaMemNode* geo = prod.MakeRecord();
    const long p_dims[] = {cfg.num_profiles};
    if (cfg.include_latitude) {
      if (cfg.latitude_scalar) {
        ASSERT_TRUE(prod.AddField(geo, "Latitude", prod.MakeDouble(45.0)));
      } else if (cfg.latitude_wrong_type) {
        std::vector<std::string_view> bad(cfg.num_profiles, "bad");
        ASSERT_TRUE(
            prod.AddField(geo, "Latitude", prod.MakeStringArray(p_dims, bad)));
      } else {
        std::vector<double> lats(cfg.num_profiles);
        for (long i = 0; i < cfg.num_profiles; ++i) {
          lats[i] = 45.0 + i;
        }
        ASSERT_TRUE(
            prod.AddField(geo, "Latitude", prod.MakeDoubleArray(p_dims, lats)));
      }
    }
    if (cfg.include_longitude) {
      std::vector<double> lons(cfg.num_profiles);
      for (long i = 0; i < cfg.num_profiles; ++i) {
        lons[i] = 12.0 + i;
      }
      ASSERT_TRUE(
          prod.AddField(geo, "Longitude", prod.MakeDoubleArray(p_dims, lons)));
    }
    if (cfg.include_time) {
      if (cfg.time_wrong_type) {
        std::vector<std::string_view> bad(cfg.num_profiles, "bad");
        ASSERT_TRUE(
            prod.AddField(geo, "Time", prod.MakeStringArray(p_dims, bad)));
      } else {
        std::vector<double> times(cfg.num_profiles);
        for (long i = 0; i < cfg.num_profiles; ++i) {
          times[i] = kSecondsFrom1993To2000 + 100.0 + i * 10.0;
        }
        ASSERT_TRUE(
            prod.AddField(geo, "Time", prod.MakeDoubleArray(p_dims, times)));
      }
    }
    if (cfg.include_sunzd) {
      std::vector<double> szas(cfg.num_profiles, 35.0);
      ASSERT_TRUE(
          prod.AddField(geo, "SunZD", prod.MakeDoubleArray(p_dims, szas)));
    }
    ASSERT_TRUE(prod.AddField(vgroup, "Geolocation", geo));
  }

  const long num_species = static_cast<long>(cfg.species.size());
  long total_alt_elems = 0;
  if (cfg.include_retrieval) {
    CodaMemNode* ret = prod.MakeRecord();
    if (cfg.include_naltitudes) {
      if (cfg.naltitudes_scalar) {
        ASSERT_TRUE(prod.AddField(ret, "Naltitudes", prod.MakeInt16(2)));
      } else if (cfg.naltitudes_wrong_type) {
        const long n_dims[] = {cfg.num_profiles * num_species};
        std::vector<std::string_view> bad(n_dims[0], "bad");
        ASSERT_TRUE(prod.AddField(ret, "Naltitudes",
                                  prod.MakeStringArray(n_dims, bad)));
      } else {
        const long n_dims[] = {cfg.num_profiles * num_species};
        std::vector<int16_t> nalts(n_dims[0]);
        for (long p = 0; p < cfg.num_profiles; ++p) {
          // Profile 0 has 1 altitude, profile 1 has 2 altitudes so max = 2
          // and profile 0 advances by (max - 1) = 1.
          const int16_t count = static_cast<int16_t>(p + 1);
          for (long s = 0; s < num_species; ++s) {
            nalts[p * num_species + s] = count;
            total_alt_elems += count;
          }
        }
        ASSERT_TRUE(prod.AddField(ret, "Naltitudes",
                                  prod.MakeInt16Array(n_dims, nalts)));
      }
    }

    if (cfg.include_species_names) {
      if (cfg.species_names_scalar) {
        ASSERT_TRUE(prod.AddField(ret, "SpeciesNames", prod.MakeChar('O')));
      } else if (cfg.species_names_wrong_type) {
        const long s_dims[] = {num_species + 1, kSpeciesNameWidth};
        std::vector<int16_t> bad(s_dims[0] * s_dims[1], 0);
        ASSERT_TRUE(prod.AddField(ret, "SpeciesNames",
                                  prod.MakeInt16Array(s_dims, bad)));
      } else {
        const long rows =
            cfg.species_names_no_trailing_row ? num_species : (num_species + 1);
        const long s_dims[] = {rows, kSpeciesNameWidth};
        std::vector<char> chars(rows * kSpeciesNameWidth, ' ');
        for (long s = 0; s < num_species; ++s) {
          const std::string& name = cfg.species[s];
          for (size_t c = 0; c < name.size() && c < kSpeciesNameWidth; ++c) {
            chars[s * kSpeciesNameWidth + c] = name[c];
          }
        }
        ASSERT_TRUE(prod.AddField(ret, "SpeciesNames",
                                  prod.MakeCharArray(s_dims, chars)));
      }
    }
    ASSERT_TRUE(prod.AddField(vgroup, "Retrieval", ret));
  }

  if (cfg.include_data) {
    CodaMemNode* data = prod.MakeRecord();
    // +1 trailing dummy element because get_main_data calls
    // coda_cursor_goto_next_array_element after reading the last element.
    const long len = (total_alt_elems > 0 ? total_alt_elems : 1) + 1;
    const long d_dims[] = {len};

    auto add_data_field = [&](std::string_view fname, bool include,
                              bool is_short, bool wrong_type, double scale) {
      if (!include) return;
      if (is_short) {
        const long s_dims[] = {1};
        const double vals[] = {1.0};
        ASSERT_TRUE(
            prod.AddField(data, fname, prod.MakeDoubleArray(s_dims, vals)));
      } else if (wrong_type) {
        std::vector<std::string_view> bad(len, "bad");
        ASSERT_TRUE(
            prod.AddField(data, fname, prod.MakeStringArray(d_dims, bad)));
      } else {
        std::vector<double> vals(len);
        for (long i = 0; i < len; ++i) {
          vals[i] = scale * (i + 1);
        }
        ASSERT_TRUE(
            prod.AddField(data, fname, prod.MakeDoubleArray(d_dims, vals)));
      }
    };

    add_data_field("Altitudes", cfg.include_altitudes, cfg.altitudes_short,
                   cfg.altitudes_wrong_type, 10.0);
    add_data_field("Profiles", cfg.include_profiles, cfg.profiles_short,
                   cfg.profiles_wrong_type, 0.01);
    add_data_field("TotalError", cfg.include_total_error, cfg.total_error_short,
                   cfg.total_error_wrong_type, 0.001);

    ASSERT_TRUE(prod.AddField(vgroup, "Data", data));
  }

  ASSERT_TRUE(prod.AddField(ghz, "Data_Vgroup", vgroup));
  ASSERT_TRUE(prod.AddField(prod.root(), "GHz", ghz));
}

TEST(HarpIngestSmrL2Test, InitErrorsBeforeAllocation) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("SMR_L2");
  ASSERT_NE(nullptr, mod);

  auto expect_fail = [&](const SmrL2Config& cfg) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateSmrL2Product(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  };

  // 1. Missing Latitude.
  {
    SmrL2Config cfg;
    cfg.include_latitude = false;
    expect_fail(cfg);
  }
  // 2. Missing Naltitudes.
  {
    SmrL2Config cfg;
    cfg.include_naltitudes = false;
    expect_fail(cfg);
  }
}

#if !defined(HARP_TEST_HAS_ASAN)
// Upstream harp-ingest-smr_l2.c allocates info->num_altitudes and
// info->sum_prev_altitudes in init_dimensions() without freeing them in
// ingestion_done(). Run in an EXPECT_EXIT subprocess so coverage flushes on
// std::exit(0) without failing HeapLeakChecker in the parent; guarded under
// ASan where LSan also checks child processes.
[[noreturn]] void RunIngestAllSpeciesAndExit() {
  {
    ScopedHarpInit init;
    if (init.status() != HARP_SUCCESS) std::exit(1);
    const harp_ingestion_module* mod = FindIngestionModule("SMR_L2");
    if (mod == nullptr) std::exit(2);

    SmrL2Config cfg;
    CodaMemProduct prod;
    if (!prod.ok()) std::exit(3);
    PopulateSmrL2Product(prod, cfg);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    if (res.status != HARP_SUCCESS) std::exit(4);
    if (res.dimensions[harp_dimension_time] != 2 ||
        res.dimensions[harp_dimension_vertical] != 2) {
      std::exit(5);
    }
    if (res.variables["datetime"].double_values[0] != 100.0 ||
        res.variables["datetime"].double_values[1] != 110.0 ||
        res.variables["latitude"].double_values[0] != 45.0 ||
        res.variables["longitude"].double_values[0] != 12.0 ||
        res.variables["solar_zenith_angle"].double_values[0] != 35.0) {
      std::exit(6);
    }
    if (res.variables["altitude"].double_values[0] != 10.0 ||
        res.variables["altitude"].double_values[2] != 190.0 ||
        res.variables["altitude"].double_values[3] != 200.0) {
      std::exit(7);
    }
    if (!res.variables["ClO_volume_mixing_ratio"].included ||
        !res.variables["BrO2_volume_mixing_ratio"].included ||
        !res.variables["O3_686_volume_mixing_ratio"].included ||
        !res.variables["temperature"].included ||
        !res.variables["pressure"].included) {
      std::exit(8);
    }
    if (std::abs(res.variables["ClO_volume_mixing_ratio"].double_values[0] -
                 0.01) > 1e-9 ||
        std::abs(res.variables["ClO_volume_mixing_ratio_uncertainty"]
                     .double_values[0] -
                 0.001) > 1e-9) {
      std::exit(9);
    }
  }
  std::exit(0);
}

TEST(HarpIngestSmrL2Test, IngestAllSpeciesSuccess) {
  EXPECT_EXIT(RunIngestAllSpeciesAndExit(), ::testing::ExitedWithCode(0), "");
}

[[noreturn]] void RunSingleSpeciesExcludesOthersAndExit() {
  {
    ScopedHarpInit init;
    if (init.status() != HARP_SUCCESS) std::exit(1);
    const harp_ingestion_module* mod = FindIngestionModule("SMR_L2");
    if (mod == nullptr) std::exit(2);

    SmrL2Config cfg;
    cfg.species = {"O3_"};
    CodaMemProduct prod;
    if (!prod.ok()) std::exit(3);
    PopulateSmrL2Product(prod, cfg);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    if (res.status != HARP_SUCCESS) std::exit(4);
    if (!res.variables["O3_volume_mixing_ratio"].included ||
        res.variables["ClO_volume_mixing_ratio"].included ||
        res.variables["temperature"].included ||
        res.variables["pressure"].included) {
      std::exit(5);
    }
  }
  std::exit(0);
}

TEST(HarpIngestSmrL2Test, SingleSpeciesExcludesOthers) {
  EXPECT_EXIT(RunSingleSpeciesExcludesOthersAndExit(),
              ::testing::ExitedWithCode(0), "");
}

[[noreturn]] void RunSmrInitAndReadErrorPathsAndExit() {
  {
    ScopedHarpInit init;
    if (init.status() != HARP_SUCCESS) std::exit(1);
    const harp_ingestion_module* mod = FindIngestionModule("SMR_L2");
    if (mod == nullptr) std::exit(2);

    auto run_with = [&](const SmrL2Config& cfg, bool stop_on_error = true) {
      CodaMemProduct prod;
      if (!prod.ok()) std::exit(3);
      PopulateSmrL2Product(prod, cfg);
      return ExerciseIngestionModule(mod, prod.product(),
                                     /*options_str=*/nullptr, stop_on_error);
    };

    // Init errors after Naltitudes allocation.
    {
      SmrL2Config cfg;
      cfg.naltitudes_scalar = true;
      if (run_with(cfg).status != -1) std::exit(9);
    }
    {
      SmrL2Config cfg;
      cfg.naltitudes_wrong_type = true;
      if (run_with(cfg).status != -1) std::exit(10);
    }
    {
      SmrL2Config cfg;
      cfg.include_species_names = false;
      if (run_with(cfg).status != -1) std::exit(11);
    }
    {
      SmrL2Config cfg;
      cfg.species_names_scalar = true;
      if (run_with(cfg).status != -1) std::exit(12);
    }
    {
      SmrL2Config cfg;
      cfg.species_names_wrong_type = true;
      if (run_with(cfg).status != -1) std::exit(13);
    }
    {
      SmrL2Config cfg;
      cfg.species_names_no_trailing_row = true;
      if (run_with(cfg).status != -1) std::exit(14);
    }

    // Geolocation variable errors.
    {
      SmrL2Config cfg;
      cfg.include_time = false;
      if (run_with(cfg, false).variables["datetime"].read_status != -1) {
        std::exit(15);
      }
    }
    {
      SmrL2Config cfg;
      cfg.time_wrong_type = true;
      if (run_with(cfg, false).variables["datetime"].read_status != -1) {
        std::exit(16);
      }
    }
    {
      SmrL2Config cfg;
      cfg.include_longitude = false;
      if (run_with(cfg, false).variables["longitude"].read_status != -1) {
        std::exit(17);
      }
    }
    {
      SmrL2Config cfg;
      cfg.include_sunzd = false;
      if (run_with(cfg, false).variables["solar_zenith_angle"].read_status !=
          -1) {
        std::exit(18);
      }
    }
    {
      SmrL2Config cfg;
      cfg.latitude_wrong_type = true;
      if (run_with(cfg, false).variables["latitude"].read_status != -1) {
        std::exit(19);
      }
    }

    // Data variable errors (missing, short, wrong type).
    {
      SmrL2Config cfg;
      cfg.include_data = false;
      IngestExerciseResult res = run_with(cfg, false);
      if (res.variables["altitude"].read_status != -1 ||
          res.variables["O3_volume_mixing_ratio"].read_status != -1 ||
          res.variables["O3_volume_mixing_ratio_uncertainty"].read_status !=
              -1) {
        std::exit(20);
      }
    }
    {
      SmrL2Config cfg;
      cfg.include_altitudes = false;
      if (run_with(cfg, false).variables["altitude"].read_status != -1) {
        std::exit(21);
      }
    }
    {
      SmrL2Config cfg;
      cfg.altitudes_short = true;
      if (run_with(cfg, false).variables["altitude"].read_status != -1) {
        std::exit(22);
      }
    }
    {
      SmrL2Config cfg;
      cfg.altitudes_wrong_type = true;
      if (run_with(cfg, false).variables["altitude"].read_status != -1) {
        std::exit(23);
      }
    }
    {
      SmrL2Config cfg;
      cfg.profiles_short = true;
      if (run_with(cfg, false)
              .variables["O3_volume_mixing_ratio"]
              .read_status != -1) {
        std::exit(24);
      }
    }
    {
      SmrL2Config cfg;
      cfg.profiles_wrong_type = true;
      if (run_with(cfg, false)
              .variables["O3_volume_mixing_ratio"]
              .read_status != -1) {
        std::exit(25);
      }
    }
  }
  std::exit(0);
}

TEST(HarpIngestSmrL2Test, InitAndReadErrorPaths) {
  EXPECT_EXIT(RunSmrInitAndReadErrorPathsAndExit(),
              ::testing::ExitedWithCode(0), "");
}
#endif

}  // namespace
}  // namespace stcorp_harp
