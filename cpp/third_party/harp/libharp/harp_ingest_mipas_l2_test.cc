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
// Unit tests for the ENVISAT MIPAS Level 2 ingestion module
// (libharp/harp-ingest-mipas_l2.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-mipas_l2.c

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/stcorp_coda/libcoda/coda.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::DoubleEq;
using ::testing::ElementsAre;
using ::testing::IsNan;

constexpr std::string_view kAllSpeciesNames[] = {
    "H2O",  "O3",  "HNO3", "CH4",  "N2O", "NO2", "F11", "ClNO",
    "N2O5", "F12", "COF2", "CCL4", "HCN", "F14", "F22",
};

constexpr std::string_view kSpeciesMdsNames[] = {
    "h2o_retrieval_mds", "o3_retrieval_mds",   "hno3_retrieval_mds",
    "ch4_retrieval_mds", "n2o_retrieval_mds",  "no2_retrieval_mds",
    "f11_retrieval_mds", "clno_retrieval_mds", "n2o5_retrieval_mds",
    "f12_retrieval_mds", "cof2_retrieval_mds", "ccl4_retrieval_mds",
    "hcn_retrieval_mds", "f14_retrieval_mds",  "f22_retrieval_mds",
};

struct MipasL2Config {
  bool set_product_info = true;
  int product_version = 4;
  std::optional<std::string> order_of_species;
  bool include_mph = true;
  bool include_abs_orbit = true;
  bool bad_abs_orbit_type = false;
  bool include_sph = true;
  bool include_order_of_species = true;
  bool bad_order_of_species_type = false;

  long num_scans = 2;
  std::vector<long> num_altitudes = {3, 2};
  bool include_scan_info = true;
  bool scan_info_not_array = false;
  bool include_zpd_crossing_time = true;
  bool include_tangent_altitude = true;
  bool bad_tangent_altitude_type = false;
  bool include_retrieval_pt = true;
  bool include_lrv_pt_flag = true;
  bool bad_lrv_pt_flag_type = false;
  bool include_retrieval_vmr = true;
  bool empty_retrieval_vmr = false;
  bool short_retrieval_vmr = false;
  bool include_lrv_vmr_flag = true;
  bool bad_lrv_vmr_flag_type = false;

  bool include_geo = true;
  long geo_count = -1;  // -1 means match num_scans.
  bool include_dsr_time = true;
  bool bad_dsr_time_type = false;
  bool include_loc_mid = true;
  bool include_latitude = true;
  bool include_angles = true;

  bool include_pt = true;
  long pt_count = -1;  // -1 means match num_scans.
  bool include_tan_press = true;
  bool bad_tan_press_type = false;
  bool include_tan_press_var_cov = true;
  bool empty_tan_press_var_cov = false;
  bool bad_tan_press_var_cov_type = false;

  bool include_species_mds = true;
  int missing_species_mds_index = -1;
  long species_mds_count = -1;  // -1 means match num_scans.
  bool include_conc_alt = true;
  bool include_conc_var_cov = true;
  bool include_vmr = true;
  bool include_avg_kernel = true;
  bool bad_avg_kernel_type = false;
};

int ExpectedNumSpecies(int version) {
  if (version < 3) return 6;
  if (version < 4) return 10;
  return 15;
}

std::string DefaultOrderOfSpecies(int num_species) {
  std::string out;
  for (int i = 0; i < num_species; ++i) {
    if (i > 0) out += ',';
    out += kAllSpeciesNames[i];
  }
  return out;
}

std::vector<uint8_t> MakePtLrv(long num_alt) {
  if (num_alt == 3) return {1, 0, 1};
  return std::vector<uint8_t>(static_cast<size_t>(num_alt), 1);
}

std::vector<uint8_t> MakeSpeciesLrv(long num_alt, long scan_idx) {
  if (num_alt == 3) return {1, 1, 0};
  if (num_alt == 2 && scan_idx == 1) return {0, 1};
  return std::vector<uint8_t>(static_cast<size_t>(num_alt), 1);
}

long CountActive(const std::vector<uint8_t>& lrv) {
  long count = 0;
  for (uint8_t v : lrv) {
    if (v != 0) ++count;
  }
  return count;
}

