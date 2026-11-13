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
// Unit tests for libharp top-level initialization, options, and import/export
// entry points (libharp/harp.c).
//
// Google3 build notes: HDF4 is disabled (#undef HAVE_HDF4); HARP_VERSION in
// libharp/config.h may differ from METADATA;
// patches/libharp/harp-netcdf.c.patch rejects files < 10 bytes with
// HARP_ERROR_FILE_READ.
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp.h.in

#include "third_party/stcorp_harp/libharp/harp.h"

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"

namespace stcorp_harp {
namespace {

using ::testing::ContainsRegex;
using ::testing::HasSubstr;

std::string* g_captured_print = nullptr;

int CapturePrint(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_captured_print != nullptr && n > 0) {
    g_captured_print->append(buf);
  }
  return n;
}

class ScopedEnvVar {
 public:
  explicit ScopedEnvVar(const char* name) : name_(name) {
    if (const char* prev = std::getenv(name_)) {
      saved_ = prev;
    }
  }
  ~ScopedEnvVar() {
    if (saved_.has_value()) {
      setenv(name_, saved_->c_str(), /*overwrite=*/1);
    } else {
      unsetenv(name_);
    }
  }
  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

 private:
  const char* name_;
  std::optional<std::string> saved_;
};

ScopedHarpProduct BuildSampleProduct(const char* source_product,
                                     const char* unit) {
  harp_product* raw_prod = nullptr;
  EXPECT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);
  if (prod == nullptr) {
    return prod;
  }
  if (source_product != nullptr) {
    EXPECT_EQ(HARP_SUCCESS,
              harp_product_set_source_product(prod.get(), source_product));
  }
  const harp_dimension_type dt_dims[] = {harp_dimension_time};
  const long dt_len[] = {2};
  harp_variable* raw_dt = nullptr;
  EXPECT_EQ(HARP_SUCCESS, harp_variable_new("datetime", harp_type_double, 1,
                                            dt_dims, dt_len, &raw_dt));
  ScopedHarpVariable dt_var(raw_dt);
  if (dt_var != nullptr) {
    dt_var->data.double_data[0] = 100.0;
    dt_var->data.double_data[1] = 200.0;
    EXPECT_EQ(HARP_SUCCESS, harp_variable_set_unit(dt_var.get(), unit));
    if (harp_product_add_variable(prod.get(), dt_var.get()) == HARP_SUCCESS) {
      dt_var.release();
    }
  }
  return prod;
}

TEST(HarpTest, VersionAndCodaDefinitionPaths) {
  ASSERT_NE(nullptr, libharp_version);
  EXPECT_THAT(libharp_version, ContainsRegex(R"RE([0-9]+[.][0-9]+)RE"));

  ScopedEnvVar restore_env("CODA_DEFINITION");
  auto clear_coda_path = absl::MakeCleanup([] {
    harp_set_coda_definition_path(nullptr);
    harp_set_error(HARP_SUCCESS, nullptr);
  });

  EXPECT_EQ(HARP_SUCCESS, harp_set_coda_definition_path("/nonexistent/coda"));
  EXPECT_EQ(HARP_SUCCESS, harp_set_coda_definition_path(nullptr));

  unsetenv("CODA_DEFINITION");
  const TempDir dir("harp_coda_dir");
  const TempFile marker("harp_coda_dir/marker.txt");
  ASSERT_TRUE(WriteFileText(marker.path(), "ok"));
  EXPECT_EQ(HARP_SUCCESS, harp_set_coda_definition_path_conditional(
                              "marker.txt", dir.c_str(), "defs"));
}

