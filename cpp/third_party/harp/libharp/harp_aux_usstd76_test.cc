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
// Unit tests for US Standard Atmosphere 1976 climatology auxiliary profiles
// (libharp/harp-aux-usstd76.c).
//
// See also:
//   https://github.com/stcorp/harp/blob/master/libharp/harp-aux-usstd76.c
//   https://github.com/stcorp/harp/blob/master/libharp/harp-internal.h

#include <array>
#include <string>
#include <string_view>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/google/util/harp_internal_prelude.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

using ::testing::HasSubstr;

struct ProfileCase {
  std::string_view name;
  double expected_first;
  double expected_last;
};

constexpr auto kProfileCases = std::to_array<ProfileCase>({
    {.name = "altitude", .expected_first = 0.0, .expected_last = 120000.0},
    {.name = "pressure",
     .expected_first = 1013.00000,
     .expected_last = 0.00003},
    {.name = "temperature",
     .expected_first = 288.200,
     .expected_last = 360.000},
    {.name = "number_density",
     .expected_first = 2.545818E+19,
     .expected_last = 5.110260E+11},
    {.name = "CH4_number_density",
     .expected_first = 4.327934e+13,
     .expected_last = 1.533094e+04},
    {.name = "CO_number_density",
     .expected_first = 4.327934e+13,
     .expected_last = 1.533094e+04},
    {.name = "CO2_number_density",
     .expected_first = 8.408400E+15,
     .expected_last = 1.789900E+07},
    {.name = "H2O_number_density",
     .expected_first = 1.973426E+17,
     .expected_last = 1.022800E+05},
    {.name = "N2O_number_density",
     .expected_first = 4.327934e+13,
     .expected_last = 1.533094e+04},
    {.name = "NO2_number_density",
     .expected_first = 5.860400E+08,
     .expected_last = 7.722140E+01},
    {.name = "O2_number_density",
     .expected_first = 5.325320E+18,
     .expected_last = 3.707650E+10},
    {.name = "O3_number_density",
     .expected_first = 6.777680E+11,
     .expected_last = 2.557000E+02},
});

TEST(HarpAuxUsstd76Test, AllProfilesReturnExpectedTables) {
  for (const auto& tc : kProfileCases) {
    const std::string name(tc.name);
    SCOPED_TRACE(name);
    int num_vertical = -1;
    const double* values = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_aux_usstd76_get_profile(
                                name.c_str(), &num_vertical, &values));
    EXPECT_EQ(50, num_vertical);
    ASSERT_NE(nullptr, values);
    EXPECT_DOUBLE_EQ(tc.expected_first, values[0]);
    EXPECT_DOUBLE_EQ(tc.expected_last, values[49]);
  }
}

TEST(HarpAuxUsstd76Test, UnknownProfileNameReturnsInvalidArgument) {
  harp_set_error(HARP_SUCCESS, nullptr);
  auto reset_err =
      absl::MakeCleanup([] { harp_set_error(HARP_SUCCESS, nullptr); });

  constexpr std::string_view kBadNames[] = {"", "O3", "SO2_number_density"};
  for (const std::string_view bad_sv : kBadNames) {
    const std::string bad_name(bad_sv);
    int num_vertical = 99;
    const double* values = nullptr;
    EXPECT_EQ(-1, harp_aux_usstd76_get_profile(bad_name.c_str(), &num_vertical,
                                               &values));
    EXPECT_EQ(HARP_ERROR_INVALID_ARGUMENT, harp_errno);
    EXPECT_THAT(
        harp_errno_to_string(HARP_ERROR_INVALID_ARGUMENT),
        HasSubstr("US standard 76 climatology does not have a profile for"));
    EXPECT_EQ(99, num_vertical);
    EXPECT_EQ(nullptr, values);
  }
}

}  // namespace
}  // namespace stcorp_harp
