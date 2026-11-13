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
// Unit tests for libharp collocation results (libharp/harp-collocation.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-collocation.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp.h.in

#include <cstdint>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/oom_fault_inject.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/google/util/temp_file.h"
#include "third_party/stcorp_harp/libharp/harp.h"

// Not declared in any public or private header; signature mirrors
// harp-collocation.c.
extern "C" {
int harp_collocation_result_add_difference(
    harp_collocation_result* collocation_result,
    const char* difference_variable_name, const char* difference_unit);
}  // extern "C"

namespace stcorp_harp {
namespace {

using ::testing::HasSubstr;

TEST(HarpCollocationTest, NewDeleteAndAddDifference) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_collocation_result_delete(nullptr);

  // 1. Standard construction with 0 differences followed by add_difference.
  harp_collocation_result* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_new(&raw, 0, nullptr, nullptr));
  ScopedHarpCollocationResult res(raw);
  EXPECT_EQ(0, res->num_differences);
  EXPECT_EQ(0L, res->num_pairs);

  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_difference(
                              res.get(), "datetime_diff", "s"));
  ASSERT_EQ(1, res->num_differences);
  EXPECT_STREQ("datetime_diff", res->difference_variable_name[0]);
  EXPECT_STREQ("s", res->difference_unit[0]);

  // 2. Upstream quirk: harp_collocation_result_new(num_differences > 0)
  // allocates and populates the difference arrays but leaves num_differences=0.
  // Restore num_differences before destruction so
  // harp_collocation_result_delete frees the allocated strings without leaking.
  const char* names[2] = {"datetime_diff", "point_distance"};
  const char* units[2] = {"s", "km"};
  harp_collocation_result* raw_with_diffs = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_new(&raw_with_diffs, 2, names, units));
  ScopedHarpCollocationResult res_with_diffs(raw_with_diffs);
  EXPECT_EQ(0, res_with_diffs->num_differences);
  EXPECT_STREQ("datetime_diff", res_with_diffs->difference_variable_name[0]);
  EXPECT_STREQ("km", res_with_diffs->difference_unit[1]);
  res_with_diffs->num_differences = 2;

  // Also test NULL name/unit arrays with num_differences > 0.
  harp_collocation_result* raw_null_diffs = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_new(&raw_null_diffs, 1, nullptr, nullptr));
  ScopedHarpCollocationResult res_null_diffs(raw_null_diffs);
  EXPECT_EQ(nullptr, res_null_diffs->difference_variable_name[0]);
  EXPECT_EQ(nullptr, res_null_diffs->difference_unit[0]);
  res_null_diffs->num_differences = 1;
}

TEST(HarpCollocationTest, AddPairMismatchAndBlockGrowth) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_collocation_result* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_new(&raw, 0, nullptr, nullptr));
  ScopedHarpCollocationResult res(raw);
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_difference(res.get(), "d", "s"));

  // Mismatched num_differences fails with HARP_ERROR_INVALID_ARGUMENT.
  EXPECT_EQ(-1, harp_collocation_result_add_pair(res.get(), 0, "a.nc", 0,
                                                 "b.nc", 0, 0, nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);

  // Add > COLLOCATION_RESULT_BLOCK_SIZE (1024) pairs to trigger realloc growth.
  for (long i = 0; i < 1025; ++i) {
    const double diff = static_cast<double>(i) * 0.5;
    ASSERT_EQ(HARP_SUCCESS,
              harp_collocation_result_add_pair(res.get(), i, "a.nc", i, "b.nc",
                                               i + 10, 1, &diff));
  }
  EXPECT_EQ(1025L, res->num_pairs);
  EXPECT_EQ(1024L, res->pair[1024]->collocation_index);
  EXPECT_DOUBLE_EQ(512.0, res->pair[1024]->difference[0]);
}

