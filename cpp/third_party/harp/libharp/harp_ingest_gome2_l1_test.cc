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
// Unit tests for the GOME2 Level 1 ingestion module
// (libharp/harp-ingest-gome2_l1.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-gome2_l1.c

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/coda_mem_product.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/ingest_test_util.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::DoubleNear;
using ::testing::ElementsAre;

constexpr int kNumBands = 6;
constexpr int kMaxReadouts = 32;
constexpr int kMaxPixels = 4096;
constexpr double kUndefinedInt32VsfValue = -2147483648.0E128;

constexpr std::array<std::string_view, kNumBands> kBandNames = {
    "BAND_1A", "BAND_1B", "BAND_2A", "BAND_2B", "BAND_3", "BAND_4"};
constexpr std::array<std::string_view, kNumBands> kWavelengthNames = {
    "WAVELENGTH_1A", "WAVELENGTH_1B", "WAVELENGTH_2A",
    "WAVELENGTH_2B", "WAVELENGTH_3",  "WAVELENGTH_4"};

struct MdrConfig {
  std::string lightsource = "Earthshine";
  bool include_lightsource = true;
  bool lightsource_is_record = true;
  bool include_output_selection = true;
  uint8_t output_selection = 0;  // 0 = radiance, 1 = transmission
  bool output_selection_wrong_type = false;

  bool include_record_header = true;
  bool include_record_start_time = true;
  bool record_start_time_wrong_type = false;
  double record_start_time = 1000.0;

  bool include_num_recs = true;
  bool num_recs_short = false;
  bool num_recs_wrong_type = false;
  std::array<uint16_t, kNumBands> num_recs = {32, 16, 8, 4, 2, 1};
  std::array<long, kNumBands> num_pixels = {2, 2, 2, 2, 2, 2};
  std::array<double, kNumBands> integration_times = {0.1875, 0.375, 0.75,
                                                     1.5,    3.0,   6.0};
  std::array<double, 10> unique_int = {0.1875, 0.375, 0.75, 1.5,  3.0,
                                       6.0,    12.0,  24.0, 48.0, 96.0};
  bool include_unique_int = true;
  bool unique_int_wrong_type = false;

  // Geolocation options.
  bool include_geo = true;
  bool geo_empty = false;
  bool geo_short = false;
  bool geo_missing_centre = false;
  bool geo_missing_centre_lat = false;
  bool geo_centre_lat_wrong_type = false;
  bool geo_missing_corner = false;
  bool geo_corner_short = false;
  bool geo_missing_corner_lat = false;
  bool geo_corner_lat_wrong_type = false;
  bool geo_missing_sza = false;
  bool geo_sza_short = false;
  bool geo_sza_wrong_type = false;
  bool geo_old_centre_short = false;

  // Cloud options.
  bool include_cloud = true;
  bool cloud_missing_fit_mode = false;
  bool cloud_fit_mode_wrong_type = false;
  uint8_t cloud_fit_mode_val = 0;
  bool cloud_missing_fit1 = false;
  bool cloud_fit1_wrong_type = false;
  double cloud_fit1_val = 500.0;
  bool cloud_missing_fit2 = false;
  bool cloud_fit2_wrong_type = false;
  double cloud_fit2_val = 0.25;

  // Spectral options.
  bool include_band0 = true;
  bool band0_empty = false;
  bool band0_short = false;
  bool band0_missing_rad = false;
  bool band0_rad_wrong_type = false;
  bool use_undefined_rad_on_readout1 = false;
  bool missing_wl0 = false;
  bool empty_wl0 = false;
  bool short_wl0 = false;
  bool wl0_wrong_type = false;
  bool missing_int_times = false;
  bool short_int_times = false;
  bool int_times_wrong_type = false;
};

struct MeasurementProductConfig {
  int format_version = 12;
  bool set_product_info = true;
  bool include_mphr = true;
  bool include_orbit_start = true;
  bool orbit_start_wrong_type = false;
  uint32_t orbit_start = 12345;
  bool include_mdr = true;
  std::vector<MdrConfig> mdrs = {MdrConfig{}};
};

CodaMemNode* MakeCornerRecord(CodaMemProduct& prod, double lat, double lon,
                              bool include_lat, bool lat_wrong_type) {
  CodaMemNode* rec = prod.MakeRecord();
  if (include_lat) {
    prod.AddField(
        rec, "latitude",
        lat_wrong_type ? prod.MakeString("bad") : prod.MakeDouble(lat));
  }
  prod.AddField(rec, "longitude", prod.MakeDouble(lon));
  return rec;
}

void PopulateOldGeoEarthActual(CodaMemProduct& prod, CodaMemNode* ls,
                               const MdrConfig& cfg) {
  CodaMemNode* geo = prod.MakeRecord();
  if (!cfg.geo_missing_centre) {
    if (cfg.geo_old_centre_short) {
      std::vector<CodaMemNode*> elems;
      elems.push_back(MakeCornerRecord(prod, 10.0, 20.0,
                                       !cfg.geo_missing_centre_lat,
                                       cfg.geo_centre_lat_wrong_type));
      const long dims[] = {1};
      prod.AddField(geo, "CENTRE_ACTUAL", prod.MakeNodeArray(dims, elems));
    } else {
      std::vector<CodaMemNode*> elems;
      elems.reserve(kNumBands * kMaxReadouts);
      for (int b = 0; b < kNumBands; ++b) {
        for (int r = 0; r < kMaxReadouts; ++r) {
          elems.push_back(MakeCornerRecord(
              prod, 10.0 + b + 0.1 * r, 20.0 + b + 0.1 * r,
              !cfg.geo_missing_centre_lat, cfg.geo_centre_lat_wrong_type));
        }
      }
      const long dims[] = {kNumBands, kMaxReadouts};
      prod.AddField(geo, "CENTRE_ACTUAL", prod.MakeNodeArray(dims, elems));
    }
  }

  if (!cfg.geo_missing_corner) {
    if (cfg.geo_corner_short) {
      std::vector<CodaMemNode*> elems;
      elems.push_back(MakeCornerRecord(prod, 1.0, 10.0, true, false));
      const long dims[] = {1};
      prod.AddField(geo, "CORNER_ACTUAL", prod.MakeNodeArray(dims, elems));
    } else {
      std::vector<CodaMemNode*> elems;
      elems.reserve(kNumBands * 4 * kMaxReadouts);
      for (int b = 0; b < kNumBands; ++b) {
        for (int c = 0; c < 4; ++c) {
          for (int r = 0; r < kMaxReadouts; ++r) {
            // Corner values A=1, B=2, C=3, D=4 -> reordered BDCA = 2, 4, 3, 1.
            elems.push_back(MakeCornerRecord(prod, 1.0 + c, 10.0 + c,
                                             !cfg.geo_missing_corner_lat,
                                             cfg.geo_corner_lat_wrong_type));
          }
        }
      }
      const long dims[] = {kNumBands, 4, kMaxReadouts};
      prod.AddField(geo, "CORNER_ACTUAL", prod.MakeNodeArray(dims, elems));
    }
  }

  std::vector<double> angle_vals(kNumBands * 3 * kMaxReadouts, 30.0);
  const long angle_dims[] = {kNumBands, 3, kMaxReadouts};
  if (!cfg.geo_missing_sza) {
    if (cfg.geo_sza_short) {
      const long short_dims[] = {1};
      const double short_vals[] = {30.0};
      prod.AddField(geo, "SOLAR_ZENITH_ACTUAL",
                    prod.MakeDoubleArray(short_dims, short_vals));
    } else if (cfg.geo_sza_wrong_type) {
      const std::string_view strs[] = {"bad"};
      const long short_dims[] = {1};
      prod.AddField(geo, "SOLAR_ZENITH_ACTUAL",
                    prod.MakeStringArray(short_dims, strs));
    } else {
      prod.AddField(geo, "SOLAR_ZENITH_ACTUAL",
                    prod.MakeDoubleArray(angle_dims, angle_vals));
    }
  }
  prod.AddField(geo, "SOLAR_AZIMUTH_ACTUAL",
                prod.MakeDoubleArray(angle_dims, angle_vals));
  prod.AddField(geo, "SAT_ZENITH_ACTUAL",
                prod.MakeDoubleArray(angle_dims, angle_vals));
  prod.AddField(geo, "SAT_AZIMUTH_ACTUAL",
                prod.MakeDoubleArray(angle_dims, angle_vals));
  prod.AddField(ls, "GEO_EARTH_ACTUAL", geo);
}

