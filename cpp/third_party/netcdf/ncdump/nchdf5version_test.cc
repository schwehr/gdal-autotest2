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

// Unit tests for //third_party/netcdf/ncdump/nchdf5version.c.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncdump/nchdf5version.c

#include <cctype>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/netcdf/google/util/cli_mains.h"
#include "third_party/netcdf/google/util/isolated_child.h"

namespace netcdf {
namespace {

TEST(Nchdf5versionTest, PrintsZeroPaddedNineDigitHdf5Version) {
  constexpr std::string_view kArgs[] = {"nchdf5version"};
  const CliResult res = RunCliInForkedChild(nchdf5version_cli_main, kArgs);
  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code);
  EXPECT_EQ(9U, res.stdout_output.size()) << res.stdout_output;
  for (unsigned char ch : res.stdout_output) {
    EXPECT_TRUE(std::isdigit(ch)) << res.stdout_output;
  }
}

}  // namespace
}  // namespace netcdf