void BuildMipasL2Product(CodaMemProduct& prod, const MipasL2Config& cfg) {
  const int num_species = ExpectedNumSpecies(cfg.product_version);
  if (cfg.set_product_info) {
    ASSERT_TRUE(prod.SetProductInfo("ENVISAT_MIPAS", "MIP_NL__2P",
                                    cfg.product_version));
  }

  if (cfg.include_mph) {
    CodaMemNode* mph = prod.MakeRecord();
    ASSERT_NE(nullptr, mph);
    if (cfg.include_abs_orbit) {
      ASSERT_TRUE(prod.AddField(mph, "abs_orbit",
                                cfg.bad_abs_orbit_type
                                    ? prod.MakeString("bad")
                                    : prod.MakeInt32(12345)));
    }
    ASSERT_TRUE(prod.AddField(prod.root(), "mph", mph));
  }

  if (cfg.include_sph) {
    CodaMemNode* sph = prod.MakeRecord();
    ASSERT_NE(nullptr, sph);
    if (cfg.include_order_of_species) {
      const std::string order = cfg.order_of_species.has_value()
                                    ? *cfg.order_of_species
                                    : DefaultOrderOfSpecies(num_species);
      ASSERT_TRUE(prod.AddField(sph, "order_of_species",
                                cfg.bad_order_of_species_type
                                    ? prod.MakeInt32(99)
                                    : prod.MakeString(order)));
    }
    ASSERT_TRUE(prod.AddField(prod.root(), "sph", sph));
  }

  auto make_scan_record = [&](long scan_idx, long num_alt) -> CodaMemNode* {
    CodaMemNode* scan = prod.MakeRecord();
    if (scan == nullptr) return nullptr;

    if (cfg.include_zpd_crossing_time) {
      const long dims[] = {num_alt};
      std::vector<double> zpd(static_cast<size_t>(num_alt), 100.0);
      if (!prod.AddField(scan, "zpd_crossing_time",
                         prod.MakeDoubleArray(dims, zpd))) {
        return nullptr;
      }
    }
    if (cfg.include_tangent_altitude) {
      const long dims[] = {num_alt};
      if (cfg.bad_tangent_altitude_type) {
        std::vector<std::string_view> bad(static_cast<size_t>(num_alt), "bad");
        if (!prod.AddField(scan, "tangent_altitude_los",
                           prod.MakeStringArray(dims, bad))) {
          return nullptr;
        }
      } else {
        std::vector<double> alts(static_cast<size_t>(num_alt));
        for (long k = 0; k < num_alt; ++k) {
          alts[static_cast<size_t>(k)] =
              static_cast<double>((num_alt - k) * 10 + scan_idx * 5);
        }
        if (!prod.AddField(scan, "tangent_altitude_los",
                           prod.MakeDoubleArray(dims, alts))) {
          return nullptr;
        }
      }
    }
    if (cfg.include_retrieval_pt) {
      CodaMemNode* rpt = prod.MakeRecord();
      if (rpt == nullptr) return nullptr;
      if (cfg.include_lrv_pt_flag) {
        const long dims[] = {num_alt};
        if (cfg.bad_lrv_pt_flag_type) {
          std::vector<std::string_view> bad(static_cast<size_t>(num_alt), "x");
          if (!prod.AddField(rpt, "lrv_p_t_flag",
                             prod.MakeStringArray(dims, bad))) {
            return nullptr;
          }
        } else {
          std::vector<uint8_t> pt_lrv = MakePtLrv(num_alt);
          if (!prod.AddField(rpt, "lrv_p_t_flag",
                             prod.MakeUint8Array(dims, pt_lrv))) {
            return nullptr;
          }
        }
      }
      if (!prod.AddField(scan, "retrieval_p_t", rpt)) return nullptr;
    }
    if (cfg.include_retrieval_vmr) {
      int vmr_len = cfg.empty_retrieval_vmr
                        ? 0
                        : (cfg.short_retrieval_vmr ? 1 : num_species);
      if (vmr_len == 0) {
        CodaMemNode* tmpl = prod.MakeRecord();
        const long dims[] = {0};
        if (!prod.AddField(scan, "retrieval_vmr",
                           prod.MakeNodeArray(dims, {}, tmpl))) {
          return nullptr;
        }
      } else {
        std::vector<CodaMemNode*> elems;
        elems.reserve(static_cast<size_t>(vmr_len));
        for (int s = 0; s < vmr_len; ++s) {
          CodaMemNode* item = prod.MakeRecord();
          if (item == nullptr) return nullptr;
          if (cfg.include_lrv_vmr_flag) {
            const long dims[] = {num_alt};
            if (cfg.bad_lrv_vmr_flag_type) {
              std::vector<std::string_view> bad(static_cast<size_t>(num_alt),
                                                "x");
              if (!prod.AddField(item, "lrv_vmr_flag",
                                 prod.MakeStringArray(dims, bad))) {
                return nullptr;
              }
            } else {
              std::vector<uint8_t> sp_lrv = MakeSpeciesLrv(num_alt, scan_idx);
              if (!prod.AddField(item, "lrv_vmr_flag",
                                 prod.MakeUint8Array(dims, sp_lrv))) {
                return nullptr;
              }
            }
          }
          elems.push_back(item);
        }
        const long dims[] = {vmr_len};
        if (!prod.AddField(scan, "retrieval_vmr",
                           prod.MakeNodeArray(dims, elems))) {
          return nullptr;
        }
      }
    }
    return scan;
  };

  if (cfg.include_scan_info) {
    if (cfg.scan_info_not_array) {
      CodaMemNode* scan = make_scan_record(0, 1);
      ASSERT_NE(nullptr, scan);
      ASSERT_TRUE(prod.AddField(prod.root(), "scan_information_mds", scan));
    } else if (cfg.num_scans == 0) {
      CodaMemNode* tmpl = make_scan_record(0, 1);
      ASSERT_NE(nullptr, tmpl);
      const long dims[] = {0};
      ASSERT_TRUE(prod.AddField(prod.root(), "scan_information_mds",
                                prod.MakeNodeArray(dims, {}, tmpl)));
    } else {
      std::vector<CodaMemNode*> scans;
      for (long i = 0; i < cfg.num_scans; ++i) {
        CodaMemNode* scan =
            make_scan_record(i, cfg.num_altitudes[static_cast<size_t>(i)]);
        ASSERT_NE(nullptr, scan);
        scans.push_back(scan);
      }
      const long dims[] = {cfg.num_scans};
      ASSERT_TRUE(prod.AddField(prod.root(), "scan_information_mds",
                                prod.MakeNodeArray(dims, scans)));
    }
  }

  const long geo_len = cfg.geo_count >= 0 ? cfg.geo_count : cfg.num_scans;
  if (cfg.include_geo) {
    auto make_geo = [&](long i) -> CodaMemNode* {
      CodaMemNode* g = prod.MakeRecord();
      if (g == nullptr) return nullptr;
      if (cfg.include_dsr_time) {
        const double dsr = (i == 0) ? 1000.0 : 0.0;
        if (!prod.AddField(g, "dsr_time",
                           cfg.bad_dsr_time_type ? prod.MakeString("bad")
                                                 : prod.MakeDouble(dsr))) {
          return nullptr;
        }
      }
      if (cfg.include_loc_mid) {
        CodaMemNode* loc = prod.MakeRecord();
        if (loc == nullptr) return nullptr;
        if (cfg.include_latitude) {
          if (!prod.AddField(loc, "latitude", prod.MakeDouble(45.0 + i))) {
            return nullptr;
          }
        }
        if (!prod.AddField(loc, "longitude", prod.MakeDouble(10.0 + i))) {
          return nullptr;
        }
        if (!prod.AddField(g, "loc_mid", loc)) return nullptr;
      }
      if (cfg.include_angles) {
        if (!prod.AddField(g, "target_sun_elev", prod.MakeDouble(30.0 + i)) ||
            !prod.AddField(g, "target_sun_azi", prod.MakeDouble(120.0 + i)) ||
            !prod.AddField(g, "sat_target_azi", prod.MakeDouble(210.0 + i))) {
          return nullptr;
        }
      }
      return g;
    };
    if (geo_len == 0) {
      CodaMemNode* tmpl = make_geo(0);
      ASSERT_NE(nullptr, tmpl);
      const long dims[] = {0};
      ASSERT_TRUE(prod.AddField(prod.root(), "scan_geolocation_ads",
                                prod.MakeNodeArray(dims, {}, tmpl)));
    } else {
      std::vector<CodaMemNode*> geos;
      for (long i = 0; i < geo_len; ++i) {
        CodaMemNode* g = make_geo(i);
        ASSERT_NE(nullptr, g);
        geos.push_back(g);
      }
      const long dims[] = {geo_len};
      ASSERT_TRUE(prod.AddField(prod.root(), "scan_geolocation_ads",
                                prod.MakeNodeArray(dims, geos)));
    }
  }

  const long pt_len = cfg.pt_count >= 0 ? cfg.pt_count : cfg.num_scans;
  if (cfg.include_pt) {
    std::vector<CodaMemNode*> pts;
    for (long i = 0; i < pt_len; ++i) {
      long num_alt = (i < static_cast<long>(cfg.num_altitudes.size()))
                         ? cfg.num_altitudes[static_cast<size_t>(i)]
                         : 1;
      long k_pt = CountActive(MakePtLrv(num_alt));
      if (k_pt == 0) k_pt = 1;
      CodaMemNode* pt = prod.MakeRecord();
      ASSERT_NE(nullptr, pt);
      const long dims[] = {k_pt};
      const long cov_dims[] = {k_pt * (k_pt + 1) / 2};
      std::vector<double> press_vals(static_cast<size_t>(k_pt));
      std::vector<double> temp_vals(static_cast<size_t>(k_pt));
      for (long k = 0; k < k_pt; ++k) {
        press_vals[static_cast<size_t>(k)] =
            static_cast<double>((k_pt - k) * 100 + i * 10);
        temp_vals[static_cast<size_t>(k)] =
            static_cast<double>(250 - k * 30 + i * 5);
      }
      std::vector<double> press_cov(static_cast<size_t>(cov_dims[0]), 0.5);
      std::vector<double> temp_cov(static_cast<size_t>(cov_dims[0]), 0.1);
      for (long k = 0; k < k_pt; ++k) {
        const double sd = static_cast<double>(k_pt - k + 1);
        press_cov[static_cast<size_t>(k)] = sd * sd;
        temp_cov[static_cast<size_t>(k)] = (sd - 1.0) * (sd - 1.0);
      }
      if (cfg.include_tan_press) {
        if (cfg.bad_tan_press_type) {
          std::vector<std::string_view> bad(static_cast<size_t>(k_pt), "bad");
          ASSERT_TRUE(
              prod.AddField(pt, "tan_press", prod.MakeStringArray(dims, bad)));
        } else {
          ASSERT_TRUE(prod.AddField(pt, "tan_press",
                                    prod.MakeDoubleArray(dims, press_vals)));
        }
      }
      if (cfg.include_tan_press_var_cov) {
        if (cfg.empty_tan_press_var_cov) {
          const long zero_dims[] = {0};
          ASSERT_TRUE(prod.AddField(pt, "tan_press_var_cov",
                                    prod.MakeDoubleArray(zero_dims, {})));
        } else if (cfg.bad_tan_press_var_cov_type) {
          std::vector<std::string_view> bad(static_cast<size_t>(cov_dims[0]),
                                            "bad");
          ASSERT_TRUE(prod.AddField(pt, "tan_press_var_cov",
                                    prod.MakeStringArray(cov_dims, bad)));
        } else {
          ASSERT_TRUE(prod.AddField(pt, "tan_press_var_cov",
                                    prod.MakeDoubleArray(cov_dims, press_cov)));
        }
      }
      ASSERT_TRUE(
          prod.AddField(pt, "temp", prod.MakeDoubleArray(dims, temp_vals)));
      ASSERT_TRUE(prod.AddField(pt, "temp_var_cov",
                                prod.MakeDoubleArray(cov_dims, temp_cov)));
      pts.push_back(pt);
    }
    const long dims[] = {pt_len};
    ASSERT_TRUE(prod.AddField(prod.root(), "pt_retrieval_mds",
                              prod.MakeNodeArray(dims, pts)));
  }

  if (cfg.include_species_mds) {
    const long mds_len =
        cfg.species_mds_count >= 0 ? cfg.species_mds_count : cfg.num_scans;
    for (int s = 0; s < num_species; ++s) {
      if (s == cfg.missing_species_mds_index) continue;
      std::vector<CodaMemNode*> mds_elems;
      for (long i = 0; i < mds_len; ++i) {
        long num_alt = (i < static_cast<long>(cfg.num_altitudes.size()))
                           ? cfg.num_altitudes[static_cast<size_t>(i)]
                           : 1;
        long k_sp = CountActive(MakeSpeciesLrv(num_alt, i));
        if (k_sp == 0) k_sp = 1;
        CodaMemNode* rec = prod.MakeRecord();
        ASSERT_NE(nullptr, rec);
        const long dims[] = {k_sp};
        const long cov_dims[] = {k_sp * (k_sp + 1) / 2};
        const long akm_dims[] = {k_sp * k_sp};

        std::vector<double> conc(static_cast<size_t>(k_sp));
        std::vector<double> vmr(static_cast<size_t>(k_sp));
        std::vector<double> conc_cov(static_cast<size_t>(cov_dims[0]), 0.0);
        std::vector<double> vmr_cov(static_cast<size_t>(cov_dims[0]), 0.0);
        std::vector<double> akm(static_cast<size_t>(akm_dims[0]));
        for (long k = 0; k < k_sp; ++k) {
          conc[static_cast<size_t>(k)] =
              static_cast<double>((k_sp - k) * 100 + s);
          vmr[static_cast<size_t>(k)] =
              static_cast<double>((k_sp - k) * 10 + s);
          const double sd = static_cast<double>(k_sp - k + 1);
          conc_cov[static_cast<size_t>(k)] = (sd + 1.0) * (sd + 1.0);
          vmr_cov[static_cast<size_t>(k)] = sd * sd;
        }
        for (long k = 0; k < akm_dims[0]; ++k) {
          akm[static_cast<size_t>(k)] =
              static_cast<double>(akm_dims[0] - k) + s * 0.01;
        }
        if (cfg.include_conc_alt) {
          ASSERT_TRUE(
              prod.AddField(rec, "conc_alt", prod.MakeDoubleArray(dims, conc)));
        }
        if (cfg.include_conc_var_cov) {
          ASSERT_TRUE(prod.AddField(rec, "conc_var_cov",
                                    prod.MakeDoubleArray(cov_dims, conc_cov)));
        }
        if (cfg.include_vmr) {
          ASSERT_TRUE(
              prod.AddField(rec, "vmr", prod.MakeDoubleArray(dims, vmr)));
        }
        ASSERT_TRUE(prod.AddField(rec, "vmr_var_cov",
                                  prod.MakeDoubleArray(cov_dims, vmr_cov)));
        if (cfg.include_avg_kernel) {
          if (cfg.bad_avg_kernel_type) {
            std::vector<std::string_view> bad(static_cast<size_t>(akm_dims[0]),
                                              "bad");
            ASSERT_TRUE(prod.AddField(rec, "avg_kernel",
                                      prod.MakeStringArray(akm_dims, bad)));
          } else {
            ASSERT_TRUE(prod.AddField(rec, "avg_kernel",
                                      prod.MakeDoubleArray(akm_dims, akm)));
          }
        }
        mds_elems.push_back(rec);
      }
      const long dims[] = {mds_len};
      ASSERT_TRUE(prod.AddField(prod.root(), kSpeciesMdsNames[s],
                                prod.MakeNodeArray(dims, mds_elems)));
    }
  }
}