void PopulateNewGeoEarthActual(CodaMemProduct& prod, CodaMemNode* ls,
                               const MdrConfig& cfg, int timer_index) {
  const uint16_t max_recs =
      *std::max_element(cfg.num_recs.begin(), cfg.num_recs.end());
  const std::string field_name =
      "GEO_EARTH_ACTUAL_" + std::to_string(timer_index);

  if (cfg.geo_empty) {
    CodaMemNode* sample = prod.MakeRecord();
    prod.AddField(sample, "CENTRE_ACTUAL",
                  MakeCornerRecord(prod, 0.0, 0.0, true, false));
    const long empty_dims[] = {0};
    prod.AddField(ls, field_name, prod.MakeNodeArray(empty_dims, {}, sample));
    return;
  }

  const long count = cfg.geo_short ? 1 : max_recs;
  std::vector<CodaMemNode*> geo_elems;
  geo_elems.reserve(static_cast<size_t>(count));
  for (long r = 0; r < count; ++r) {
    CodaMemNode* rec = prod.MakeRecord();
    if (!cfg.geo_missing_centre) {
      prod.AddField(rec, "CENTRE_ACTUAL",
                    MakeCornerRecord(prod, 10.0 + 0.1 * r, 20.0 + 0.1 * r,
                                     !cfg.geo_missing_centre_lat,
                                     cfg.geo_centre_lat_wrong_type));
    }
    if (!cfg.geo_missing_corner) {
      const long num_corners = cfg.geo_corner_short ? 2 : 4;
      std::vector<CodaMemNode*> corners;
      corners.reserve(static_cast<size_t>(num_corners));
      for (long c = 0; c < num_corners; ++c) {
        corners.push_back(MakeCornerRecord(prod, 1.0 + c, 10.0 + c,
                                           !cfg.geo_missing_corner_lat,
                                           cfg.geo_corner_lat_wrong_type));
      }
      const long corner_dims[] = {num_corners};
      prod.AddField(rec, "CORNER_ACTUAL",
                    prod.MakeNodeArray(corner_dims, corners));
    }
    const long angle_dims[] = {3};
    const double sza_vals[] = {25.0, 30.0 + r, 35.0};
    if (!cfg.geo_missing_sza) {
      if (cfg.geo_sza_short) {
        const long short_dims[] = {1};
        const double short_vals[] = {25.0};
        prod.AddField(rec, "SOLAR_ZENITH_ACTUAL",
                      prod.MakeDoubleArray(short_dims, short_vals));
      } else if (cfg.geo_sza_wrong_type) {
        const std::string_view strs[] = {"a", "b", "c"};
        prod.AddField(rec, "SOLAR_ZENITH_ACTUAL",
                      prod.MakeStringArray(angle_dims, strs));
      } else {
        prod.AddField(rec, "SOLAR_ZENITH_ACTUAL",
                      prod.MakeDoubleArray(angle_dims, sza_vals));
      }
    }
    const double saa_vals[] = {40.0, 45.0, 50.0};
    const double vza_vals[] = {10.0, 15.0, 20.0};
    const double vaa_vals[] = {60.0, 65.0, 70.0};
    prod.AddField(rec, "SOLAR_AZIMUTH_ACTUAL",
                  prod.MakeDoubleArray(angle_dims, saa_vals));
    prod.AddField(rec, "SAT_ZENITH_ACTUAL",
                  prod.MakeDoubleArray(angle_dims, vza_vals));
    prod.AddField(rec, "SAT_AZIMUTH_ACTUAL",
                  prod.MakeDoubleArray(angle_dims, vaa_vals));
    geo_elems.push_back(rec);
  }
  const long dims[] = {count};
  prod.AddField(ls, field_name, prod.MakeNodeArray(dims, geo_elems));
}