TEST(HarpCollocationTest, RemovePairSortAndSwapDatasets) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_collocation_result* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_new(&raw, 0, nullptr, nullptr));
  ScopedHarpCollocationResult res(raw);

  // Add pairs out of order with ties on product/sample to cover all sort
  // tie-breakers, including duplicate pairs and duplicate collocation_index.
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_pair(res.get(), 30, "a2.nc", 5, "b1.nc",
                                             2, 0, nullptr));
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_pair(res.get(), 10, "a1.nc", 2, "b2.nc",
                                             4, 0, nullptr));
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_pair(res.get(), 20, "a1.nc", 2, "b1.nc",
                                             3, 0, nullptr));
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_pair(res.get(), 22, "a1.nc", 2, "b1.nc",
                                             2, 0, nullptr));
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_pair(res.get(), 25, "a1.nc", 2, "b1.nc",
                                             1, 0, nullptr));
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_pair(res.get(), 15, "a1.nc", 1, "b2.nc",
                                             4, 0, nullptr));
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_pair(res.get(), 18, "a2.nc", 1, "b2.nc",
                                             4, 0, nullptr));
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_pair(res.get(), 12, "a1.nc", 3, "b2.nc",
                                             4, 0, nullptr));
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_pair(res.get(), 25, "a1.nc", 2, "b1.nc",
                                             1, 0, nullptr));
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_pair(res.get(), 99, "a0.nc", 0, "b0.nc",
                                             0, 0, nullptr));

  // Invalid index errors.
  EXPECT_EQ(-1, harp_collocation_result_remove_pair_at_index(res.get(), -1));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_collocation_result_remove_pair_at_index(res.get(), 10));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);

  // Remove index 0 to exercise the left-shift loop, then remove last element.
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_remove_pair_at_index(res.get(), 9));
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_remove_pair_at_index(res.get(), 0));
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_pair(res.get(), 30, "a2.nc", 5, "b1.nc",
                                             2, 0, nullptr));
  EXPECT_EQ(9L, res->num_pairs);

  // Sort by A and then re-sort reverse order to hit all compare_by_a branches.
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_sort_by_a(res.get()));
  EXPECT_EQ(15L, res->pair[0]->collocation_index);
  EXPECT_EQ(25L, res->pair[1]->collocation_index);
  EXPECT_EQ(25L, res->pair[2]->collocation_index);
  EXPECT_EQ(22L, res->pair[3]->collocation_index);
  EXPECT_EQ(20L, res->pair[4]->collocation_index);
  EXPECT_EQ(10L, res->pair[5]->collocation_index);
  EXPECT_EQ(12L, res->pair[6]->collocation_index);
  EXPECT_EQ(18L, res->pair[7]->collocation_index);
  EXPECT_EQ(30L, res->pair[8]->collocation_index);

  // Sort by B (exercises product_index_a != and sample_index_a < / > ties).
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_sort_by_b(res.get()));
  EXPECT_EQ(25L, res->pair[0]->collocation_index);
  EXPECT_EQ(25L, res->pair[1]->collocation_index);
  EXPECT_EQ(22L, res->pair[2]->collocation_index);
  EXPECT_EQ(30L, res->pair[3]->collocation_index);
  EXPECT_EQ(20L, res->pair[4]->collocation_index);
  EXPECT_EQ(15L, res->pair[5]->collocation_index);
  EXPECT_EQ(10L, res->pair[6]->collocation_index);
  EXPECT_EQ(12L, res->pair[7]->collocation_index);
  EXPECT_EQ(18L, res->pair[8]->collocation_index);

  // Reverse pairs and re-sort by A and B to cover opposite comparator signs.
  for (long i = 0; i < res->num_pairs / 2; ++i) {
    harp_collocation_pair* tmp = res->pair[i];
    res->pair[i] = res->pair[res->num_pairs - 1 - i];
    res->pair[res->num_pairs - 1 - i] = tmp;
  }
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_sort_by_a(res.get()));
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_sort_by_b(res.get()));

  // Sort by collocation_index: 10, 12, 15, 18, 20, 22, 25, 25, 30.
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_sort_by_collocation_index(res.get()));
  EXPECT_EQ(10L, res->pair[0]->collocation_index);
  EXPECT_EQ(30L, res->pair[8]->collocation_index);

  // Swap datasets A and B.
  harp_dataset* orig_a = res->dataset_a;
  harp_dataset* orig_b = res->dataset_b;
  const long orig_prod_a = res->pair[0]->product_index_a;
  const long orig_samp_a = res->pair[0]->sample_index_a;
  const long orig_prod_b = res->pair[0]->product_index_b;
  const long orig_samp_b = res->pair[0]->sample_index_b;
  harp_collocation_result_swap_datasets(res.get());
  EXPECT_EQ(orig_b, res->dataset_a);
  EXPECT_EQ(orig_a, res->dataset_b);
  EXPECT_EQ(orig_prod_b, res->pair[0]->product_index_a);
  EXPECT_EQ(orig_samp_b, res->pair[0]->sample_index_a);
  EXPECT_EQ(orig_prod_a, res->pair[0]->product_index_b);
  EXPECT_EQ(orig_samp_a, res->pair[0]->sample_index_b);
}

