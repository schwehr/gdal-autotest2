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
// Tests internal OpenJPEG CPU clock helper (src/lib/openjp2/opj_clock.c).
//
// See also:
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/opj_clock.c
//   https://github.com/uclouvain/openjpeg/blob/master/src/lib/openjp2/opj_clock.h

#include <cstdint>

#include "testing/base/public/gunit.h"
#include "third_party/openjpeg2/google/util/opj_internal.h"

namespace openjpeg2 {
namespace {

TEST(OpjClockTest, ReturnsNonNegativeMonotonicCpuTime) {
  const OPJ_FLOAT64 start = opj_clock();
  EXPECT_GE(start, 0.0);

  // Perform trivial CPU work so process user/sys time cannot decrease.
  volatile uint64_t acc = 0;
  for (uint64_t i = 0; i < 100000; ++i) {
    acc += i;
  }
  (void)acc;

  const OPJ_FLOAT64 end = opj_clock();
  EXPECT_GE(end, start);
}

}  // namespace
}  // namespace openjpeg2