void PopulateMeasurementProduct(CodaMemProduct& prod,
                                const MeasurementProductConfig& cfg) {
  if (cfg.set_product_info) {
    ASSERT_TRUE(prod.SetProductInfo("EPS", "GOME_xxx_1B", cfg.format_version));
  }

  if (cfg.include_mphr) {
    CodaMemNode* mphr = prod.MakeRecord();
    if (cfg.include_orbit_start) {
      prod.AddField(mphr, "ORBIT_START",
                    cfg.orbit_start_wrong_type
                        ? prod.MakeString("bad")
                        : prod.MakeUint32(cfg.orbit_start));
    }
    ASSERT_TRUE(prod.AddField(prod.root(), "MPHR", mphr));
  }

  if (!cfg.include_mdr) {
    return;
  }

  std::vector<CodaMemNode*> mdr_nodes;
  mdr_nodes.reserve(cfg.mdrs.size());
  for (const MdrConfig& mcfg : cfg.mdrs) {
    CodaMemNode* mdr = prod.MakeRecord();
    if (!mcfg.include_lightsource) {
      if (!mdr_nodes.empty()) {
        prod.AddUnavailableField(mdr, mcfg.lightsource);
      }
      mdr_nodes.push_back(mdr);
      continue;
    }
    if (!mcfg.lightsource_is_record) {
      prod.AddField(mdr, mcfg.lightsource, prod.MakeInt32(1));
      mdr_nodes.push_back(mdr);
      continue;
    }

    CodaMemNode* ls = prod.MakeRecord();
    if (mcfg.include_output_selection) {
      prod.AddField(ls, "OUTPUT_SELECTION",
                    mcfg.output_selection_wrong_type
                        ? prod.MakeString("bad")
                        : prod.MakeUint8(mcfg.output_selection));
    }

    if (mcfg.include_record_header) {
      CodaMemNode* hdr = prod.MakeRecord();
      if (mcfg.include_record_start_time) {
        prod.AddField(hdr, "RECORD_START_TIME",
                      mcfg.record_start_time_wrong_type
                          ? prod.MakeString("bad")
                          : prod.MakeDouble(mcfg.record_start_time));
      }
      prod.AddField(ls, "RECORD_HEADER", hdr);
    }

    if (mcfg.include_num_recs) {
      if (mcfg.num_recs_short) {
        const long short_dims[] = {1};
        const uint16_t short_vals[] = {mcfg.num_recs[0]};
        prod.AddField(ls, "NUM_RECS",
                      prod.MakeUint16Array(short_dims, short_vals));
      } else if (mcfg.num_recs_wrong_type) {
        const long dims[] = {kNumBands};
        const std::string_view strs[] = {"a", "b", "c", "d", "e", "f"};
        prod.AddField(ls, "NUM_RECS", prod.MakeStringArray(dims, strs));
      } else {
        const long dims[] = {kNumBands};
        prod.AddField(ls, "NUM_RECS",
                      prod.MakeUint16Array(dims, mcfg.num_recs));
      }
    }

    int timer_index = 1;
    const uint16_t max_recs =
        *std::max_element(mcfg.num_recs.begin(), mcfg.num_recs.end());
    const double min_int = 6.0 / max_recs;
    for (int t = 0; t < 10; ++t) {
      if (std::fabs(mcfg.unique_int[t] - min_int) < 0.01) {
        timer_index = t + 1;
        break;
      }
    }

    if (mcfg.include_unique_int) {
      const long udims[] = {10};
      if (mcfg.unique_int_wrong_type) {
        const std::string_view strs[] = {"0", "1", "2", "3", "4",
                                         "5", "6", "7", "8", "9"};
        prod.AddField(ls, "UNIQUE_INT", prod.MakeStringArray(udims, strs));
      } else {
        prod.AddField(ls, "UNIQUE_INT",
                      prod.MakeDoubleArray(udims, mcfg.unique_int));
      }
    }

    if (mcfg.include_geo) {
      if (cfg.format_version >= 12) {
        PopulateNewGeoEarthActual(prod, ls, mcfg, timer_index);
      } else {
        PopulateOldGeoEarthActual(prod, ls, mcfg);
      }
    }

    if (mcfg.include_cloud) {
      CodaMemNode* cloud = prod.MakeRecord();
      const long cdims[] = {kMaxReadouts};
      if (!mcfg.cloud_missing_fit_mode) {
        if (mcfg.cloud_fit_mode_wrong_type) {
          const long sdims[] = {1};
          const std::string_view strs[] = {"bad"};
          prod.AddField(cloud, "FIT_MODE", prod.MakeStringArray(sdims, strs));
        } else {
          std::array<uint8_t, kMaxReadouts> fit_mode{};
          fit_mode.fill(mcfg.cloud_fit_mode_val);
          prod.AddField(cloud, "FIT_MODE",
                        prod.MakeUint8Array(cdims, fit_mode));
        }
      }
      if (!mcfg.cloud_missing_fit1) {
        if (mcfg.cloud_fit1_wrong_type) {
          const long sdims[] = {1};
          const std::string_view strs[] = {"bad"};
          prod.AddField(cloud, "FIT_1", prod.MakeStringArray(sdims, strs));
        } else {
          std::array<double, kMaxReadouts> fit1{};
          for (int k = 0; k < kMaxReadouts; ++k) {
            fit1[k] = mcfg.cloud_fit1_val + k;
          }
          prod.AddField(cloud, "FIT_1", prod.MakeDoubleArray(cdims, fit1));
        }
      }
      if (!mcfg.cloud_missing_fit2) {
        if (mcfg.cloud_fit2_wrong_type) {
          const long sdims[] = {1};
          const std::string_view strs[] = {"bad"};
          prod.AddField(cloud, "FIT_2", prod.MakeStringArray(sdims, strs));
        } else {
          std::array<double, kMaxReadouts> fit2{};
          for (int k = 0; k < kMaxReadouts; ++k) {
            fit2[k] = mcfg.cloud_fit2_val + 0.01 * k;
          }
          prod.AddField(cloud, "FIT_2", prod.MakeDoubleArray(cdims, fit2));
        }
      }
      prod.AddField(ls, "CLOUD", cloud);
    }

    for (int b = 0; b < kNumBands; ++b) {
      if (b == 0 && !mcfg.include_band0) {
        continue;
      }
      const long nr = (b == 0 && mcfg.band0_empty)   ? 0
                      : (b == 0 && mcfg.band0_short) ? 1
                                                     : mcfg.num_recs[b];
      const long np = mcfg.num_pixels[b];
      const long bdims[] = {nr, np};
      if (b == 0 && mcfg.band0_empty) {
        CodaMemNode* sample = prod.MakeRecord();
        prod.AddField(sample, "RAD", prod.MakeDouble(1.0));
        prod.AddField(ls, kBandNames[b], prod.MakeNodeArray(bdims, {}, sample));
      } else {
        std::vector<CodaMemNode*> belems;
        belems.reserve(static_cast<size_t>(nr * np));
        for (long r = 0; r < nr; ++r) {
          for (long p = 0; p < np; ++p) {
            CodaMemNode* pix = prod.MakeRecord();
            if (!(b == 0 && mcfg.band0_missing_rad)) {
              if (b == 0 && mcfg.band0_rad_wrong_type) {
                prod.AddField(pix, "RAD", prod.MakeString("bad"));
              } else if (b == 0 && mcfg.use_undefined_rad_on_readout1 &&
                         r == 1 && p == 0) {
                prod.AddField(pix, "RAD",
                              prod.MakeDouble(kUndefinedInt32VsfValue));
              } else {
                prod.AddField(pix, "RAD",
                              prod.MakeDouble(100.0 * (b + 1) + 10.0 * r + p));
              }
            }
            belems.push_back(pix);
          }
        }
        prod.AddField(ls, kBandNames[b], prod.MakeNodeArray(bdims, belems));
      }

      if (!(b == 0 && mcfg.missing_wl0)) {
        if (b == 0 && mcfg.empty_wl0) {
          const long wdims[] = {0};
          prod.AddField(ls, kWavelengthNames[b],
                        prod.MakeDoubleArray(wdims, {}));
        } else if (b == 0 && mcfg.short_wl0) {
          const long wdims[] = {1};
          const double wvals[] = {240.0};
          prod.AddField(ls, kWavelengthNames[b],
                        prod.MakeDoubleArray(wdims, wvals));
        } else if (b == 0 && mcfg.wl0_wrong_type) {
          const long wdims[] = {np};
          std::vector<std::string_view> strs(static_cast<size_t>(np), "bad");
          prod.AddField(ls, kWavelengthNames[b],
                        prod.MakeStringArray(wdims, strs));
        } else {
          const long wdims[] = {np};
          std::vector<double> wvals(static_cast<size_t>(np));
          for (long p = 0; p < np; ++p) {
            wvals[static_cast<size_t>(p)] = 240.0 + 100.0 * b + p;
          }
          prod.AddField(ls, kWavelengthNames[b],
                        prod.MakeDoubleArray(wdims, wvals));
        }
      }
    }

    if (!mcfg.missing_int_times) {
      if (mcfg.short_int_times) {
        const long idims[] = {1};
        const double ivals[] = {mcfg.integration_times[0]};
        prod.AddField(ls, "INTEGRATION_TIMES",
                      prod.MakeDoubleArray(idims, ivals));
      } else if (mcfg.int_times_wrong_type) {
        const long idims[] = {kNumBands};
        const std::string_view strs[] = {"a", "b", "c", "d", "e", "f"};
        prod.AddField(ls, "INTEGRATION_TIMES",
                      prod.MakeStringArray(idims, strs));
      } else {
        const long idims[] = {kNumBands};
        prod.AddField(ls, "INTEGRATION_TIMES",
                      prod.MakeDoubleArray(idims, mcfg.integration_times));
      }
    }

    prod.AddField(mdr, mcfg.lightsource, ls);
    mdr_nodes.push_back(mdr);
  }
  const long mdr_dims[] = {static_cast<long>(mdr_nodes.size())};
  if (mdr_nodes.empty()) {
    CodaMemNode* sample = prod.MakeRecord();
    prod.AddField(sample, "Earthshine", prod.MakeRecord());
    ASSERT_TRUE(prod.AddField(prod.root(), "MDR",
                              prod.MakeNodeArray(mdr_dims, mdr_nodes, sample)));
  } else {
    ASSERT_TRUE(prod.AddField(prod.root(), "MDR",
                              prod.MakeNodeArray(mdr_dims, mdr_nodes)));
  }
}

