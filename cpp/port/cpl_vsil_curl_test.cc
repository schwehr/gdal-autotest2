// Tests for cpl_vsil_curl:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsil_curl.cpp
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
#include "third_party/gdal/port/cpl_vsil_curl_priv.h"

namespace autotest2 {
namespace {

TEST(CplVsilCurlTest, StubFunctionsWithoutCurl) {
  VSIInstallCurlFileHandler();
  VSICurlClearCache();
  VSICurlPartialClearCache("/vsicurl/http://example.com");
  VSICurlAuthParametersChanged();
  VSINetworkStatsReset();
  EXPECT_EQ(nullptr, VSINetworkStatsGetAsSerializedJSON(nullptr));
  EXPECT_EQ(FALSE, VSICurlInstallReadCbk(nullptr, nullptr, nullptr, FALSE));
  EXPECT_EQ(FALSE, VSICurlUninstallReadCbk(nullptr));
}

}  // namespace
}  // namespace autotest2