TEST(HarpTest, OptionsGetSetAndValidation) {
  const int orig_colloc = harp_get_option_create_collocation_datetime();
  const int orig_afgl = harp_get_option_enable_aux_afgl86();
  const int orig_usstd = harp_get_option_enable_aux_usstd76();
  const int orig_hdf5 = harp_get_option_hdf5_compression();
  const int orig_uncert = harp_get_option_propagate_uncertainty();
  const int orig_regrid = harp_get_option_regrid_out_of_bounds();
  auto restore = absl::MakeCleanup([&] {
    harp_set_option_create_collocation_datetime(orig_colloc);
    harp_set_option_enable_aux_afgl86(orig_afgl);
    harp_set_option_enable_aux_usstd76(orig_usstd);
    harp_set_option_hdf5_compression(orig_hdf5);
    harp_set_option_propagate_uncertainty(orig_uncert);
    harp_set_option_regrid_out_of_bounds(orig_regrid);
    harp_set_error(HARP_SUCCESS, nullptr);
  });

  EXPECT_EQ(HARP_SUCCESS, harp_set_option_create_collocation_datetime(1));
  EXPECT_EQ(1, harp_get_option_create_collocation_datetime());
  EXPECT_EQ(HARP_SUCCESS, harp_set_option_create_collocation_datetime(0));
  EXPECT_EQ(-1, harp_set_option_create_collocation_datetime(2));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  EXPECT_EQ(HARP_SUCCESS, harp_set_option_enable_aux_afgl86(1));
  EXPECT_EQ(1, harp_get_option_enable_aux_afgl86());
  EXPECT_EQ(HARP_SUCCESS, harp_set_option_enable_aux_afgl86(0));
  EXPECT_EQ(-1, harp_set_option_enable_aux_afgl86(-1));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  EXPECT_EQ(HARP_SUCCESS, harp_set_option_enable_aux_usstd76(1));
  EXPECT_EQ(1, harp_get_option_enable_aux_usstd76());
  EXPECT_EQ(HARP_SUCCESS, harp_set_option_enable_aux_usstd76(0));
  EXPECT_EQ(-1, harp_set_option_enable_aux_usstd76(2));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  EXPECT_EQ(HARP_SUCCESS, harp_set_option_hdf5_compression(6));
  EXPECT_EQ(6, harp_get_option_hdf5_compression());
  EXPECT_EQ(-1, harp_set_option_hdf5_compression(-1));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_set_option_hdf5_compression(10));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  EXPECT_EQ(HARP_SUCCESS, harp_set_option_propagate_uncertainty(1));
  EXPECT_EQ(1, harp_get_option_propagate_uncertainty());
  EXPECT_EQ(-1, harp_set_option_propagate_uncertainty(2));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  EXPECT_EQ(HARP_SUCCESS, harp_set_option_regrid_out_of_bounds(2));
  EXPECT_EQ(2, harp_get_option_regrid_out_of_bounds());
  EXPECT_EQ(-1, harp_set_option_regrid_out_of_bounds(3));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
}

TEST(HarpTest, InitDoneRefcountingAndAuxEnvVars) {
  ScopedEnvVar restore_afgl_env("HARP_AUX_AFGL86");
  ScopedEnvVar restore_usstd_env("HARP_AUX_USSTD76");
  const int orig_afgl = harp_get_option_enable_aux_afgl86();
  const int orig_usstd = harp_get_option_enable_aux_usstd76();
  auto restore_opts = absl::MakeCleanup([&] {
    harp_set_option_enable_aux_afgl86(orig_afgl);
    harp_set_option_enable_aux_usstd76(orig_usstd);
    harp_set_error(HARP_SUCCESS, nullptr);
  });

  setenv("HARP_AUX_AFGL86", "1", /*overwrite=*/1);
  setenv("HARP_AUX_USSTD76", "1", /*overwrite=*/1);
  ASSERT_EQ(HARP_SUCCESS, harp_set_option_enable_aux_afgl86(0));
  ASSERT_EQ(HARP_SUCCESS, harp_set_option_enable_aux_usstd76(0));

  ScopedHarpInit outer;
  ASSERT_EQ(HARP_SUCCESS, outer.status());
  EXPECT_EQ(1, harp_get_option_enable_aux_afgl86());
  EXPECT_EQ(1, harp_get_option_enable_aux_usstd76());

  // Nested init/done keeps library initialized until outer guard exits.
  ScopedHarpInit inner;
  ASSERT_EQ(HARP_SUCCESS, inner.status());
}