struct SunReferenceConfig {
  int format_version = 12;
  bool giadr_as_array = false;
  bool giadr_empty_array = false;
  bool include_giadr = true;
  bool include_num_pixels = true;
  bool num_pixels_short = false;
  bool num_pixels_wrong_type = false;
  bool include_viadr_smr = true;
  long num_viadr_smr = 2;
  bool include_start_utc = true;
  bool start_utc_wrong_type = false;
  bool include_smr = true;
  bool smr_short = false;
  bool smr_wrong_type = false;
  std::array<uint16_t, kNumBands> num_pixels = {2, 3, 2, 2, 2, 2};
};

void PopulateSunReferenceProduct(CodaMemProduct& prod,
                                 const SunReferenceConfig& cfg) {
  ASSERT_TRUE(prod.SetProductInfo("EPS", "GOME_xxx_1B", cfg.format_version));

  CodaMemNode* mphr = prod.MakeRecord();
  prod.AddField(mphr, "ORBIT_START", prod.MakeUint32(54321));
  ASSERT_TRUE(prod.AddField(prod.root(), "MPHR", mphr));

  if (cfg.include_giadr) {
    auto make_giadr_rec = [&]() -> CodaMemNode* {
      CodaMemNode* g = prod.MakeRecord();
      if (cfg.include_num_pixels) {
        if (cfg.num_pixels_short) {
          const long dims[] = {1};
          const uint16_t vals[] = {cfg.num_pixels[0]};
          prod.AddField(g, "NUMBER_OF_PIXELS",
                        prod.MakeUint16Array(dims, vals));
        } else if (cfg.num_pixels_wrong_type) {
          const long dims[] = {kNumBands};
          const std::string_view strs[] = {"a", "b", "c", "d", "e", "f"};
          prod.AddField(g, "NUMBER_OF_PIXELS",
                        prod.MakeStringArray(dims, strs));
        } else {
          const long dims[] = {kNumBands};
          prod.AddField(g, "NUMBER_OF_PIXELS",
                        prod.MakeUint16Array(dims, cfg.num_pixels));
        }
      }
      return g;
    };

    if (cfg.giadr_as_array) {
      if (cfg.giadr_empty_array) {
        const long dims[] = {0};
        ASSERT_TRUE(
            prod.AddField(prod.root(), "GIADR_Bands",
                          prod.MakeNodeArray(dims, {}, make_giadr_rec())));
      } else {
        const long dims[] = {1};
        CodaMemNode* elems[] = {make_giadr_rec()};
        ASSERT_TRUE(prod.AddField(prod.root(), "GIADR_Bands",
                                  prod.MakeNodeArray(dims, elems)));
      }
    } else {
      ASSERT_TRUE(prod.AddField(prod.root(), "GIADR_Bands", make_giadr_rec()));
    }
  }

  if (cfg.include_viadr_smr) {
    if (cfg.num_viadr_smr == 0) {
      CodaMemNode* sample = prod.MakeRecord();
      prod.AddField(sample, "START_UTC_SUN", prod.MakeDouble(0.0));
      const long dims[] = {0};
      ASSERT_TRUE(prod.AddField(prod.root(), "VIADR_SMR",
                                prod.MakeNodeArray(dims, {}, sample)));
      return;
    }

    std::vector<CodaMemNode*> smr_elems;
    smr_elems.reserve(static_cast<size_t>(cfg.num_viadr_smr));
    std::vector<double> smr_vals(kMaxPixels);
    std::vector<double> wl_vals(kMaxPixels);
    for (long i = 0; i < cfg.num_viadr_smr; ++i) {
      CodaMemNode* rec = prod.MakeRecord();
      if (cfg.include_start_utc) {
        prod.AddField(rec, "START_UTC_SUN",
                      cfg.start_utc_wrong_type
                          ? prod.MakeString("bad")
                          : prod.MakeDouble(2000.0 + 10.0 * i));
      }
      prod.AddField(rec, "END_UTC_SUN", prod.MakeDouble(2005.0 + 10.0 * i));
      const long spec_len = cfg.smr_short ? 4 : kMaxPixels;
      const long sdims[] = {spec_len};
      for (long p = 0; p < spec_len; ++p) {
        smr_vals[static_cast<size_t>(p)] = 100.0 * (i + 1) + p;
        wl_vals[static_cast<size_t>(p)] = 250.0 + 0.1 * p;
      }
      if (cfg.include_smr) {
        if (cfg.smr_wrong_type) {
          const long bad_dims[] = {1};
          const std::string_view strs[] = {"bad"};
          prod.AddField(rec, "SMR", prod.MakeStringArray(bad_dims, strs));
        } else {
          prod.AddField(
              rec, "SMR",
              prod.MakeDoubleArray(
                  sdims, std::span<const double>(
                             smr_vals.data(), static_cast<size_t>(spec_len))));
        }
      }
      prod.AddField(
          rec, "LAMBDA_SMR",
          prod.MakeDoubleArray(
              sdims, std::span<const double>(wl_vals.data(),
                                             static_cast<size_t>(spec_len))));
      smr_elems.push_back(rec);
    }
    const long vdims[] = {cfg.num_viadr_smr};
    ASSERT_TRUE(prod.AddField(prod.root(), "VIADR_SMR",
                              prod.MakeNodeArray(vdims, smr_elems)));
  }
}

