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
// Unit tests for HARP ingestion options parsing and management
// (libharp/harp-ingestion-options.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingestion-options.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-ingestion.h

#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/google/util/oom_fault_inject.h"
#include "third_party/stcorp_harp/google/util/scoped_harp.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::HasSubstr;

TEST(HarpIngestionOptionsTest, NewSetGetReplaceRemoveAndCopy) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // Deleting nullptr is a safe no-op.
  harp_ingestion_options_delete(nullptr);

  harp_ingestion_options* raw_opts = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
  ScopedHarpIngestionOptions opts(raw_opts);
  ASSERT_NE(nullptr, opts.get());
  EXPECT_EQ(0, opts->num_options);

  // Querying or removing a non-existent option fails cleanly.
  EXPECT_EQ(0, harp_ingestion_options_has_option(opts.get(), "missing"));
  const char* val = nullptr;
  EXPECT_EQ(-1, harp_ingestion_options_get_option(opts.get(), "missing", &val));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
  EXPECT_EQ(-1, harp_ingestion_options_remove_option(opts.get(), "missing"));
  EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);

  // Add > BLOCK_SIZE (16) options to exercise realloc growth in
  // ingestion_options_add_option.
  for (int i = 0; i < 20; ++i) {
    const std::string name = "opt_" + std::to_string(i);
    const std::string value = "val_" + std::to_string(i);
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_set_option(
                                opts.get(), name.c_str(), value.c_str()));
  }
  EXPECT_EQ(20, opts->num_options);
  EXPECT_EQ(1, harp_ingestion_options_has_option(opts.get(), "opt_0"));
  EXPECT_EQ(1, harp_ingestion_options_has_option(opts.get(), "opt_19"));

  ASSERT_EQ(HARP_SUCCESS,
            harp_ingestion_options_get_option(opts.get(), "opt_17", &val));
  EXPECT_STREQ("val_17", val);

  // Replace an existing option in place.
  ASSERT_EQ(HARP_SUCCESS,
            harp_ingestion_options_set_option(opts.get(), "opt_5", "updated"));
  EXPECT_EQ(20, opts->num_options);
  ASSERT_EQ(HARP_SUCCESS,
            harp_ingestion_options_get_option(opts.get(), "opt_5", &val));
  EXPECT_STREQ("updated", val);

  // Deep-copy options and verify independence.
  harp_ingestion_options* raw_copy = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_copy(opts.get(), &raw_copy));
  ScopedHarpIngestionOptions copy(raw_copy);
  EXPECT_EQ(20, copy->num_options);
  ASSERT_EQ(HARP_SUCCESS,
            harp_ingestion_options_get_option(copy.get(), "opt_5", &val));
  EXPECT_STREQ("updated", val);

  // Remove first, middle, and last options from `opts` without affecting
  // `copy`.
  ASSERT_EQ(HARP_SUCCESS,
            harp_ingestion_options_remove_option(opts.get(), "opt_0"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_ingestion_options_remove_option(opts.get(), "opt_10"));
  ASSERT_EQ(HARP_SUCCESS,
            harp_ingestion_options_remove_option(opts.get(), "opt_19"));
  EXPECT_EQ(17, opts->num_options);
  EXPECT_EQ(0, harp_ingestion_options_has_option(opts.get(), "opt_0"));
  EXPECT_EQ(0, harp_ingestion_options_has_option(opts.get(), "opt_10"));
  EXPECT_EQ(0, harp_ingestion_options_has_option(opts.get(), "opt_19"));
  EXPECT_EQ(1, harp_ingestion_options_has_option(copy.get(), "opt_0"));
  EXPECT_EQ(1, harp_ingestion_options_has_option(copy.get(), "opt_19"));
}

TEST(HarpIngestionOptionsTest, SetOptionFromStringAndFromStringValid) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  // 1. Empty string yields 0 options.
  {
    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_from_string("", &raw_opts));
    ScopedHarpIngestionOptions opts(raw_opts);
    EXPECT_EQ(0, opts->num_options);
  }

  // 2. Single and semicolon-separated options with whitespace, underscores,
  // digits, and duplicate-key overwrite.
  {
    harp_ingestion_options* raw_opts = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_ingestion_options_from_string(
                  "  alpha_1 = first ; beta2=second  ; alpha_1 = final ;",
                  &raw_opts));
    ScopedHarpIngestionOptions opts(raw_opts);
    EXPECT_EQ(2, opts->num_options);

    const char* val = nullptr;
    ASSERT_EQ(HARP_SUCCESS,
              harp_ingestion_options_get_option(opts.get(), "alpha_1", &val));
    EXPECT_STREQ("final", val);
    ASSERT_EQ(HARP_SUCCESS,
              harp_ingestion_options_get_option(opts.get(), "beta2", &val));
    EXPECT_STREQ("second", val);

    // Append/update via harp_ingestion_options_set_option_from_string.
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_set_option_from_string(
                                opts.get(), " gamma_3 = third "));
    ASSERT_EQ(HARP_SUCCESS,
              harp_ingestion_options_get_option(opts.get(), "gamma_3", &val));
    EXPECT_STREQ("third", val);
  }
}

