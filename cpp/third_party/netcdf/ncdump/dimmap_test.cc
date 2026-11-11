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

// Unit tests for //third_party/netcdf/ncdump/dimmap.c.
//
// Runs each dimmap lifecycle inside an isolated child process because
// dimmap_init() stores allocations in static globals with no free API.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncdump/dimmap.c

#include <array>
#include <cstdio>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncdump_globals_stub.h"
// clang-format off
#include "third_party/netcdf/include/netcdf.h"
// clang-format on

namespace netcdf {
namespace {

constexpr std::array<std::string_view, 1> kChildArgv = {"dimmap_test"};

TEST(DimmapTest, InitZeroDimensionsSucceeds) {
  const CliResult res = RunCliInForkedChild(
      [](int, char**) -> int {
        ncdump_stub_reset_globals();
        if (dimmap_init(0) != NC_NOERR) {
          std::fprintf(stderr, "dimmap_init(0) failed\n");
          return 1;
        }
        return 0;
      },
      kChildArgv);
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
}

TEST(DimmapTest, InitDefaultsAndStoreRoundTripBidirectionalMapping) {
  const CliResult res = RunCliInForkedChild(
      [](int, char**) -> int {
        ncdump_stub_reset_globals();
        constexpr int kNumDims = 4;
        if (dimmap_init(kNumDims) != NC_NOERR) {
          std::fprintf(stderr, "dimmap_init(4) failed\n");
          return 1;
        }

        for (int i = 0; i < kNumDims; ++i) {
          if (dimmap_odimid(i) != -1 || dimmap_idimid(i) != -1 ||
              dimmap_ounlim(i) != 0 || dimmap_iunlim(i) != 0) {
            std::fprintf(stderr, "unexpected initial state at index %d\n", i);
            return 1;
          }
        }

        if (dimmap_store(0, 2, 1, 0) != NC_NOERR ||
            dimmap_store(1, 0, 0, 1) != NC_NOERR ||
            dimmap_store(2, 3, 1, 1) != NC_NOERR) {
          std::fprintf(stderr, "dimmap_store failed\n");
          return 1;
        }

        if (dimmap_odimid(0) != 2 || dimmap_idimid(2) != 0 ||
            dimmap_iunlim(0) != 1 || dimmap_ounlim(2) != 0) {
          std::fprintf(stderr, "mismatch for mapping 0 -> 2\n");
          return 1;
        }
        if (dimmap_odimid(1) != 0 || dimmap_idimid(0) != 1 ||
            dimmap_iunlim(1) != 0 || dimmap_ounlim(0) != 1) {
          std::fprintf(stderr, "mismatch for mapping 1 -> 0\n");
          return 1;
        }
        if (dimmap_odimid(2) != 3 || dimmap_idimid(3) != 2 ||
            dimmap_iunlim(2) != 1 || dimmap_ounlim(3) != 1) {
          std::fprintf(stderr, "mismatch for mapping 2 -> 3\n");
          return 1;
        }

        // Unstored slots retain their sentinel/default values.
        if (dimmap_odimid(3) != -1 || dimmap_idimid(1) != -1 ||
            dimmap_iunlim(3) != 0 || dimmap_ounlim(1) != 0) {
          std::fprintf(stderr, "unmapped slots did not retain defaults\n");
          return 1;
        }

        // Overwrite an existing mapping and verify updated unlimited flags.
        if (dimmap_store(1, 0, 1, 0) != NC_NOERR) {
          std::fprintf(stderr, "overwrite dimmap_store failed\n");
          return 1;
        }
        if (dimmap_iunlim(1) != 1 || dimmap_ounlim(0) != 0) {
          std::fprintf(stderr, "updated unlimited flags mismatch\n");
          return 1;
        }
        return 0;
      },
      kChildArgv);

  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;
}

}  // namespace
}  // namespace netcdf