TEST(HarpIngestGome2L1Test, IngestRadianceNewVersionAllBandsAndContinuousMdrs) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOME2_L1");
  ASSERT_NE(nullptr, mod);

  // Include (first MDR must define the Earthshine record field for CODA):
  // - MDR 0 (mdr3): valid Earthshine (readout_offset == 1, max_num_recs == 16)
  // - MDR 1 (mdr4): valid Earthshine at +6.0s with same NUM_RECS (offset == 0)
  // - MDR 2 (mdr_trans): invalid (OUTPUT_SELECTION == 1 for radiance)
  // - MDR 3 (mdr5): valid Earthshine at +20.0s (time gap + after invalid MDR
  //                 -> readout_offset == 1, max_num_recs == 10 for NaN padding)
  // - MDR 4 (mdr_unavail): invalid (unavailable Earthshine field)
  MdrConfig mdr_unavail;
  mdr_unavail.include_lightsource = false;

  MdrConfig mdr_trans;
  mdr_trans.output_selection = 1;

  MdrConfig mdr3;
  mdr3.record_start_time = 1000.0;
  mdr3.num_recs = {16, 8, 4, 2, 2, 1};
  mdr3.use_undefined_rad_on_readout1 = true;

  MdrConfig mdr4;
  mdr4.record_start_time = 1006.0;
  mdr4.num_recs = {16, 8, 4, 2, 2, 1};
  mdr4.cloud_fit_mode_val = 1;  // invalid cloud data -> NaN

  MdrConfig mdr5;
  mdr5.record_start_time = 1026.0;
  mdr5.num_recs = {10, 8, 4, 2, 2, 1};
  mdr5.unique_int[0] = 6.0 / 10.0;
  mdr5.cloud_fit1_val = 400.0;

  MeasurementProductConfig cfg;
  cfg.format_version = 12;
  cfg.mdrs = {mdr3, mdr4, mdr_trans, mdr5, mdr_unavail};

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateMeasurementProduct(prod, cfg);

  IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
  ASSERT_EQ(HARP_SUCCESS, res.status);
  // 31 (mdr3) + 32 (mdr4) + 31 (mdr5) = 94 time steps.
  EXPECT_EQ(94, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(kMaxPixels, res.dimensions[harp_dimension_spectral]);
  EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(12345));

  // datetime: readout_offset=1 on mdr3 skips j=0, loop increments once ->
  // 1000 + 0.1875. readout_offset=0 on mdr4 starts at 1006 + 0.1875.
  EXPECT_DOUBLE_EQ(1000.0 + 0.1875, res.variables["datetime"].double_values[0]);
  EXPECT_DOUBLE_EQ(1006.0 + 0.1875,
                   res.variables["datetime"].double_values[31]);

  // scan_subindex: mdr3 (offset=1) starts with 0; mdr4 (offset=0) starts
  // with 15.
  EXPECT_EQ(0, res.variables["scan_subindex"].int8_values[0]);
  EXPECT_EQ(15, res.variables["scan_subindex"].int8_values[31]);
  EXPECT_EQ(0, res.variables["scan_subindex"].int8_values[32]);

  // scan_direction_type: index=0 -> 1 (backward), index=1 -> 0 (forward),
  // index=25 -> 1 (backward).
  EXPECT_EQ(1, res.variables["scan_direction_type"].int8_values[0]);
  EXPECT_EQ(0, res.variables["scan_direction_type"].int8_values[1]);
  EXPECT_EQ(1, res.variables["scan_direction_type"].int8_values[25]);

  // latitude_bounds: corners ABCD (1,2,3,4) -> BDCA (2,4,3,1) on valid readout.
  // Note: mdr3 has max_num_recs=16 and readout_offset=1, so readout 0 (index 0)
  // stays NaN and readout 1 starts at index 1.
  EXPECT_TRUE(std::isnan(res.variables["latitude_bounds"].double_values[0]));
  EXPECT_DOUBLE_EQ(2.0, res.variables["latitude_bounds"].double_values[4]);
  EXPECT_DOUBLE_EQ(4.0, res.variables["latitude_bounds"].double_values[5]);
  EXPECT_DOUBLE_EQ(3.0, res.variables["latitude_bounds"].double_values[6]);
  EXPECT_DOUBLE_EQ(1.0, res.variables["latitude_bounds"].double_values[7]);

  // Undefined VSF radiance at band 0, readout 1, pixel 0 -> NaN at row 1.
  EXPECT_TRUE(std::isnan(
      res.variables["wavelength_photon_radiance"].double_values[kMaxPixels]));

  // Cloud variables: mdr3 averages fit_data[2..3] at [0..1] and fit_data[4..5]
  // at [2..3], mdr4 is NaN (fit_mode!=0), and mdr5 has trailing NaN padding
  // for non-divisor max_num_recs=10.
  EXPECT_THAT(res.variables["cloud_top_pressure"].double_values[0],
              DoubleNear(std::sqrt(502.0 * 503.0), 1e-9));
  EXPECT_THAT(res.variables["cloud_top_pressure"].double_values[2],
              DoubleNear(std::sqrt(504.0 * 505.0), 1e-9));
  EXPECT_TRUE(
      std::isnan(res.variables["cloud_top_pressure"].double_values[31]));
  EXPECT_TRUE(
      std::isnan(res.variables["cloud_top_pressure"].double_values[93]));
  EXPECT_DOUBLE_EQ((0.27 + 0.28) / 2.0,
                   res.variables["cloud_fraction"].double_values[0]);
  EXPECT_DOUBLE_EQ((0.29 + 0.30) / 2.0,
                   res.variables["cloud_fraction"].double_values[2]);
  EXPECT_TRUE(std::isnan(res.variables["cloud_fraction"].double_values[31]));
  EXPECT_TRUE(std::isnan(res.variables["cloud_fraction"].double_values[93]));
}

TEST(HarpIngestGome2L1Test, IngestRadianceOldVersionAndBandFilter) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOME2_L1");
  ASSERT_NE(nullptr, mod);

  MdrConfig mdr;
  mdr.num_recs = {32, 16, 8, 4, 2, 1};
  mdr.num_pixels = {3, 2, 2, 2, 2, 2};
  mdr.cloud_fit2_val = -3000.0;  // Below -2147 -> invalid cloud data -> NaN.

  MeasurementProductConfig cfg;
  cfg.format_version = 11;
  cfg.mdrs = {mdr};

  CodaMemProduct prod;
  ASSERT_TRUE(prod.ok());
  PopulateMeasurementProduct(prod, cfg);

  IngestExerciseResult res =
      ExerciseIngestionModule(mod, prod.product(), "band=band-1a");
  ASSERT_EQ(HARP_SUCCESS, res.status);
  EXPECT_EQ(31, res.dimensions[harp_dimension_time]);
  EXPECT_EQ(3, res.dimensions[harp_dimension_spectral]);
  EXPECT_DOUBLE_EQ(10.1, res.variables["latitude"].double_values[0]);
  EXPECT_DOUBLE_EQ(20.1, res.variables["longitude"].double_values[0]);
  EXPECT_DOUBLE_EQ(2.0, res.variables["latitude_bounds"].double_values[0]);
  EXPECT_DOUBLE_EQ(4.0, res.variables["latitude_bounds"].double_values[1]);
  EXPECT_DOUBLE_EQ(3.0, res.variables["latitude_bounds"].double_values[2]);
  EXPECT_DOUBLE_EQ(1.0, res.variables["latitude_bounds"].double_values[3]);
  EXPECT_DOUBLE_EQ(11.0, res.variables["longitude_bounds"].double_values[0]);
  EXPECT_DOUBLE_EQ(30.0,
                   res.variables["solar_zenith_angle_toa"].double_values[0]);
  EXPECT_TRUE(std::isnan(res.variables["cloud_fraction"].double_values[0]));
  EXPECT_DOUBLE_EQ(240.0, res.variables["wavelength"].double_values[0]);
  EXPECT_DOUBLE_EQ(0.1875, res.variables["integration_time"].double_values[0]);

  // Also test old version with max_num_recs < 32 and readout_offset == 1.
  MdrConfig mdr_slow;
  mdr_slow.num_recs = {16, 8, 4, 2, 2, 1};
  mdr_slow.cloud_fit1_val = -3000000.0;  // Below -2147483 -> NaN.
  MeasurementProductConfig cfg_slow;
  cfg_slow.format_version = 10;
  cfg_slow.mdrs = {mdr_slow};
  CodaMemProduct prod_slow;
  ASSERT_TRUE(prod_slow.ok());
  PopulateMeasurementProduct(prod_slow, cfg_slow);
  IngestExerciseResult res_slow =
      ExerciseIngestionModule(mod, prod_slow.product(), "band=band-2a");
  ASSERT_EQ(HARP_SUCCESS, res_slow.status);
  EXPECT_TRUE(
      std::isnan(res_slow.variables["cloud_top_pressure"].double_values[0]));
}

