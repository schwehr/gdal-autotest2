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
// Unit tests for IASI Level 1 ingestion module
// (libharp/harp-ingest-iasi_l1.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingest-iasi_l1.c

#include <cmath>
#include <cstdint>
#include <cstdlib>
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

constexpr long kScansPerScanline = 30;
constexpr long kSpectraPerScan = 4;
constexpr long kSpectraPerScanline = kScansPerScanline * kSpectraPerScan;
constexpr long kNumPixels = 4;

struct MdrOptions {
  bool dummy_record = false;
  bool omit_inner_mdr = false;
  bool include_record_header = true;
  bool include_start_time = true;
  bool start_time_wrong_type = false;
  double start_time = 1000.0;
  bool include_loc = true;
  bool loc_wrong_type = false;
  bool include_sun_angles = true;
  bool sun_angles_short = false;
  bool sun_angles_wrong_type = false;
  bool include_metop_angles = true;
  bool include_nsfirst = true;
  bool nsfirst_wrong_type = false;
  int32_t nsfirst = 100;
  bool include_nslast = true;
  bool nslast_wrong_type = false;
  int32_t nslast = 103;
  bool include_dwn = true;
  bool dwn_wrong_type = false;
  double dwn = 25.0;
  bool include_spect = true;
  bool spect_scalar = false;
  bool spect_short = false;
};

struct IasiL1Config {
  bool set_product_info = true;
  bool include_mphr = true;
  bool include_orbit_start = true;
  bool orbit_start_wrong_type = false;
  uint32_t orbit_start = 54321;

  bool include_mdr = true;
  bool mdr_scalar = false;
  bool mdr_empty = false;
  std::vector<MdrOptions> mdrs;

  bool include_giadr = true;
  bool include_nb_scale = true;
  bool nb_scale_wrong_type = false;
  int16_t nb_scale = 2;
  bool include_scale_factor = true;
  bool scale_factor_scalar = false;
  bool scale_factor_wrong_type = false;
  bool include_ns_first_scale = true;
  bool ns_first_scale_wrong_type = false;
  bool include_ns_last_scale = true;
  bool ns_last_scale_wrong_type = false;
};

