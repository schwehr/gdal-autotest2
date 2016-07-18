// Tests managing C types with unique_ptr.
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

#include <memory>

#include "gunit.h"
#include "autotest2/cpp/util/cpl_memory.h"

namespace {

// Tests access to a duplicated string.
// Leak checkers can make sure that CPLFree is called.
TEST(CplMemoryTest, CplFreeDeleter) {
  {
    std::unique_ptr<char, autotest2::CplFreeDeleter> str(CPLStrdup("foo"));
    ASSERT_STREQ("foo", str.get());
    // Should call CPLFree() when scope closes.
  }
  SUCCEED();
}

TEST(CplMemoryTest, CplFreeDeleterNullptr) {
  {
    std::unique_ptr<char, autotest2::CplFreeDeleter> str;
    ASSERT_EQ(nullptr, str);
  }
  SUCCEED();
}

TEST(CplMemoryTest, CplFreeDeleterMore) {
  std::unique_ptr<char, autotest2::CplFreeDeleter> str;
  EXPECT_EQ(nullptr, str);
  str.reset(CPLStrdup("baz"));
  EXPECT_STREQ("baz", str.get());
  std::unique_ptr<char, autotest2::CplFreeDeleter> str2(CPLStrdup("quuux"));
  swap(str, str2);
  EXPECT_STREQ("quuux", str.get());
  EXPECT_STREQ("baz", str2.get());
  str.reset(CPLStrdup("enough"));
}

TEST(CplMemoryTest, FreeDeleter) {
  {
    std::unique_ptr<char, autotest2::FreeDeleter> str(strdup("bar"));
    ASSERT_STREQ(str.get(), "bar");
  }
  SUCCEED();
}

TEST(CplMemoryTest, FreeDeleterNullptr) {
  {
    std::unique_ptr<char, autotest2::FreeDeleter> str;
    ASSERT_EQ(nullptr, str);
  }
  SUCCEED();
}

TEST(CplMemoryTest, FreeDeleterMore) {
  std::unique_ptr<char, autotest2::FreeDeleter> str;
  EXPECT_EQ(nullptr, str);
  str.reset(strdup("baz"));
  EXPECT_STREQ("baz", str.get());
  std::unique_ptr<char, autotest2::FreeDeleter> str2(strdup("quuux"));
  swap(str, str2);
  EXPECT_STREQ("quuux", str.get());
  EXPECT_STREQ("baz", str2.get());
}


}  // namespace