TEST(HarpIngestGome2L1Test, IngestTransmissionSunAndMoonModes) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOME2_L1");
  ASSERT_NE(nullptr, mod);

  // 1. Transmission mode: MDR 0 has OUTPUT_SELECTION=0 (skipped), MDR 1 has 1.
  {
    MdrConfig skipped;
    skipped.output_selection = 0;
    MdrConfig valid;
    valid.output_selection = 1;
    valid.num_recs = {32, 32, 32, 32, 32, 32};

    MeasurementProductConfig cfg;
    cfg.format_version = 12;
    cfg.mdrs = {skipped, valid};

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateMeasurementProduct(prod, cfg);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "data=transmission;band=band-1b");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(31, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(2, res.dimensions[harp_dimension_spectral]);
    EXPECT_DOUBLE_EQ(210.0, res.variables["transmittance"].double_values[0]);
  }

  // 2. Sun mode: verify wavelength_photon_irradiance_sun is included and moon
  // is excluded.
  {
    MdrConfig mdr;
    mdr.lightsource = "Sun";
    mdr.include_output_selection = false;
    mdr.include_unique_int = false;
    mdr.include_geo = false;
    mdr.include_cloud = false;
    mdr.num_recs = {32, 32, 32, 32, 32, 32};

    MeasurementProductConfig cfg;
    cfg.format_version = 12;
    cfg.mdrs = {mdr};

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateMeasurementProduct(prod, cfg);

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "data=sun;band=band-3");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_TRUE(res.variables["wavelength_photon_irradiance_sun"].included);
    EXPECT_FALSE(res.variables["wavelength_photon_irradiance_moon"].included);
    EXPECT_DOUBLE_EQ(
        510.0,
        res.variables["wavelength_photon_irradiance_sun"].double_values[0]);
  }

  // 3. Moon mode: verify wavelength_photon_irradiance_moon is included and sun
  // is excluded.
  {
    MdrConfig mdr;
    mdr.lightsource = "Moon";
    mdr.include_output_selection = false;
    mdr.include_unique_int = false;
    mdr.include_geo = false;
    mdr.include_cloud = false;
    mdr.num_recs = {32, 32, 32, 32, 32, 32};

    MeasurementProductConfig cfg;
    cfg.format_version = 12;
    cfg.mdrs = {mdr};

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateMeasurementProduct(prod, cfg);

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "data=moon;band=band-4");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_FALSE(res.variables["wavelength_photon_irradiance_sun"].included);
    EXPECT_TRUE(res.variables["wavelength_photon_irradiance_moon"].included);
    EXPECT_DOUBLE_EQ(
        610.0,
        res.variables["wavelength_photon_irradiance_moon"].double_values[0]);
  }
}

TEST(HarpIngestGome2L1Test, IngestSunReferenceRecordAndArrayGiadr) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOME2_L1");
  ASSERT_NE(nullptr, mod);

  // 1. GIADR_Bands as record, all bands (4096 pixels).
  {
    SunReferenceConfig cfg;
    cfg.giadr_as_array = false;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateSunReferenceProduct(prod, cfg);

    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "data=sun_reference");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(kMaxPixels, res.dimensions[harp_dimension_spectral]);
    EXPECT_THAT(res.variables["orbit_index"].int32_values, ElementsAre(54321));
    EXPECT_THAT(res.variables["datetime_start"].double_values,
                ElementsAre(2000.0, 2010.0));
    EXPECT_THAT(res.variables["datetime_stop"].double_values,
                ElementsAre(2005.0, 2015.0));
    EXPECT_DOUBLE_EQ(
        100.0, res.variables["wavelength_photon_irradiance"].double_values[0]);
    EXPECT_DOUBLE_EQ(250.0, res.variables["wavelength"].double_values[0]);
  }

  // 2. GIADR_Bands as array, filtered to band-2a (offset = 2 + 3 = 5, len = 2).
  {
    SunReferenceConfig cfg;
    cfg.giadr_as_array = true;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateSunReferenceProduct(prod, cfg);

    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "data=sun_reference;band=band-2a");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(2, res.dimensions[harp_dimension_time]);
    EXPECT_EQ(2, res.dimensions[harp_dimension_spectral]);
    EXPECT_THAT(res.variables["wavelength_photon_irradiance"].double_values,
                ElementsAre(105.0, 106.0, 205.0, 206.0));
  }
}

TEST(HarpIngestGome2L1Test, DirectIngestionInitBandNamesAndErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOME2_L1");
  ASSERT_NE(nullptr, mod);

  // 1. Missing product info -> coda_get_product_version returns 0 ->
  // HARP_ERROR_INGESTION (format_version < 5).
  {
    MeasurementProductConfig cfg;
    cfg.set_product_info = false;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateMeasurementProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 2. Format version < 5 -> HARP_ERROR_INGESTION.
  {
    MeasurementProductConfig cfg;
    cfg.format_version = 4;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateMeasurementProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_INGESTION, res.failed_errno);
  }

  // 3. Direct ingestion_init with band="BAND_1A" (band_name_in_file match) and
  // band="invalid" (band_name_to_band_nr returns -1 -> all bands), plus
  // num_pixels changing across MDRs.
  {
    MdrConfig mdr0;
    mdr0.num_pixels = {2, 2, 2, 2, 2, 2};
    MdrConfig mdr1;
    mdr1.num_pixels = {3, 2, 2, 2, 2, 2};
    MeasurementProductConfig cfg;
    cfg.format_version = 12;
    cfg.mdrs = {mdr0, mdr1};

    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateMeasurementProduct(prod, cfg);

    for (const char* opt : {"band=BAND_1A", "band=invalid"}) {
      harp_ingestion_options* raw_opts = nullptr;
      ASSERT_EQ(HARP_SUCCESS,
                harp_ingestion_options_from_string(opt, &raw_opts));
      ScopedHarpIngestionOptions opts(raw_opts);

      harp_product_definition* prod_def = nullptr;
      void* user_data = nullptr;
      harp_set_error(HARP_SUCCESS, nullptr);
      ASSERT_EQ(0, mod->ingestion_init(mod, prod.product(), opts.get(),
                                       &prod_def, &user_data));
      auto done_guard =
          absl::MakeCleanup([&] { mod->ingestion_done(user_data); });
      EXPECT_EQ(HARP_ERROR_INGESTION, harp_errno);
      harp_set_error(HARP_SUCCESS, nullptr);
    }
  }
}