CodaMemNode* MakeMdrWrapper(CodaMemProduct& prod, const MdrOptions& opt,
                            int mdr_index) {
  CodaMemNode* wrapper = prod.MakeRecord();
  if (opt.omit_inner_mdr) {
    return wrapper;
  }

  CodaMemNode* inner = prod.MakeRecord();
  if (opt.dummy_record) {
    // Field 0 unavailable makes coda_cursor_get_record_field_available_status
    // return is_mdr = 0, exercising dummy MDR skipping.
    prod.AddUnavailableField(inner, "RECORD_HEADER");
    prod.AddField(wrapper, "MDR", inner);
    return wrapper;
  }

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

  if (opt.include_loc) {
    const long loc_dims[] = {kScansPerScanline, kSpectraPerScan, 2};
    if (opt.loc_wrong_type) {
      std::vector<std::string_view> bad(kSpectraPerScanline * 2, "bad");
      prod.AddField(inner, "GGeoSondLoc", prod.MakeStringArray(loc_dims, bad));
    } else {
      std::vector<double> locs(kSpectraPerScanline * 2);
      for (long i = 0; i < kSpectraPerScanline; ++i) {
        locs[2 * i] = -10.0 + 0.1 * i + mdr_index;      // lon
        locs[2 * i + 1] = 20.0 + 0.05 * i + mdr_index;  // lat
      }
      prod.AddField(inner, "GGeoSondLoc", prod.MakeDoubleArray(loc_dims, locs));
    }
  }

  if (opt.include_sun_angles) {
    if (opt.sun_angles_short) {
      const long short_dims[] = {1};
      const double vals[] = {10.0};
      prod.AddField(inner, "GGeoSondAnglesSUN",
                    prod.MakeDoubleArray(short_dims, vals));
    } else if (opt.sun_angles_wrong_type) {
      const long ang_dims[] = {kScansPerScanline, kSpectraPerScan, 2};
      std::vector<std::string_view> bad(kSpectraPerScanline * 2, "bad");
      prod.AddField(inner, "GGeoSondAnglesSUN",
                    prod.MakeStringArray(ang_dims, bad));
    } else {
      const long ang_dims[] = {kScansPerScanline, kSpectraPerScan, 2};
      std::vector<double> vals(kSpectraPerScanline * 2);
      for (long i = 0; i < kSpectraPerScanline; ++i) {
        vals[2 * i] = 30.0 + 0.1 * i;       // SZA
        vals[2 * i + 1] = 120.0 + 0.1 * i;  // SAA
      }
      prod.AddField(inner, "GGeoSondAnglesSUN",
                    prod.MakeDoubleArray(ang_dims, vals));
    }
  }

  if (opt.include_metop_angles) {
    const long ang_dims[] = {kScansPerScanline, kSpectraPerScan, 2};
    std::vector<double> vals(kSpectraPerScanline * 2);
    for (long i = 0; i < kSpectraPerScanline; ++i) {
      vals[2 * i] = 5.0 + 0.1 * i;        // VZA
      vals[2 * i + 1] = 200.0 + 0.1 * i;  // VAA
    }
    prod.AddField(inner, "GGeoSondAnglesMETOP",
                  prod.MakeDoubleArray(ang_dims, vals));
  }

  if (opt.include_nsfirst) {
    prod.AddField(inner, "IDefNsfirst1b",
                  opt.nsfirst_wrong_type ? prod.MakeString("bad")
                                         : prod.MakeInt32(opt.nsfirst));
  }
  if (opt.include_nslast) {
    prod.AddField(inner, "IDefNslast1b",
                  opt.nslast_wrong_type ? prod.MakeString("bad")
                                        : prod.MakeInt32(opt.nslast));
  }
  if (opt.include_dwn) {
    prod.AddField(
        inner, "IDefSpectDWn1b",
        opt.dwn_wrong_type ? prod.MakeString("bad") : prod.MakeDouble(opt.dwn));
  }

  if (opt.include_spect) {
    if (opt.spect_scalar) {
      prod.AddField(inner, "GS1cSpect", prod.MakeInt16(1));
    } else if (opt.spect_short) {
      const long dims[] = {1, 1, kNumPixels};
      const int16_t vals[kNumPixels] = {10, 20, 30, 40};
      prod.AddField(inner, "GS1cSpect", prod.MakeInt16Array(dims, vals));
    } else {
      const long dims[] = {kScansPerScanline, kSpectraPerScan, kNumPixels};
      std::vector<int16_t> vals(kSpectraPerScanline * kNumPixels);
      for (long r = 0; r < kSpectraPerScanline; ++r) {
        for (long p = 0; p < kNumPixels; ++p) {
          vals[r * kNumPixels + p] =
              static_cast<int16_t>((r + 1) * 100 + p + mdr_index);
        }
      }
      prod.AddField(inner, "GS1cSpect", prod.MakeInt16Array(dims, vals));
    }
  }

  prod.AddField(wrapper, "MDR", inner);
  return wrapper;
}

