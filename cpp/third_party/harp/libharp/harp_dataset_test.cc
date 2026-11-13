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
// Unit tests for HARP dataset management, import, printing, and prefiltering
// (libharp/harp-dataset.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-dataset.c

#include <sys/stat.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::HasSubstr;

std::string* g_print_buffer = nullptr;

int CapturePrint(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_print_buffer != nullptr && n >= 0) {
    g_print_buffer->append(buf);
  }
  return n;
}

void WriteSampleNetcdfProduct(const std::filesystem::path& path,
                              const char* source_product, double dt_start,
                              double dt_stop) {
  harp_product* raw_prod = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_product_new(&raw_prod));
  ScopedHarpProduct prod(raw_prod);
  ASSERT_EQ(HARP_SUCCESS,
            harp_product_set_source_product(prod.get(), source_product));
  ASSERT_EQ(HARP_SUCCESS, harp_product_set_history(prod.get(), "test history"));

  const harp_dimension_type dim_types[] = {harp_dimension_time};
  const long dims[] = {2};
  harp_variable* raw_dt = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_variable_new("datetime", harp_type_double, 1,
                                            dim_types, dims, &raw_dt));
  ScopedHarpVariable dt_var(raw_dt);
  ASSERT_EQ(HARP_SUCCESS,
            harp_variable_set_unit(dt_var.get(), "days since 2000-01-01"));
  dt_var->data.double_data[0] = dt_start;
  dt_var->data.double_data[1] = dt_stop;
  ASSERT_EQ(HARP_SUCCESS, harp_product_add_variable(prod.get(), dt_var.get()));
  dt_var.release();

  ASSERT_EQ(HARP_SUCCESS, harp_export(path.c_str(), "netcdf", prod.get()));
}

harp_product_metadata* MakeMetadata(const char* filename,
                                    const char* source_product, double dt_start,
                                    double dt_stop,
                                    const char* history = nullptr) {
  harp_product_metadata* raw_meta = nullptr;
  if (harp_product_metadata_new(&raw_meta) != HARP_SUCCESS) {
    return nullptr;
  }
  raw_meta->filename = strdup(filename);
  raw_meta->source_product = strdup(source_product);
  raw_meta->datetime_start = dt_start;
  raw_meta->datetime_stop = dt_stop;
  if (history != nullptr) {
    raw_meta->history = strdup(history);
  }
  return raw_meta;
}

