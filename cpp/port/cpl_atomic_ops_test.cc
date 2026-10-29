// Tests public atomic operations:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_atomic_ops.h
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_atomic_ops.cpp
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

#include "third_party/gdal/port/cpl_atomic_ops.h"

#include "testing/base/public/gunit.h"

namespace {

// Tests adding, incrementing, and decrementing.
TEST(CplAtomicOpsTest, Atomic) {
  volatile int value = 0;

  ASSERT_EQ(1, CPLAtomicAdd(&value, 1));
  ASSERT_EQ(-1, CPLAtomicAdd(&value, -2));
  ASSERT_EQ(-1, CPLAtomicAdd(&value, -0));

  ASSERT_EQ(0, CPLAtomicInc(&value));
  ASSERT_EQ(1, CPLAtomicInc(&value));
  ASSERT_EQ(0, CPLAtomicDec(&value));
  ASSERT_EQ(-1, CPLAtomicDec(&value));

  ASSERT_TRUE(CPLAtomicCompareAndExchange(&value, -1, 42));
  ASSERT_EQ(42, value);
  ASSERT_FALSE(CPLAtomicCompareAndExchange(&value, -1, 99));
  ASSERT_EQ(42, value);
}

}  // namespace
