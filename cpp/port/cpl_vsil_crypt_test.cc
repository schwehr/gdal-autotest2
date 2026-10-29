// Tests for cpl_vsil_crypt:
//   https://github.com/OSGeo/gdal/blob/master/port/cpl_vsil_crypt.cpp
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
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

// //third_party/gdal:port_lib is built without HAVE_CRYPTOPP, so /vsicrypt/
// is served by VSIDummyCryptFilesystemHandler, which rejects every operation
// with CPLE_NotSupported. If crypto++ support is ever enabled, this test will
// need real round-trip coverage instead.
TEST(CplVsilCryptTest, DummyHandlerWithoutCryptoPP) {
  WithQuietHandler quiet;

  const GByte key[16] = {};
  VSISetCryptKey(key, sizeof(key));

  constexpr char kPath[] = "/vsicrypt/key=secret,file=/vsimem/test.bin";

  CPLErrorReset();
  EXPECT_EQ(nullptr, VSIFOpenL(kPath, "rb"));
  EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());

  CPLErrorReset();
  VSIStatBufL stat_buf;
  EXPECT_EQ(-1, VSIStatL(kPath, &stat_buf));
  EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
}

}  // namespace
}  // namespace autotest2