TEST(HarpIngestMipasL2Test, IngestVersion4AllSpeciesSuccess) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("MIPAS_L2");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  MipasL2Config cfg;
  cfg.product_version = 4;
  BuildMipasL2Product(prod, cfg);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status) << "failed var: " << res.failed_variable;
  EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(3, res.dimensions[harp_dimension_vertical]);

  ASSERT_EQ(2u, res.variables["datetime"].double_values.size());
  EXPECT_DOUBLE_EQ(1000.0, res.variables["datetime"].double_values[0]);
  EXPECT_TRUE(std::isnan(res.variables["datetime"].double_values[1]));

  EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(12345));
  EXPECT_THAT(res.variables["latitude"].double_values, ElementsAre(45.0, 46.0));
  EXPECT_THAT(res.variables["longitude"].double_values,
              ElementsAre(10.0, 11.0));
  EXPECT_THAT(res.variables["solar_elevation_angle"].double_values,
              ElementsAre(30.0, 31.0));
  EXPECT_THAT(res.variables["solar_azimuth_angle"].double_values,
              ElementsAre(120.0, 121.0));
  EXPECT_THAT(res.variables["viewing_azimuth_angle"].double_values,
              ElementsAre(210.0, 211.0));

  // Scan 0 has lrv_p_t_flag raw {1,0,1} -> reversed {1,0,1}; scan 1 has {1,1}
  // -> {1,1,0}.
  EXPECT_THAT(res.variables["pressure"].double_values,
              ElementsAre(DoubleEq(100.0), IsNan(), DoubleEq(200.0),
                          DoubleEq(110.0), DoubleEq(210.0), IsNan()));
  EXPECT_THAT(res.variables["pressure_uncertainty"].double_values,
              ElementsAre(DoubleEq(2.0), IsNan(), DoubleEq(3.0), DoubleEq(2.0),
                          DoubleEq(3.0), IsNan()));
  EXPECT_THAT(res.variables["temperature"].double_values,
              ElementsAre(DoubleEq(220.0), IsNan(), DoubleEq(250.0),
                          DoubleEq(225.0), DoubleEq(255.0), IsNan()));

  // Check first (H2O) and last (CHClF2 / F22) species values and AVK.
  EXPECT_THAT(res.variables["H2O_number_density"].double_values,
              ElementsAre(IsNan(), DoubleEq(100.0), DoubleEq(200.0),
                          DoubleEq(100.0), IsNan(), IsNan()));
  EXPECT_THAT(res.variables["CHClF2_volume_mixing_ratio"].double_values,
              ElementsAre(IsNan(), DoubleEq(24.0), DoubleEq(34.0),
                          DoubleEq(24.0), IsNan(), IsNan()));
  EXPECT_THAT(res.variables["H2O_volume_mixing_ratio_avk"].double_values,
              ElementsAre(IsNan(), IsNan(), IsNan(), IsNan(), DoubleEq(1.0),
                          DoubleEq(2.0), IsNan(), DoubleEq(3.0), DoubleEq(4.0),
                          DoubleEq(1.0), IsNan(), IsNan(), IsNan(), IsNan(),
                          IsNan(), IsNan(), IsNan(), IsNan()));
  EXPECT_TRUE(res.variables["CHClF2_volume_mixing_ratio_avk"].included);
}