TEST(HarpIngestGome2L1Test, MeasurementInitAndVariableErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOME2_L1");
  ASSERT_NE(nullptr, mod);

  auto run_expect_init_error = [&](const MeasurementProductConfig& cfg) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateMeasurementProduct(prod, cfg);
    harp_set_error(HARP_SUCCESS, nullptr);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
  };

  // Init-time errors.
  {
    MeasurementProductConfig cfg;
    cfg.include_mdr = false;
    run_expect_init_error(cfg);
  }
  {
    MeasurementProductConfig cfg;
    cfg.mdrs.clear();
    run_expect_init_error(cfg);
  }
  {
    MeasurementProductConfig cfg;
    cfg.mdrs[0].include_record_header = false;
    run_expect_init_error(cfg);
  }
  {
    MeasurementProductConfig cfg;
    cfg.mdrs[0].include_record_start_time = false;
    run_expect_init_error(cfg);
  }
  {
    MeasurementProductConfig cfg;
    cfg.mdrs[0].record_start_time_wrong_type = true;
    run_expect_init_error(cfg);
  }
  {
    MeasurementProductConfig cfg;
    cfg.mdrs[0].include_num_recs = false;
    run_expect_init_error(cfg);
  }
  {
    MeasurementProductConfig cfg;
    cfg.mdrs[0].num_recs_short = true;
    run_expect_init_error(cfg);
  }
  {
    MeasurementProductConfig cfg;
    cfg.mdrs[0].num_recs_wrong_type = true;
    run_expect_init_error(cfg);
  }
  {
    MeasurementProductConfig cfg;
    cfg.mdrs[0].include_unique_int = false;
    run_expect_init_error(cfg);
  }
  {
    MeasurementProductConfig cfg;
    cfg.mdrs[0].unique_int_wrong_type = true;
    run_expect_init_error(cfg);
  }
  {
    MeasurementProductConfig cfg;
    cfg.mdrs[0].unique_int.fill(99.0);
    run_expect_init_error(cfg);
  }
  {
    MeasurementProductConfig cfg;
    cfg.mdrs[0].include_band0 = false;
    run_expect_init_error(cfg);
  }

  // Variable read errors (stop_on_error = false exercises every failing
  // reader).
  auto run_var_errors = [&](const MdrConfig& mcfg, int format_version,
                            const char* var_name, const char* opt = nullptr) {
    MeasurementProductConfig cfg;
    cfg.format_version = format_version;
    cfg.mdrs = {mcfg};
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateMeasurementProduct(prod, cfg);
    harp_set_error(HARP_SUCCESS, nullptr);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product(), opt,
                                                       /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables[var_name].read_status) << var_name;
  };

  // orbit_index errors.
  {
    MeasurementProductConfig cfg;
    cfg.include_orbit_start = false;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateMeasurementProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "band=band-1a", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
  }
  {
    MeasurementProductConfig cfg;
    cfg.orbit_start_wrong_type = true;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateMeasurementProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "band=band-1a", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.variables["orbit_index"].read_status);
  }

  // New-version GEO_EARTH_ACTUAL_* errors.
  for (const auto& [mutator, var] :
       std::vector<std::pair<void (*)(MdrConfig&), const char*>>{
           {[](MdrConfig& c) { c.include_geo = false; }, "latitude"},
           {[](MdrConfig& c) { c.geo_empty = true; }, "latitude"},
           {[](MdrConfig& c) { c.geo_short = true; }, "latitude"},
           {[](MdrConfig& c) { c.geo_missing_centre = true; }, "latitude"},
           {[](MdrConfig& c) { c.geo_missing_centre_lat = true; }, "latitude"},
           {[](MdrConfig& c) { c.geo_centre_lat_wrong_type = true; },
            "latitude"},
           {[](MdrConfig& c) { c.geo_missing_corner = true; },
            "latitude_bounds"},
           {[](MdrConfig& c) { c.geo_corner_short = true; }, "latitude_bounds"},
           {[](MdrConfig& c) { c.geo_missing_corner_lat = true; },
            "latitude_bounds"},
           {[](MdrConfig& c) { c.geo_corner_lat_wrong_type = true; },
            "latitude_bounds"},
           {[](MdrConfig& c) { c.geo_missing_sza = true; },
            "solar_zenith_angle_toa"},
           {[](MdrConfig& c) { c.geo_sza_short = true; },
            "solar_zenith_angle_toa"},
           {[](MdrConfig& c) { c.geo_sza_wrong_type = true; },
            "solar_zenith_angle_toa"},
       }) {
    MdrConfig mcfg;
    mutator(mcfg);
    run_var_errors(mcfg, 12, var, "band=band-1a");
  }

  // Old-version GEO_EARTH_ACTUAL errors.
  for (const auto& [mutator, var] :
       std::vector<std::pair<void (*)(MdrConfig&), const char*>>{
           {[](MdrConfig& c) { c.include_geo = false; }, "latitude"},
           {[](MdrConfig& c) { c.geo_missing_centre = true; }, "latitude"},
           {[](MdrConfig& c) { c.geo_old_centre_short = true; }, "latitude"},
           {[](MdrConfig& c) { c.geo_missing_centre_lat = true; }, "latitude"},
           {[](MdrConfig& c) { c.geo_centre_lat_wrong_type = true; },
            "latitude"},
           {[](MdrConfig& c) { c.geo_missing_corner = true; },
            "latitude_bounds"},
           {[](MdrConfig& c) { c.geo_corner_short = true; }, "latitude_bounds"},
           {[](MdrConfig& c) { c.geo_missing_sza = true; },
            "solar_zenith_angle_toa"},
           {[](MdrConfig& c) { c.geo_sza_short = true; },
            "solar_zenith_angle_toa"},
           {[](MdrConfig& c) { c.geo_sza_wrong_type = true; },
            "solar_zenith_angle_toa"},
       }) {
    MdrConfig mcfg;
    mutator(mcfg);
    run_var_errors(mcfg, 11, var, "band=band-1a");
  }

  // Cloud errors.
  for (const auto& mutator : std::vector<void (*)(MdrConfig&)>{
           [](MdrConfig& c) { c.include_cloud = false; },
           [](MdrConfig& c) { c.cloud_missing_fit_mode = true; },
           [](MdrConfig& c) { c.cloud_fit_mode_wrong_type = true; },
           [](MdrConfig& c) { c.cloud_missing_fit1 = true; },
           [](MdrConfig& c) { c.cloud_fit1_wrong_type = true; },
       }) {
    MdrConfig mcfg;
    mutator(mcfg);
    run_var_errors(mcfg, 12, "cloud_top_pressure", "band=band-1a");
  }
  {
    MdrConfig mcfg;
    mcfg.cloud_missing_fit2 = true;
    run_var_errors(mcfg, 12, "cloud_fraction", "band=band-1a");
  }

  // Spectral errors (BAND_1A, WAVELENGTH_1A, INTEGRATION_TIMES).
  {
    MdrConfig mcfg;
    mcfg.band0_empty = true;
    run_var_errors(mcfg, 12, "wavelength_photon_radiance", "band=band-1a");
  }
  {
    MdrConfig mcfg;
    mcfg.band0_short = true;
    run_var_errors(mcfg, 12, "wavelength_photon_radiance", "band=band-1a");
  }
  {
    MdrConfig mcfg;
    mcfg.band0_missing_rad = true;
    run_var_errors(mcfg, 12, "wavelength_photon_radiance", "band=band-1a");
  }
  {
    MdrConfig mcfg;
    mcfg.band0_rad_wrong_type = true;
    run_var_errors(mcfg, 12, "wavelength_photon_radiance", "band=band-1a");
  }
  {
    MdrConfig mcfg;
    mcfg.missing_wl0 = true;
    run_var_errors(mcfg, 12, "wavelength", "band=band-1a");
  }
  {
    MdrConfig mcfg;
    mcfg.empty_wl0 = true;
    run_var_errors(mcfg, 12, "wavelength", "band=band-1a");
  }
  {
    MdrConfig mcfg;
    mcfg.short_wl0 = true;
    run_var_errors(mcfg, 12, "wavelength", "band=band-1a");
  }
  {
    MdrConfig mcfg;
    mcfg.wl0_wrong_type = true;
    run_var_errors(mcfg, 12, "wavelength", "band=band-1a");
  }
  {
    MdrConfig mcfg;
    mcfg.missing_int_times = true;
    run_var_errors(mcfg, 12, "integration_time", "band=band-1a");
  }
  {
    MdrConfig mcfg;
    mcfg.short_int_times = true;
    run_var_errors(mcfg, 12, "integration_time", "band=band-2a");
  }
  {
    MdrConfig mcfg;
    mcfg.int_times_wrong_type = true;
    run_var_errors(mcfg, 12, "integration_time", "band=band-1a");
  }

  // Multi-MDR error: MDR 0 valid, MDR 1 has missing OUTPUT_SELECTION and
  // wrong-type OUTPUT_SELECTION -> both skipped as invalid MDRs, leaving 1
  // valid MDR. Also cover missing lightsource and non-record lightsource MDRs.
  {
    MdrConfig valid;
    MdrConfig invalid_missing_os;
    invalid_missing_os.include_output_selection = false;
    MdrConfig invalid_wrong_os;
    invalid_wrong_os.output_selection_wrong_type = true;
    MeasurementProductConfig cfg;
    cfg.mdrs = {valid, invalid_missing_os, invalid_wrong_os};
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateMeasurementProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(31, res.dimensions[harp_dimension_time]);
  }
  for (const auto& mutator : std::vector<void (*)(MdrConfig&)>{
           [](MdrConfig& c) { c.include_lightsource = false; },
           [](MdrConfig& c) { c.lightsource_is_record = false; },
       }) {
    MdrConfig mcfg;
    mutator(mcfg);
    MeasurementProductConfig cfg;
    cfg.mdrs = {mcfg};
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateMeasurementProduct(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(0, res.dimensions[harp_dimension_time]);
  }
}