TEST(HarpDatasetTest, NewDeleteAndAddLookupPrint) {
  const absl::Cleanup reset_err = [] { harp_set_error(HARP_SUCCESS, nullptr); };
  harp_dataset_delete(nullptr);

  harp_dataset* raw_ds = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dataset_new(&raw_ds));
  ScopedHarpDataset ds(raw_ds);

  EXPECT_EQ(0L, ds->num_products);
  EXPECT_EQ(0, harp_dataset_has_product(ds.get(), "missing"));

  long idx = -1;
  harp_set_error(HARP_SUCCESS, nullptr);
  EXPECT_EQ(-1, harp_dataset_get_index_from_source_product(ds.get(), "missing",
                                                           &idx));
  EXPECT_EQ(HARP_ERROR_INVALID_NAME, harp_errno);

  // Add > BLOCK_SIZE (16) products out of sorted order, including one with
  // NULL metadata and one whose metadata is later replaced.
  ASSERT_EQ(HARP_SUCCESS,
            harp_dataset_add_product(ds.get(), "prod_null", nullptr));
  for (int i = 19; i >= 1; --i) {
    const std::string name = absl::StrCat("prod_", i < 10 ? "0" : "", i);
    ScopedHarpProductMetadata meta(
        MakeMetadata(absl::StrCat(name, ".nc").c_str(), name.c_str(), 1.0 * i,
                     1.0 * i + 0.5, "hist"));
    ASSERT_NE(nullptr, meta.get());
    ASSERT_EQ(HARP_SUCCESS,
              harp_dataset_add_product(ds.get(), name.c_str(), meta.get()));
    // Ownership transferred to ds; history was cleared to save memory.
    meta.release();
  }
  EXPECT_EQ(20L, ds->num_products);
  EXPECT_EQ(1, harp_dataset_has_product(ds.get(), "prod_05"));
  EXPECT_EQ(1, harp_dataset_has_product(ds.get(), "prod_null"));

  ASSERT_EQ(HARP_SUCCESS, harp_dataset_get_index_from_source_product(
                              ds.get(), "prod_05", &idx));
  EXPECT_EQ(nullptr, ds->metadata[idx]->history);

  // Verify sorted_index ordering ("prod_01" < ... < "prod_19" < "prod_null").
  EXPECT_STREQ("prod_01", ds->source_product[ds->sorted_index[0]]);
  EXPECT_STREQ("prod_null", ds->source_product[ds->sorted_index[19]]);

  // Replace existing metadata for "prod_05".
  {
    ScopedHarpProductMetadata replacement(
        MakeMetadata("replaced.nc", "prod_05", 10.0, 11.0));
    ASSERT_EQ(HARP_SUCCESS,
              harp_dataset_add_product(ds.get(), "prod_05", replacement.get()));
    replacement.release();
    EXPECT_EQ(20L, ds->num_products);
    EXPECT_STREQ("replaced.nc", ds->metadata[idx]->filename);
  }

  // Mismatched source_product in metadata must fail without taking ownership.
  {
    ScopedHarpProductMetadata mismatched(
        MakeMetadata("bad.nc", "other_source", 0.0, 1.0));
    EXPECT_EQ(-1,
              harp_dataset_add_product(ds.get(), "prod_05", mismatched.get()));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }

  // Print dataset (exercises both metadata != NULL and metadata == NULL paths).
  std::string output;
  g_print_buffer = &output;
  auto reset_buf = absl::MakeCleanup([] { g_print_buffer = nullptr; });
  harp_dataset_print(ds.get(), CapturePrint);
  EXPECT_THAT(output,
              HasSubstr("filename,datetime_start,datetime_stop,time,latitude,"
                        "longitude,vertical,spectral,source_product\n"));
  EXPECT_THAT(output, HasSubstr("replaced.nc,"));
  EXPECT_THAT(output, HasSubstr(",,,,,,,,prod_null\n"));
}

TEST(HarpDatasetTest, ImportMissingAndDotfiles) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_dataset* raw_ds = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_dataset_new(&raw_ds));
  ScopedHarpDataset ds(raw_ds);

  // Dotfiles are silently ignored.
  EXPECT_EQ(HARP_SUCCESS, harp_dataset_import(
                              ds.get(), "/nonexistent/.hidden_file", nullptr));
  EXPECT_EQ(0L, ds->num_products);

  // Missing path returns HARP_ERROR_FILE_NOT_FOUND.
  const std::filesystem::path missing =
      std::filesystem::path(::testing::TempDir()) / "does_not_exist_ds";
  EXPECT_EQ(-1, harp_dataset_import(ds.get(), missing.c_str(), nullptr));
  EXPECT_EQ(HARP_ERROR_FILE_NOT_FOUND, harp_errno);

  // Non-ENOENT stat() error (ENOTDIR) returns HARP_ERROR_FILE_OPEN.
  TempFile regular("not_a_dir_ds");
  ASSERT_TRUE(WriteFileText(regular.path(), "x"));
  const std::filesystem::path enotdir = regular.path() / "child";
  EXPECT_EQ(-1, harp_dataset_import(ds.get(), enotdir.c_str(), nullptr));
  EXPECT_EQ(HARP_ERROR_FILE_OPEN, harp_errno);
}

