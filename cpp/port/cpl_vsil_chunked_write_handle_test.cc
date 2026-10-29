// Tests for cpl_vsil_chunked_write_handle:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsil_chunked_write_handle.cpp
//
// Copyright 2026 Google Inc. All Rights Reserved.
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

#include "testing/base/public/gunit.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

// PLACEHOLDER: google3 gdal is built without HAVE_CURL, so the whole of
// cpl_vsil_chunked_write_handle.cpp (VSIChunkedWriteHandle) is compiled out
// and nothing in it can be exercised from this test.  The single assertion
// below only pins the !HAVE_CURL stub in cpl_vsil_curl.cpp so that this
// target is not empty.  Replace with real VSIChunkedWriteHandle tests if curl
// support is ever enabled.
TEST(CplVsilChunkedWriteHandleTest, PlaceholderWithoutCurl) {
  EXPECT_EQ(nullptr, VSINetworkStatsGetAsSerializedJSON(nullptr));
}

}  // namespace
}  // namespace autotest2
