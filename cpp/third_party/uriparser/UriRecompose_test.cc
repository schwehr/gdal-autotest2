// Copyright 2018 Google Inc. All Rights Reserved.
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
// Test UriRecompose.c
//
// A = ASCII

#include <cstring>

#include "gtest/gtest.h"
#include "uriparser/Uri.h"
#include "uriparser/UriBase.h"

namespace {

class UriRecomposeTest : public ::testing::Test {
 protected:
  UriRecomposeTest() {
    memset(&state_, 0, sizeof(state_));
    memset(&uri_, 0, sizeof(uri_));
    state_.uri = &uri_;
  }
  ~UriRecomposeTest() { uriFreeUriMembersA(&uri_); }

  UriParserStateA state_;
  UriUriA uri_;
};

TEST_F(UriRecomposeTest, ToStringCharsRequired) {
  ASSERT_EQ(URI_SUCCESS, uriParseUriA(&state_, "http://example.com/"));
  int length = 0;
  ASSERT_EQ(URI_SUCCESS, uriToStringCharsRequiredA(state_.uri, &length));
  EXPECT_LE(19, length);
}

}  // namespace