TEST(HarpDatasetTest, ImportDirectoryAndPathFiles) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  TempDir root("dataset_import_dir");
  const std::filesystem::path sub = root.child("subdir");
  std::filesystem::create_directories(sub);

  // Create dotfile (must be ignored), two NetCDF files, and a .pth file.
  ASSERT_TRUE(WriteFileText(root.child(".ignored.nc"), "junk"));
  const std::filesystem::path nc1 = root.child("p1.nc");
  const std::filesystem::path nc2 = sub / "p2.nc";
  WriteSampleNetcdfProduct(nc1, "src_1", 1.0, 2.0);
  WriteSampleNetcdfProduct(nc2, "src_2", 3.0, 4.0);

  // Import directory recursively.
  {
    harp_dataset* raw_ds = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_new(&raw_ds));
    ScopedHarpDataset ds(raw_ds);
    ASSERT_EQ(HARP_SUCCESS,
              harp_dataset_import(ds.get(), root.c_str(), nullptr));
    EXPECT_EQ(2L, ds->num_products);
    EXPECT_EQ(1, harp_dataset_has_product(ds.get(), "src_1"));
    EXPECT_EQ(1, harp_dataset_has_product(ds.get(), "src_2"));
  }

  // Import via a .pth file (with CRLF and LF line endings) and single file.
  {
    TempFile pth("dataset_list.pth");
    ASSERT_TRUE(WriteFileText(
        pth.path(), absl::StrCat(nc1.native(), "\r\n", nc2.native(), "\n")));
    harp_dataset* raw_ds = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_new(&raw_ds));
    ScopedHarpDataset ds(raw_ds);
    ASSERT_EQ(HARP_SUCCESS,
              harp_dataset_import(ds.get(), pth.c_str(), nullptr));
    EXPECT_EQ(2L, ds->num_products);
  }

  // Import single NetCDF file directly, and test corrupt file failure.
  {
    harp_dataset* raw_ds = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_new(&raw_ds));
    ScopedHarpDataset ds(raw_ds);
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_import(ds.get(), nc1.c_str(), ""));
    EXPECT_EQ(1L, ds->num_products);

    TempFile bad("bad_dataset_entry.bin");
    ASSERT_TRUE(WriteFileText(bad.path(), "not a valid harp file"));
    EXPECT_EQ(-1, harp_dataset_import(ds.get(), bad.c_str(), nullptr));
  }

  // Directory containing an unreadable/corrupt file propagates error from
  // add_directory().
  {
    TempDir bad_dir("dataset_bad_dir");
    ASSERT_TRUE(
        WriteFileText(bad_dir.child("corrupt.bin"), "not a valid file"));
    harp_dataset* raw_ds = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_new(&raw_ds));
    ScopedHarpDataset ds(raw_ds);
    EXPECT_EQ(-1, harp_dataset_import(ds.get(), bad_dir.c_str(), nullptr));
  }

  // Unreadable .pth file and unreadable directory hit fopen()/opendir() errors
  // (skipped if running as root where mode 0000 remains readable).
  if (geteuid() != 0) {
    TempFile unreadable_pth("unreadable.pth");
    ASSERT_TRUE(WriteFileText(unreadable_pth.path(), "a.nc\n"));
    ASSERT_EQ(0, chmod(unreadable_pth.c_str(), 0000));
    auto restore_pth =
        absl::MakeCleanup([&] { chmod(unreadable_pth.c_str(), 0644); });
    harp_dataset* raw_ds = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_new(&raw_ds));
    ScopedHarpDataset ds(raw_ds);
    EXPECT_EQ(-1,
              harp_dataset_import(ds.get(), unreadable_pth.c_str(), nullptr));
    EXPECT_EQ(HARP_ERROR_FILE_OPEN, harp_errno);

    TempDir unreadable_dir("unreadable_dir");
    ASSERT_EQ(0, chmod(unreadable_dir.c_str(), 0000));
    auto restore_dir =
        absl::MakeCleanup([&] { chmod(unreadable_dir.c_str(), 0755); });
    EXPECT_EQ(-1,
              harp_dataset_import(ds.get(), unreadable_dir.c_str(), nullptr));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }
}

