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

// Unit tests for //third_party/netcdf/ncdump/indent.c.
//
// Covers indent level initialization, incrementing, decrementing with zero
// clamping, and stdout emission across small and multi-chunk indent widths.
//
// See also:
// https://github.com/Unidata/netcdf-c/blob/main/ncdump/indent.c

#include <array>
#include <cstdio>
#include <string>
#include <string_view>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/netcdf/google/util/isolated_child.h"
#include "third_party/netcdf/google/util/ncdump_globals_stub.h"

namespace netcdf {
namespace {

constexpr std::array<std::string_view, 1> kChildArgv = {"indent_test"};

TEST(IndentTest, InitMoreLessAndClampToZero) {
  ncdump_stub_reset_globals();
  absl::Cleanup cleanup = [] {
    indent_init();
    ncdump_stub_reset_globals();
  };

  indent_init();
  EXPECT_EQ(0, indent_get());

  // Decrementing from zero clamps cleanly at zero.
  indent_less();
  EXPECT_EQ(0, indent_get());

  indent_more();
  EXPECT_EQ(2, indent_get());
  indent_more();
  EXPECT_EQ(4, indent_get());
  indent_more();
  EXPECT_EQ(6, indent_get());

  indent_less();
  EXPECT_EQ(4, indent_get());
  indent_less();
  EXPECT_EQ(2, indent_get());
  indent_less();
  EXPECT_EQ(0, indent_get());
  indent_less();
  EXPECT_EQ(0, indent_get());

  indent_more();
  indent_more();
  EXPECT_EQ(4, indent_get());
  indent_init();
  EXPECT_EQ(0, indent_get());
}

TEST(IndentTest, OutputEmitsExactSpaceCountsAcrossSmallAndMultiChunkWidths) {
  const CliResult res = RunCliInForkedChild(
      [](int, char**) -> int {
        ncdump_stub_reset_globals();
        indent_init();

        const auto emit_marked = []() {
          std::printf("[");
          indent_out();
          std::printf("]|");
        };

        // 0 spaces
        emit_marked();
        // 2, 4, 6, 8 spaces (within indent_small == 8)
        for (int i = 0; i < 4; ++i) {
          indent_more();
          emit_marked();
        }
        // 10 spaces (1 loop chunk of 8 + 2)
        indent_more();
        emit_marked();
        // 16 spaces (1 loop chunk of 8 + 8)
        indent_more();
        indent_more();
        indent_more();
        emit_marked();
        // 18 spaces (2 loop chunks of 8 + 2)
        indent_more();
        emit_marked();
        // Step down to 16 and reset to 0
        indent_less();
        emit_marked();
        indent_init();
        emit_marked();
        return 0;
      },
      kChildArgv);

  ASSERT_TRUE(res.ok()) << res.harness_error;
  EXPECT_EQ(0, res.exit_code) << res.stderr_output;

  const std::string expected =
      "[]|"
      "[  ]|"
      "[    ]|"
      "[      ]|"
      "[        ]|"
      "[          ]|"
      "[                ]|"
      "[                  ]|"
      "[                ]|"
      "[]|";
  EXPECT_EQ(expected, res.stdout_output);
}

}  // namespace
}  // namespace netcdf