TEST(HarpTest, DimensionNamesAndFileConventionParsing) {
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  struct DimCase {
    harp_dimension_type dim;
    const char* name;
  };
  constexpr DimCase kDimCases[] = {
      {.dim = harp_dimension_independent, .name = "independent"},
      {.dim = harp_dimension_time, .name = "time"},
      {.dim = harp_dimension_latitude, .name = "latitude"},
      {.dim = harp_dimension_longitude, .name = "longitude"},
      {.dim = harp_dimension_spectral, .name = "spectral"},
      {.dim = harp_dimension_vertical, .name = "vertical"},
  };
  for (const auto& tc : kDimCases) {
    EXPECT_STREQ(tc.name, harp_get_dimension_type_name(tc.dim));
    harp_dimension_type parsed = harp_dimension_independent;
    ASSERT_EQ(HARP_SUCCESS, harp_parse_dimension_type(tc.name, &parsed));
    EXPECT_EQ(tc.dim, parsed);
  }

  harp_dimension_type bad_dim = harp_dimension_time;
  EXPECT_EQ(-1, harp_parse_dimension_type("altitude", &bad_dim));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  int major = 0;
  int minor = 0;
  ASSERT_EQ(HARP_SUCCESS,
            harp_parse_file_convention("CF-1.6 HARP-1.12", &major, &minor));
  EXPECT_EQ(1, major);
  EXPECT_EQ(12, minor);

  EXPECT_EQ(-1, harp_parse_file_convention("CF-1.6", &major, &minor));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  EXPECT_EQ(-1, harp_parse_file_convention("HARP-x.y", &major, &minor));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
}

TEST(HarpTest, ExportImportMetadataAndImportTestRoundTrip) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  ScopedHarpProduct prod = BuildSampleProduct(nullptr, "days since 2000-01-01");
  ASSERT_NE(nullptr, prod.get());

  // Unsupported export formats.
  const TempFile dummy("unused_export.dat");
  EXPECT_EQ(-1, harp_export(dummy.c_str(), "csv", prod.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  // HDF4 is disabled in google3 (#undef HAVE_HDF4).
  EXPECT_EQ(-1, harp_export(dummy.c_str(), "hdf4", prod.get()));
  EXPECT_EQ(HARP_ERROR_NO_HDF4_SUPPORT, harp_errno);

  for (const char* fmt : {"netcdf", "hdf5"}) {
    const std::string basename = std::string("roundtrip_") + fmt + ".dat";
    const TempFile out_file(basename);
    ASSERT_EQ(HARP_SUCCESS, harp_export(out_file.c_str(), fmt, prod.get()));

    // 1. Import with NULL operations populates empty source_product.
    harp_product* raw_imported = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_import(out_file.c_str(), nullptr, nullptr, &raw_imported));
    ScopedHarpProduct imported(raw_imported);
    EXPECT_EQ(2L, imported->dimension[harp_dimension_time]);
    EXPECT_STREQ(basename.c_str(), imported->source_product);

    // 2. Import with valid operations and failing operations.
    harp_product* raw_filtered = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_import(out_file.c_str(), "keep(datetime)",
                                        nullptr, &raw_filtered));
    ScopedHarpProduct filtered(raw_filtered);
    EXPECT_EQ(1, filtered->num_variables);

    harp_product* raw_bad_op = nullptr;
    EXPECT_EQ(-1, harp_import(out_file.c_str(), "keep(nonexistent_var)",
                              nullptr, &raw_bad_op));

    // 3. Import metadata.
    harp_product_metadata* raw_meta = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_import_product_metadata(out_file.c_str(),
                                                         nullptr, &raw_meta));
    ScopedHarpProductMetadata meta(raw_meta);
    EXPECT_STREQ(out_file.c_str(), meta->filename);
    EXPECT_EQ(2L, meta->dimension[harp_dimension_time]);
    EXPECT_DOUBLE_EQ(100.0, meta->datetime_start);
    EXPECT_DOUBLE_EQ(200.0, meta->datetime_stop);

    // 4. Import test succeeds and prints summary.
    std::string printed;
    g_captured_print = &printed;
    auto clear_print = absl::MakeCleanup([] { g_captured_print = nullptr; });
    EXPECT_EQ(0, harp_import_test(out_file.c_str(), CapturePrint));
    EXPECT_THAT(printed, HasSubstr("[OK]"));
    EXPECT_THAT(printed, HasSubstr("time=2"));
  }
}