TEST(HarpDatasetTest, ImportDatasetCsvPathFileSuccessAndErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  constexpr std::string_view kHeader =
      "filename,datetime_start,datetime_stop,time,latitude,longitude,vertical,"
      "spectral,source_product\n";

  // 1. Valid dataset CSV (.pth) with explicit datetimes, empty datetimes,
  //    quoted filename with comma, and CRLF.
  {
    TempFile csv_pth("dataset_valid.pth");
    ASSERT_TRUE(WriteFileText(
        csv_pth.path(),
        absl::StrCat(
            kHeader,
            "\"a,b.nc\",20200101T000000,20200102T120000,2,3,4,5,6,src_a\r\n",
            "c.nc,,,1,0,0,0,0,src_b\n")));
    harp_dataset* raw_ds = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_new(&raw_ds));
    ScopedHarpDataset ds(raw_ds);
    ASSERT_EQ(HARP_SUCCESS,
              harp_dataset_import(ds.get(), csv_pth.c_str(), nullptr));
    EXPECT_EQ(2L, ds->num_products);

    long idx_a = -1;
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_get_index_from_source_product(
                                ds.get(), "src_a", &idx_a));
    EXPECT_STREQ("a,b.nc", ds->metadata[idx_a]->filename);
    EXPECT_EQ(2L, ds->metadata[idx_a]->dimension[harp_dimension_time]);
    EXPECT_EQ(3L, ds->metadata[idx_a]->dimension[harp_dimension_latitude]);
    EXPECT_EQ(4L, ds->metadata[idx_a]->dimension[harp_dimension_longitude]);
    EXPECT_EQ(5L, ds->metadata[idx_a]->dimension[harp_dimension_vertical]);
    EXPECT_EQ(6L, ds->metadata[idx_a]->dimension[harp_dimension_spectral]);

    long idx_b = -1;
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_get_index_from_source_product(
                                ds.get(), "src_b", &idx_b));
    EXPECT_EQ(1, harp_ismininf(ds->metadata[idx_b]->datetime_start));
    EXPECT_EQ(1, harp_isplusinf(ds->metadata[idx_b]->datetime_stop));
  }

  // 2. Dataset CSV error cases (all close the FILE* cleanly before returning).
  struct BadCsvCase {
    std::string body;
    int expected_errno;
  };
  const BadCsvCase kBadCases[] = {
      {.body = std::string(4100, 'x') + "\n",
       .expected_errno = HARP_ERROR_INVALID_ARGUMENT},
      {.body = "\n", .expected_errno = HARP_ERROR_INVALID_ARGUMENT},
      {.body = "\"unterminated,20200101T000000,20200102T000000,1,0,0,0,0,s\n",
       .expected_errno = HARP_ERROR_INVALID_FORMAT},
      {.body = "a.nc,\"bad,20200102T000000,1,0,0,0,0,s\n",
       .expected_errno = HARP_ERROR_INVALID_FORMAT},
      {.body = "a.nc,notadate,20200102T000000,1,0,0,0,0,s\n",
       .expected_errno = HARP_ERROR_INVALID_FORMAT},
      {.body = "a.nc,20200101T000000,\"bad,1,0,0,0,0,s\n",
       .expected_errno = HARP_ERROR_INVALID_FORMAT},
      {.body = "a.nc,20200101T000000,notadate,1,0,0,0,0,s\n",
       .expected_errno = HARP_ERROR_INVALID_FORMAT},
      {.body = "a.nc,,,bad,0,0,0,0,s\n",
       .expected_errno = HARP_ERROR_INVALID_FORMAT},
      {.body = "a.nc,,,1,bad,0,0,0,s\n",
       .expected_errno = HARP_ERROR_INVALID_FORMAT},
      {.body = "a.nc,,,1,0,bad,0,0,s\n",
       .expected_errno = HARP_ERROR_INVALID_FORMAT},
      {.body = "a.nc,,,1,0,0,bad,0,s\n",
       .expected_errno = HARP_ERROR_INVALID_FORMAT},
      {.body = "a.nc,,,1,0,0,0,bad,s\n",
       .expected_errno = HARP_ERROR_INVALID_FORMAT},
      {.body = "a.nc,,,1,0,0,0,0,\"unterminated\n",
       .expected_errno = HARP_ERROR_INVALID_FORMAT},
  };

  for (const auto& tc : kBadCases) {
    harp_set_error(HARP_SUCCESS, nullptr);
    TempFile csv_pth("dataset_bad_csv.pth");
    ASSERT_TRUE(WriteFileText(csv_pth.path(), absl::StrCat(kHeader, tc.body)));
    harp_dataset* raw_ds = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_new(&raw_ds));
    ScopedHarpDataset ds(raw_ds);
    EXPECT_EQ(-1, harp_dataset_import(ds.get(), csv_pth.c_str(), nullptr));
    EXPECT_EQ(tc.expected_errno, harp_errno);
  }

  // 3. Regular .pth file referencing a missing file propagates error.
  {
    TempFile pth("dataset_missing_ref.pth");
    const std::filesystem::path missing =
        std::filesystem::path(::testing::TempDir()) / "missing_from_pth.nc";
    ASSERT_TRUE(
        WriteFileText(pth.path(), absl::StrCat(missing.native(), "\n")));
    harp_dataset* raw_ds = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_new(&raw_ds));
    ScopedHarpDataset ds(raw_ds);
    EXPECT_EQ(-1, harp_dataset_import(ds.get(), pth.c_str(), nullptr));
    EXPECT_EQ(HARP_ERROR_FILE_NOT_FOUND, harp_errno);
  }