TEST(HarpCollocationTest, FilterForSourceProductsAndCollocationIndices) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  auto make_sample = []() {
    harp_collocation_result* raw = nullptr;
    EXPECT_EQ(HARP_SUCCESS,
              harp_collocation_result_new(&raw, 0, nullptr, nullptr));
    ScopedHarpCollocationResult r(raw);
    EXPECT_EQ(HARP_SUCCESS,
              harp_collocation_result_add_pair(r.get(), 10, "a1.nc", 0, "b1.nc",
                                               0, 0, nullptr));
    EXPECT_EQ(HARP_SUCCESS,
              harp_collocation_result_add_pair(r.get(), 20, "a2.nc", 1, "b1.nc",
                                               1, 0, nullptr));
    EXPECT_EQ(HARP_SUCCESS,
              harp_collocation_result_add_pair(r.get(), 30, "a1.nc", 2, "b2.nc",
                                               2, 0, nullptr));
    EXPECT_EQ(HARP_SUCCESS,
              harp_collocation_result_add_pair(r.get(), 40, "a2.nc", 3, "b2.nc",
                                               3, 0, nullptr));
    return r;
  };

  // 1. Filter for source product A (unknown & known).
  {
    ScopedHarpCollocationResult r = make_sample();
    EXPECT_EQ(-1, harp_collocation_result_filter_for_source_product_a(
                      r.get(), "missing.nc"));
    harp_set_error(HARP_SUCCESS, nullptr);
    ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_filter_for_source_product_a(
                                r.get(), "a1.nc"));
    ASSERT_EQ(2L, r->num_pairs);
    EXPECT_EQ(10L, r->pair[0]->collocation_index);
    EXPECT_EQ(30L, r->pair[1]->collocation_index);
  }

  // 2. Filter for source product B (unknown & known).
  {
    ScopedHarpCollocationResult r = make_sample();
    EXPECT_EQ(-1, harp_collocation_result_filter_for_source_product_b(
                      r.get(), "missing.nc"));
    harp_set_error(HARP_SUCCESS, nullptr);
    ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_filter_for_source_product_b(
                                r.get(), "b2.nc"));
    ASSERT_EQ(2L, r->num_pairs);
    EXPECT_EQ(30L, r->pair[0]->collocation_index);
    EXPECT_EQ(40L, r->pair[1]->collocation_index);
  }

  // 3. Filter for collocation indices preserves requested order; missing index
  // returns error without leaking pairs.
  {
    ScopedHarpCollocationResult r = make_sample();
    int32_t bad_indices[] = {20, 25};
    EXPECT_EQ(-1, harp_collocation_result_filter_for_collocation_indices(
                      r.get(), 2, bad_indices));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
    harp_set_error(HARP_SUCCESS, nullptr);

    int32_t good_indices[] = {40, 10};
    ASSERT_EQ(HARP_SUCCESS,
              harp_collocation_result_filter_for_collocation_indices(
                  r.get(), 2, good_indices));
    ASSERT_EQ(2L, r->num_pairs);
    EXPECT_EQ(40L, r->pair[0]->collocation_index);
    EXPECT_EQ(10L, r->pair[1]->collocation_index);
  }
}