TEST(HarpIngestMipasL2Test, IngestVersion4WithSpeciesOptionFiltersLevels) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("MIPAS_L2");
  ASSERT_NE(nullptr, mod);

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  MipasL2Config cfg;
  cfg.product_version = 4;
  BuildMipasL2Product(prod, cfg);

  IngestExerciseResult res =
      ExerciseIngestionModule(mod, prod.product(), "species=O3");
  ASSERT_EQ(HARP_SUCCESS, res.status) << "failed var: " << res.failed_variable;

  EXPECT_TRUE(res.variables["O3_number_density"].included);
  EXPECT_TRUE(res.variables["O3_volume_mixing_ratio_avk"].included);
  EXPECT_FALSE(res.variables["H2O_number_density"].included);
  EXPECT_FALSE(res.variables["CHClF2_volume_mixing_ratio"].included);

  // Scan 0 O3 lrv reversed = {0, 1, 1} -> keeps indices 1 and 2 of altitude
  // {10, 20, 30}. Scan 1 O3 lrv reversed = {1, 0, 0} -> keeps index 0 of
  // altitude {15, 25}.
  EXPECT_THAT(res.variables["altitude"].double_values,
              ElementsAre(DoubleEq(20.0), DoubleEq(30.0), IsNan(),
                          DoubleEq(15.0), IsNan(), DoubleEq(0.0)));
  // Scan 0 pressure {100, NaN, 200} filtered by {0, 1, 1} -> {NaN, 200, NaN}.
  EXPECT_THAT(res.variables["pressure"].double_values,
              ElementsAre(IsNan(), DoubleEq(200.0), IsNan(), DoubleEq(110.0),
                          IsNan(), IsNan()));
  EXPECT_THAT(res.variables["pressure_uncertainty"].double_values,
              ElementsAre(IsNan(), DoubleEq(3.0), IsNan(), DoubleEq(2.0),
                          IsNan(), IsNan()));
  EXPECT_THAT(res.variables["O3_volume_mixing_ratio"].double_values,
              ElementsAre(DoubleEq(11.0), DoubleEq(21.0), IsNan(),
                          DoubleEq(11.0), IsNan(), IsNan()));
  EXPECT_THAT(
      res.variables["O3_volume_mixing_ratio_avk"].double_values,
      ElementsAre(DoubleEq(1.01), DoubleEq(2.01), IsNan(), DoubleEq(3.01),
                  DoubleEq(4.01), IsNan(), IsNan(), IsNan(), IsNan(),
                  DoubleEq(1.01), IsNan(), IsNan(), IsNan(), IsNan(), IsNan(),
                  IsNan(), IsNan(), IsNan()));
}