void PopulateIasiL1Product(CodaMemProduct& prod, const IasiL1Config& cfg) {
  if (cfg.set_product_info) {
    ASSERT_TRUE(prod.SetProductInfo("EPS", "IASI_xxx_1C", 1));
  }

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
      CodaMemNode* sample = MakeMdrWrapper(prod, MdrOptions{}, 0);
      const long dims[] = {0};
      ASSERT_TRUE(prod.AddField(prod.root(), "MDR",
                                prod.MakeNodeArray(dims, {}, sample)));
    } else {
      std::vector<CodaMemNode*> elems;
      elems.reserve(cfg.mdrs.size());
      for (size_t i = 0; i < cfg.mdrs.size(); ++i) {
        elems.push_back(MakeMdrWrapper(prod, cfg.mdrs[i], static_cast<int>(i)));
      }
      const long dims[] = {static_cast<long>(elems.size())};
      ASSERT_TRUE(
          prod.AddField(prod.root(), "MDR", prod.MakeNodeArray(dims, elems)));
    }
  }

  if (cfg.include_giadr) {
    CodaMemNode* giadr = prod.MakeRecord();
    if (cfg.include_nb_scale) {
      ASSERT_TRUE(prod.AddField(giadr, "IDefScaleSondNbScale",
                                cfg.nb_scale_wrong_type
                                    ? prod.MakeString("bad")
                                    : prod.MakeInt16(cfg.nb_scale)));
    }
    const long sf_dims[] = {2};
    if (cfg.include_scale_factor) {
      if (cfg.scale_factor_scalar) {
        ASSERT_TRUE(prod.AddField(giadr, "IDefScaleSondScaleFactor",
                                  prod.MakeInt16(1)));
      } else if (cfg.scale_factor_wrong_type) {
        const std::string_view bad[] = {"1", "2"};
        ASSERT_TRUE(prod.AddField(giadr, "IDefScaleSondScaleFactor",
                                  prod.MakeStringArray(sf_dims, bad)));
      } else {
        const int16_t sfs[] = {1, 2};
        ASSERT_TRUE(prod.AddField(giadr, "IDefScaleSondScaleFactor",
                                  prod.MakeInt16Array(sf_dims, sfs)));
      }
    }
    if (cfg.include_ns_first_scale) {
      if (cfg.ns_first_scale_wrong_type) {
        const std::string_view bad[] = {"100", "102"};
        ASSERT_TRUE(prod.AddField(giadr, "IDefScaleSondNsfirst",
                                  prod.MakeStringArray(sf_dims, bad)));
      } else {
        const int16_t firsts[] = {100, 102};
        ASSERT_TRUE(prod.AddField(giadr, "IDefScaleSondNsfirst",
                                  prod.MakeInt16Array(sf_dims, firsts)));
      }
    }
    if (cfg.include_ns_last_scale) {
      if (cfg.ns_last_scale_wrong_type) {
        const std::string_view bad[] = {"101", "103"};
        ASSERT_TRUE(prod.AddField(giadr, "IDefScaleSondNslast",
                                  prod.MakeStringArray(sf_dims, bad)));
      } else {
        const int16_t lasts[] = {101, 103};
        ASSERT_TRUE(prod.AddField(giadr, "IDefScaleSondNslast",
                                  prod.MakeInt16Array(sf_dims, lasts)));
      }
    }
    ASSERT_TRUE(prod.AddField(prod.root(), "GIADR_ScaleFactors", giadr));
  }
}

#if !defined(HARP_TEST_HAS_ASAN)
// Upstream harp-ingest-iasi_l1.c allocates info->mdr_cursors in
// init_dimensions() without freeing it in ingestion_done(). Run in an
// EXPECT_EXIT subprocess so coverage flushes on std::exit(0) without failing
// HeapLeakChecker in the parent; guarded under ASan where LSan checks children.
[[noreturn]] void RunIasiL1SuccessAndExit() {
  {
    ScopedHarpInit init;
    if (init.status() != HARP_SUCCESS) std::exit(1);

    const harp_ingestion_module* mod = FindIngestionModule("IASI_L1");
    if (mod == nullptr) std::exit(2);

    MdrOptions dummy_mdr;
    dummy_mdr.dummy_record = true;

    MdrOptions mdr1;
    mdr1.start_time = 1000.0;
    mdr1.nsfirst = 100;
    mdr1.nslast = 103;

    MdrOptions mdr2;
    mdr2.start_time = 2000.0;
    mdr2.nsfirst = 100;
    mdr2.nslast = 102;  // Fewer active channels than num_pixels.

    IasiL1Config cfg;
    cfg.mdrs = {dummy_mdr, mdr1, mdr2};

    CodaMemProduct prod;
    if (!prod.ok()) std::exit(3);
    PopulateIasiL1Product(prod, cfg);

    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    if (res.status != HARP_SUCCESS) std::exit(4);
    if (res.dimensions[harp_dimension_time] != 2 * kSpectraPerScanline ||
        res.dimensions[harp_dimension_spectral] != kNumPixels) {
      std::exit(5);
    }
    if (res.variables["orbit_index"].int32_values[0] != 54321 ||
        res.variables["datetime"].double_values[0] != 1000.0 ||
        std::abs(res.variables["datetime"].double_values[4] -
                 (1000.0 + 8.0 / 37.0)) > 1e-12 ||
        res.variables["datetime"].double_values[kSpectraPerScanline] !=
            2000.0) {
      std::exit(6);
    }
    if (res.variables["longitude"].double_values[0] != -9.0 ||
        res.variables["latitude"].double_values[0] != 21.0 ||
        res.variables["solar_zenith_angle"].double_values[0] != 30.0 ||
        res.variables["solar_azimuth_angle"].double_values[0] != 120.0 ||
        res.variables["sensor_zenith_angle"].double_values[0] != 5.0 ||
        res.variables["sensor_azimuth_angle"].double_values[0] != 200.0) {
      std::exit(7);
    }
    if (res.variables["scan_subindex"].int8_values[0] != 0 ||
        res.variables["scan_subindex"].int8_values[119] != 119 ||
        res.variables["scan_subindex"].int8_values[120] != 0) {
      std::exit(8);
    }
    if (std::abs(res.variables["wavenumber_radiance"].float_values[0] - 10.1f) >
            1e-4f ||
        std::abs(res.variables["wavenumber_radiance"].float_values[2] - 1.03f) >
            1e-4f) {
      std::exit(9);
    }
    const long mdr2_off = kSpectraPerScanline * kNumPixels;
    if (std::abs(res.variables["wavenumber"].float_values[0] - 2500.0f) >
            1e-3f ||
        std::abs(res.variables["wavenumber"].float_values[mdr2_off + 2] -
                 2550.0f) > 1e-3f ||
        res.variables["wavenumber"].float_values[mdr2_off + 3] != 0.0f) {
      std::exit(10);
    }
  }
  std::exit(0);
}