TEST(HarpCollocationTest, WriteReadRoundTripAndRangeFilters) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  TempFile csv("collocation_roundtrip.csv");

  // 1. Write/read invalid argument & open-failure checks.
  harp_collocation_result* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_new(&raw, 0, nullptr, nullptr));
  ScopedHarpCollocationResult res(raw);

  EXPECT_EQ(-1, harp_collocation_result_write(nullptr, res.get()));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_collocation_result_write(csv.c_str(), nullptr));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_collocation_result_write("/nonexistent_dir_xyz/out.csv",
                                              res.get()));
  EXPECT_EQ(HARP_ERROR_FILE_OPEN, harp_errno);

  harp_collocation_result* read_res = nullptr;
  EXPECT_EQ(-1, harp_collocation_result_read(nullptr, &read_res));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_collocation_result_read(csv.c_str(), &read_res));
  EXPECT_EQ(HARP_ERROR_FILE_OPEN, harp_errno);
  harp_set_error(HARP_SUCCESS, nullptr);

  // 2. Populate differences and pairs, write, and read back.
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_difference(
                              res.get(), "datetime_diff", "s"));
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_difference(
                              res.get(), "point_distance", "km"));
  const double d0[2] = {1.5, 10.25};
  const double d1[2] = {-2.5, 5.0};
  const double d2[2] = {3.0, 7.5};
  const double d3[2] = {4.0, 8.5};
  const double d4[2] = {6.0, 9.5};
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_pair(
                              res.get(), 1, "a1.nc", 0, "b1.nc", 2, 2, d0));
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_pair(
                              res.get(), 2, "a1.nc", 1, "b1.nc", 3, 2, d4));
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_pair(
                              res.get(), 2, "a2.nc", 1, "b2.nc", 3, 2, d1));
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_pair(
                              res.get(), 2, "a2.nc", 1, "b1.nc", 3, 2, d2));
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_pair(
                              res.get(), 3, "a1.nc", 4, "b2.nc", 5, 2, d3));

  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_write(csv.c_str(), res.get()));

  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_read(csv.c_str(), &read_res));
  ScopedHarpCollocationResult full(read_res);
  ASSERT_EQ(2, full->num_differences);
  EXPECT_STREQ("datetime_diff", full->difference_variable_name[0]);
  EXPECT_STREQ("s", full->difference_unit[0]);
  EXPECT_STREQ("point_distance", full->difference_variable_name[1]);
  EXPECT_STREQ("km", full->difference_unit[1]);
  ASSERT_EQ(5L, full->num_pairs);
  EXPECT_DOUBLE_EQ(1.5, full->pair[0]->difference[0]);
  EXPECT_DOUBLE_EQ(10.25, full->pair[0]->difference[1]);

  // 3. Read with min/max collocation index and source_product_a/b filters.
  // Pair 1 is skipped by min_collocation_index=2; pair 2 (a2, b2) is skipped
  // by source_product_b="b1.nc"; pair 3 is skipped by max_collocation_index=2.
  harp_collocation_result* raw_filtered = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_read_range(csv.c_str(), 2, 2, "a2.nc",
                                               "b1.nc", &raw_filtered));
  ScopedHarpCollocationResult filtered(raw_filtered);
  ASSERT_EQ(1L, filtered->num_pairs);
  EXPECT_EQ(2L, filtered->pair[0]->collocation_index);
  EXPECT_DOUBLE_EQ(3.0, filtered->pair[0]->difference[0]);
}