TEST(HarpIngestMipasL2Test, IngestVersionsZeroOneTwoThreeFeatureGating) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("MIPAS_L2");
  ASSERT_NE(nullptr, mod);

  struct VersionCase {
    int version;
    bool expect_angles;
    bool expect_core_akm;
    bool expect_v3_species;
    bool expect_v4_species;
    std::string_view order_override;
  };
  const VersionCase kCases[] = {
      {.version = 0,
       .expect_angles = false,
       .expect_core_akm = false,
       .expect_v3_species = false,
       .expect_v4_species = false,
       .order_override = ""},
      {.version = 1,
       .expect_angles = true,
       .expect_core_akm = false,
       .expect_v3_species = false,
       .expect_v4_species = false,
       .order_override = ""},
      {.version = 2,
       .expect_angles = true,
       .expect_core_akm = true,
       .expect_v3_species = false,
       .expect_v4_species = false,
       .order_override = "h2o,o3,hno3,ch4,n2o,no2   "},
      {.version = 3,
       .expect_angles = true,
       .expect_core_akm = true,
       .expect_v3_species = true,
       .expect_v4_species = false,
       .order_override = ""},
  };

  for (const auto& tc : kCases) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    MipasL2Config cfg;
    cfg.product_version = tc.version;
    if (!tc.order_override.empty()) {
      cfg.order_of_species = std::string(tc.order_override);
    }
    BuildMipasL2Product(prod, cfg);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status) << "version=" << tc.version;
    EXPECT_EQ(tc.expect_angles,
              res.variables["solar_elevation_angle"].included);
    EXPECT_EQ(tc.expect_angles, res.variables["solar_azimuth_angle"].included);
    EXPECT_EQ(tc.expect_angles,
              res.variables["viewing_azimuth_angle"].included);
    EXPECT_EQ(tc.expect_core_akm,
              res.variables["H2O_volume_mixing_ratio_avk"].included);
    EXPECT_EQ(tc.expect_core_akm,
              res.variables["O3_volume_mixing_ratio_avk"].included);
    EXPECT_EQ(tc.expect_v3_species,
              res.variables["CCl3F_number_density"].included);
    EXPECT_EQ(tc.expect_v3_species,
              res.variables["CCl2F2_volume_mixing_ratio_avk"].included);
    EXPECT_EQ(tc.expect_v4_species,
              res.variables["COF2_number_density"].included);
    EXPECT_EQ(tc.expect_v4_species,
              res.variables["CHClF2_volume_mixing_ratio_avk"].included);
  }
}

