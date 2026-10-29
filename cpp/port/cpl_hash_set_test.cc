// Tests public functions in cpl_hash_set:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_hash_set.h
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_hash_set.cpp
//
// Copyright 2014 Google Inc. All Rights Reserved.
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

#include "third_party/gdal/port/cpl_hash_set.h"

#include <array>
#include <numeric>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace {

// Tests a set with strings.
TEST(CplHashSetTest, HashStr) {
  CPLHashSet* set =
      CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, CPLFree);
  ASSERT_NE(nullptr, set);

  ASSERT_EQ(0, CPLHashSetSize(set));
  ASSERT_EQ(nullptr, CPLHashSetLookup(set, "one"));

  // Insert returns true because "one" is not already in the set.
  ASSERT_TRUE(CPLHashSetInsert(set, CPLStrdup("one")));
  ASSERT_EQ(1, CPLHashSetSize(set));
  ASSERT_NE(nullptr, CPLHashSetLookup(set, "one"));

  // Insert returns false because "one" is already in the set.
  ASSERT_FALSE(CPLHashSetInsert(set, CPLStrdup("one")));
  ASSERT_EQ(1, CPLHashSetSize(set));

  ASSERT_TRUE(CPLHashSetRemove(set, "one"));
  ASSERT_FALSE(CPLHashSetRemove(set, "one"));
  ASSERT_EQ(0, CPLHashSetSize(set));
  ASSERT_EQ(nullptr, CPLHashSetLookup(set, "one"));

  CPLHashSetDestroy(set);
}

TEST(CplHashSetTest, HashPointer) {
  CPLHashSet* set =
      CPLHashSetNew(CPLHashSetHashPointer, CPLHashSetEqualPointer, nullptr);
  int one = 1;
  int two = 2;
  int three = 3;
  ASSERT_TRUE(CPLHashSetInsert(set, &one));
  ASSERT_TRUE(CPLHashSetInsert(set, &two));
  ASSERT_TRUE(CPLHashSetInsert(set, &three));
  ASSERT_FALSE(CPLHashSetInsert(set, &two));
  ASSERT_EQ(3, CPLHashSetSize(set));

  ASSERT_EQ(&two, CPLHashSetLookup(set, &two));
  ASSERT_TRUE(CPLHashSetRemove(set, &two));
  ASSERT_EQ(nullptr, CPLHashSetLookup(set, &two));

  CPLHashSetDestroy(set);
}

int SumValues(void* elt, void* user_data) {
  int* accum = static_cast<int*>(user_data);
  *accum += *static_cast<int*>(elt);
  return TRUE;
}

constexpr int kTestSumThreshold = 5;

int SumValuesUntilThreshold(void* elt, void* user_data) {
  int* accum = static_cast<int*>(user_data);
  *accum += *static_cast<int*>(elt);

  return *accum >= kTestSumThreshold ? FALSE : TRUE;
}