TEST(HarpTest, ImportErrorPathsMissingDirectoryCorruptAndInvalidVerify) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_product* prod = nullptr;
  harp_product_metadata* meta = nullptr;
  std::string printed;
  g_captured_print = &printed;
  auto clear_print = absl::MakeCleanup([] { g_captured_print = nullptr; });

  // 1. Missing file.
  const TempFile missing("does_not_exist.nc");
  EXPECT_EQ(-1, harp_import(missing.c_str(), nullptr, nullptr, &prod));
  EXPECT_EQ(HARP_ERROR_FILE_NOT_FOUND, harp_errno);
  EXPECT_EQ(-1, harp_import_test(missing.c_str(), CapturePrint));
  EXPECT_EQ(HARP_ERROR_FILE_NOT_FOUND, harp_errno);
  EXPECT_EQ(-1, harp_import_product_metadata(missing.c_str(), nullptr, &meta));
  EXPECT_EQ(HARP_ERROR_FILE_NOT_FOUND, harp_errno);

  // 2. Directory instead of regular file, and ENOTDIR via file-as-parent path.
  const TempDir dir("not_a_regular_file");
  EXPECT_EQ(-1, harp_import(dir.c_str(), nullptr, nullptr, &prod));
  EXPECT_EQ(HARP_ERROR_FILE_OPEN, harp_errno);

  const TempFile parent_file("regular_parent_file.dat");
  ASSERT_TRUE(WriteFileText(parent_file.path(), "data"));
  const std::string enotdir_path = parent_file.string() + "/child.nc";
  EXPECT_EQ(-1, harp_import(enotdir_path.c_str(), nullptr, nullptr, &prod));
  EXPECT_EQ(HARP_ERROR_FILE_OPEN, harp_errno);

  // 3. Unreadable file triggers open() failure when not running as root.
  if (geteuid() != 0) {
    const TempFile unreadable("unreadable.nc");
    ASSERT_TRUE(WriteFileText(unreadable.path(), "0123456789"));
    ASSERT_EQ(0, chmod(unreadable.c_str(), 0));
    auto restore_perms =
        absl::MakeCleanup([&] { chmod(unreadable.c_str(), 0644); });
    EXPECT_EQ(-1, harp_import(unreadable.c_str(), nullptr, nullptr, &prod));
    EXPECT_EQ(HARP_ERROR_FILE_OPEN, harp_errno);
  }

  // 4. Truncated/corrupt netCDF magic (< 10 bytes per google3 patch and
  // corrupt >= 10-byte netCDF header).
  constexpr std::array<uint8_t, 4> kShortNetcdf = {'C', 'D', 'F', 1};
  const TempFile short_nc("short_magic.nc");
  ASSERT_TRUE(WriteFileBytes(short_nc.path(), kShortNetcdf));
  EXPECT_EQ(-1, harp_import(short_nc.c_str(), nullptr, nullptr, &prod));
  EXPECT_EQ(HARP_ERROR_FILE_READ, harp_errno);
  EXPECT_EQ(-1, harp_import_test(short_nc.c_str(), CapturePrint));
  EXPECT_EQ(-1, harp_import_product_metadata(short_nc.c_str(), nullptr, &meta));

  constexpr std::array<uint8_t, 16> kBadNetcdf = {
      'C', 'D', 'F', 1, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 1, 0, 0, 0, 9};
  const TempFile bad_nc("corrupt_magic.nc");
  ASSERT_TRUE(WriteFileBytes(bad_nc.path(), kBadNetcdf));
  EXPECT_EQ(-1, harp_import(bad_nc.c_str(), nullptr, nullptr, &prod));
  EXPECT_EQ(HARP_ERROR_NETCDF, harp_errno);
  EXPECT_EQ(-1, harp_import_test(bad_nc.c_str(), CapturePrint));
  EXPECT_EQ(-1, harp_import_product_metadata(bad_nc.c_str(), nullptr, &meta));

  // 5. HDF4 magic and unknown bytes fall through to ingestion (unsupported).
  // Note: Corrupt HDF5 magic is not tested here because upstream
  // harp_hdf5_add_error_message() in harp-hdf5.c leaks H5Eget_major/minor
  // strings under HeapLeakChecker/ASan.
  constexpr std::array<uint8_t, 16> kHdf4Magic = {
      0x0e, 0x03, 0x13, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  const TempFile bad_hdf4("fake_hdf4.hdf");
  ASSERT_TRUE(WriteFileBytes(bad_hdf4.path(), kHdf4Magic));
  EXPECT_EQ(-1, harp_import(bad_hdf4.c_str(), nullptr, nullptr, &prod));
  EXPECT_EQ(-1, harp_import_test(bad_hdf4.c_str(), CapturePrint));
  EXPECT_EQ(-1, harp_import_product_metadata(bad_hdf4.c_str(), nullptr, &meta));

  const TempFile unknown_file("unknown_bytes.dat");
  ASSERT_TRUE(WriteFileText(unknown_file.path(), "not_a_known_format"));
  EXPECT_EQ(-1, harp_import(unknown_file.c_str(), nullptr, nullptr, &prod));
  EXPECT_EQ(-1, harp_import_test(unknown_file.c_str(), CapturePrint));
  EXPECT_EQ(-1,
            harp_import_product_metadata(unknown_file.c_str(), nullptr, &meta));

  // 6. HARP NetCDF file whose variable has NaN valid_min passes
  // harp_export/harp_import_netcdf and fails harp_product_verify.
  ScopedHarpProduct bad_var_prod =
      BuildSampleProduct("src.nc", "days since 2000-01-01");
  ASSERT_NE(nullptr, bad_var_prod.get());
  {
    const harp_dimension_type dims[] = {harp_dimension_time};
    const long len[] = {2};
    harp_variable* raw_temp = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_variable_new("temperature", harp_type_double,
                                              1, dims, len, &raw_temp));
    ScopedHarpVariable temp_var(raw_temp);
    temp_var->data.double_data[0] = 280.0;
    temp_var->data.double_data[1] = 290.0;
    temp_var->valid_min.double_data = harp_nan();
    ASSERT_EQ(HARP_SUCCESS,
              harp_product_add_variable(bad_var_prod.get(), temp_var.get()));
    temp_var.release();
  }
  const TempFile invalid_harp_nc("invalid_verify.nc");
  ASSERT_EQ(HARP_SUCCESS,
            harp_export(invalid_harp_nc.c_str(), "netcdf", bad_var_prod.get()));

  EXPECT_EQ(-1, harp_import(invalid_harp_nc.c_str(), nullptr, nullptr, &prod));
  EXPECT_EQ(HARP_ERROR_INVALID_VARIABLE, harp_errno);

  printed.clear();
  EXPECT_EQ(1, harp_import_test(invalid_harp_nc.c_str(), CapturePrint));
  EXPECT_THAT(printed, HasSubstr("[FAIL]"));
}

}  // namespace
}  // namespace stcorp_harp
