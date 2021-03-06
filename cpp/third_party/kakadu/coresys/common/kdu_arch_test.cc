// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests for coresys/common/kdu_arch.cpp.

#include "coresys/common/kdu_arch.h"
#include "logging.h"
#include "gunit.h"

namespace kdu_core {
namespace {

TEST(KduArchTest, KduGetMmxLevel) {
  EXPECT_LE(0, kdu_get_mmx_level());
  EXPECT_GT(7, kdu_get_mmx_level());
}

// No implementation exists for kdu_get_sparcvis_exists().

TEST(KduArchTest, KduGetNumProcessors) {
  EXPECT_GE(kdu_get_num_processors(), 1);
  EXPECT_LT(kdu_get_num_processors(), 1e5);
}

}  // namespace
}  // namespace kdu_core