TEST(HarpIngestMipasL2Test, EmptyScansAndZeroAltitudesProduceEmptyDims) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("MIPAS_L2");
  ASSERT_NE(nullptr, mod);

  // 1. Zero scans in scan_information_mds.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    MipasL2Config cfg;
    cfg.product_version = 2;
    cfg.num_scans = 0;
    cfg.num_altitudes.clear();
    cfg.include_geo = false;
    cfg.include_pt = false;
    cfg.include_species_mds = false;
    BuildMipasL2Product(prod, cfg);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(0, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(0, res.dimensions[harp_dimension_vertical]);
  }

  // 2. Non-zero scans, but all zpd_crossing_time arrays have 0 elements.
  {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    MipasL2Config cfg;
    cfg.product_version = 2;
    cfg.num_scans = 1;
    cfg.num_altitudes = {0};
    cfg.include_geo = false;
    cfg.include_pt = false;
    cfg.include_species_mds = false;
    BuildMipasL2Product(prod, cfg);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(0, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(0, res.dimensions[harp_dimension_vertical]);
  }
}

TEST(HarpIngestMipasL2Test, InitAndMetadataErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("MIPAS_L2");
  ASSERT_NE(nullptr, mod);

  auto expect_init_error = [&](const MipasL2Config& cfg, int expected_err) {
    harp_set_error(HARP_SUCCESS, nullptr);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    BuildMipasL2Product(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(expected_err, res.failed_errno);
    harp_set_error(HARP_SUCCESS, nullptr);
  };

  // 1. Missing product version metadata: coda_get_product_version yields -1
  // (num_species = 6), which mismatches the 15-species order_of_species.
  {
    MipasL2Config cfg;
    cfg.set_product_info = false;
    expect_init_error(cfg, HARP_ERROR_INGESTION);
  }
  // 2. Missing /sph and /sph/order_of_species and bad type.
  {
    MipasL2Config cfg;
    cfg.include_sph = false;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  {
    MipasL2Config cfg;
    cfg.include_order_of_species = false;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  {
    MipasL2Config cfg;
    cfg.bad_order_of_species_type = true;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  // 3. Mismatched species count and missing species per product version.
  {
    MipasL2Config cfg;
    cfg.product_version = 2;
    cfg.order_of_species = "H2O,O3";
    expect_init_error(cfg, HARP_ERROR_INGESTION);
  }
  {
    MipasL2Config cfg;
    cfg.product_version = 2;
    cfg.order_of_species = "H2O,O3,HNO3,CH4,N2O,UNKNOWN";
    expect_init_error(cfg, HARP_ERROR_INGESTION);
  }
  {
    MipasL2Config cfg;
    cfg.product_version = 3;
    cfg.order_of_species = "H2O,O3,HNO3,CH4,N2O,NO2,F11,CLNO,N2O5,UNKNOWN";
    expect_init_error(cfg, HARP_ERROR_INGESTION);
  }
  {
    MipasL2Config cfg;
    cfg.product_version = 4;
    cfg.order_of_species =
        "H2O,O3,HNO3,CH4,N2O,NO2,F11,CLNO,N2O5,F12,COF2,CCL4,HCN,F14,UNKNOWN";
    expect_init_error(cfg, HARP_ERROR_INGESTION);
  }
  // 4. scan_information_mds structural errors.
  {
    MipasL2Config cfg;
    cfg.include_scan_info = false;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  {
    MipasL2Config cfg;
    cfg.scan_info_not_array = true;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  {
    MipasL2Config cfg;
    cfg.include_zpd_crossing_time = false;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  // 5. Dataset cursor errors (missing or too short).
  {
    MipasL2Config cfg;
    cfg.include_geo = false;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  {
    MipasL2Config cfg;
    cfg.geo_count = 0;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  {
    MipasL2Config cfg;
    cfg.geo_count = 1;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  {
    MipasL2Config cfg;
    cfg.include_pt = false;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  {
    MipasL2Config cfg;
    cfg.missing_species_mds_index = 0;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  // 6. Logical retrieval vector (lrv) errors.
  {
    MipasL2Config cfg;
    cfg.include_retrieval_pt = false;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  {
    MipasL2Config cfg;
    cfg.include_lrv_pt_flag = false;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  {
    MipasL2Config cfg;
    cfg.bad_lrv_pt_flag_type = true;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  {
    MipasL2Config cfg;
    cfg.include_retrieval_vmr = false;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  {
    MipasL2Config cfg;
    cfg.empty_retrieval_vmr = true;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  {
    MipasL2Config cfg;
    cfg.include_lrv_vmr_flag = false;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  {
    MipasL2Config cfg;
    cfg.bad_lrv_vmr_flag_type = true;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
  {
    MipasL2Config cfg;
    cfg.short_retrieval_vmr = true;
    expect_init_error(cfg, HARP_ERROR_CODA);
  }
}

TEST(HarpIngestMipasL2Test, VariableReadErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("MIPAS_L2");
  ASSERT_NE(nullptr, mod);

  auto check_read_errors = [&](const MipasL2Config& cfg,
                               const std::vector<std::string>& failing_vars) {
    harp_set_error(HARP_SUCCESS, nullptr);
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    BuildMipasL2Product(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), /*options_str=*/nullptr, /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    for (const std::string& var : failing_vars) {
      EXPECT_EQ(-1, res.variables[var].read_status) << "var=" << var;
    }
    harp_set_error(HARP_SUCCESS, nullptr);
  };

  // Missing /mph, missing /mph/abs_orbit, bad /mph/abs_orbit type.
  {
    MipasL2Config cfg;
    cfg.product_version = 2;
    cfg.include_mph = false;
    check_read_errors(cfg, {"orbit_index"});
  }
  {
    MipasL2Config cfg;
    cfg.product_version = 2;
    cfg.include_abs_orbit = false;
    check_read_errors(cfg, {"orbit_index"});
  }
  {
    MipasL2Config cfg;
    cfg.product_version = 2;
    cfg.bad_abs_orbit_type = true;
    check_read_errors(cfg, {"orbit_index"});
  }
  // Missing and bad type for altitude, geolocation, angles.
  {
    MipasL2Config cfg;
    cfg.product_version = 2;
    cfg.include_tangent_altitude = false;
    check_read_errors(cfg, {"altitude"});
  }
  {
    MipasL2Config cfg;
    cfg.product_version = 2;
    cfg.bad_tangent_altitude_type = true;
    check_read_errors(cfg, {"altitude"});
  }
  {
    MipasL2Config cfg;
    cfg.product_version = 2;
    cfg.include_dsr_time = false;
    cfg.include_loc_mid = false;
    cfg.include_angles = false;
    check_read_errors(
        cfg, {"datetime", "latitude", "longitude", "solar_elevation_angle",
              "solar_azimuth_angle", "viewing_azimuth_angle"});
  }
  {
    MipasL2Config cfg;
    cfg.product_version = 2;
    cfg.bad_dsr_time_type = true;
    cfg.include_latitude = false;
    check_read_errors(cfg, {"datetime", "latitude"});
  }
  // Missing and bad type for profile data, uncertainty, and averaging kernel.
  {
    MipasL2Config cfg;
    cfg.product_version = 2;
    cfg.include_tan_press = false;
    cfg.empty_tan_press_var_cov = true;
    cfg.include_conc_alt = false;
    cfg.include_conc_var_cov = false;
    cfg.include_avg_kernel = false;
    check_read_errors(
        cfg, {"pressure", "pressure_uncertainty", "H2O_number_density",
              "H2O_number_density_uncertainty", "H2O_volume_mixing_ratio_avk"});
  }
  {
    MipasL2Config cfg;
    cfg.product_version = 2;
    cfg.bad_tan_press_type = true;
    cfg.bad_tan_press_var_cov_type = true;
    cfg.bad_avg_kernel_type = true;
    check_read_errors(cfg, {"pressure", "pressure_uncertainty",
                            "H2O_volume_mixing_ratio_avk"});
  }
}

}  // namespace
}  // namespace stcorp_harp