TEST(HarpIngestGome2L1Test, SunReferenceErrorPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("GOME2_L1");
  ASSERT_NE(nullptr, mod);

  auto run_init_err = [&](const SunReferenceConfig& cfg) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateSunReferenceProduct(prod, cfg);
    harp_set_error(HARP_SUCCESS, nullptr);
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "data=sun_reference");
    EXPECT_EQ(-1, res.status);
  };

  {
    SunReferenceConfig cfg;
    cfg.include_viadr_smr = false;
    run_init_err(cfg);
  }
  {
    SunReferenceConfig cfg;
    cfg.include_giadr = false;
    run_init_err(cfg);
  }
  {
    SunReferenceConfig cfg;
    cfg.giadr_as_array = true;
    cfg.giadr_empty_array = true;
    run_init_err(cfg);
  }
  {
    SunReferenceConfig cfg;
    cfg.include_num_pixels = false;
    run_init_err(cfg);
  }
  {
    SunReferenceConfig cfg;
    cfg.num_pixels_short = true;
    run_init_err(cfg);
  }
  {
    SunReferenceConfig cfg;
    cfg.num_pixels_wrong_type = true;
    run_init_err(cfg);
  }

  auto run_var_err = [&](const SunReferenceConfig& cfg, const char* var) {
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateSunReferenceProduct(prod, cfg);
    harp_set_error(HARP_SUCCESS, nullptr);
    IngestExerciseResult res = ExerciseIngestionModule(
        mod, prod.product(), "data=sun_reference", /*stop_on_error=*/false);
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(-1, res.variables[var].read_status) << var;
  };

  {
    SunReferenceConfig cfg;
    cfg.num_viadr_smr = 0;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateSunReferenceProduct(prod, cfg);
    IngestExerciseResult res =
        ExerciseIngestionModule(mod, prod.product(), "data=sun_reference");
    ASSERT_EQ(HARP_SUCCESS, res.status);
    EXPECT_EQ(0, res.dimensions[harp_dimension_time]);

    // Invoke read_all directly on empty VIADR_SMR to cover
    // coda_cursor_goto_first_array_element error paths in get_smr_*.
    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_from_string(
                                "data=sun_reference", &raw_opts));
    ScopedHarpIngestionOptions opts(raw_opts);
    harp_product_definition* prod_def = nullptr;
    void* user_data = nullptr;
    ASSERT_EQ(0, mod->ingestion_init(mod, prod.product(), opts.get(), &prod_def,
                                     &user_data));
    auto done_guard =
        absl::MakeCleanup([&] { mod->ingestion_done(user_data); });
    double dummy = 0.0;
    harp_array arr{};
    arr.double_data = &dummy;
    EXPECT_EQ(-1, prod_def->variable_definition[0]->read_all(user_data, arr));
    EXPECT_EQ(-1, prod_def->variable_definition[3]->read_all(user_data, arr));
    harp_set_error(HARP_SUCCESS, nullptr);
  }
  {
    SunReferenceConfig cfg;
    cfg.include_start_utc = false;
    run_var_err(cfg, "datetime_start");
  }
  {
    SunReferenceConfig cfg;
    cfg.start_utc_wrong_type = true;
    run_var_err(cfg, "datetime_start");
  }
  {
    SunReferenceConfig cfg;
    cfg.include_smr = false;
    run_var_err(cfg, "wavelength_photon_irradiance");
  }
  {
    SunReferenceConfig cfg;
    cfg.smr_short = true;
    run_var_err(cfg, "wavelength_photon_irradiance");
  }
  {
    SunReferenceConfig cfg;
    cfg.smr_wrong_type = true;
    run_var_err(cfg, "wavelength_photon_irradiance");
  }
}

}  // namespace
}  // namespace stcorp_harp
