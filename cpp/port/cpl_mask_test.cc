// Test convenience functions:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_mask.h
//
// Copyright 2024 Google Inc. All Rights Reserved.
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

#include "third_party/gdal/port/cpl_mask.h"

#include <cstddef>
#include <set>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(CplMaskTest, CreateMaskDefaultsRespected) {
  constexpr std::size_t size = 71;

  auto* m = CPLMaskCreate(size, /*default_value=*/true);
  absl::Cleanup mask_cleanup = [m] { VSIFree(m); };

  for (std::size_t i = 0; i < size; ++i) {
    EXPECT_TRUE(CPLMaskGet(m, i));
  }
}

TEST(CplMaskTest, CplMaskSetIsAbleToSetBitsInMask) {
  constexpr std::size_t size = 71;

  auto* m = CPLMaskCreate(size, /*default_value=*/false);
  absl::Cleanup mask_cleanup = [m] { VSIFree(m); };

  std::set<int> kTestBitIndices = {0, 10, 33, 70};
  for (const int i : kTestBitIndices) {
    CPLMaskSet(m, i);
  }

  for (std::size_t i = 0; i < size; ++i) {
    if (kTestBitIndices.contains(i)) {
      EXPECT_TRUE(CPLMaskGet(m, i));
    } else {
      EXPECT_FALSE(CPLMaskGet(m, i));
    }
  }
}

TEST(CplMaskTest, CplMaskClearIsAbleToClearBitsInMask) {
  constexpr std::size_t size = 71;

  auto* m = CPLMaskCreate(size, /*default_value=*/true);
  absl::Cleanup mask_cleanup = [m] { VSIFree(m); };

  std::set<int> kTestBitIndices = {0, 10, 33, 70};
  for (const int i : kTestBitIndices) {
    CPLMaskClear(m, i);
  }

  for (std::size_t i = 0; i < size; ++i) {
    if (kTestBitIndices.contains(i)) {
      EXPECT_FALSE(CPLMaskGet(m, i));
    } else {
      EXPECT_TRUE(CPLMaskGet(m, i));
    }
  }
}

TEST(CplMaskTest, CplMaskMergeMergesSetBits) {
  constexpr std::size_t size = 71;

  auto* m1 = CPLMaskCreate(size, /*default_value=*/false);
  absl::Cleanup m1_cleanup = [m1] { VSIFree(m1); };
  auto* m2 = CPLMaskCreate(size, /*default_value=*/false);
  absl::Cleanup m2_cleanup = [m2] { VSIFree(m2); };

  std::set<int> kTestBitIndices1 = {0, 13};
  for (const int i : kTestBitIndices1) {
    CPLMaskSet(m1, i);
  }

  for (std::size_t i = 0; i < size; ++i) {
    if (kTestBitIndices1.contains(i)) {
      EXPECT_TRUE(CPLMaskGet(m1, i));
    } else {
      EXPECT_FALSE(CPLMaskGet(m1, i));
    }
  }

  std::set<int> kTestBitIndices2 = {7, 51};
  for (const int i : kTestBitIndices2) {
    CPLMaskSet(m2, i);
  }

  for (std::size_t i = 0; i < size; ++i) {
    if (kTestBitIndices2.contains(i)) {
      EXPECT_TRUE(CPLMaskGet(m2, i));
    } else {
      EXPECT_FALSE(CPLMaskGet(m2, i));
    }
  }

  CPLMaskMerge(m1, m2, size);

  // m1 should have the outcome of the merge
  for (std::size_t i = 0; i < size; ++i) {
    if (kTestBitIndices1.contains(i) || kTestBitIndices2.contains(i)) {
      EXPECT_TRUE(CPLMaskGet(m1, i));
    } else {
      EXPECT_FALSE(CPLMaskGet(m1, i));
    }
  }

  // m2 should be unchanged
  for (std::size_t i = 0; i < size; ++i) {
    if (kTestBitIndices2.contains(i)) {
      EXPECT_TRUE(CPLMaskGet(m2, i));
    } else {
      EXPECT_FALSE(CPLMaskGet(m2, i));
    }
  }
}

TEST(CplMaskTest, CplMaskClearAllClearsAllBits) {
  constexpr std::size_t size = 71;

  auto* m = CPLMaskCreate(size, /*default_value=*/false);
  absl::Cleanup mask_cleanup = [m] { VSIFree(m); };

  std::set<int> kTestBitIndices = {9, 36};
  for (const int i : kTestBitIndices) {
    CPLMaskSet(m, i);
  }

  for (std::size_t i = 0; i < size; ++i) {
    if (kTestBitIndices.contains(i)) {
      EXPECT_TRUE(CPLMaskGet(m, i));
    } else {
      EXPECT_FALSE(CPLMaskGet(m, i));
    }
  }

  CPLMaskClearAll(m, size);

  for (std::size_t i = 0; i < size; ++i) {
    EXPECT_FALSE(CPLMaskGet(m, i));
  }
}

TEST(CplMaskTest, CplMaskSetAllSetsAllBits) {
  constexpr std::size_t size = 71;

  auto* m = CPLMaskCreate(size, /*default_value=*/false);
  absl::Cleanup mask_cleanup = [m] { VSIFree(m); };

  std::set<int> kTestBitIndices = {9, 36};
  for (const int i : kTestBitIndices) {
    CPLMaskSet(m, i);
  }

  for (std::size_t i = 0; i < size; ++i) {
    if (kTestBitIndices.contains(i)) {
      EXPECT_TRUE(CPLMaskGet(m, i));
    } else {
      EXPECT_FALSE(CPLMaskGet(m, i));
    }
  }

  CPLMaskSetAll(m, size);

  for (std::size_t i = 0; i < size; ++i) {
    EXPECT_TRUE(CPLMaskGet(m, i));
  }
}

}  // namespace
}  // namespace autotest2