TEST(HarpIngestIasiL1Test, IngestSuccessWithDummyAndMultiMdr) {
  EXPECT_EXIT(RunIasiL1SuccessAndExit(), ::testing::ExitedWithCode(0), "");
}
#endif

TEST(HarpIngestIasiL1Test, InitErrorsBeforeMdrAllocation) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const harp_ingestion_module* mod = FindIngestionModule("IASI_L1");
  ASSERT_NE(nullptr, mod);

  // 1. Missing MDR field.
  {
    IasiL1Config cfg;
    cfg.include_mdr = false;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateIasiL1Product(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }

  // 2. MDR is scalar (not an array).
  {
    IasiL1Config cfg;
    cfg.mdr_scalar = true;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateIasiL1Product(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
  }

  // 3. MDR array is empty (0 elements).
  {
    IasiL1Config cfg;
    cfg.mdr_empty = true;
    CodaMemProduct prod;
    ASSERT_TRUE(prod.ok());
    PopulateIasiL1Product(prod, cfg);
    IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
    EXPECT_EQ(-1, res.status);
    EXPECT_EQ(HARP_ERROR_CODA, res.failed_errno);
  }
}

#if !defined(HARP_TEST_HAS_ASAN)
[[noreturn]] void RunInitAndGiadrErrorsAndExit() {
  {
    ScopedHarpInit init;
    if (init.status() != HARP_SUCCESS) std::exit(1);
    const harp_ingestion_module* mod = FindIngestionModule("IASI_L1");
    if (mod == nullptr) std::exit(2);

    auto expect_init_fail = [&](const IasiL1Config& cfg, int code) {
      CodaMemProduct prod;
      if (!prod.ok()) std::exit(code);
      PopulateIasiL1Product(prod, cfg);
      IngestExerciseResult res = ExerciseIngestionModule(mod, prod.product());
      if (res.status != -1) std::exit(code);
    };

    // MDR[0] missing inner MDR field.
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.omit_inner_mdr = true;
      cfg.mdrs = {m};
      expect_init_fail(cfg, 10);
    }
    // All MDRs are dummy (valid_scanlines == 0).
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.dummy_record = true;
      cfg.mdrs = {m};
      expect_init_fail(cfg, 11);
    }
    // Missing GS1cSpect.
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.include_spect = false;
      cfg.mdrs = {m};
      expect_init_fail(cfg, 12);
    }
    // Scalar GS1cSpect (coda_cursor_get_array_dim fails).
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.spect_scalar = true;
      cfg.mdrs = {m};
      expect_init_fail(cfg, 13);
    }
    // Missing GIADR_ScaleFactors.
    {
      IasiL1Config cfg;
      cfg.mdrs = {MdrOptions{}};
      cfg.include_giadr = false;
      expect_init_fail(cfg, 14);
    }
    // Missing IDefScaleSondNbScale.
    {
      IasiL1Config cfg;
      cfg.mdrs = {MdrOptions{}};
      cfg.include_nb_scale = false;
      expect_init_fail(cfg, 15);
    }
    // Wrong type IDefScaleSondNbScale.
    {
      IasiL1Config cfg;
      cfg.mdrs = {MdrOptions{}};
      cfg.nb_scale_wrong_type = true;
      expect_init_fail(cfg, 16);
    }
    // Missing IDefScaleSondScaleFactor.
    {
      IasiL1Config cfg;
      cfg.mdrs = {MdrOptions{}};
      cfg.include_scale_factor = false;
      expect_init_fail(cfg, 17);
    }
    // Scalar IDefScaleSondScaleFactor.
    {
      IasiL1Config cfg;
      cfg.mdrs = {MdrOptions{}};
      cfg.scale_factor_scalar = true;
      expect_init_fail(cfg, 18);
    }
    // Wrong type IDefScaleSondScaleFactor.
    {
      IasiL1Config cfg;
      cfg.mdrs = {MdrOptions{}};
      cfg.scale_factor_wrong_type = true;
      expect_init_fail(cfg, 19);
    }
    // Missing IDefScaleSondNsfirst.
    {
      IasiL1Config cfg;
      cfg.mdrs = {MdrOptions{}};
      cfg.include_ns_first_scale = false;
      expect_init_fail(cfg, 20);
    }
    // Wrong type IDefScaleSondNsfirst.
    {
      IasiL1Config cfg;
      cfg.mdrs = {MdrOptions{}};
      cfg.ns_first_scale_wrong_type = true;
      expect_init_fail(cfg, 21);
    }
    // Missing IDefScaleSondNslast.
    {
      IasiL1Config cfg;
      cfg.mdrs = {MdrOptions{}};
      cfg.include_ns_last_scale = false;
      expect_init_fail(cfg, 22);
    }
    // Wrong type IDefScaleSondNslast.
    {
      IasiL1Config cfg;
      cfg.mdrs = {MdrOptions{}};
      cfg.ns_last_scale_wrong_type = true;
      expect_init_fail(cfg, 23);
    }
  }
  std::exit(0);
}