TEST(HarpCollocationTest, ReadRejectsMalformedCsvInputs) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  const std::string long_line(4096, 'x');
  const std::string valid_header =
      "collocation_index,source_product_a,index_a,source_product_b,index_b\n";

  struct BadCsvCase {
    std::string_view name;
    std::string content;
  };
  const BadCsvCase kBadCases[] = {
      {"empty", ""},
      {"long_header", long_line + "\n"},
      {"bad_col1", "wrong,source_product_a,index_a,source_product_b,index_b\n"},
      {"bad_col2",
       "collocation_index,wrong,index_a,source_product_b,index_b\n"},
      {"bad_col3",
       "collocation_index,source_product_a,wrong,source_product_b,index_b\n"},
      {"bad_col4",
       "collocation_index,source_product_a,index_a,wrong,index_b\n"},
      {"bad_col5",
       "collocation_index,source_product_a,index_a,source_product_b,wrong\n"},
      {"unterminated_header_col", "\"unterminated\n"},
      {"unterminated_col2", "collocation_index,\"unterminated\n"},
      {"unterminated_col3",
       "collocation_index,source_product_a,\"unterminated\n"},
      {"unterminated_col4",
       "collocation_index,source_product_a,index_a,\"unterminated\n"},
      {"unterminated_col5",
       "collocation_index,source_product_a,index_a,source_product_b,"
       "\"unterminated\n"},
      {"missing_col2", "collocation_index\n"},
      {"missing_col3", "collocation_index,source_product_a\n"},
      {"missing_col4", "collocation_index,source_product_a,index_a\n"},
      {"missing_col5",
       "collocation_index,source_product_a,index_a,source_product_b\n"},
      {"bad_diff_header",
       "collocation_index,source_product_a,index_a,source_product_b,index_b,"
       "diff [unclosed\n"},
      {"long_data_line", valid_header + long_line + "\n"},
      {"empty_data_line", valid_header + "\n"},
      {"bad_colloc_idx", valid_header + "not_int,a.nc,0,b.nc,0\n"},
      {"bad_src_a", valid_header + "0,\"unterminated,0,b.nc,0\n"},
      {"bad_idx_a", valid_header + "0,a.nc,bad,b.nc,0\n"},
      {"bad_src_b", valid_header + "0,a.nc,0,\"unterminated,0\n"},
      {"bad_idx_b", valid_header + "0,a.nc,0,b.nc,bad\n"},
      {"bad_diff_val",
       "collocation_index,source_product_a,index_a,source_product_b,index_b,"
       "d [s]\n0,a.nc,0,b.nc,0,not_a_double\n"},
  };

  for (const auto& tc : kBadCases) {
    SCOPED_TRACE(std::string(tc.name));
    TempFile file(std::string("bad_") + std::string(tc.name) + ".csv");
    ASSERT_TRUE(WriteFileText(file.path(), tc.content));
    harp_collocation_result* res = nullptr;
    EXPECT_EQ(-1, harp_collocation_result_read(file.c_str(), &res));
    EXPECT_EQ(nullptr, res);
    harp_set_error(HARP_SUCCESS, nullptr);
  }

  // CRLF line endings parse cleanly.
  TempFile crlf("crlf.csv");
  ASSERT_TRUE(WriteFileText(crlf.path(),
                            "collocation_index,source_product_a,index_a,"
                            "source_product_b,index_b\r\n1,a.nc,0,b.nc,2\r\n"));
  harp_collocation_result* raw_crlf = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_read(crlf.c_str(), &raw_crlf));
  ScopedHarpCollocationResult crlf_res(raw_crlf);
  EXPECT_EQ(1L, crlf_res->num_pairs);
}

TEST(HarpCollocationTest, ShallowCopyAndDelete) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());

  harp_collocation_result_shallow_delete(nullptr);

  harp_collocation_result* raw = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_new(&raw, 0, nullptr, nullptr));
  ScopedHarpCollocationResult orig(raw);
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_add_difference(orig.get(), "d", "s"));
  const double d = 4.5;
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_pair(
                              orig.get(), 7, "a.nc", 1, "b.nc", 2, 1, &d));

  harp_collocation_result* raw_copy = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_shallow_copy(orig.get(), &raw_copy));
  ScopedHarpCollocationResultShallow copy(raw_copy);
  EXPECT_EQ(orig->dataset_a, copy->dataset_a);
  EXPECT_EQ(orig->dataset_b, copy->dataset_b);
  ASSERT_EQ(1L, copy->num_pairs);
  EXPECT_NE(orig->pair[0], copy->pair[0]);
  EXPECT_EQ(7L, copy->pair[0]->collocation_index);
  EXPECT_DOUBLE_EQ(4.5, copy->pair[0]->difference[0]);
}