#if !defined(ABSL_HAVE_ADDRESS_SANITIZER) && !defined(__SANITIZE_ADDRESS__)
  // Upstream add_path_file() returns -1 on an empty line without calling
  // fclose(stream), which LSan flags as a leaked FILE* under asan-chlor.
  {
    TempFile pth("dataset_empty_line.pth");
    ASSERT_TRUE(WriteFileText(pth.path(), "\n"));
    harp_dataset* raw_ds = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_new(&raw_ds));
    ScopedHarpDataset ds(raw_ds);
    EXPECT_EQ(-1, harp_dataset_import(ds.get(), pth.c_str(), nullptr));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  }
#endif
}

TEST(HarpDatasetTest, PrefilterComparisonOperatorsAndEdgeCases) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  // Empty dataset and NULL operations are no-ops.
  {
    harp_dataset* raw_ds = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_new(&raw_ds));
    ScopedHarpDataset ds(raw_ds);
    EXPECT_EQ(HARP_SUCCESS, harp_dataset_prefilter(ds.get(), "datetime > 0"));
    EXPECT_EQ(HARP_SUCCESS, harp_dataset_prefilter(ds.get(), nullptr));
  }

  auto build_dataset = []() -> ScopedHarpDataset {
    harp_dataset* raw_ds = nullptr;
    if (harp_dataset_new(&raw_ds) != HARP_SUCCESS) {
      return nullptr;
    }
    ScopedHarpDataset ds(raw_ds);
    // p1: [1, 2], p2: [5, 5], p3: [8, 10], p4: NULL metadata (never dropped)
    struct Entry {
      const char* name;
      double start;
      double stop;
      bool has_meta;
    };
    constexpr Entry kEntries[] = {
        {.name = "p1", .start = 1.0, .stop = 2.0, .has_meta = true},
        {.name = "p2", .start = 5.0, .stop = 5.0, .has_meta = true},
        {.name = "p3", .start = 8.0, .stop = 10.0, .has_meta = true},
        {.name = "p4", .start = 0.0, .stop = 0.0, .has_meta = false},
    };
    for (const auto& e : kEntries) {
      if (e.has_meta) {
        ScopedHarpProductMetadata meta(
            MakeMetadata(e.name, e.name, e.start, e.stop));
        if (harp_dataset_add_product(ds.get(), e.name, meta.get()) != 0) {
          return nullptr;
        }
        meta.release();
      } else {
        if (harp_dataset_add_product(ds.get(), e.name, nullptr) != 0) {
          return nullptr;
        }
      }
    }
    return ds;
  };

  struct ComparisonCase {
    std::string_view ops;
    bool expect_p1;
    bool expect_p2;
    bool expect_p3;
  };
  // Note: upstream prefilter_comparison() in harp-dataset.c passes
  // operation->unit_converter unconditionally to
  // harp_unit_converter_convert_double() without checking != NULL. Using a unit
  // different from "days since 2000-01-01" (here "hours since 2000-01-01",
  // where 5 days == 120 hours) ensures unit_converter is non-NULL.
  constexpr ComparisonCase kCases[] = {
      {.ops = "datetime == 120 [hours since 2000-01-01]",
       .expect_p1 = false,
       .expect_p2 = true,
       .expect_p3 = false},
      {.ops = "datetime != 120 [hours since 2000-01-01]",
       .expect_p1 = true,
       .expect_p2 = false,
       .expect_p3 = true},
      {.ops = "datetime_start < 120 [hours since 2000-01-01]",
       .expect_p1 = true,
       .expect_p2 = false,
       .expect_p3 = false},
      {.ops = "datetime_start <= 120 [hours since 2000-01-01]",
       .expect_p1 = true,
       .expect_p2 = true,
       .expect_p3 = false},
      {.ops = "datetime_stop > 120 [hours since 2000-01-01]",
       .expect_p1 = false,
       .expect_p2 = false,
       .expect_p3 = true},
      {.ops = "datetime_stop >= 120 [hours since 2000-01-01]",
       .expect_p1 = false,
       .expect_p2 = true,
       .expect_p3 = true},
      // Non-datetime comparison, set(), and unsupported keep() stop cleanly
      // without dropping any products (covers harp_dataset_filter no-op path).
      {.ops = "latitude > 0; set(\"afgl86\", \"true\"); keep(datetime)",
       .expect_p1 = true,
       .expect_p2 = true,
       .expect_p3 = true},
  };

  for (const auto& tc : kCases) {
    ScopedHarpDataset ds = build_dataset();
    ASSERT_NE(nullptr, ds.get());
    std::string ops(tc.ops);
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_prefilter(ds.get(), ops.c_str()))
        << tc.ops;
    EXPECT_EQ(tc.expect_p1 ? 1 : 0, harp_dataset_has_product(ds.get(), "p1"))
        << tc.ops;
    EXPECT_EQ(tc.expect_p2 ? 1 : 0, harp_dataset_has_product(ds.get(), "p2"))
        << tc.ops;
    EXPECT_EQ(tc.expect_p3 ? 1 : 0, harp_dataset_has_product(ds.get(), "p3"))
        << tc.ops;
    EXPECT_EQ(1, harp_dataset_has_product(ds.get(), "p4")) << tc.ops;
  }

  // Invalid operation syntax and incompatible unit for datetime.
  {
    ScopedHarpDataset ds = build_dataset();
    EXPECT_EQ(-1, harp_dataset_prefilter(ds.get(), "invalid_op("));
    EXPECT_EQ(HARP_ERROR_OPERATION_SYNTAX, harp_errno);

    EXPECT_EQ(-1, harp_dataset_prefilter(ds.get(), "datetime > 1 [m]"));
    EXPECT_EQ(HARP_ERROR_UNIT_CONVERSION, harp_errno);
  }
}

