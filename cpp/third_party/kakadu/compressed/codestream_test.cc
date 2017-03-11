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
// Tests for coresys/compressed/codestream.cpp.

#include "gunit.h"
#include "coresys/common/kdu_compressed.h"

namespace kdu_core {
namespace {

TEST(KduThreadsTest, Create) {
  kdu_thread_env env;
  env.create();

  for (int i=0; i < 10; i++) {
    EXPECT_TRUE(env.add_thread());
  }
  env.destroy();
}

// TODO(schwehr): More tests please!

}  // namespace
}  // namespace kdu_core