TEST(HarpCollocationTest, OutOfMemoryPaths) {
  ScopedHarpInit init;
  ASSERT_EQ(HARP_SUCCESS, init.status());
  const absl::Cleanup reset_err = [] { harp_set_error(HARP_SUCCESS, nullptr); };

  const char* names[2] = {"datetime_diff", "point_distance"};
  const char* units[2] = {"s", "km"};

  // 1. Early OOM steps 1..5 inside harp_collocation_result_new(2, ...) fail
  // before strdup allocations when num_differences is still 0 (steps >= 6
  // would leak earlier duplicated strings due to upstream num_differences=0).
  for (int n = 1; n <= 5; ++n) {
    harp_set_error(HARP_SUCCESS, nullptr);
    ScopedOomCountdown guard(n);
    harp_collocation_result* res = nullptr;
    EXPECT_EQ(-1, harp_collocation_result_new(&res, 2, names, units));
    EXPECT_TRUE(guard.triggered());
  }

  // 1b. Sweep OOM across harp_collocation_result_add_difference on a fresh
  // collocation result.
  EXPECT_GT(SweepOomUntilSuccess([&]() {
              harp_collocation_result* work = nullptr;
              {
                ScopedSuppressOom suppress;
                if (harp_collocation_result_new(&work, 0, nullptr, nullptr) !=
                    HARP_SUCCESS) {
                  return;
                }
              }
              harp_collocation_result_add_difference(work, "datetime_diff",
                                                     "s");
              ScopedSuppressOom suppress;
              harp_collocation_result_delete(work);
            }),
            0);

  // 2. OOM inside shallow_copy, add_pair (fresh result since shallow_copy
  // aliases datasets), filter_for_collocation_indices, and CSV read.
  harp_collocation_result* raw_base = nullptr;
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_new(&raw_base, 0, nullptr, nullptr));
  ScopedHarpCollocationResult base(raw_base);
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_difference(
                              base.get(), "datetime_diff", "s"));
  const double d = 1.5;
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_pair(
                              base.get(), 1, "a1.nc", 0, "b1.nc", 0, 1, &d));
  ASSERT_EQ(HARP_SUCCESS, harp_collocation_result_add_pair(
                              base.get(), 2, "a2.nc", 1, "b2.nc", 1, 1, &d));

  EXPECT_GT(SweepOomUntilSuccess([&]() {
              harp_collocation_result* copy = nullptr;
              if (harp_collocation_result_shallow_copy(base.get(), &copy) ==
                  HARP_SUCCESS) {
                ScopedSuppressOom suppress;
                harp_collocation_result_shallow_delete(copy);
              }
            }),
            0);

  EXPECT_GT(SweepOomUntilSuccess([&]() {
              harp_collocation_result* work = nullptr;
              {
                ScopedSuppressOom suppress;
                if (harp_collocation_result_new(&work, 0, nullptr, nullptr) !=
                        HARP_SUCCESS ||
                    harp_collocation_result_add_difference(
                        work, "datetime_diff", "s") != HARP_SUCCESS) {
                  harp_collocation_result_delete(work);
                  return;
                }
              }
              harp_collocation_result_add_pair(work, 3, "a3.nc", 2, "b3.nc", 2,
                                               1, &d);
              ScopedSuppressOom suppress;
              harp_collocation_result_delete(work);
            }),
            0);

  EXPECT_GT(SweepOomUntilSuccess([&]() {
              harp_collocation_result* work = nullptr;
              {
                ScopedSuppressOom suppress;
                if (harp_collocation_result_shallow_copy(base.get(), &work) !=
                    HARP_SUCCESS) {
                  return;
                }
              }
              int32_t keep[] = {2};
              harp_collocation_result_filter_for_collocation_indices(work, 1,
                                                                     keep);
              ScopedSuppressOom suppress;
              harp_collocation_result_shallow_delete(work);
            }),
            0);

  // 3. OOM during CSV read.
  TempFile csv("colloc_oom.csv");
  ASSERT_EQ(HARP_SUCCESS,
            harp_collocation_result_write(csv.c_str(), base.get()));
  EXPECT_GT(
      SweepOomUntilSuccess([&]() {
        harp_collocation_result* res = nullptr;
        if (harp_collocation_result_read(csv.c_str(), &res) == HARP_SUCCESS) {
          ScopedSuppressOom suppress;
          harp_collocation_result_delete(res);
        }
      }),
      0);
}

}  // namespace
}  // namespace stcorp_harp