struct SyntaxErrorCase {
  std::string_view input;
  std::string_view expected_substr;
};

constexpr SyntaxErrorCase kSyntaxErrorCases[] = {
    {.input = "", .expected_substr = "expected option name"},
    {.input = "   ", .expected_substr = "expected option name"},
    {.input = "1bad=val", .expected_substr = "expected option name"},
    {.input = "_bad=val", .expected_substr = "expected option name"},
    {.input = "=val", .expected_substr = "expected option name"},
    {.input = "name", .expected_substr = "expected '='"},
    {.input = "name val", .expected_substr = "expected '='"},
    {.input = "name!", .expected_substr = "expected '='"},
    {.input = "name=", .expected_substr = "expected option value"},
    {.input = "name=   ", .expected_substr = "expected option value"},
    {.input = "name=val extra",
     .expected_substr = "trailing characters after option value"},
    {.input = "name=val;other=1",
     .expected_substr = "trailing characters after option value"},
};

TEST(HarpIngestionOptionsTest, SyntaxErrorsInSetOptionFromString) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  harp_ingestion_options* raw_opts = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_opts));
  ScopedHarpIngestionOptions opts(raw_opts);

  for (const auto& tc : kSyntaxErrorCases) {
    harp_set_error(HARP_SUCCESS, nullptr);
    const std::string s(tc.input);
    EXPECT_EQ(-1, harp_ingestion_options_set_option_from_string(opts.get(),
                                                                s.c_str()))
        << "input: '" << tc.input << "'";
    EXPECT_EQ(HARP_ERROR_INGESTION_OPTION_SYNTAX, harp_errno);
    EXPECT_THAT(harp_errno_to_string(harp_errno),
                HasSubstr(tc.expected_substr));
  }
  EXPECT_EQ(0, opts->num_options);
  // Note: harp_ingestion_options_from_string() is not invoked on syntax errors
  // because upstream ingestion_options_from_string() returns -1 without
  // deleting its newly allocated harp_ingestion_options struct (leaks under
  // HeapLeakChecker and ASan/LSan).
}

TEST(HarpIngestionOptionsTest, OutOfMemoryPaths) {
  harp_set_error(HARP_SUCCESS, nullptr);
  const absl::Cleanup reset_err = [] { harp_set_error(HARP_SUCCESS, nullptr); };

  // 1. OOM on strdup(str) (step 1) and harp_ingestion_options_new (step 2) in
  // harp_ingestion_options_from_string; n >= 3 is avoided because upstream
  // leaks options in ingestion_options_from_string on inner failure.
  for (int n = 1; n <= 2; ++n) {
    harp_set_error(HARP_SUCCESS, nullptr);
    ScopedOomCountdown guard(n);
    harp_ingestion_options* opts = nullptr;
    EXPECT_EQ(-1, harp_ingestion_options_from_string("k1=v1", &opts));
    EXPECT_TRUE(guard.triggered());
    EXPECT_EQ(HARP_ERROR_OUT_OF_MEMORY, harp_errno);
  }

  // 2. Build 17 options and sweep copy/set_option_from_string OOM (exercising
  // BLOCK_SIZE=16 realloc OOM cleanly).
  harp_ingestion_options* raw_src = nullptr;
  ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_new(&raw_src));
  ScopedHarpIngestionOptions src(raw_src);
  for (int i = 0; i < 17; ++i) {
    const std::string name = "k" + std::to_string(i);
    const std::string val = "v" + std::to_string(i);
    ASSERT_EQ(HARP_SUCCESS, harp_ingestion_options_set_option(
                                src.get(), name.c_str(), val.c_str()));
  }

  EXPECT_GT(
      SweepOomUntilSuccess([&]() {
        harp_ingestion_options* copy = nullptr;
        if (harp_ingestion_options_copy(src.get(), &copy) == HARP_SUCCESS) {
          ScopedSuppressOom suppress;
          harp_ingestion_options_delete(copy);
        }
      }),
      0);

  EXPECT_GT(
      SweepOomUntilSuccess([&]() {
        harp_ingestion_options* work = nullptr;
        {
          ScopedSuppressOom suppress;
          if (harp_ingestion_options_copy(src.get(), &work) != HARP_SUCCESS) {
            return;
          }
        }
        harp_ingestion_options_set_option(work, "extra_opt", "new_val");
        ScopedSuppressOom suppress;
        harp_ingestion_options_delete(work);
      }),
      0);

  EXPECT_GT(
      SweepOomUntilSuccess([&]() {
        harp_ingestion_options* work = nullptr;
        {
          ScopedSuppressOom suppress;
          if (harp_ingestion_options_copy(src.get(), &work) != HARP_SUCCESS) {
            return;
          }
        }
        harp_ingestion_options_set_option_from_string(work, "gamma = val");
        ScopedSuppressOom suppress;
        harp_ingestion_options_delete(work);
      }),
      0);
}

}  // namespace
}  // namespace stcorp_harp