TEST(HarpIngestIasiL1Test, InitAndGiadrErrors) {
  EXPECT_EXIT(RunInitAndGiadrErrorsAndExit(), ::testing::ExitedWithCode(0), "");
}

[[noreturn]] void RunVariableReadErrorPathsAndExit() {
  {
    ScopedHarpInit init;
    if (init.status() != HARP_SUCCESS) std::exit(1);
    const harp_ingestion_module* mod = FindIngestionModule("IASI_L1");
    if (mod == nullptr) std::exit(2);

    auto run_with = [&](const IasiL1Config& cfg) {
      CodaMemProduct prod;
      if (!prod.ok()) std::exit(3);
      PopulateIasiL1Product(prod, cfg);
      return ExerciseIngestionModule(mod, prod.product(),
                                     /*options_str=*/nullptr,
                                     /*stop_on_error=*/false);
    };

    // Missing MPHR/ORBIT_START and wrong-type ORBIT_START.
    {
      IasiL1Config cfg;
      cfg.mdrs = {MdrOptions{}};
      cfg.include_orbit_start = false;
      if (run_with(cfg).variables["orbit_index"].read_status != -1) {
        std::exit(10);
      }
    }
    {
      IasiL1Config cfg;
      cfg.mdrs = {MdrOptions{}};
      cfg.orbit_start_wrong_type = true;
      if (run_with(cfg).variables["orbit_index"].read_status != -1) {
        std::exit(11);
      }
    }

    // Missing and wrong-type GGeoSondLoc (latitude / longitude).
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.include_loc = false;
      cfg.mdrs = {m};
      IngestExerciseResult res = run_with(cfg);
      if (res.variables["latitude"].read_status != -1 ||
          res.variables["longitude"].read_status != -1) {
        std::exit(12);
      }
    }
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.loc_wrong_type = true;
      cfg.mdrs = {m};
      IngestExerciseResult res = run_with(cfg);
      if (res.variables["latitude"].read_status != -1 ||
          res.variables["longitude"].read_status != -1) {
        std::exit(13);
      }
    }

    // Angle errors: missing, short array, wrong type.
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.include_sun_angles = false;
      cfg.mdrs = {m};
      IngestExerciseResult res = run_with(cfg);
      if (res.variables["solar_zenith_angle"].read_status != -1 ||
          res.variables["solar_azimuth_angle"].read_status != -1) {
        std::exit(14);
      }
    }
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.sun_angles_short = true;
      cfg.mdrs = {m};
      if (run_with(cfg).variables["solar_azimuth_angle"].read_status != -1) {
        std::exit(15);
      }
    }
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.sun_angles_wrong_type = true;
      cfg.mdrs = {m};
      if (run_with(cfg).variables["solar_zenith_angle"].read_status != -1) {
        std::exit(16);
      }
    }

    // Spectra errors: missing/wrong-type IDefNsfirst1b, missing/short
    // GS1cSpect.
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.include_nsfirst = false;
      cfg.mdrs = {m};
      IngestExerciseResult res = run_with(cfg);
      if (res.variables["wavenumber_radiance"].read_status != -1 ||
          res.variables["wavenumber"].read_status != -1) {
        std::exit(17);
      }
    }
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.nsfirst_wrong_type = true;
      cfg.mdrs = {m};
      IngestExerciseResult res = run_with(cfg);
      if (res.variables["wavenumber_radiance"].read_status != -1 ||
          res.variables["wavenumber"].read_status != -1) {
        std::exit(18);
      }
    }
    {
      IasiL1Config cfg;
      MdrOptions m1;
      MdrOptions m2;
      m2.include_spect = false;
      cfg.mdrs = {m1, m2};
      if (run_with(cfg).variables["wavenumber_radiance"].read_status != -1) {
        std::exit(19);
      }
    }
    {
      IasiL1Config cfg;
      MdrOptions m1;
      MdrOptions m2;
      m2.spect_short = true;
      cfg.mdrs = {m1, m2};
      if (run_with(cfg).variables["wavenumber_radiance"].read_status != -1) {
        std::exit(20);
      }
    }

    // Wavenumber errors: missing/wrong-type IDefSpectDWn1b, IDefNslast1b,
    // bounds.
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.include_dwn = false;
      cfg.mdrs = {m};
      if (run_with(cfg).variables["wavenumber"].read_status != -1) {
        std::exit(21);
      }
    }
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.dwn_wrong_type = true;
      cfg.mdrs = {m};
      if (run_with(cfg).variables["wavenumber"].read_status != -1) {
        std::exit(22);
      }
    }
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.include_nslast = false;
      cfg.mdrs = {m};
      if (run_with(cfg).variables["wavenumber"].read_status != -1) {
        std::exit(23);
      }
    }
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.nslast_wrong_type = true;
      cfg.mdrs = {m};
      if (run_with(cfg).variables["wavenumber"].read_status != -1) {
        std::exit(24);
      }
    }
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.nsfirst = 105;
      m.nslast = 100;  // last < first
      cfg.mdrs = {m};
      if (run_with(cfg).variables["wavenumber"].read_status != -1) {
        std::exit(25);
      }
    }
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.nsfirst = 100;
      m.nslast = 110;  // 11 > num_pixels (4)
      cfg.mdrs = {m};
      if (run_with(cfg).variables["wavenumber"].read_status != -1) {
        std::exit(26);
      }
    }

#if !defined(MEMORY_SANITIZER)
    // Upstream read_datetime() allocates uninitialized mdr_times and loops
    // over it even when get_main_data() fails.
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.include_start_time = false;
      cfg.mdrs = {m};
      if (run_with(cfg).variables["datetime"].read_status != -1) {
        std::exit(27);
      }
    }
    {
      IasiL1Config cfg;
      MdrOptions m;
      m.start_time_wrong_type = true;
      cfg.mdrs = {m};
      if (run_with(cfg).variables["datetime"].read_status != -1) {
        std::exit(28);
      }
    }
#endif
  }
  std::exit(0);
}

TEST(HarpIngestIasiL1Test, VariableReadErrorPaths) {
  EXPECT_EXIT(RunVariableReadErrorPathsAndExit(), ::testing::ExitedWithCode(0),
              "");
}
#endif

}  // namespace
}  // namespace stcorp_harp
