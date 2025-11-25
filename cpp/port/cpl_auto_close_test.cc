// Test CPLJson related code
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_auto_close.h
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

#include "port/cpl_auto_close.h"

#include "gunit.h"

namespace autotest2 {
namespace {

TEST(AutoCloseTest, WillInvokeCloseFunctionEveryTimePointerGoesOutOfScope) {
  static int called = 0;

  struct AutoCloseTestContainer {
    static void Close(void *obj) { ++called; };
  };

  {
    class ToBeClosed {};

    ToBeClosed obj_1, obj_2;

    ToBeClosed *p1 = &obj_1;
    CPL_AUTO_CLOSE_WARP(p1, AutoCloseTestContainer::Close);

    ToBeClosed *p2 = &obj_2;
    CPL_AUTO_CLOSE_WARP(p2, AutoCloseTestContainer::Close);
  }

  EXPECT_EQ(called, 2);
}

}  // namespace
}  // namespace autotest2