TEST(HarpDatasetTest, PrefilterCollocationSuccessAndErrors) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  auto build_dataset = []() -> ScopedHarpDataset {
    harp_dataset* raw_ds = nullptr;
    if (harp_dataset_new(&raw_ds) != HARP_SUCCESS) {
      return nullptr;
    }
    ScopedHarpDataset ds(raw_ds);
    for (const char* name : {"a1", "a2", "a3", "b1", "b2", "b3"}) {
      if (harp_dataset_add_product(ds.get(), name, nullptr) != HARP_SUCCESS) {
        return nullptr;
      }
    }
    return ds;
  };

  TempFile coll_csv("dataset_collocation.csv");
  ASSERT_TRUE(WriteFileText(
      coll_csv.path(),
      "collocation_index,source_product_a,index_a,source_product_b,index_b\n"
      "0,a1,0,b1,0\n"
      "2,a2,1,b2,1\r\n"
      "5,a3,2,b3,2\n"));

  // collocate_left with min=1, max=3 matches only collocation_index 2 -> a2.
  {
    ScopedHarpDataset ds = build_dataset();
    const std::string ops =
        absl::StrCat("collocate_left(\"", coll_csv.string(), "\", 1, 3)");
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_prefilter(ds.get(), ops.c_str()));
    EXPECT_EQ(1L, ds->num_products);
    EXPECT_EQ(1, harp_dataset_has_product(ds.get(), "a2"));
  }

  // collocate_right without index bounds matches b1, b2, b3.
  {
    ScopedHarpDataset ds = build_dataset();
    const std::string ops =
        absl::StrCat("collocate_right(\"", coll_csv.string(), "\")");
    ASSERT_EQ(HARP_SUCCESS, harp_dataset_prefilter(ds.get(), ops.c_str()));
    EXPECT_EQ(3L, ds->num_products);
    EXPECT_EQ(1, harp_dataset_has_product(ds.get(), "b1"));
    EXPECT_EQ(1, harp_dataset_has_product(ds.get(), "b2"));
    EXPECT_EQ(1, harp_dataset_has_product(ds.get(), "b3"));
  }

  // Error cases for collocation file prefiltering.
  {
    ScopedHarpDataset ds = build_dataset();
    const std::string missing_ops = "collocate_left(\"/no/such/coll.csv\")";
    EXPECT_EQ(-1, harp_dataset_prefilter(ds.get(), missing_ops.c_str()));
    EXPECT_EQ(HARP_ERROR_FILE_OPEN, harp_errno);
  }

  struct BadCollCase {
    std::string contents;
    int expected_errno;
  };
  const BadCollCase kBadCollCases[] = {
      {.contents = "", .expected_errno = HARP_ERROR_INVALID_ARGUMENT},
      {.contents = "header\n\n", .expected_errno = HARP_ERROR_INVALID_ARGUMENT},
      {.contents = "header\n" + std::string(4100, 'x') + "\n",
       .expected_errno = HARP_ERROR_INVALID_ARGUMENT},
      {.contents = "header\nbad_idx,a1,0,b1,0\n",
       .expected_errno = HARP_ERROR_INVALID_FORMAT},
      {.contents = "header\n0,\"unterminated,0,b1,0\n",
       .expected_errno = HARP_ERROR_INVALID_FORMAT},
      {.contents = "header\n0,a1,bad_ia,b1,0\n",
       .expected_errno = HARP_ERROR_INVALID_FORMAT},
      {.contents = "header\n0,a1,0,\"unterminated,0\n",
       .expected_errno = HARP_ERROR_INVALID_FORMAT},
  };
  for (const auto& tc : kBadCollCases) {
    harp_set_error(HARP_SUCCESS, nullptr);
    TempFile bad_coll("bad_coll.csv");
    ASSERT_TRUE(WriteFileText(bad_coll.path(), tc.contents));
    ScopedHarpDataset ds = build_dataset();
    const std::string ops =
        absl::StrCat("collocate_right(\"", bad_coll.string(), "\")");
    EXPECT_EQ(-1, harp_dataset_prefilter(ds.get(), ops.c_str()));
    EXPECT_EQ(tc.expected_errno, harp_errno);
  }
}

}  // namespace
}  // namespace stcorp_harp