TEST(CplHashSetTest, CPLHashSetForeach) {
  constexpr int kHashSetSize = 10;
  constexpr std::array<int, kHashSetSize> data({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

  CPLHashSet* set = CPLHashSetNew(nullptr, nullptr, nullptr);
  absl::Cleanup set_cleaner = [set] { CPLHashSetDestroy(set); };

  for (int i = 0; i < kHashSetSize; ++i) {
    void* elt = const_cast<void*>(static_cast<const void*>(&data[i]));
    ASSERT_EQ(CPLHashSetInsert(set, elt), TRUE);
  }
  ASSERT_EQ(CPLHashSetSize(set), kHashSetSize);

  {
    // Since fnIterFunc (SumValues) always returns TRUE, CPLHashSetForeach()
    // should sum all entries in the set.
    int sum = 0;
    CPLHashSetForeach(set, SumValues, &sum);
    ASSERT_EQ(sum, std::reduce(data.begin(), data.end()));
  }

  {
    // fnIterFunc (SumValuesUntilThreshold) should be run, and if it returns
    // FALSE, the CPLHashSetForeach() should stop iterating in the set.
    int sum = 0;
    CPLHashSetForeach(set, SumValuesUntilThreshold, &sum);
    ASSERT_LT(sum, std::reduce(data.begin(), data.end()));
    ASSERT_GE(sum, kTestSumThreshold);
  }

  // Null callback is a no-op.
  CPLHashSetForeach(set, nullptr, nullptr);
}

TEST(CplHashSetTest, RehashClearDeferAndRecycling) {
  CPLHashSet* set =
      CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, CPLFree);
  absl::Cleanup set_cleaner = [set] { CPLHashSetDestroy(set); };

  // Insert 200 strings to trigger multiple rehashes and collisions, and fill
  // the recycling list (> 128 elements) upon removal/clear.
  for (int i = 0; i < 200; ++i) {
    EXPECT_TRUE(CPLHashSetInsert(set, CPLStrdup(CPLSPrintf("key_%d", i))));
  }
  EXPECT_EQ(200, CPLHashSetSize(set));

  // Lookup existing and non-existing keys (traverses collision chains).
  for (int i = 0; i < 200; ++i) {
    EXPECT_NE(nullptr, CPLHashSetLookup(set, CPLSPrintf("key_%d", i)));
  }
  EXPECT_EQ(nullptr, CPLHashSetLookup(set, "nonexistent_key"));

  // Remove using CPLHashSetRemoveDeferRehash down to trigger deferred rehash,
  // then insert an element to trigger the deferred rehash on insert.
  for (int i = 0; i < 150; ++i) {
    EXPECT_TRUE(CPLHashSetRemoveDeferRehash(set, CPLSPrintf("key_%d", i)));
  }
  EXPECT_EQ(50, CPLHashSetSize(set));
  EXPECT_TRUE(CPLHashSetInsert(set, CPLStrdup("trigger_deferred_rehash")));
  EXPECT_EQ(51, CPLHashSetSize(set));

  // Remove remaining elements with normal CPLHashSetRemove (triggers immediate
  // shrink rehash and reuses/returns list elements to recycling list).
  for (int i = 150; i < 200; ++i) {
    EXPECT_TRUE(CPLHashSetRemove(set, CPLSPrintf("key_%d", i)));
  }
  EXPECT_TRUE(CPLHashSetRemove(set, "trigger_deferred_rehash"));
  EXPECT_EQ(0, CPLHashSetSize(set));

  // Re-insert elements (reuses from psRecyclingList) and then CPLHashSetClear.
  for (int i = 0; i < 160; ++i) {
    EXPECT_TRUE(CPLHashSetInsert(set, CPLStrdup(CPLSPrintf("again_%d", i))));
  }
  EXPECT_EQ(160, CPLHashSetSize(set));
  CPLHashSetClear(set);
  EXPECT_EQ(0, CPLHashSetSize(set));

  // Insert a couple more so CPLHashSetDestroy also frees elements via
  // fnFreeEltFunc.
  EXPECT_TRUE(CPLHashSetInsert(set, CPLStrdup("final_1")));
  EXPECT_TRUE(CPLHashSetInsert(set, CPLStrdup("final_2")));
}

TEST(CplHashSetTest, NullStringHashAndEqual) {
  EXPECT_EQ(0UL, CPLHashSetHashStr(nullptr));
  EXPECT_FALSE(CPLHashSetEqualStr(nullptr, "abc"));
  EXPECT_FALSE(CPLHashSetEqualStr("abc", nullptr));
  EXPECT_TRUE(CPLHashSetEqualStr(nullptr, nullptr));
  EXPECT_TRUE(CPLHashSetEqualStr("abc", "abc"));
  EXPECT_FALSE(CPLHashSetEqualStr("abc", "def"));
}

}  // namespace
