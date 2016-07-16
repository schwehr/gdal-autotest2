// Tests public functions in cpl_hash_set:
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_hash_set.h
//   http://trac.osgeo.org/gdal/browser/trunk/gdal/port/cpl_hash_set.cpp
//
// Copyright 2014 Google Inc. All Rights Reserved.
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

#include "gunit.h"
#include "gcore/gdal_priv.h"
#include "port/cpl_hash_set.h"

namespace {

// Tests a set with strings.
TEST(CplHashSetTest, HashStr) {
  CPLHashSet* set = CPLHashSetNew(CPLHashSetHashStr,
                                  CPLHashSetEqualStr,
                                  CPLFree);
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
  CPLHashSet* set = CPLHashSetNew(CPLHashSetHashPointer,
                                  CPLHashSetEqualPointer,
                                  NULL);
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

}  // namespace
