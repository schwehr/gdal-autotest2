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

#include "third_party/stcorp_harp/google/util/oom_fault_inject.h"

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/stcorp_harp/libharp/harp.h"

namespace stcorp_harp {
namespace {

TEST(OomFaultInjectTest, SweepOomHandlesProductCreation) {
  harp_set_error(HARP_SUCCESS, nullptr);
  const absl::Cleanup clear_err = [] { harp_set_error(HARP_SUCCESS, nullptr); };

  int success_count = 0;
  const int swept = SweepOomUntilSuccess([&]() {
    harp_product* product = nullptr;
    if (harp_product_new(&product) == HARP_SUCCESS) {
      ++success_count;
      harp_product_delete(product);
    } else {
      EXPECT_EQ(HARP_ERROR_OUT_OF_MEMORY, harp_errno);
    }
  });
  EXPECT_GT(swept, 0);
  EXPECT_EQ(1, success_count);
}

TEST(OomFaultInjectTest, SuppressPreventsCountdownTrigger) {
  ScopedOomCountdown countdown(1);
  {
    ScopedSuppressOom suppress;
    harp_product* product = nullptr;
    ASSERT_EQ(HARP_SUCCESS, harp_product_new(&product));
    harp_product_delete(product);
    EXPECT_FALSE(countdown.triggered());
  }
  harp_product* product = nullptr;
  EXPECT_EQ(-1, harp_product_new(&product));
  EXPECT_TRUE(countdown.triggered());
  EXPECT_EQ(nullptr, product);
  harp_set_error(HARP_SUCCESS, nullptr);
}

}  // namespace
}  // namespace stcorp_harp
